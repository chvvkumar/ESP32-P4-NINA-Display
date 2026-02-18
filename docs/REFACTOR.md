# Codebase Refactor Plan

This document proposes a reorganization of the ESP32-P4-NINA-Display codebase to improve maintainability, reduce file sizes, eliminate duplication, and establish clearer module boundaries.

---

## Table of Contents

1. [Current State](#1-current-state)
2. [Proposed File Structure](#2-proposed-file-structure)
3. [Detailed Split Plans](#3-detailed-split-plans)
4. [Code Quality Improvements](#4-code-quality-improvements)
5. [Dead Code Removal](#5-dead-code-removal)
6. [Dependency Diagram](#6-dependency-diagram)
7. [Migration Strategy](#7-migration-strategy)

---

## 1. Current State

### File Sizes

| File | Lines | Status |
|------|------:|--------|
| `main/nina_client.c` | 1,341 | **Too large** - 5 distinct logical groups |
| `main/ui/nina_dashboard.c` | 1,085 | **Too large** - layout + updates + thumbnail + gestures |
| `main/web_server.c` | 829 | **Too large** - 200-line embedded HTML + handlers + screenshot |
| `main/main.c` | 467 | Acceptable, but contains JPEG decode logic |
| `main/app_config.c` | 454 | Acceptable, some duplication |
| `main/mqtt_ha.c` | 391 | Good size, well-scoped |
| `main/ui/themes.c` | 263 | Good size |
| `main/nina_client_old.c` | 709 | **Dead code** - not compiled |

**Total active source:** ~5,437 lines across 8 files (excluding headers).

### Key Problems

1. **Three files over 800 lines** with mixed responsibilities
2. **200-line HTML string embedded** in C source (unmaintainable)
3. **Duplicated logic** in multiple places (defaults init, exposure calc, color thresholds)
4. **709 lines of dead code** (`nina_client_old.c`) sitting in the repo
5. **No thread safety** on shared config struct
6. **Boilerplate-heavy** JSON config parsing (~140 lines of repetitive code)

---

## 2. Proposed File Structure

```
main/
├── main.c                          # Entry point, task spawning, WiFi init (~200 lines)
├── tasks.c / tasks.h               # data_update_task + input_task (~220 lines)
├── jpeg_utils.c / jpeg_utils.h     # JPEG fetch, decode, grayscale conversion (~100 lines)
│
├── nina_client.h                   # Data structures (unchanged)
├── nina_client.c                   # Public polling API + HTTP utilities (~350 lines)
├── nina_api_fetchers.c             # Individual REST endpoint fetchers (~450 lines)
├── nina_sequence.c / .h            # Sequence JSON tree walkers (~200 lines)
├── nina_websocket.c / .h           # WebSocket client lifecycle + event handling (~250 lines)
│
├── app_config.h                    # Config struct (unchanged)
├── app_config.c                    # NVS persistence + helpers (~350 lines, deduplicated)
│
├── web_server.h                    # (unchanged)
├── web_server.c                    # HTTP handlers + server setup (~550 lines)
├── config_ui.html                  # Extracted HTML/CSS/JS (embedded via EMBED_TXTFILES)
│
├── mqtt_ha.h / .c                  # (unchanged, already well-scoped)
│
└── ui/
    ├── nina_dashboard.h            # Public API (unchanged)
    ├── nina_dashboard.c            # Dashboard init, page creation, theme apply (~450 lines)
    ├── nina_dashboard_update.c     # Data update functions, arc animation (~350 lines)
    ├── nina_thumbnail.c / .h       # Thumbnail overlay create/show/hide (~120 lines)
    ├── themes.h / .c               # (unchanged, already well-scoped)
    └── ui_helpers.h                # Shared style factories, widget helpers (~50 lines)
```

### Expected Outcome

| Metric | Before | After |
|--------|--------|-------|
| Largest file | 1,341 lines | ~550 lines |
| Files > 500 lines | 4 | 1-2 |
| Dead code | 709 lines | 0 |
| Duplicated blocks | ~8 | 0 |

---

## 3. Detailed Split Plans

### 3.1 Split `nina_client.c` (1,341 lines) into 4 files

The current file contains five distinct logical groups that have minimal coupling between them.

#### `nina_client.c` (keep) -> ~350 lines
**Retains:** Public polling API and HTTP utilities.

```
Stays here:
  - my_timegm()                     (lines 27-54)    Time conversion utility
  - parse_iso8601()                  (lines 56-94)    ISO 8601 parser
  - http_get_json()                  (lines 96-135)   Generic HTTP GET + JSON parse
  - nina_poll_state_init()           (lines 1132-1134) Init poll state
  - nina_client_poll()               (lines 1136-1250) Tiered polling orchestrator
  - nina_client_poll_heartbeat()     (lines 1252-1257) Background heartbeat
  - nina_client_get_data()           (lines 1070-1126) Legacy full-fetch (keep for now)
  - nina_client_fetch_prepared_image() (lines 1259-1340) JPEG fetch
  - fixup_exposure_timing()          NEW - extracted helper (see Section 4.2)
```

**Rationale:** This file becomes the public interface. It includes the HTTP helpers it uses directly, and calls into the fetchers and WebSocket modules.

#### `nina_api_fetchers.c` (new) -> ~450 lines
**Extracts:** All individual REST API endpoint fetch functions.

```
Moves here:
  - fetch_camera_info_robust()       (lines 145-192)
  - fetch_filter_robust_ex()         (lines 198-242)
  - fetch_image_history_robust()     (lines 248-311)
  - fetch_profile_robust()           (lines 316-339)
  - fetch_guider_robust()            (lines 344-388)
  - fetch_mount_robust()             (lines 393-409)
  - fetch_focuser_robust()           (lines 708-723)
  - fetch_switch_info()              (lines 730-845)
```

**Header:** `nina_api_fetchers.h` - declares all fetch functions. Only included by `nina_client.c`.

**Dependencies:** Uses `http_get_json()` and `parse_iso8601()` from `nina_client.c` (expose via internal header or keep in shared `nina_client_internal.h`).

#### `nina_sequence.c` (new) -> ~200 lines
**Extracts:** The recursive sequence JSON tree walkers.

```
Moves here:
  - find_time_condition()            (lines 412-443)
  - find_active_target_container()   (lines 446-465)
  - find_active_container_name()     (lines 469-520)
  - find_running_step_name()         (lines 524-554)
  - find_running_smart_exposure()    (lines 557-583)
  - fetch_sequence_counts_optional() (lines 590-703)
```

**Header:** `nina_sequence.h` - declares `fetch_sequence_counts_optional()` (the only function called externally). Tree walkers remain `static` within this file.

**Rationale:** Sequence parsing is the most complex logic in the client. Isolating it makes the recursive algorithms easier to understand and modify independently.

#### `nina_websocket.c` (new) -> ~250 lines
**Extracts:** WebSocket client lifecycle and message handling.

```
Moves here:
  - handle_websocket_message()       (lines 859-953)
  - websocket_event_handler()        (lines 958-991)
  - build_ws_url()                   (lines 997-1019)
  - nina_websocket_start()           (lines 1021-1052)
  - nina_websocket_stop()            (lines 1054-1064)
```

**Header:** `nina_websocket.h` - declares `nina_websocket_start()`, `nina_websocket_stop()`, and the state variables needed by the polling logic.

**Rationale:** WebSocket code has a distinct lifecycle (connect/disconnect/reconnect) and event model that's separate from HTTP polling.

---

### 3.2 Split `nina_dashboard.c` (1,085 lines) into 4 files

#### `nina_dashboard.c` (keep) -> ~450 lines
**Retains:** Initialization, page creation, theme application, gestures.

```
Stays here:
  - extract_host_from_url()          (lines 99-122)
  - update_styles()                  (lines 125-174)
  - create_bento_box()               (lines 176-181)
  - create_small_label()             (lines 183-188)
  - create_value_label()             (lines 190-195)
  - apply_theme_to_page()            (lines 198-226)
  - nina_dashboard_apply_theme()     (lines 228-264)
  - create_dashboard_page()          (lines 267-541)
  - create_page_indicator()          (lines 544-569)
  - gesture_event_cb()               (lines 576-597)
  - target_name_click_cb()           (lines 606-619)
  - create_nina_dashboard()          (lines 650-700)
  - nina_dashboard_show_page()       (lines 702-725)
  - nina_dashboard_get_active_page() (lines 727-729)
  - nina_dashboard_set_page_change_cb() (lines 571-573)
```

#### `nina_dashboard_update.c` (new) -> ~350 lines
**Extracts:** All data-push and animation functions.

```
Moves here:
  - arc_reset_complete_cb()          (lines 732-735)
  - arc_fill_complete_cb()           (lines 737-751)
  - nina_dashboard_update_status()   (lines 761-777)
  - update_nina_dashboard_page()     (lines 780-1012)
```

**Rationale:** The 233-line `update_nina_dashboard_page()` function is the hot path that changes most frequently as new data fields are added. Isolating updates from layout makes both easier to work on.

**Further improvement:** Split `update_nina_dashboard_page()` into sub-functions:

```c
// In nina_dashboard_update.c
static void update_header_widgets(page_widgets_t *w, nina_client_t *data, int idx);
static void update_sequence_widgets(page_widgets_t *w, nina_client_t *data);
static void update_exposure_arc(page_widgets_t *w, nina_client_t *data, int idx);
static void update_guider_widgets(page_widgets_t *w, nina_client_t *data, int idx);
static void update_power_widgets(page_widgets_t *w, nina_client_t *data);

void update_nina_dashboard_page(int page_index, nina_client_t *data) {
    page_widgets_t *w = &pages[page_index];
    if (!data->connected) {
        clear_all_widgets(w);
        return;
    }
    update_header_widgets(w, data, page_index);
    update_sequence_widgets(w, data);
    update_exposure_arc(w, data, page_index);
    update_guider_widgets(w, data, page_index);
    update_power_widgets(w, data);
}
```

#### `nina_thumbnail.c` (new) -> ~120 lines
**Extracts:** Thumbnail overlay creation and management.

```
Moves here:
  - thumbnail_overlay_click_cb()     (lines 600-603)
  - create_thumbnail_overlay()       (lines 622-647)
  - nina_dashboard_thumbnail_requested()      (lines 1015-1017)
  - nina_dashboard_clear_thumbnail_request()  (lines 1019-1021)
  - nina_dashboard_set_thumbnail()   (lines 1023-1064)
  - nina_dashboard_hide_thumbnail()  (lines 1066-1080)
  - nina_dashboard_thumbnail_visible() (lines 1082-1084)
```

**Header:** `nina_thumbnail.h` - declares the public thumbnail API.

**Rationale:** Thumbnail is a self-contained feature (overlay creation, image display, request/response lifecycle) with no coupling to the main dashboard grid.

#### `ui_helpers.h` (new) -> ~50 lines
**Extracts:** Shared widget factory functions and style references.

```
Moves here:
  - create_bento_box()
  - create_small_label()
  - create_value_label()
  - Extern declarations for shared styles (style_bento_box, style_label_small, etc.)
```

**Rationale:** These tiny factories are used by both `nina_dashboard.c` and potentially `nina_thumbnail.c`. A shared header avoids circular dependencies.

---

### 3.3 Refactor `web_server.c` (829 lines)

#### Extract HTML to separate file

**Current problem:** Lines 20-220 contain a ~6,000-character minified HTML string that is impossible to edit, debug, or format.

**Solution:** Use ESP-IDF's `EMBED_TXTFILES` feature:

1. Create `main/config_ui.html` with properly formatted HTML/CSS/JS
2. Update `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "nina_client.c" "nina_api_fetchers.c" "nina_sequence.c"
         "nina_websocket.c" "app_config.c" "web_server.c" "mqtt_ha.c"
         "tasks.c" "jpeg_utils.c"
         "ui/nina_dashboard.c" "ui/nina_dashboard_update.c"
         "ui/nina_thumbnail.c" "ui/themes.c"
    INCLUDE_DIRS "." "ui"
    EMBED_TXTFILES "config_ui.html"
)
```

3. In `web_server.c`, replace the static string with:

```c
extern const uint8_t config_html_start[] asm("_binary_config_ui_html_start");
extern const uint8_t config_html_end[]   asm("_binary_config_ui_html_end");

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)config_html_start,
                    config_html_end - config_html_start);
    return ESP_OK;
}
```

**Impact:** web_server.c drops from 829 to ~630 lines. The HTML becomes editable with syntax highlighting, formatting, and proper version control diffs.

#### Reduce config_post_handler() boilerplate

Replace ~140 lines of repetitive JSON field extraction with a helper macro (see Section 4.3).

**Result:** `web_server.c` drops to ~550 lines total.

---

### 3.4 Extract tasks from `main.c` (467 lines)

#### `tasks.c` (new) -> ~220 lines
**Extracts:** The two FreeRTOS task functions.

```
Moves here:
  - data_update_task()               (lines 269-415)
  - input_task()                     (lines 136-173)
  - on_page_changed()                (lines 128-134)
```

#### `jpeg_utils.c` (new) -> ~100 lines
**Extracts:** JPEG decode and grayscale conversion.

```
Moves here:
  - fetch_and_show_thumbnail()       (lines 179-267)
```

**Header:** `jpeg_utils.h` - declares single function:
```c
void fetch_and_show_thumbnail(const char *base_url, int page_index, bool manual_request);
```

#### `main.c` (keep) -> ~200 lines
**Retains:** `app_main()`, `wifi_init()`, `event_handler()`, shared globals.

**Rationale:** `main.c` becomes a clean entry point that initializes subsystems and spawns tasks. No business logic.

---

## 4. Code Quality Improvements

### 4.1 Deduplicate config defaults initialization

**Problem:** `app_config_init()` has two identical 30-line blocks setting defaults (lines 30-55 and 63-90).

**Fix:**
```c
static void set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(app_config_t));
    strcpy(cfg->api_url_1, "http://astromele2.lan:1888/v2/api/");
    strcpy(cfg->api_url_2, "http://astromele3.lan:1888/v2/api/");
    strcpy(cfg->ntp_server, "pool.ntp.org");
    strcpy(cfg->rms_thresholds_1, DEFAULT_RMS_THRESHOLDS);
    strcpy(cfg->rms_thresholds_2, DEFAULT_RMS_THRESHOLDS);
    strcpy(cfg->rms_thresholds_3, DEFAULT_RMS_THRESHOLDS);
    strcpy(cfg->hfr_thresholds_1, DEFAULT_HFR_THRESHOLDS);
    strcpy(cfg->hfr_thresholds_2, DEFAULT_HFR_THRESHOLDS);
    strcpy(cfg->hfr_thresholds_3, DEFAULT_HFR_THRESHOLDS);
    cfg->brightness = 50;
    cfg->color_brightness = 100;
    strcpy(cfg->mqtt_broker_url, "mqtt://192.168.1.250");
    strcpy(cfg->mqtt_topic_prefix, "ninadisplay");
    cfg->mqtt_port = 1883;
}
```

**Saves:** ~30 lines, eliminates sync risk.

### 4.2 Extract exposure timing fixup

**Problem:** Identical exposure calculation logic appears in `nina_client_get_data()` (lines 1100-1117) and `nina_client_poll()` (lines 1226-1243).

**Fix:**
```c
static void fixup_exposure_timing(nina_client_t *data) {
    if (data->exposure_current < 0) {
        float remaining = -data->exposure_current;
        if (data->exposure_total > 0) {
            data->exposure_current = data->exposure_total - remaining;
        } else {
            data->exposure_current = 0;
        }
        int rem_int = (int)remaining;
        int mins = rem_int / 60;
        int secs = rem_int % 60;
        snprintf(data->time_remaining, sizeof(data->time_remaining),
                 "%d:%02d", mins, secs);
    }
    // Clamp values
    if (data->exposure_current < 0) data->exposure_current = 0;
    if (data->exposure_total < 0) data->exposure_total = 0;
    if (data->exposure_current > data->exposure_total && data->exposure_total > 0) {
        data->exposure_current = data->exposure_total;
    }
}
```

**Saves:** ~25 lines of duplication.

### 4.3 Macro for JSON config field extraction

**Problem:** `config_post_handler()` has ~20 identical blocks for parsing JSON string fields.

**Fix:**
```c
#define JSON_TO_STRING(root, key, dest) do { \
    cJSON *_item = cJSON_GetObjectItem(root, key); \
    if (cJSON_IsString(_item)) { \
        strncpy(dest, _item->valuestring, sizeof(dest) - 1); \
        dest[sizeof(dest) - 1] = '\0'; \
    } \
} while (0)

#define JSON_TO_INT(root, key, dest) do { \
    cJSON *_item = cJSON_GetObjectItem(root, key); \
    if (cJSON_IsNumber(_item)) { \
        dest = _item->valueint; \
    } \
} while (0)

#define JSON_TO_BOOL(root, key, dest) do { \
    cJSON *_item = cJSON_GetObjectItem(root, key); \
    if (cJSON_IsBool(_item)) { \
        dest = cJSON_IsTrue(_item); \
    } \
} while (0)
```

**Usage:**
```c
// Before: 7 lines per field
cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
if (cJSON_IsString(ssid)) {
    strncpy(cfg->wifi_ssid, ssid->valuestring, sizeof(cfg->wifi_ssid) - 1);
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
}

// After: 1 line per field
JSON_TO_STRING(root, "wifi_ssid", cfg->wifi_ssid);
```

**Saves:** ~100 lines in `config_post_handler()`.

### 4.4 Unify threshold color evaluation

**Problem:** `app_config_get_rms_color()` and `app_config_get_hfr_color()` are nearly identical (37 lines each), differing only in which JSON field they read and the default threshold values.

**Fix:**
```c
static uint32_t get_threshold_color(float value, const char *json_str,
                                     float default_good_max, float default_ok_max,
                                     int instance_index) {
    uint32_t good_color = 0x15803d, ok_color = 0xca8a04, bad_color = 0xb91c1c;
    float good_max = default_good_max, ok_max = default_ok_max;

    cJSON *root = cJSON_Parse(json_str);
    if (root) {
        cJSON *gm = cJSON_GetObjectItem(root, "good_max");
        if (cJSON_IsNumber(gm)) good_max = gm->valuedouble;
        cJSON *om = cJSON_GetObjectItem(root, "ok_max");
        if (cJSON_IsNumber(om)) ok_max = om->valuedouble;
        good_color = parse_color_field(root, "good_color", good_color);
        ok_color   = parse_color_field(root, "ok_color", ok_color);
        bad_color  = parse_color_field(root, "bad_color", bad_color);
        cJSON_Delete(root);
    }

    uint32_t color = (value <= good_max) ? good_color
                   : (value <= ok_max)   ? ok_color
                   :                       bad_color;

    return app_config_apply_brightness(color,
               app_config_get()->color_brightness);
}

uint32_t app_config_get_rms_color(float rms, int instance_index) {
    const char *json = get_thresholds_field(instance_index, THRESHOLD_RMS);
    return get_threshold_color(rms, json, 0.5f, 1.0f, instance_index);
}

uint32_t app_config_get_hfr_color(float hfr, int instance_index) {
    const char *json = get_thresholds_field(instance_index, THRESHOLD_HFR);
    return get_threshold_color(hfr, json, 2.0f, 3.5f, instance_index);
}
```

**Saves:** ~25 lines.

### 4.5 Break up `update_nina_dashboard_page()` (233 lines)

**Problem:** Single function handles header, sequence info, exposure arc, RMS, HFR, flip time, stars, target time, and power widgets.

**Fix:** Extract into focused sub-functions:

```c
static void update_disconnected_state(page_widgets_t *w, int page_index);
static void update_header(page_widgets_t *w, nina_client_t *d, int idx);
static void update_sequence_info(page_widgets_t *w, nina_client_t *d);
static void update_exposure_arc(page_widgets_t *w, nina_client_t *d, int idx);
static void update_guider_stats(page_widgets_t *w, nina_client_t *d, int idx);
static void update_mount_info(page_widgets_t *w, nina_client_t *d);
static void update_image_stats(page_widgets_t *w, nina_client_t *d);
static void update_power_widgets(page_widgets_t *w, nina_client_t *d);

void update_nina_dashboard_page(int page_index, nina_client_t *data) {
    page_widgets_t *w = &pages[page_index];
    if (!data->connected) {
        update_disconnected_state(w, page_index);
        return;
    }
    update_header(w, data, page_index);
    update_sequence_info(w, data);
    update_exposure_arc(w, data, page_index);
    update_guider_stats(w, data, page_index);
    update_mount_info(w, data);
    update_image_stats(w, data);
    update_power_widgets(w, data);
}
```

**Impact:** Each sub-function is 20-40 lines and focused on one UI section. Much easier to understand and modify.

### 4.6 Add mutex protection to `app_config_t`

**Problem:** `app_config_get()` returns a raw pointer that `data_update_task`, web server handlers, and MQTT handlers all read/write without synchronization. While rare, a config save during a read could cause torn reads.

**Fix:**
```c
// In app_config.c
static SemaphoreHandle_t s_config_mutex;

void app_config_init(void) {
    s_config_mutex = xSemaphoreCreateMutex();
    // ... existing init code
}

app_config_t app_config_get_snapshot(void) {
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    app_config_t copy = s_config;
    xSemaphoreGive(s_config_mutex);
    return copy;
}

void app_config_save(const app_config_t *cfg) {
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    memcpy(&s_config, cfg, sizeof(app_config_t));
    // ... NVS write
    xSemaphoreGive(s_config_mutex);
}
```

**Trade-off:** `app_config_t` is ~2KB, so stack copies are feasible on ESP32-P4 (has PSRAM). For hot-path reads (like filter colors during UI update), the snapshot approach avoids holding a lock across LVGL calls.

### 4.7 Per-instance config field accessors

**Problem:** `api_url_1/2/3`, `filter_colors_1/2/3`, `rms_thresholds_1/2/3`, `hfr_thresholds_1/2/3` each need switch statements to access by index.

**Fix:** Create a generic accessor:

```c
// In app_config.c
typedef enum {
    FIELD_API_URL,
    FIELD_FILTER_COLORS,
    FIELD_RMS_THRESHOLDS,
    FIELD_HFR_THRESHOLDS,
} config_field_t;

const char *app_config_get_instance_field(int index, config_field_t field) {
    app_config_t *cfg = app_config_get();
    // Use offsetof() and field size to compute pointer
    static const struct {
        size_t offsets[3];
        size_t size;
    } field_map[] = {
        [FIELD_API_URL]         = { {offsetof(app_config_t, api_url_1),
                                     offsetof(app_config_t, api_url_2),
                                     offsetof(app_config_t, api_url_3)}, 128 },
        [FIELD_FILTER_COLORS]   = { {offsetof(app_config_t, filter_colors_1),
                                     offsetof(app_config_t, filter_colors_2),
                                     offsetof(app_config_t, filter_colors_3)}, 512 },
        // ... etc
    };
    if (index < 0 || index > 2) index = 0;
    return (const char *)((uint8_t *)cfg + field_map[field].offsets[index]);
}
```

**Saves:** Eliminates 4 separate switch-statement accessors. All per-instance lookups go through one function.

**Note:** This is a longer-term improvement. The ideal fix would be to convert the config struct to use arrays (`api_url[3]` instead of `api_url_1/2/3`), but that requires an NVS migration strategy since the struct is stored as a raw blob.

---

## 5. Dead Code Removal

### 5.1 ~~Delete `nina_client_old.c` (709 lines)~~ ✅ DONE

~~**Status:** Not included in `CMakeLists.txt`, not compiled, not referenced.~~

~~**Contents:** Older version of the NINA client that uses `sequence/state` endpoint. All functionality has been superseded by the current `nina_client.c`.~~

~~**Action:** Delete the file. If historical reference is needed, it remains in git history.~~

### 5.2 Audit `bsp_extra` component

**Location:** `components/bsp_extra/`

**Status:** Audio codec and player functionality that is present but never called from any source file.

**Action:** Remove the component unless audio features are planned. Document in this file if kept intentionally.

### 5.3 Remove unused theme fields

**Fields `stars_color` and `saturated_color`** in `theme_t` are defined and populated for all 14 themes but never referenced in the dashboard code.

**Action:** Remove from struct and all theme definitions, or implement their intended usage.

---

## 6. Dependency Diagram

### Current Dependencies

```
                    +----------+
                    | main.c   |
                    +----+-----+
                         |
          +--------------+--------------+------------------+
          |              |              |                   |
    +-----v-----+  +----v----+  +------v------+   +-------v-------+
    | nina_      |  | web_    |  | nina_       |   | mqtt_ha.c     |
    | client.c   |  | server.c|  | dashboard.c |   |               |
    +-----+------+  +----+----+  +------+------+   +-------+-------+
          |              |              |                   |
          |         +----v----+   +----v-----+             |
          |         | app_    |   | themes.c |             |
          +-------->| config.c|   +----------+             |
                    +---------+        ^                   |
                         ^             |                   |
                         +-------------+-------------------+
```

### Proposed Dependencies (after split)

```
                        +----------+
                        | main.c   |
                        +----+-----+
                             |
              +--------------+-----+-----------+
              |              |     |            |
        +-----v-----+  +----v--+  |    +-------v-------+
        | tasks.c    |  | wifi  |  |    | web_server.c  |
        +-----+------+  +-------+  |    +-------+-------+
              |                     |            |
     +--------+--------+     +-----v-----+ +----v----+
     |        |         |     | mqtt_ha.c | | app_    |
+----v---+ +--v----+ +--v-+  +-----------+ | config.c|
| nina_  | | nina_ | |jpeg|                +---------+
|client.c| | dash  | |util|
+---+----+ | board | +----+
    |      +---+---+
+---v--------+ |
| nina_api_  | +---+------+------+
| fetchers.c | |   |      |      |
+------------+ | +--v--+ +-v---+ +-v--------+
| nina_      | | |dash_| |thumb| |ui_helpers|
| sequence.c | | |upd. | |nail | |          |
+------------+ | +-----+ +-----+ +----------+
| nina_      | |
| websocket.c | +----v-----+
+--------------+ | themes.c |
                 +----------+
```

**Key improvement:** No file exceeds ~550 lines. Each module has a single, clear responsibility.

---

## 7. Migration Strategy

### Phase 1: Quick Wins (no risk, immediate value) ✅ COMPLETE

1. ~~**Delete `nina_client_old.c`**~~ ✅ - 709 lines removed
2. ~~**Extract HTML to `config_ui.html`**~~ ✅ - via `EMBED_TXTFILES`; `web_server.c` uses `_binary_config_ui_html_start/end`; `CMakeLists.txt` updated
3. ~~**Add `set_defaults()` helper**~~ ✅ - in `app_config.c`; duplicate 30-line init blocks replaced
4. ~~**Add JSON parsing macros**~~ ✅ - `JSON_TO_STRING` / `JSON_TO_INT` / `JSON_TO_BOOL` in `web_server.c`; `config_post_handler()` reduced from ~140 to ~40 lines
5. ~~**Extract `fixup_exposure_timing()`**~~ ✅ - `static` helper in `nina_client.c`; called from both `nina_client_get_data()` and `nina_client_poll()`

**Estimated effort:** 2-3 hours. No functional changes. All changes are internal refactors.

### Phase 2: Module Extraction (medium risk, structural improvement) ✅ COMPLETE

- [x] 6. **Extract `nina_websocket.c`** from `nina_client.c` ✅ — WS lifecycle + event handling in dedicated module with `nina_websocket.h`
- [x] 7. **Extract `nina_api_fetchers.c`** from `nina_client.c` ✅ — All 8 REST endpoint fetchers with `nina_api_fetchers.h`
- [x] 8. **Extract `nina_sequence.c`** from `nina_client.c` ✅ — Recursive JSON tree walkers with `nina_sequence.h`
- [x] 9. **Extract `nina_thumbnail.c`** from `nina_dashboard.c` ✅ — Thumbnail overlay create/show/hide with `nina_thumbnail.h`
- [x] 10. **Extract `nina_dashboard_update.c`** from `nina_dashboard.c` ✅ — Data push + arc animation functions

Additional files created:
- `nina_client_internal.h` — shared `http_get_json()` and `parse_iso8601()` for fetcher/sequence modules
- `ui/nina_dashboard_internal.h` — shared `dashboard_page_t`, styles, and state for dashboard sub-modules
- `nina_websocket.h` declarations removed from `nina_client.h`; `esp_websocket_client.h` dependency moved to `nina_websocket.c`
- `CMakeLists.txt` updated with all new source files

### Phase 3: Code Quality (low risk, cleaner codebase)

- [ ] 11. **Unify threshold color functions** in `app_config.c`
- [ ] 12. **Break up `update_nina_dashboard_page()`** into sub-functions
- [ ] 13. **Extract `tasks.c` and `jpeg_utils.c`** from `main.c`
- [ ] 14. **Add config mutex** for thread safety
- [ ] 15. **Create `ui_helpers.h`** for shared widget factories

**Estimated effort:** 3-4 hours.

### Phase 4: Stretch Goals (higher risk, bigger payoff)

- [ ] 16. **Convert per-instance config fields to arrays** (requires NVS migration)
- [ ] 17. **Support concurrent WebSocket connections** (per-instance WebSocket)
- [ ] 18. **Remove unused `bsp_extra` component**
- [ ] 19. **Remove unused theme fields** (`stars_color`, `saturated_color`)

**Estimated effort:** 6-8 hours. Requires careful NVS migration testing.

---

### Testing Strategy

Since this project has no unit tests, validation after each phase should include:

1. **Build succeeds:** `idf.py build` completes without errors or warnings
2. **Flash and boot:** Device boots, WiFi connects, AP broadcasts
3. **UI renders:** Dashboard displays correctly for all configured instances
4. **Config UI works:** Web server serves config page, save/load functions correctly
5. **Live features:** Thumbnail fetch, page swiping, theme switching, brightness adjustment
6. **MQTT:** Home Assistant discovery and control still function (if enabled)

Consider adding a basic smoke test script that verifies the web API endpoints respond correctly after each refactor phase.
