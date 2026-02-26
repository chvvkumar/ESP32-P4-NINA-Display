# ESP32-P4 NINA Display

[![Build (main)](https://img.shields.io/github/actions/workflow/status/chvvkumar/ESP32-P4-NINA-Display/build.yml?branch=main&label=build%20(main)&logo=github)](https://github.com/chvvkumar/ESP32-P4-NINA-Display/actions/workflows/build.yml?query=branch%3Amain)
[![Build (snd)](https://img.shields.io/github/actions/workflow/status/chvvkumar/ESP32-P4-NINA-Display/build.yml?branch=snd&label=build%20(snd)&logo=github)](https://github.com/chvvkumar/ESP32-P4-NINA-Display/actions/workflows/build.yml?query=branch%3Asnd)
[![Version](https://img.shields.io/github/v/release/chvvkumar/ESP32-P4-NINA-Display?label=version&logo=semver&logoColor=white)](https://github.com/chvvkumar/ESP32-P4-NINA-Display/releases/latest)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.2-blue?logo=espressif)](https://github.com/espressif/esp-idf/tree/v5.5.2)
[![LVGL](https://img.shields.io/badge/LVGL-v9.2.0-red?logo=data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCI+PHBhdGggZmlsbD0iI2ZmZiIgZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==)](https://lvgl.io/)

![Flash Size](https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.flash_size&label=Flash&logo=memory&color=orange)
![PSRAM](https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.psram&label=PSRAM&logo=memory&color=purple)
![Factory Size](https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.factory_size&label=Factory%20Binary&logo=chip&color=green)
![OTA Size](https://img.shields.io/badge/dynamic/json?url=https://raw.githubusercontent.com/chvvkumar/ESP32-P4-NINA-Display/badges/firmware-metrics.json&query=$.ota_size&label=OTA%20Binary&logo=chip&color=green)

A touchscreen dashboard for [N.I.N.A. astrophotography software](https://nighttime-imaging.eu/), built for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** (720x720). It polls the [ninaAPI Advanced plugin](https://github.com/christian-photo/ninaAPI) over HTTP and WebSocket to show real-time session data — exposure arcs, guiding RMS, filter status, sequence progress, power draw, and more. Monitor up to three separate N.I.N.A. instances from a single device.

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
- [Display Interface](#display-interface)
- [Navigation](#navigation)
- [Multi-Instance Support](#multi-instance-support)
- [Themes & Filter Colors](#themes--filter-colors)
- [MQTT / Home Assistant Integration](#mqtt--home-assistant-integration)
- [Web API](#web-api)
- [Building from Source](#building-from-source)

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
    <td align="center"><img src="images/display_settings.jpg" alt="Display Settings" width="480"></td>
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

### Web Configuration UI

<table align="center">
  <tr>
    <td align="center"><img src="images/main.jpg" alt="Main Settings" width="480"></td>
    <td align="center"><img src="images/nina.jpg" alt="NINA Settings" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/network.jpg" alt="Network Settings" width="480"></td>
    <td align="center"><img src="images/display.jpg" alt="Display Settings" width="480"></td>
  </tr>
  <tr>
    <td align="center"><img src="images/system.jpg" alt="System Settings" width="480"></td>
  </tr>
</table>

---

## Prerequisites

### Hardware

- **[Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416)** — currently the only tested board. The layout is tuned for its 720x720 display.

### Software (on the N.I.N.A. PC)

- [N.I.N.A. Nighttime Imaging 'N' Astronomy](https://nighttime-imaging.eu/)
- [ninaAPI Advanced plugin](https://github.com/christian-photo/ninaAPI) by Christian Palm — install it from N.I.N.A.'s plugin manager, then enable it. Required for operation.

<p align="center">
  <img src="images/NINA_Plugin.jpg" alt="NINA Plugin" width="1200">
</p>

---

## Installation

### Flashing a pre-built release

Download the latest factory binary from the [Releases page](../../releases). Each release includes step-by-step flashing instructions.

No build environment needed — just a Chromium-based browser and the [ESP Web Flasher](https://espressif.github.io/esptool-js/). Flash `nina-display-factory.bin` at address `0x0000`.

> **Note:** If your board shows warnings about outdated `esp-hosted` co-processor firmware, flash `network_adapter.bin` at address `0x0000` to the ESP32-C6 coprocessor. A pre-built binary is included in the [`firmware/`](firmware/) folder of this repository.

---

## First-Time Setup

On first boot (or whenever WiFi is not connected), the device broadcasts a WiFi access point named **`NINA-DISPLAY`** (password: `12345678`). Connect to it from your phone or laptop, then open `http://192.168.4.1` in a browser.

Set at minimum:

1. **WiFi credentials** — your observatory/home network where NINA runs.
2. **NINA host(s)** — the IP or hostname of each NINA PC (e.g. `192.168.1.50`). The device builds the full API URL automatically. You can configure up to three instances.

Save settings. The device reconnects to your network and starts polling immediately.

> Once the device connects to your network, the config AP is automatically disabled to reduce radio interference. If the WiFi connection drops, the AP reappears so you can reconfigure.

---

## Display Interface

### Summary Page

The first page shows glassmorphic cards for all configured NINA instances at a glance. Each card displays the instance name, active filter, target name, progress bar, RMS, HFR, and time to meridian flip. Tap a card to jump to that instance's full dashboard.

### Instance Pages

Each NINA instance gets a dedicated 720x720 bento-box grid:

| Section | Data shown |
|---|---|
| Header | Target name, profile name, WiFi signal bars, connection status dot |
| Sequence | Container name, current step |
| Exposure arc | Animated arc showing exposure progress; color follows the active filter |
| Filter & timing | Active filter name, elapsed / remaining time, loop count |
| Guiding | Total RMS, RA RMS, Dec RMS (arcseconds); color shifts at configurable thresholds |
| Image stats | HFR, star count |
| Mount | Time to meridian flip |
| Session | Target time, elapsed imaging time |
| Power | Input voltage (V), current (A), power (W), PWM percentages for dew heaters |

A pulsing dot in the header indicates an active API request. When solid and colored, you're connected. Grey means NINA isn't reachable. After 30 seconds without data, an amber "Last update" warning appears. After 2 minutes, the page dims to indicate stale data.

#### Touch interactions

Every section on the instance page is tappable:

| Tap area | Opens |
|---|---|
| Header | Full-screen JPEG preview of the last captured image (tap anywhere to dismiss) |
| Sequence box | Sequence detail overlay (container, step, exposure counts) |
| Exposure arc | Camera & weather overlay (camera name, temperature, dew point, humidity, pressure) |
| Filter label (arc center) | Filter wheel overlay (current filter, available filters) |
| RMS box | RMS history graph (up to 500 points, RA/Dec/Total series, threshold lines) |
| HFR box (short tap) | HFR history graph (up to 500 points, threshold lines) |
| HFR box (long press) | Autofocus overlay (V-curve, focus position) |
| Flip time box | Mount overlay (RA/Dec, altitude, azimuth, meridian flip state) |
| Session time box | Session statistics overlay (total exposures, imaging time, target altitude, dawn) |
| Stars box | Image statistics overlay (star count, HFR, FWHM, eccentricity, SNR) |
| Power row | Jump back to Summary page |

### Settings Page

On-device display settings accessible by swiping past the last instance page:

- **Theme** — cycle through 14 built-in dark themes
- **Backlight brightness** — slider (0–100%)
- **Text/color brightness** — slider (0–100%) for dark-site dimming
- **Update rate** — polling interval (1–10 s)
- **Graph update interval** — how often graph data is sampled (2–30 s)
- **Auto-rotate** — toggle, interval, transition effect (instant/fade/slide), skip disconnected

### System Info Page

The last page shows: device IP, hostname, WiFi SSID/RSSI, heap and PSRAM usage, chip info, IDF version, uptime, and task count.

---

## Navigation

- **Swipe** left/right to cycle through pages (wraps around)
- **BOOT button** (GPIO 35) advances to the next page
- **Auto-rotate** cycles pages on a configurable interval with fade, slide, or instant transitions
- **Page indicator dots** at the bottom show NINA instance pages
- **Active page override** in config to boot directly to a specific page

Page order: Summary → NINA instances (1..N) → Settings → System Info.

---

## Multi-Instance Support

Add a second or third API URL in the config page. Only the **active (visible) page** gets full 2-second polling. Background instances get a 10-second heartbeat. When the summary page is active, all instances receive full polling.

Auto-rotate can be configured to skip disconnected instances and to include or exclude specific pages via a bitmask.

---

## Themes & Filter Colors

Six built-in dark themes: Bento Default, OLED Black, Red Night, Monochrome, All Black, and Midnight Industrial. Select one from the web config. Theme changes apply instantly without a reboot.

A color brightness slider (0–100%) uniformly adjusts all theme colors for dark-site use.

Filter colors are configured per-instance as hex values. When NINA reports a new filter name, it's automatically added to the config with a default color you can customize. RMS and HFR threshold colors are also configurable — set your acceptable limits, and values outside range shift from green to amber to red.

---

## MQTT / Home Assistant Integration

Enable MQTT in the web config and point it at your broker. The device publishes Home Assistant auto-discovery configs for:

- **Screen Brightness** — a dimmable light entity (0–100%) to control the display backlight
- **Text Brightness** — a dimmable light entity (0–100%) to control theme color intensity
- **Reboot** — a button entity to restart the device remotely

Enables screen dimming via HA automation during a dark-site session or display integration into observatory control workflows.

---

## Web API

The on-device HTTP server (port 80) exposes endpoints beyond the config UI:

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/config` | GET/POST | Read or write the full device configuration |
| `/api/brightness` | POST | Set display brightness (0–100%) live |
| `/api/color-brightness` | POST | Set color brightness (0–100%) live |
| `/api/theme` | POST | Switch theme live |
| `/api/screenshot` | GET | Capture a JPEG screenshot of the current display |
| `/api/reboot` | POST | Reboot the device |
| `/api/factory-reset` | POST | Erase config and restore defaults |

### Screenshots

The `/api/screenshot` endpoint captures a live JPEG of the current display using the ESP32-P4's hardware JPEG encoder. Open it in a browser or fetch it with `curl`:

```bash
# Save a screenshot
curl -o screenshot.jpg http://<device-ip>/api/screenshot
```

---

## Building from Source

Standard **ESP-IDF** project targeting IDF 5.5.x. Requires the IDF toolchain installed and `IDF_PATH` set.

```bash
# Activate IDF environment (once per terminal session)
. $IDF_PATH/export.sh

# Build
idf.py build

# Flash and open serial monitor
idf.py flash monitor

# Wipe build directory if you hit stale artifact issues
idf.py fullclean
```

Dependencies are managed via **IDF Component Manager** (`idf_component.yml`). Don't edit files under `managed_components/` directly — run `idf.py update-dependencies` to refresh them.

### source files

| File | Purpose |
|---|---|
| `main/main.c` | Boot sequence, WiFi init, task spawning |
| `main/tasks.c` | Polling loop, WebSocket lifecycle, auto-rotate, thumbnail fetch |
| `main/nina_client.h/.c` | Poll orchestration and tiered request scheduling |
| `main/nina_api_fetchers.h/.c` | Individual REST endpoint fetchers (camera, guider, mount, etc.) |
| `main/nina_sequence.h/.c` | Sequence JSON parsing (container, step, exposure counts) |
| `main/nina_websocket.h/.c` | WebSocket client with concurrent multi-instance connections |
| `main/app_config.h/.c` | NVS-backed config (WiFi, URLs, themes, thresholds, MQTT) |
| `main/web_server.h/.c` | On-device HTTP config UI + JSON API |
| `main/mqtt_ha.h/.c` | MQTT + Home Assistant auto-discovery |
| `main/jpeg_utils.h/.c` | Hardware JPEG decoding via `esp_driver_jpeg` |
| `main/ui/nina_dashboard.h/.c` | LVGL page management, swipe gestures, page indicator dots |
| `main/ui/nina_dashboard_update.c` | Live data widget update logic |
| `main/ui/nina_summary.h/.c` | Summary page with glassmorphic instance cards |
| `main/ui/nina_sysinfo.h/.c` | System info page |
| `main/ui/nina_thumbnail.h/.c` | Full-screen thumbnail overlay |
| `main/ui/themes.h/.c` | 6 built-in dark themes |
