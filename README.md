# pebble-potsface

A feature-rich Pebble watchface with weather, heart rate monitoring, and customizable display options. Built for the Pebble SDK 3 with support for all major Pebble watch platforms.

## Features

### Time & Date
- Large, clear time display using a custom Jersey font in 12 or 24-hour format (follows device settings)
- Date display showing day of week, month, and day (e.g., "Mon Jan 15")
- Date visibility can be toggled via settings

### Weather
- Real-time weather fetched from the [Open-Meteo API](https://open-meteo.com/) — no API key required
- Displays current temperature and a human-readable condition (e.g., "Clear", "Cloudy", "Rain", "T-Storm")
- Uses your phone's geolocation to show local weather
- Automatically refreshes every 30 minutes

### Heart Rate Monitoring
- Displays current heart rate in BPM and the rate of change (e.g., "120 BPM | Δ15")
- **HR alert system**: monitors a 60-second sliding window of samples; if heart rate changes by more than 30 BPM, an alert fires — the background turns red (on color displays) and the watch vibrates
- Alert clears automatically after 60 seconds
- Only available on watches with health hardware; shows "-- BPM" on unsupported devices

### Battery Indicator
- Visual bar showing the current battery level
- Color-coded on supported displays: red (≤20%), yellow (21–40%), green (≥41%)

### Bluetooth Status
- Displays a Bluetooth icon when disconnected from your phone
- Vibrates with a double pulse on disconnection

## Settings

Configurable via the Pebble app settings (powered by Clay):

| Setting | Default | Description |
|---|---|---|
| Background Color | Black | Watchface background color |
| Text Color | White | Color for all text elements |
| Temperature Unit | Celsius | Toggle between °C and °F |
| Show Date | On | Show or hide the date display |

## Platform Support

Compatible with all major Pebble watch platforms:

- **Aplite** — Pebble Classic / Steel (black & white)
- **Basalt** — Pebble Time / Steel (color)
- **Chalk** — Pebble Time Round (color, round)
- **Diorite** — Pebble 2 (black & white)
- **Emery** — Pebble Time 2 (color, round)
- **Flint** — Pebble Time Round (black & white variant)
- **Gabbro** — Pebble 2 SE (color, round)

UI elements are dynamically repositioned on round displays and adapt to system overlays using the Pebble UnobstructedArea API.

## Building

Requires the Pebble SDK 3 and Node.js (for the Clay settings UI dependency).

```sh
npm install
pebble build
```
