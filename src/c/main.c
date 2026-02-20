#include <pebble.h>

// Persistent storage key
#define SETTINGS_KEY 1
#define HR_ALERT_DELTA_BPM 30
#define HR_ALERT_WINDOW_SEC 60
#define HR_SAMPLE_BUFFER_SIZE 96
#define HR_FAST_SAMPLE_PERIOD_SEC 1

// Define our settings struct
typedef struct ClaySettings {
  GColor BackgroundColor;
  GColor TextColor;
  bool TemperatureUnit; // false = Celsius, true = Fahrenheit
  bool ShowDate;
} ClaySettings;

// An instance of the struct
static ClaySettings settings;

static Window *s_main_window;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_hr_layer;
static TextLayer *s_weather_layer;

// Custom fonts
static GFont s_time_font;
static GFont s_date_font;

// Battery
static Layer *s_battery_layer;
static int s_battery_level;

// Bluetooth
static BitmapLayer *s_bt_icon_layer;
static GBitmap *s_bt_icon_bitmap;

// Unobstructed area
static Layer *s_window_layer;
static AppTimer *s_hr_alert_timer;
static bool s_hr_alert_active;
#if defined(PBL_HEALTH)
static HealthValue s_hr_sample_values[HR_SAMPLE_BUFFER_SIZE];
static time_t s_hr_sample_times[HR_SAMPLE_BUFFER_SIZE];
static int s_hr_sample_count;
#endif
static HealthValue s_last_filtered_hr;
static HealthValue s_last_raw_hr;
static uint32_t s_last_window_delta;

static void prv_update_display();
#if defined(PBL_HEALTH)
static void hr_alert_timer_callback(void *context);
#endif

static void prv_update_hr_display() {
  static char s_hr_buffer[24];

  if (s_last_filtered_hr > 0) {
    snprintf(s_hr_buffer, sizeof(s_hr_buffer), "%lu BPM | Δ%lu",
             (uint32_t)s_last_filtered_hr,
             (uint32_t)s_last_window_delta);
  } else {
    snprintf(s_hr_buffer, sizeof(s_hr_buffer), "-- BPM");
  }

  text_layer_set_text(s_hr_layer, s_hr_buffer);
}

#if defined(PBL_HEALTH)
static void prv_set_hr_alert_active(bool active) {
  if (s_hr_alert_timer) {
    app_timer_cancel(s_hr_alert_timer);
    s_hr_alert_timer = NULL;
  }

  s_hr_alert_active = active;
  prv_update_display();

  if (active) {
    s_hr_alert_timer = app_timer_register(HR_ALERT_WINDOW_SEC * 1000, hr_alert_timer_callback, NULL);
  }
}

static void hr_alert_timer_callback(void *context) {
  s_hr_alert_timer = NULL;
  s_hr_alert_active = false;
  prv_update_display();
}
#endif

#if defined(PBL_HEALTH)
/**
 * Returns the current heart-rate value for the given metric when available,
 * otherwise 0. This centralizes accessibility checks for both filtered and
 * raw BPM queries.
 */
static HealthValue prv_get_heart_rate_metric(HealthMetric metric) {
  HealthServiceAccessibilityMask accessible =
      health_service_metric_accessible(metric, time(NULL), time(NULL));
  if (!(accessible & HealthServiceAccessibilityMaskAvailable)) {
    return 0;
  }

  HealthValue value = health_service_peek_current_value(metric);
  return value > 0 ? value : 0;
}

/**
 * Removes raw HR samples that are older than the configured alert window.
 */
static void prv_prune_old_hr_samples(time_t now) {
  while (s_hr_sample_count > 0 && (now - s_hr_sample_times[0]) > HR_ALERT_WINDOW_SEC) {
    for (int index = 1; index < s_hr_sample_count; index++) {
      s_hr_sample_times[index - 1] = s_hr_sample_times[index];
      s_hr_sample_values[index - 1] = s_hr_sample_values[index];
    }
    s_hr_sample_count--;
  }
}

/**
 * Stores a new raw HR sample and keeps the sample buffer bounded.
 */
static void prv_store_raw_hr_sample(HealthValue raw_hr, time_t now) {
  if (raw_hr <= 0) {
    return;
  }

  prv_prune_old_hr_samples(now);

  if (s_hr_sample_count >= HR_SAMPLE_BUFFER_SIZE) {
    for (int index = 1; index < s_hr_sample_count; index++) {
      s_hr_sample_times[index - 1] = s_hr_sample_times[index];
      s_hr_sample_values[index - 1] = s_hr_sample_values[index];
    }
    s_hr_sample_count--;
  }

  s_hr_sample_times[s_hr_sample_count] = now;
  s_hr_sample_values[s_hr_sample_count] = raw_hr;
  s_hr_sample_count++;
}

/**
 * Calculates max-min BPM from the raw HR samples in the current alert window.
 * This catches both sudden rises and sudden drops quickly.
 */
static uint32_t prv_calculate_window_delta_bpm(void) {
  if (s_hr_sample_count < 2) {
    return 0;
  }

  HealthValue min_value = s_hr_sample_values[0];
  HealthValue max_value = s_hr_sample_values[0];

  for (int index = 1; index < s_hr_sample_count; index++) {
    if (s_hr_sample_values[index] < min_value) {
      min_value = s_hr_sample_values[index];
    }
    if (s_hr_sample_values[index] > max_value) {
      max_value = s_hr_sample_values[index];
    }
  }

  return (uint32_t)(max_value - min_value);
}

/**
 * Applies alert rules for the latest computed window delta. If the threshold is
 * met, background alert is activated and vibration is emitted once per event.
 */
static void prv_evaluate_hr_alert(uint32_t delta_bpm) {
  if (delta_bpm < HR_ALERT_DELTA_BPM) {
    return;
  }

  if (!s_hr_alert_active) {
    vibes_short_pulse();
  }
  prv_set_hr_alert_active(true);
}

/**
 * Reads current HR metrics, updates the display values, records raw samples,
 * and evaluates the 60-second jump/drop alert.
 */
static void prv_handle_heart_rate_update(void) {
  s_last_filtered_hr = prv_get_heart_rate_metric(HealthMetricHeartRateBPM);
  s_last_raw_hr = prv_get_heart_rate_metric(HealthMetricHeartRateRawBPM);

  if (s_last_raw_hr > 0) {
    time_t now = time(NULL);
    prv_store_raw_hr_sample(s_last_raw_hr, now);
    s_last_window_delta = prv_calculate_window_delta_bpm();
    prv_evaluate_hr_alert(s_last_window_delta);
  }

  if (s_last_filtered_hr > 0 || s_last_raw_hr > 0) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "HR filtered=%lu raw=%lu delta=%lu", (uint32_t)s_last_filtered_hr,
            (uint32_t)s_last_raw_hr, (uint32_t)s_last_window_delta);
  }

  prv_update_hr_display();
}

static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventHeartRateUpdate) {
    prv_handle_heart_rate_update();
  }
}
#endif

// Set default settings
static void prv_default_settings() {
  settings.BackgroundColor = GColorBlack;
  settings.TextColor = GColorWhite;
  settings.TemperatureUnit = false; // Celsius
  settings.ShowDate = true;
}

// Save settings to persistent storage
static void prv_save_settings() {
  persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));
}

// Read settings from persistent storage
static void prv_load_settings() {
  // Set defaults first
  prv_default_settings();
  // Then override with any saved values
  persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));
}

// Apply settings to UI elements
static void prv_update_display() {
  // Set background color
  GColor bg_color = s_hr_alert_active ? PBL_IF_COLOR_ELSE(GColorRed, settings.BackgroundColor)
                                      : settings.BackgroundColor;
  window_set_background_color(s_main_window, bg_color);

  // Set text colors
  text_layer_set_text_color(s_time_layer, settings.TextColor);
  text_layer_set_text_color(s_date_layer, settings.TextColor);
  text_layer_set_text_color(s_hr_layer, settings.TextColor);
  text_layer_set_text_color(s_weather_layer, settings.TextColor);

  // Show/hide date based on setting
  layer_set_hidden(text_layer_get_layer(s_date_layer), !settings.ShowDate);

  // Mark battery layer for redraw (color may have changed)
  layer_mark_dirty(s_battery_layer);
}

static void update_time() {
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  static char s_time_buffer[8];
  strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ?
                                                    "%H:%M" : "%I:%M", tick_time);
  text_layer_set_text(s_time_layer, s_time_buffer);

  static char s_date_buffer[16];
  strftime(s_date_buffer, sizeof(s_date_buffer), "%a %b %d", tick_time);
  text_layer_set_text(s_date_layer, s_date_buffer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();

  // Get weather update every 30 minutes
  if (tick_time->tm_min % 30 == 0) {
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
    app_message_outbox_send();
  }
}

static void battery_callback(BatteryChargeState state) {
  s_battery_level = state.charge_percent;
  layer_mark_dirty(s_battery_layer);
}

static void battery_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Find the width of the bar (inside the border)
  int bar_width = ((s_battery_level * (bounds.size.w - 4)) / 100);

  // Draw the border using the text color
  graphics_context_set_stroke_color(ctx, settings.TextColor);
  graphics_draw_round_rect(ctx, bounds, 2);

  // Choose color based on battery level
  GColor bar_color;
  if (s_battery_level <= 20) {
    bar_color = PBL_IF_COLOR_ELSE(GColorRed, settings.TextColor);
  } else if (s_battery_level <= 40) {
    bar_color = PBL_IF_COLOR_ELSE(GColorChromeYellow, settings.TextColor);
  } else {
    bar_color = PBL_IF_COLOR_ELSE(GColorGreen, settings.TextColor);
  }

  // Draw the filled bar inside the border
  graphics_context_set_fill_color(ctx, bar_color);
  graphics_fill_rect(ctx, GRect(2, 2, bar_width, bounds.size.h - 4), 1, GCornerNone);
}

static void bluetooth_callback(bool connected) {
  // Show icon if disconnected
  layer_set_hidden(bitmap_layer_get_layer(s_bt_icon_layer), connected);

  if (!connected) {
    vibes_double_pulse();
  }
}

// AppMessage received handler
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  // Check for weather data
  Tuple *temp_tuple = dict_find(iterator, MESSAGE_KEY_TEMPERATURE);
  Tuple *conditions_tuple = dict_find(iterator, MESSAGE_KEY_CONDITIONS);

  if (temp_tuple && conditions_tuple) {
    static char temperature_buffer[8];
    static char conditions_buffer[32];
    static char weather_layer_buffer[32];

    int temp_value = (int)temp_tuple->value->int32;

    // Convert to Fahrenheit if setting is enabled
    if (settings.TemperatureUnit) {
      temp_value = (temp_value * 9 / 5) + 32;
      snprintf(temperature_buffer, sizeof(temperature_buffer), "%d°F", temp_value);
    } else {
      snprintf(temperature_buffer, sizeof(temperature_buffer), "%d°C", temp_value);
    }

    snprintf(conditions_buffer, sizeof(conditions_buffer), "%s", conditions_tuple->value->cstring);
    snprintf(weather_layer_buffer, sizeof(weather_layer_buffer), "%s %s", temperature_buffer, conditions_buffer);
    text_layer_set_text(s_weather_layer, weather_layer_buffer);
  }

  // Check for Clay settings data
  Tuple *bg_color_t = dict_find(iterator, MESSAGE_KEY_BackgroundColor);
  if (bg_color_t) {
    settings.BackgroundColor = GColorFromHEX(bg_color_t->value->int32);
  }

  Tuple *text_color_t = dict_find(iterator, MESSAGE_KEY_TextColor);
  if (text_color_t) {
    settings.TextColor = GColorFromHEX(text_color_t->value->int32);
  }

  Tuple *temp_unit_t = dict_find(iterator, MESSAGE_KEY_TemperatureUnit);
  if (temp_unit_t) {
    settings.TemperatureUnit = temp_unit_t->value->int32 == 1;
  }

  Tuple *show_date_t = dict_find(iterator, MESSAGE_KEY_ShowDate);
  if (show_date_t) {
    settings.ShowDate = show_date_t->value->int32 == 1;
  }

  // Save and apply if any settings were changed
  if (bg_color_t || text_color_t || temp_unit_t || show_date_t) {
    prv_save_settings();
    prv_update_display();

    // Refetch weather if the temperature unit changed so the display updates
    if (temp_unit_t) {
      DictionaryIterator *iter;
      app_message_outbox_begin(&iter);
      dict_write_uint8(iter, MESSAGE_KEY_REQUEST_WEATHER, 1);
      app_message_outbox_send();
    }
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}

// Unobstructed area handlers
#if !defined(PBL_PLATFORM_APLITE)
static void prv_unobstructed_will_change(GRect final_unobstructed_screen_area, void *context) {
  // Hide BT icon during the transition to reduce clutter
  layer_set_hidden(bitmap_layer_get_layer(s_bt_icon_layer), true);
}

static void prv_unobstructed_change(AnimationProgress progress, void *context) {
  GRect bounds = layer_get_unobstructed_bounds(s_window_layer);

  // Reposition time, date, and weather to fit in the available space
  int date_height = 30;
  int block_height = 56 + date_height;
  int time_y = (bounds.size.h / 2) - (block_height / 2) - 10;
  int date_y = time_y + 56;
  int hr_y = bounds.size.h - PBL_IF_ROUND_ELSE(60, 50);
  int weather_y = bounds.size.h - PBL_IF_ROUND_ELSE(40, 30);

  GRect time_frame = layer_get_frame(text_layer_get_layer(s_time_layer));
  time_frame.origin.y = time_y;
  layer_set_frame(text_layer_get_layer(s_time_layer), time_frame);

  GRect date_frame = layer_get_frame(text_layer_get_layer(s_date_layer));
  date_frame.origin.y = date_y;
  layer_set_frame(text_layer_get_layer(s_date_layer), date_frame);

  GRect hr_frame = layer_get_frame(text_layer_get_layer(s_hr_layer));
  hr_frame.origin.y = hr_y;
  layer_set_frame(text_layer_get_layer(s_hr_layer), hr_frame);

  GRect weather_frame = layer_get_frame(text_layer_get_layer(s_weather_layer));
  weather_frame.origin.y = weather_y;
  layer_set_frame(text_layer_get_layer(s_weather_layer), weather_frame);
}

static void prv_unobstructed_did_change(void *context) {
  GRect full_bounds = layer_get_bounds(s_window_layer);
  GRect bounds = layer_get_unobstructed_bounds(s_window_layer);
  bool obstructed = !grect_equal(&full_bounds, &bounds);

  // Keep BT icon hidden when obstructed, otherwise restore based on connection
  if (obstructed) {
    layer_set_hidden(bitmap_layer_get_layer(s_bt_icon_layer), true);
  } else {
    layer_set_hidden(bitmap_layer_get_layer(s_bt_icon_layer),
      connection_service_peek_pebble_app_connection());
  }
}
#endif

static void main_window_load(Window *window) {
  s_window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(s_window_layer);

  // Load custom fonts
  s_time_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_JERSEY_56));
  s_date_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_JERSEY_24));

  // Center the time + date block vertically
  int date_height = 30;
  int block_height = 56 + date_height;
  int time_y = (bounds.size.h / 2) - (block_height / 2) - 10;
  int date_y = time_y + 56;

  // Create the time TextLayer
  s_time_layer = text_layer_create(
      GRect(0, time_y, bounds.size.w, 60));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, settings.TextColor);
  text_layer_set_font(s_time_layer, s_time_font);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);

  // Create the date TextLayer — just below the time
  s_date_layer = text_layer_create(
      GRect(0, date_y, bounds.size.w, 30));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, settings.TextColor);
  text_layer_set_font(s_date_layer, s_date_font);
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);

  int hr_y = bounds.size.h - PBL_IF_ROUND_ELSE(60, 50);
  s_hr_layer = text_layer_create(
      GRect(0, hr_y, bounds.size.w, 22));
  text_layer_set_background_color(s_hr_layer, GColorClear);
  text_layer_set_text_color(s_hr_layer, settings.TextColor);
  text_layer_set_font(s_hr_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_hr_layer, GTextAlignmentCenter);
  text_layer_set_text(s_hr_layer, "-- BPM");

  // Create weather TextLayer — aligned to the bottom of the screen
  int weather_y = bounds.size.h - PBL_IF_ROUND_ELSE(40, 30);
  s_weather_layer = text_layer_create(
      GRect(0, weather_y, bounds.size.w, 25));
  text_layer_set_background_color(s_weather_layer, GColorClear);
  text_layer_set_text_color(s_weather_layer, settings.TextColor);
  text_layer_set_font(s_weather_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_weather_layer, GTextAlignmentCenter);
  text_layer_set_text(s_weather_layer, "Loading...");

  // Create battery meter Layer — visible bar near the top
  int bar_width = bounds.size.w / 2;
  int bar_x = (bounds.size.w - bar_width) / 2;
  int bar_y = PBL_IF_ROUND_ELSE(bounds.size.h / 8, bounds.size.h / 28);
  s_battery_layer = layer_create(GRect(bar_x, bar_y, bar_width, 8));
  layer_set_update_proc(s_battery_layer, battery_update_proc);

  // Create the Bluetooth icon GBitmap
  s_bt_icon_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_BT_ICON);
  int bt_y = bar_y + 12;
  s_bt_icon_layer = bitmap_layer_create(GRect((bounds.size.w - 30) / 2, bt_y, 30, 30));
  bitmap_layer_set_bitmap(s_bt_icon_layer, s_bt_icon_bitmap);
  bitmap_layer_set_compositing_mode(s_bt_icon_layer, GCompOpSet);

  // Add layers to the Window
  layer_add_child(s_window_layer, text_layer_get_layer(s_time_layer));
  layer_add_child(s_window_layer, text_layer_get_layer(s_date_layer));
  layer_add_child(s_window_layer, text_layer_get_layer(s_hr_layer));
  layer_add_child(s_window_layer, text_layer_get_layer(s_weather_layer));
  layer_add_child(s_window_layer, s_battery_layer);
  layer_add_child(s_window_layer, bitmap_layer_get_layer(s_bt_icon_layer));

  // Apply saved settings
  prv_update_display();

  #if !defined(PBL_PLATFORM_APLITE)
  // Apply correct layout in case Quick View is already active
  prv_unobstructed_change(0, NULL);
  prv_unobstructed_did_change(NULL);

  // Subscribe to unobstructed area events
  UnobstructedAreaHandlers handlers = {
    .will_change = prv_unobstructed_will_change,
    .change = prv_unobstructed_change,
    .did_change = prv_unobstructed_did_change
  };
  unobstructed_area_service_subscribe(handlers, NULL);
  #endif
}

static void main_window_unload(Window *window) {
  #if !defined(PBL_PLATFORM_APLITE)
  unobstructed_area_service_unsubscribe();
  #endif

  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_hr_layer);
  text_layer_destroy(s_weather_layer);
  fonts_unload_custom_font(s_time_font);
  fonts_unload_custom_font(s_date_font);
  layer_destroy(s_battery_layer);
  gbitmap_destroy(s_bt_icon_bitmap);
  bitmap_layer_destroy(s_bt_icon_layer);
}

static void init() {
  // Load settings before creating UI
  prv_load_settings();

  s_main_window = window_create();
  window_set_background_color(s_main_window, settings.BackgroundColor);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  update_time();

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  battery_state_service_subscribe(battery_callback);
  battery_callback(battery_state_service_peek());

  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = bluetooth_callback
  });

  #if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
  health_service_set_heart_rate_sample_period(HR_FAST_SAMPLE_PERIOD_SEC);
  prv_handle_heart_rate_update();
  #else
  s_last_filtered_hr = 0;
  s_last_raw_hr = 0;
  s_last_window_delta = 0;
  prv_update_hr_display();
  #endif

  // Register AppMessage callbacks
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  // Open AppMessage
  const int inbox_size = 256;
  const int outbox_size = 256;
  app_message_open(inbox_size, outbox_size);
}

static void deinit() {
  if (s_hr_alert_timer) {
    app_timer_cancel(s_hr_alert_timer);
    s_hr_alert_timer = NULL;
  }

  #if defined(PBL_HEALTH)
  health_service_set_heart_rate_sample_period(0);
  health_service_events_unsubscribe();
  #endif
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
