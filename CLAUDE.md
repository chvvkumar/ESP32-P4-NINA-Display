# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-P4 LVGL dashboard for [N.I.N.A. astrophotography software](https://nighttime-imaging.eu/) running on a Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (720×720 display). It connects to the [ninaAPI Advanced API plugin](https://github.com/christian-photo/ninaAPI) via HTTP polling and WebSocket to display real-time session data.

## Build Commands

This is a standard **ESP-IDF** project. `IDF_PATH` must be set and the IDF environment activated before running any of these:

```bash
# Activate IDF environment first (once per terminal session)
. $IDF_PATH/export.sh   # Linux/macOS
# or on Windows: %IDF_PATH%\export.bat

idf.py build            # Compile
idf.py flash            # Flash to device (auto-detects port)
idf.py monitor          # Serial monitor (115200 baud)
idf.py flash monitor    # Flash + monitor in one command
idf.py menuconfig       # Configure Kconfig options (sdkconfig)
idf.py fullclean        # Remove build/ directory entirely
```

There are no unit tests in this project.

## Architecture

### Task Structure (`main/main.c`)

Two FreeRTOS tasks run after initialization:

- **`data_update_task`** — 2-second polling loop. Manages the tiered NINA API HTTP requests, WebSocket lifecycle, WiFi RSSI, JPEG thumbnail fetching, and calls into `nina_dashboard` to push updates. Only the *active* (visible) page gets full tiered polling; background instances get a 10-second heartbeat.
- **`input_task`** — Polls GPIO 35 (BOOT button) with 200 ms debounce to switch pages manually.

`app_main()` initializes: NVS → config → WiFi (STA+AP dual-mode) → web server → display (LVGL via BSP) → dashboard UI → spawns tasks.

### Module Responsibilities

| File | Purpose |
|------|---------|
| `main/main.c` | Entry point, task orchestration, JPEG decode + thumbnail display |
| `main/nina_client.h/.c` | NINA REST API polling (tiered) + WebSocket event handling |
| `main/app_config.h/.c` | NVS-backed persistent config (WiFi, API URLs, theme, filter colors, thresholds) |
| `main/web_server.h/.c` | On-device HTTP server — single-page config UI served from device AP |
| `main/ui/nina_dashboard.h/.c` | LVGL widget tree, arc animation, swipe gestures, thumbnail overlay |
| `main/ui/themes.h/.c` | 14 built-in dark color themes (`theme_t` struct) |
| `components/bsp_extra/` | Audio codec/player BSP extension (present but not called from main.c) |

### Key Data Structures

**`nina_client_t`** (`nina_client.h`) — the live session data model. Holds camera state, guider RMS, focuser, sequence progress, filter wheel, power monitoring, and WebSocket state for one NINA instance.

**`app_config_t`** (`app_config.h`) — persistent config. Holds WiFi credentials, up to 3 NINA instance URLs (`api_url_1/2/3`), theme index, brightness, per-filter JSON colors and brightness, and RMS/HFR threshold JSON.

**`nina_poll_state_t`** (`nina_client.h`) — per-instance polling timers. Controls tiered request scheduling (fast: every cycle; slow: every 30 s; sequence: every 10 s).

### NINA API Polling Strategy

Polling is tiered to avoid hammering the API:
- **Every 2 s (active page):** Camera info (determines connectivity)
- **Once on connect:** Profile, available filters, image history, power switch info (cached in `poll_state`)
- **Every cycle:** Guider RMS
- **Every 10 s:** Sequence JSON (container name, step, exposure counts)
- **Every 30 s:** Focuser position, mount/meridian flip
- **Every 10 s (background instances):** Camera heartbeat only

### WiFi Topology

Dual-mode (STA + AP simultaneously):
- **STA** — connects to the observatory/home network to reach NINA computers
- **AP** — always broadcasts `"AllSky-Config"` (no password) for headless web configuration

### UI Layout

Each NINA instance has its own LVGL page (720×720, bento-box grid). Pages are navigated by horizontal swipe gestures or the BOOT button. Key UI elements:
- 300×300 arc widget (animated, filter-color-matched) for exposure progress
- Guider RMS values with threshold-based color coding
- Thumbnail overlay (tap header → fetches JPEG from `/prepared-image`, hardware JPEG decoded on ESP32-P4, displayed fullscreen)
- Page indicator dots at the bottom (when > 1 instance configured)
- Connection status pulsing dot during active API calls

### LVGL Integration

LVGL 9.2.0 via `managed_components/lvgl__lvgl` and `espressif__esp_lvgl_port`. All LVGL calls must be wrapped with the display mutex:
```c
lvgl_port_lock(0);
// ... LVGL API calls ...
lvgl_port_unlock();
```
The `esp_lvgl_port` component handles the display flush and touch input tasks internally.

### Component Dependencies

Managed automatically by **IDF Component Manager** (`idf_component.yml`). Do not manually edit `managed_components/` — run `idf.py update-dependencies` to refresh. Key managed components:
- `lvgl/lvgl 9.2.0` — graphics
- `waveshare/esp32_p4_wifi6_touch_lcd_4b` — BSP (LCD + touch init)
- `espressif/esp_websocket_client` — WebSocket
- `espressif/esp_hosted` + `esp_wifi_remote` — WiFi via ESP32-C6 coprocessor over SPI

### Configuration Persistence

Config is stored in NVS under namespace `"app_conf"`, key `"config"`, as a raw binary blob of `app_config_t`. Filter colors, RMS thresholds, and HFR thresholds are stored as JSON strings within the struct fields. Use `app_config_save()` after any mutation; `app_config_factory_reset()` erases the NVS partition.
