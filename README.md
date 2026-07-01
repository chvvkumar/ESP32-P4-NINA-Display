<h1 align="center">
  <img src="images/logo.png" alt="logo" height="40" style="vertical-align: middle;"> ESP32-P4 NINA Display
</h1>

<p align="center">
  <a href="https://github.com/chvvkumar/ESP32-P4-NINA-Display/actions/workflows/build.yml?query=branch%3Amain"><img src="https://img.shields.io/github/actions/workflow/status/chvvkumar/ESP32-P4-NINA-Display/build.yml?branch=main&label=build%20(main)&logo=github" alt="Build (main)"></a>
  <a href="https://github.com/chvvkumar/ESP32-P4-NINA-Display/actions/workflows/build.yml?query=branch%3Adev"><img src="https://img.shields.io/github/actions/workflow/status/chvvkumar/ESP32-P4-NINA-Display/build.yml?branch=dev&label=build%20(dev)&logo=github" alt="Build (dev)"></a>
</p>

<p align="center">
  <a href="https://github.com/chvvkumar/ESP32-P4-NINA-Display/releases/latest"><img src="https://img.shields.io/github/v/release/chvvkumar/ESP32-P4-NINA-Display?label=version&logo=semver&logoColor=white" alt="Version"></a>
  <a href="https://github.com/espressif/esp-idf/tree/v5.5.2"><img src="https://img.shields.io/badge/ESP--IDF-v5.5.2-blue?logo=espressif" alt="ESP-IDF"></a>
  <a href="https://lvgl.io/"><img src="https://img.shields.io/badge/LVGL-v9.5.0-red?logo=data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCI+PHBhdGggZmlsbD0iI2ZmZiIgZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==" alt="LVGL"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.flash_size&label=Flash&logo=memory&color=orange" alt="Flash Size">
  <img src="https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.psram&label=PSRAM&logo=memory&color=purple" alt="PSRAM">
  <img src="https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.factory_size&label=Factory%20Binary&logo=chip&color=green" alt="Factory Size">
  <img src="https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.ota_size&label=OTA%20Binary&logo=chip&color=green" alt="OTA Size">
</p>

A touchscreen dashboard for [N.I.N.A. astrophotography software](https://nighttime-imaging.eu/), built for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** (720x720). It polls the [ninaAPI Advanced plugin](https://github.com/christian-photo/ninaAPI) over HTTP and WebSocket to show real-time session data: exposure arcs, guiding RMS, filter status, sequence progress, power draw, and more. Monitor up to three separate N.I.N.A. instances from one device. Beyond NINA it also runs a Spotify Now Playing screen, AllSky environmental panel, clock with weather and 3D moon phase, and GOES/solar imagery.

<p align="center">
  <img src="images/pic1.jpg" alt="Device" width="1200">
</p>

<p align="center">
  <img src="images/imagepreview.jpg" alt="Image Preview" width="1200">
</p>

## Table of Contents

- [Screenshots](#screenshots)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [First-Time Setup](#first-time-setup)
- [Security](#security)
- [Display Interface](#display-interface)
- [Features](#features)
- [Web API](#web-api)

---

## Screenshots

### Device Display

<table align="center">
  <tr>
    <td align="center"><img src="images/summary.jpg" alt="Summary Page" width="480"></td>
    <td align="center"><img src="images/R.jpg" alt="Red Filter" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/Ha.jpg" alt="Hydrogen-Alpha Filter" width="480"></td>
    <td align="center"><img src="images/Oiii.jpg" alt="OIII Filter" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/B1.jpg" alt="Broadband Filter" width="480"></td>
    <td align="center"><img src="images/autofocus.jpg" alt="Autofocus" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/camera%20statistics.jpg" alt="Camera Statistics" width="480"></td>
    <td align="center"><img src="images/image_statistics.jpg" alt="Image Statistics" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/RMS.jpg" alt="RMS Graph" width="480"></td>
    <td align="center"><img src="images/RA_DEC.jpg" alt="RA/DEC Graph" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/HFR.jpg" alt="HFR Graph" width="480"></td>
    <td align="center"><img src="images/HFR_history.jpg" alt="HFR History Graph" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/display_settings.jpg" alt="On-device Settings" width="480"></td>
    <td align="center"><img src="images/systemstats.jpg" alt="System Stats" width="480"></td>
  </tr>
</table>

### Toast Notifications

<table align="center">
  <tr>
    <td align="center"><img src="images/cam_notifications.jpg" alt="Camera Connected Notification" width="480"></td>
    <td align="center"><img src="images/sequence_notifications.jpg" alt="Sequence Started Notification" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/safe_safety_notifications.jpg" alt="Safe Notification" width="480"></td>
    <td align="center"><img src="images/unsafe_safety_notifications.jpg" alt="Unsafe Notification" width="480"></td>
  </tr>
</table>

### Spotify Album Art Display

<table align="center">
  <tr>
    <td align="center" colspan="2"><img src="images/Spotify_desk.jpg" alt="Spotify on two devices" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/Spotify_overlay.jpg" alt="Spotify overlay with controls" width="480"></td>
    <td align="center"><img src="images/Spotify_overlay_minimal.jpg" alt="Spotify minimal overlay" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>With controls</em></td>
    <td align="center"><em>Controls hidden</em></td>
  </tr>
</table>

### Clock & Weather

<table align="center">
  <tr>
    <td align="center"><img src="images/clock-face.jpg" alt="Clock Face with Weather" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>Clock face with weather data and forecast bars</em></td>
  </tr>
</table>

### Moon Phase Display

Real-time 3D rendered Moon with accurate phase, libration, and sub-solar lighting. Optional glow halo and starfield background.

<table align="center">
  <tr>
    <td align="center"><img src="images/moon.jpg" alt="Moon phase" width="480"></td>
    <td align="center"><img src="images/moon_glow.jpg" alt="Moon with glow halo" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>Default</em></td>
    <td align="center"><em>Glow halo</em></td>
  </tr>
  <tr>
    <td align="center"><img src="images/moon_stars.jpg" alt="Moon with starfield" width="480"></td>
    <td align="center"><img src="images/moon_glow_stars.jpg" alt="Moon with glow and starfield" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>Starfield background</em></td>
    <td align="center"><em>Glow and starfield</em></td>
  </tr>
</table>

### Web Configuration UI

Every setting is managed from the on-device web UI, organized into tabs.

<table align="center">
  <tr>
    <td align="center"><img src="images/settings_nina.jpg" alt="N.I.N.A. instance settings" width="480"></td>
    <td align="center"><img src="images/settings_display.jpg" alt="Display and page navigation settings" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>N.I.N.A. instances (hosts, filter colors, RMS/HFR thresholds)</em></td>
    <td align="center"><em>Display (theme, home page, auto-cycle, page rotation)</em></td>
  </tr>
  <tr>
    <td align="center"><img src="images/settings_behavior.jpg" alt="Behavior settings" width="480"></td>
    <td align="center"><img src="images/settings_system.jpg" alt="System settings" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>Behavior (power, polling, notifications)</em></td>
    <td align="center"><em>System (network, MQTT, firmware, auth)</em></td>
  </tr>
  <tr>
    <td align="center"><img src="images/settings_clock.jpg" alt="Clock and weather settings" width="480"></td>
    <td align="center"><img src="images/settings_spotify.jpg" alt="Spotify settings" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>Clock &amp; Weather (provider, location, units)</em></td>
    <td align="center"><em>Spotify (client ID, playback options)</em></td>
  </tr>
  <tr>
    <td align="center"><img src="images/settings_allsky.jpg" alt="AllSky settings" width="480"></td>
    <td align="center"><img src="images/settings_image.jpg" alt="Image display settings" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>AllSky (connection, per-quadrant field mappings)</em></td>
    <td align="center"><em>Image Display (GOES / Moon / Solar / custom URL)</em></td>
  </tr>
  <tr>
    <td align="center"><img src="images/settings_logs.jpg" alt="Logs tab" width="480"></td>
    <td align="center"><img src="images/settings_backup.jpg" alt="Backup and restore tab" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>Logs (live console output, crash log, download)</em></td>
    <td align="center"><em>Backup (export / restore configuration)</em></td>
  </tr>
  <tr>
    <td align="center" colspan="2"><img src="images/settings_api.jpg" alt="HTTP API reference tab" width="480"></td>
  </tr>
  <tr>
    <td align="center" colspan="2"><em>API (in-UI reference for every HTTP endpoint)</em></td>
  </tr>
</table>

---

## Prerequisites

### Hardware

- **[Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416)** — currently the only tested board. The layout is tuned for its 720x720 display.
- **[3D Printed Stand](https://www.thingiverse.com/thing:7321463)** (optional) — a printable stand for the display, designed by [@chicago925](https://github.com/chicago925) ([#116](https://github.com/chvvkumar/ESP32-P4-NINA-Display/issues/116)).

### Software (on the N.I.N.A. PC)

- [N.I.N.A. Nighttime Imaging 'N' Astronomy](https://nighttime-imaging.eu/)
- [ninaAPI Advanced plugin](https://github.com/christian-photo/ninaAPI) by Christian Palm — install it from N.I.N.A.'s plugin manager, then enable it. Required for operation.

<p align="center">
  <img src="images/NINA_Plugin.jpg" alt="NINA Plugin" width="800">
</p>

---

## Installation

Download the latest factory binary from the [Releases page](../../releases). No build environment is needed: use a Chromium-based browser and the [ESP Web Flasher](https://espressif.github.io/esptool-js/), then flash `nina-display-factory.bin` at address `0x0000`. Each release includes step-by-step flashing instructions.

> **Note:** If your board warns about outdated `esp-hosted` co-processor firmware, flash `network_adapter.bin` at address `0x0000` to the ESP32-C6 coprocessor. A pre-built binary is in the [`firmware/`](firmware/) folder.

After the first flash, further updates can be delivered over the air from GitHub releases (System tab in the web UI).

---

## First-Time Setup

On first boot (or whenever WiFi is not connected), the device broadcasts a WiFi access point named **`NINA-DISPLAY`** (password: `12345678`). Connect to it from your phone or laptop and open `http://192.168.4.1`.

Set at minimum:

1. **WiFi credentials** — your observatory/home network where NINA runs.
2. **NINA host(s)** — the IP or hostname of each NINA PC (e.g. `192.168.1.50`). The device builds the full API URL automatically. Up to three instances.

Save. The device reconnects to your network and starts polling immediately.

> Once connected to your network, the config AP is disabled to reduce radio interference. If the WiFi connection drops, the AP reappears so you can reconfigure.

---

## Security

The web interface is protected by a session-cookie login (default password: `changeme123!`; there is no username). **Change it on first boot:** web UI, System tab, Admin Password section.

- Authentication is enabled by default and covers all configuration, reboot, factory reset, OTA, and secret-returning endpoints. Sessions expire after 12 hours.
- Transport is HTTP, not HTTPS. Treat your LAN segment as the trust boundary.
- Secrets (WiFi/MQTT passwords, Spotify client ID) are redacted as `********` in API responses regardless of the auth setting.
- Authentication can be disabled (System tab). When off, every endpoint is open to anyone on the LAN, including reboot, factory reset, and OTA. Use only on trusted networks.

---

## Display Interface

The **Summary page** shows a card for each configured NINA instance: name, active filter, target, progress, RMS, HFR, and time to meridian flip. Tap a card to jump to that instance's full dashboard.

Each **instance page** is a 720x720 bento-box grid: header (target, profile, WiFi, connection dot), sequence, an animated exposure arc colored by the active filter, filter/timing, guiding RMS, image stats (HFR, star count), mount flip time, session time, and power (voltage, current, watts, dew-heater PWM). A pulsing header dot marks an active API request; grey means NINA is unreachable, and the page dims after prolonged stale data.

### Touch interactions

Every section on an instance page is tappable:

| Tap area | Opens |
|---|---|
| Header | Full-screen JPEG preview of the last captured image (tap to dismiss) |
| Sequence box | Sequence detail overlay (container, step, exposure counts) |
| Exposure arc | Camera & weather overlay (camera name, temperature, dew point, humidity, pressure) |
| Filter label (arc center) | Filter wheel overlay (current and available filters) |
| RMS box | RMS history graph (up to 500 points, RA/Dec/Total, threshold lines) |
| HFR box (short tap) | HFR history graph (up to 500 points, threshold lines) |
| HFR box (long press) | Autofocus overlay (V-curve, focus position) |
| Flip time box | Mount overlay (RA/Dec, altitude, azimuth, meridian flip state) |
| Session time box | Session statistics overlay (total exposures, imaging time, target altitude, dawn) |
| Stars box | Image statistics overlay (star count, HFR, FWHM, eccentricity, SNR) |
| Power row | Jump back to Summary page |

### On-device settings

Swipe to the **Settings** page for on-device configuration in four tabs: Display (theme, brightness), Behavior (auto-rotate, sleep, timeouts), Nodes (NINA hosts), and System. The web UI exposes the full set of options. The **System Info** page reports IP, WiFi, CPU, memory, PSRAM, uptime, and task count.

---

## Features

### Navigation

Swipe left/right to cycle pages, or press the **BOOT button** (GPIO 35) to advance. **Auto-cycle** rotates through a selectable, reorderable list of pages on a configurable interval with fade, slide, or instant transitions, and can skip disconnected instances. The **Home Page** setting picks the page the display settles on; when it is a NINA instance, the display also returns there whenever that instance is online. Page-indicator dots at the bottom mark the NINA instance pages.

### Multi-Instance Support

Add up to three NINA hosts. Only the active (visible) page gets full ~2-second polling; background instances get a slower heartbeat (default 30 s). When the Summary page is active, all instances are polled at full rate.

### Themes & Filter Colors

Nine built-in dark themes, selectable in the Display tab; changes apply instantly without a reboot. A color-brightness slider (0-100%) uniformly dims all theme colors for dark-site use. Filter colors are set per-instance as hex values, and new filters reported by NINA are added automatically with a default color. RMS and HFR threshold colors are configurable, so out-of-range values shift green to amber to red.

### Spotify

The display doubles as a full-screen Spotify Now Playing screen with album art, playback controls, progress bar, and scrolling track info. NINA and AllSky polling pause while the Spotify page is active to free bandwidth for album-art downloads.

Setup (Spotify tab in the web UI):

1. Create a free app at [developer.spotify.com](https://developer.spotify.com).
2. Set its redirect URI to `http://127.0.0.1:8000/callback`.
3. Paste the **Client ID** into the Spotify tab and **Save**.
4. Click **Login with Spotify** and approve access.
5. Spotify redirects to a "can't connect" page: copy the full URL from the address bar and paste it into the Redirect URL field.

### MQTT / Home Assistant

Point MQTT at your broker to publish the device's own state via Home Assistant auto-discovery: screen brightness and text brightness (dimmable light entities), a reboot button, and an uptime sensor. Home Assistant can then set brightness or reboot the device, for example dimming during a dark-site session. NINA session data is not published over MQTT.

### AllSky, Weather, and Imagery

- **AllSky** — a four-quadrant environmental panel (thermal, sky quality, ambient, power) fed from an AllSky API with configurable field mappings and thresholds.
- **Weather** — OpenWeatherMap, Open-Meteo, or Weather Underground data on the clock page.
- **Image Display** — full-screen GOES satellite, NASA SDO/SOHO solar imagery, or a custom image URL.

---

## Web API

The device serves an HTTP API on port 80. A full, always-current reference for every endpoint (with copy-paste `curl` examples) lives in the web UI's **API tab**.

The `/api/screenshot` endpoint captures a live JPEG of the current display using the ESP32-P4's hardware JPEG encoder:

```bash
curl -o screenshot.jpg http://<device-ip>/api/screenshot
```
