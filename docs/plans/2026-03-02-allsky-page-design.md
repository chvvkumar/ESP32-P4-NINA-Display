# AllSky Camera Metrics Page — Design Document

**Date:** 2026-03-02
**Status:** Approved

## Overview

Add a new LVGL page displaying AllSky camera metrics from the AllSky API (`http://<hostname>/all`). The page uses a fixed four-quadrant layout with fully configurable JSON key mappings, threshold-colored gradient bars, and a WebUI tab for configuration including a JSON tree browser for key picking.

## API Source

AllSky REST API: `GET http://<hostname>:8080/all` returns nested JSON with modules:
- `pistatus` — CPU temp, disk, clocks, voltages
- `allskyfans` — fan status, temperature threshold
- `allskyina260` — voltage, current, power (INA260 sensor)
- `allskydew` — ambient temp, dew point, humidity, heater status
- `allskytsl2591SQM` — sky quality meter (mag/arcsec²)
- `allskymqttsubscribe` — cloud status, outdoor weather, GPIO states
- `allskytemp` — additional temperature sensors

## Page Layout

### Position in Navigation

```
Page 0: AllSky (NEW)
Page 1: Summary
Pages 2..N+1: NINA instances
Page N+2: Settings
Page N+3: SysInfo
total_page_count = instance_count + 4
```

AllSky is the first page. Existing page indices shift by +1. Auto-rotate bitmask gains bit 5 for AllSky (backward-compatible). Page indicator dots remain NINA-only (indices 2..N+1).

### Four-Quadrant Grid (720x720)

```
+──────────────────+──────────────────+
|    THERMAL       |       SQM        |
|     13.9°        |      7.58        |
|    CPU TEMP      |   mag/arcsec²    |
|  ████░░░░░░░░    |  ████████░░░░    |
|   SSD: 4.8°C     | Overcast Stars:1 |
+──────────────────+──────────────────+
|  AMBIENT  ○ ○    |      POWER       |
|   -11.7°         |       5.8        |
|   TEMP °C        |      WATTS       |
|  ██░░░░░░░░░░    |  █████░░░░░░░    |
| HUM:57.7 DEW:-18 |   12.0V    0.5A  |
+──────────────────+──────────────────+
```

Each quadrant is a `create_bento_box()` container using existing theme styles. Four boxes sit in a 2-column flex layout.

### Widget Structure Per Quadrant

| Element | Font | Alignment | Notes |
|---------|------|-----------|-------|
| Title | INTER_BOLD_20, uppercase | Left | e.g., "THERMAL" |
| Main value | INTER_BOLD_48 | Center | Colored by threshold gradient |
| Unit label | INTER_REGULAR_14 | Center | Below main value |
| Bar | lv_bar, full width | — | Gradient between threshold min/max colors |
| Sub-value 1 | INTER_REGULAR_14 | Left | Label + value + suffix |
| Sub-value 2 | INTER_REGULAR_14 | Right | Label + value + suffix |

AMBIENT quadrant has two 10px indicator dots next to the title (filled = on, outline = off) for fan and heater status.

## Configuration

### New Fields in `app_config_t` (version → 17)

```c
char allsky_hostname[128];          // "allskypi5.lan:8080"
uint16_t allsky_update_interval_s;  // 1-300, default 5
float allsky_dew_offset;            // °C above ambient for dew alert, default 5.0
char allsky_field_config[1536];     // JSON: key mappings per quadrant
char allsky_thresholds[1024];       // JSON: threshold configs per field
```

### Default Field Config JSON

All text fields (title, unit, label, suffix) and key paths are user-editable:

```json
{
  "thermal": {
    "title": "THERMAL",
    "main": {"key": "pistatus.AS_CPUTEMP", "unit": "CPU TEMP"},
    "bar_threshold": "cpu_temp",
    "sub1": {"label": "SSD", "key": "allskyfans.OTH_TEMPERATURE", "suffix": "°C"},
    "sub2": null
  },
  "sqm": {
    "title": "SQM",
    "main": {"key": "allskytsl2591SQM.AS_MPSAS", "unit": "mag/arcsec²"},
    "bar_threshold": "sqm",
    "sub1": {"label": "", "key": "allskymqttsubscribe.MQTT_Cloud_status"},
    "sub2": {"label": "Stars", "key": ""}
  },
  "ambient": {
    "title": "AMBIENT",
    "main": {"key": "allskydew.AS_DEWCONTROLAMBIENT", "unit": "TEMP °C"},
    "bar_threshold": "ambient",
    "sub1": {"label": "HUM", "key": "allskydew.AS_DEWCONTROLHUMIDITY", "suffix": "%"},
    "sub2": {"label": "DEW", "key": "allskydew.AS_DEWCONTROLDEW", "suffix": "°C"},
    "dot1": {"key": "allskyfans.OTH_FANS", "on_value": "On"},
    "dot2": {"key": "allskydew.AS_DEWCONTROLHEATER", "on_value": "On"}
  },
  "power": {
    "title": "POWER",
    "main": {"key": "allskyina260.AS_INA260POWER", "unit": "WATTS"},
    "bar_threshold": "power",
    "sub1": {"label": "", "key": "allskyina260.AS_INA260VOLTAGE", "suffix": "V"},
    "sub2": {"label": "", "key": "allskyina260.AS_INA260CURRENT", "suffix": "A"}
  }
}
```

### Default Threshold Config JSON

Each threshold has min/max values and min/max gradient colors:

```json
{
  "cpu_temp":  {"min": 0,   "max": 80,  "min_color": "#3b82f6", "max_color": "#ef4444"},
  "ssd_temp":  {"min": 0,   "max": 70,  "min_color": "#3b82f6", "max_color": "#ef4444"},
  "sqm":       {"min": 16,  "max": 22,  "min_color": "#ef4444", "max_color": "#22c55e"},
  "ambient":   {"min": -30, "max": 40,  "min_color": "#3b82f6", "max_color": "#ef4444"},
  "humidity":  {"min": 0,   "max": 100, "min_color": "#22c55e", "max_color": "#3b82f6"},
  "dew_point": {"min": -30, "max": 30,  "min_color": "#3b82f6", "max_color": "#ef4444"},
  "amps":      {"min": 0,   "max": 5,   "min_color": "#22c55e", "max_color": "#ef4444"}
}
```

### Dew Point Auto-Coloring

If `dew_point_value < ambient_value + allsky_dew_offset`, the dew point text is colored with `dew_point.min_color` (safe = cold enough). Otherwise, normal gradient coloring applies (approaching condensation = warmer color).

## Data Model

### Runtime Data (PSRAM, not persisted)

```c
#define ALLSKY_MAX_FIELDS 16

typedef struct {
    bool connected;                            // API reachable
    char field_values[ALLSKY_MAX_FIELDS][32];  // Extracted string values
    int64_t last_poll_ms;
    SemaphoreHandle_t mutex;
} allsky_client_t;
```

Field index mapping (order matches extraction during poll):
- 0-2: thermal main, sub1, sub2
- 3-5: sqm main, sub1, sub2
- 6-10: ambient main, sub1, sub2, dot1, dot2
- 11-13: power main, sub1, sub2

### Polling Flow

1. HTTP GET `http://<hostname>/all`
2. Parse with cJSON
3. For each configured key path, walk JSON tree (split on `.`, `cJSON_GetObjectItem` at each level)
4. Store extracted string values in `field_values[]`
5. Set `connected` flag
6. Free cJSON tree

## HTTP Polling Architecture

### New Task: `allsky_poll_task`

- Pinned to Core 0 (networking core), 8KB stack
- Polls at `allsky_update_interval_s` rate
- Shares `allsky_client_t` with data_update_task via mutex
- When API unreachable: `connected = false`, all fields show "--"

### Integration with data_update_task

data_update_task (Core 1, LVGL) reads `allsky_client_t` each cycle and calls `allsky_page_update()` when the AllSky page is visible.

## WebUI — AllSky Tab

### New Tab in config_ui.html

Added alongside existing Config/Display/System tabs.

**Sections:**
1. **Connection** — hostname input, update interval dropdown (1s-5min), dew offset input
2. **Thresholds** — per field: min/max inputs, two color pickers (min_color, max_color), preview gradient bar
3. **Field Mappings** — "Fetch JSON" button loads live API data into expandable tree browser. User selects a display field slot, clicks a JSON tree leaf to assign it. All titles, units, labels, and suffixes are editable text inputs.

### JSON Tree Browser

- "Fetch JSON" button: browser directly fetches `http://<hostname>/all`
- Renders expandable tree with module names as collapsible sections
- Each leaf shows: key name + current value (for easy identification)
- Click a leaf to assign it to the currently selected field slot
- Key path stored as dot-notation (e.g., `"pistatus.AS_CPUTEMP"`)

### New API Endpoints

- `GET /api/allsky-config` — returns hostname, interval, dew_offset, field_config, thresholds
- `POST /api/allsky-config` — validates and saves all AllSky config to NVS

## New Files

| File | Purpose |
|------|---------|
| `main/ui/nina_allsky.h` | AllSky page header (create, update, theme) |
| `main/ui/nina_allsky.c` | LVGL four-quadrant page implementation |
| `main/allsky_client.h` | AllSky data struct and poll function declarations |
| `main/allsky_client.c` | HTTP fetcher, JSON key extraction, poll logic |

## Modified Files

| File | Changes |
|------|---------|
| `main/app_config.h/.c` | Add allsky fields, bump version to 17, migration defaults |
| `main/ui/nina_dashboard.c` | Add allsky_obj, shift page indices, theme hook |
| `main/ui/nina_dashboard_internal.h` | Expose allsky_obj pointer |
| `main/tasks.c/.h` | Spawn allsky_poll_task, add allsky UI update |
| `main/web_server.c` | Register `/api/allsky-config` endpoints |
| `main/config_ui.html` | Add AllSky tab with tree browser and threshold editors |
| `main/CMakeLists.txt` | Add new .c files to SRCS |

## Theme & Style Compliance

- All colors from `current_theme` (bg_main, bento_bg, bento_border, label_color, text_color)
- Widget borders follow existing `widget_style` setting
- Color brightness dimming applied to threshold gradient colors
- Shows "--" for unavailable fields (same pattern as NINA pages)
- Respects screen rotation, sleep, and all existing display settings
