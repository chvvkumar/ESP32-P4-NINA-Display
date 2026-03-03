# AllSky Camera Metrics Page — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a four-quadrant LVGL page displaying AllSky camera metrics with configurable JSON key mappings, threshold-colored gradient bars, and a WebUI configuration tab.

**Architecture:** Fixed four-quadrant layout (THERMAL/SQM/AMBIENT/POWER) on page index 0. All existing pages shift +1. AllSky API polled by a dedicated FreeRTOS task on Core 0; LVGL updates from data_update_task on Core 1. Config stored as JSON strings in app_config_t (same pattern as filter_colors/rms_thresholds). WebUI gets a new "AllSky" tab with threshold editors and a JSON tree browser for key mapping.

**Tech Stack:** ESP-IDF, LVGL 9.2.0, cJSON, FreeRTOS, NVS

**Design doc:** `docs/plans/2026-03-02-allsky-page-design.md`

---

## Dependency Graph

```
Task 1 (Config) ──┬── Task 2 (AllSky Client) ──┐
                   ├── Task 3 (LVGL Page) ──────┤── Task 5 (Dashboard Integration)
                   ├── Task 6 (Web API) ────────┤── Task 7 (Polling Task)
                   └── Task 4 (WebUI Tab) ──────┘
```

**Parallelizable after Task 1:** Tasks 2, 3, 4, 6 can all run simultaneously.
**Sequential:** Task 5 depends on Tasks 2+3. Task 7 depends on Tasks 2+3+5+6.

---

### Task 1: App Config — Add AllSky Fields

**Files:**
- Modify: `main/app_config.h:16,20-64`
- Modify: `main/app_config.c:607-651` (set_defaults), `main/app_config.c:1262-1307` (migrate_from_v15 becomes migrate_from_v16), `main/app_config.c:1440-1598` (version detection chain)

**Step 1: Add fields to app_config_t**

In `main/app_config.h`, bump version and add fields after `hostname`:

```c
#define APP_CONFIG_VERSION 17  // was 16

typedef struct {
    // ... existing fields through line 63 ...
    char     hostname[32];

    // AllSky integration
    char     allsky_hostname[128];          // AllSky API host:port (e.g., "allskypi5.lan:8080")
    uint16_t allsky_update_interval_s;      // Poll interval 1-300s (default 5)
    float    allsky_dew_offset;             // °C above ambient for dew alert (default 5.0)
    char     allsky_field_config[1536];     // JSON key mappings per quadrant
    char     allsky_thresholds[1024];       // JSON threshold configs per field
} app_config_t;
```

**Step 2: Add default values in set_defaults()**

In `main/app_config.c` function `set_defaults()` (line ~647, before closing brace), add:

```c
    // AllSky defaults
    cfg->allsky_hostname[0] = '\0';  // empty = not configured
    cfg->allsky_update_interval_s = 5;
    cfg->allsky_dew_offset = 5.0f;
    strcpy(cfg->allsky_field_config,
        "{\"thermal\":{\"title\":\"THERMAL\",\"main\":{\"key\":\"pistatus.AS_CPUTEMP\",\"unit\":\"CPU TEMP\"},"
        "\"bar_threshold\":\"cpu_temp\",\"sub1\":{\"label\":\"SSD\",\"key\":\"allskyfans.OTH_TEMPERATURE\",\"suffix\":\"\\u00b0C\"},\"sub2\":null},"
        "\"sqm\":{\"title\":\"SQM\",\"main\":{\"key\":\"allskytsl2591SQM.AS_MPSAS\",\"unit\":\"mag/arcsec\\u00b2\"},"
        "\"bar_threshold\":\"sqm\",\"sub1\":{\"label\":\"\",\"key\":\"allskymqttsubscribe.MQTT_Cloud_status\"},\"sub2\":{\"label\":\"Stars\",\"key\":\"\"}},"
        "\"ambient\":{\"title\":\"AMBIENT\",\"main\":{\"key\":\"allskydew.AS_DEWCONTROLAMBIENT\",\"unit\":\"TEMP \\u00b0C\"},"
        "\"bar_threshold\":\"ambient\",\"sub1\":{\"label\":\"HUM\",\"key\":\"allskydew.AS_DEWCONTROLHUMIDITY\",\"suffix\":\"%\"},"
        "\"sub2\":{\"label\":\"DEW\",\"key\":\"allskydew.AS_DEWCONTROLDEW\",\"suffix\":\"\\u00b0C\"},"
        "\"dot1\":{\"key\":\"allskyfans.OTH_FANS\",\"on_value\":\"On\"},\"dot2\":{\"key\":\"allskydew.AS_DEWCONTROLHEATER\",\"on_value\":\"On\"}},"
        "\"power\":{\"title\":\"POWER\",\"main\":{\"key\":\"allskyina260.AS_INA260POWER\",\"unit\":\"WATTS\"},"
        "\"bar_threshold\":\"power\",\"sub1\":{\"label\":\"\",\"key\":\"allskyina260.AS_INA260VOLTAGE\",\"suffix\":\"V\"},"
        "\"sub2\":{\"label\":\"\",\"key\":\"allskyina260.AS_INA260CURRENT\",\"suffix\":\"A\"}}}");
    strcpy(cfg->allsky_thresholds,
        "{\"cpu_temp\":{\"min\":0,\"max\":80,\"min_color\":\"#3b82f6\",\"max_color\":\"#ef4444\"},"
        "\"ssd_temp\":{\"min\":0,\"max\":70,\"min_color\":\"#3b82f6\",\"max_color\":\"#ef4444\"},"
        "\"sqm\":{\"min\":16,\"max\":22,\"min_color\":\"#ef4444\",\"max_color\":\"#22c55e\"},"
        "\"ambient\":{\"min\":-30,\"max\":40,\"min_color\":\"#3b82f6\",\"max_color\":\"#ef4444\"},"
        "\"humidity\":{\"min\":0,\"max\":100,\"min_color\":\"#22c55e\",\"max_color\":\"#3b82f6\"},"
        "\"dew_point\":{\"min\":-30,\"max\":30,\"min_color\":\"#3b82f6\",\"max_color\":\"#ef4444\"},"
        "\"amps\":{\"min\":0,\"max\":5,\"min_color\":\"#22c55e\",\"max_color\":\"#ef4444\"}}");
```

**Step 3: Add v16→v17 migration**

1. Copy the current `app_config_t` as `app_config_v16_t` (typedef) before the struct definition changes.
2. Create `migrate_from_v16()` that calls `set_defaults()` then copies all v16 fields. AllSky fields get defaults from `set_defaults()`.
3. In the version detection chain, add a `version_check == 16` case before the existing `version_check == 15` case.
4. Rename existing `migrate_from_v15` reference to still work (the old v16 type IS the old `app_config_t` before our changes).

The migration pattern follows `migrate_from_v15` (line 1262) exactly — call `set_defaults()`, then copy all old fields. New AllSky fields keep their defaults.

**Step 4: Add AllSky fields to validate_config()**

In `validate_config()` (line 1313), add:
```c
    if (cfg->allsky_update_interval_s < 1 || cfg->allsky_update_interval_s > 300) {
        cfg->allsky_update_interval_s = 5;
        fixed = true;
    }
    if (cfg->allsky_dew_offset < -50.0f || cfg->allsky_dew_offset > 50.0f) {
        cfg->allsky_dew_offset = 5.0f;
        fixed = true;
    }
    if (cfg->allsky_field_config[0] == '\0') {
        // Re-apply default field config (call set_defaults helper or inline)
        fixed = true;
    }
    if (cfg->allsky_thresholds[0] == '\0') {
        // Re-apply default thresholds
        fixed = true;
    }
```

**Step 5: Commit**

```bash
git add main/app_config.h main/app_config.c
git commit -m "feat: add AllSky config fields to app_config (v16→v17 migration)"
```

---

### Task 2: AllSky Client Module

**Files:**
- Create: `main/allsky_client.h`
- Create: `main/allsky_client.c`

**Step 1: Create allsky_client.h**

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ALLSKY_MAX_FIELDS 16

/* Field index mapping (matches extraction order in allsky_client_poll) */
#define ALLSKY_F_THERMAL_MAIN   0
#define ALLSKY_F_THERMAL_SUB1   1
#define ALLSKY_F_THERMAL_SUB2   2
#define ALLSKY_F_SQM_MAIN       3
#define ALLSKY_F_SQM_SUB1       4
#define ALLSKY_F_SQM_SUB2       5
#define ALLSKY_F_AMBIENT_MAIN   6
#define ALLSKY_F_AMBIENT_SUB1   7
#define ALLSKY_F_AMBIENT_SUB2   8
#define ALLSKY_F_AMBIENT_DOT1   9
#define ALLSKY_F_AMBIENT_DOT2  10
#define ALLSKY_F_POWER_MAIN    11
#define ALLSKY_F_POWER_SUB1    12
#define ALLSKY_F_POWER_SUB2    13

typedef struct {
    bool connected;
    char field_values[ALLSKY_MAX_FIELDS][32];
    int64_t last_poll_ms;
    SemaphoreHandle_t mutex;
} allsky_data_t;

/** Initialize the allsky data struct (create mutex). */
void allsky_data_init(allsky_data_t *data);

/** Lock the data mutex. Returns true on success. */
bool allsky_data_lock(allsky_data_t *data, int timeout_ms);

/** Unlock the data mutex. */
void allsky_data_unlock(allsky_data_t *data);

/**
 * Poll the AllSky API and extract field values.
 * @param hostname  e.g., "allskypi5.lan:8080"
 * @param field_config_json  JSON string mapping field indices to key paths
 * @param data  Shared data struct (locked internally during write)
 */
void allsky_client_poll(const char *hostname, const char *field_config_json, allsky_data_t *data);
```

**Step 2: Create allsky_client.c**

Key implementation details:
- Uses `esp_http_client` for HTTP GET (same pattern as `nina_client_internal.h` `http_get_json()`)
- URL constructed as `http://<hostname>/all`
- Response buffer allocated from PSRAM (`heap_caps_malloc(... MALLOC_CAP_SPIRAM)`)
- Parse response with `cJSON_Parse()`
- For each quadrant in field_config_json, extract configured key paths using `resolve_json_key()` helper
- `resolve_json_key(cJSON *root, const char *dotpath)` splits on `.` and walks `cJSON_GetObjectItem()` at each level
- On success: lock mutex, copy string values, set `connected = true`, set `last_poll_ms`, unlock
- On failure: lock mutex, set `connected = false`, unlock
- Free cJSON tree and response buffer

The `resolve_json_key()` helper:
```c
static const char *resolve_json_key(cJSON *root, const char *dotpath) {
    if (!root || !dotpath || dotpath[0] == '\0') return NULL;
    char path[128];
    strncpy(path, dotpath, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    cJSON *node = root;
    char *token = strtok(path, ".");
    while (token && node) {
        node = cJSON_GetObjectItem(node, token);
        token = strtok(NULL, ".");
    }
    if (!node) return NULL;
    if (cJSON_IsString(node)) return node->valuestring;
    if (cJSON_IsNumber(node)) {
        // Return the printed number (use a static buffer per call — not thread-safe, but we only call from one task)
        static char num_buf[32];
        if (node->valuedouble == (double)(int)node->valuedouble)
            snprintf(num_buf, sizeof(num_buf), "%d", node->valueint);
        else
            snprintf(num_buf, sizeof(num_buf), "%.2f", node->valuedouble);
        return num_buf;
    }
    if (cJSON_IsBool(node)) return cJSON_IsTrue(node) ? "true" : "false";
    return NULL;
}
```

Extract fields from field_config_json:
```c
static void extract_fields(cJSON *api_data, const char *field_config_json, allsky_data_t *data) {
    cJSON *cfg = cJSON_Parse(field_config_json);
    if (!cfg) return;

    // Field extraction pairs: (quadrant_name, field_name, field_index)
    struct { const char *quad; const char *field; int idx; } map[] = {
        {"thermal", "main",  ALLSKY_F_THERMAL_MAIN},
        {"thermal", "sub1",  ALLSKY_F_THERMAL_SUB1},
        {"thermal", "sub2",  ALLSKY_F_THERMAL_SUB2},
        {"sqm",     "main",  ALLSKY_F_SQM_MAIN},
        {"sqm",     "sub1",  ALLSKY_F_SQM_SUB1},
        {"sqm",     "sub2",  ALLSKY_F_SQM_SUB2},
        {"ambient", "main",  ALLSKY_F_AMBIENT_MAIN},
        {"ambient", "sub1",  ALLSKY_F_AMBIENT_SUB1},
        {"ambient", "sub2",  ALLSKY_F_AMBIENT_SUB2},
        {"ambient", "dot1",  ALLSKY_F_AMBIENT_DOT1},
        {"ambient", "dot2",  ALLSKY_F_AMBIENT_DOT2},
        {"power",   "main",  ALLSKY_F_POWER_MAIN},
        {"power",   "sub1",  ALLSKY_F_POWER_SUB1},
        {"power",   "sub2",  ALLSKY_F_POWER_SUB2},
    };

    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        cJSON *quad = cJSON_GetObjectItem(cfg, map[i].quad);
        if (!quad) { data->field_values[map[i].idx][0] = '\0'; continue; }

        cJSON *field = cJSON_GetObjectItem(quad, map[i].field);
        if (!field || cJSON_IsNull(field)) { data->field_values[map[i].idx][0] = '\0'; continue; }

        const char *key = NULL;
        if (cJSON_IsObject(field)) {
            cJSON *k = cJSON_GetObjectItem(field, "key");
            if (cJSON_IsString(k)) key = k->valuestring;
        }
        if (!key || key[0] == '\0') { data->field_values[map[i].idx][0] = '\0'; continue; }

        const char *val = resolve_json_key(api_data, key);
        if (val)
            strncpy(data->field_values[map[i].idx], val, 31);
        else
            data->field_values[map[i].idx][0] = '\0';
        data->field_values[map[i].idx][31] = '\0';
    }

    cJSON_Delete(cfg);
}
```

**Step 3: Commit**

```bash
git add main/allsky_client.h main/allsky_client.c
git commit -m "feat: add AllSky HTTP client module with JSON key extraction"
```

---

### Task 3: LVGL AllSky Page

**Files:**
- Create: `main/ui/nina_allsky.h`
- Create: `main/ui/nina_allsky.c`

**Step 1: Create nina_allsky.h**

```c
#pragma once
#include "lvgl.h"
#include "allsky_client.h"

/** Create the AllSky four-quadrant page. Returns the page container (hidden). */
lv_obj_t *allsky_page_create(lv_obj_t *parent);

/** Update displayed values from AllSky data. Call with display lock held. */
void allsky_page_update(const allsky_data_t *data);

/** Apply current theme colors. Call with display lock held. */
void allsky_page_apply_theme(void);

/** Refresh labels/bars from config (e.g., after config change). */
void allsky_page_refresh_config(void);
```

**Step 2: Create nina_allsky.c**

Key structure:
- Page container sized to `SCREEN_SIZE - 2*OUTER_PADDING`
- 2x2 grid using flex layout: outer column flow, two row containers, each with two `create_bento_box()` cells
- Each quadrant stores widget pointers in a static struct:

```c
typedef struct {
    lv_obj_t *box;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_main_value;
    lv_obj_t *lbl_unit;
    lv_obj_t *bar;
    lv_obj_t *lbl_sub1;
    lv_obj_t *lbl_sub2;
    // AMBIENT only:
    lv_obj_t *dot1;
    lv_obj_t *dot2;
} allsky_quadrant_t;

static lv_obj_t *allsky_page = NULL;
static allsky_quadrant_t quads[4];  // 0=thermal, 1=sqm, 2=ambient, 3=power
```

Each quadrant creation function:
1. `create_bento_box(parent)` — uses existing style
2. Set box to flex column, full width of half the available area minus gap
3. Title label: `lv_font_montserrat_20`, left aligned, uppercase
4. Main value: `lv_font_montserrat_48`, center, initially "--"
5. Unit label: `lv_font_montserrat_14`, center
6. Bar: `lv_bar_create()`, range 0-100, width LV_PCT(100), height 6px
7. Bottom row: two labels in a row flex container, space-between

For AMBIENT quadrant: add two 10px circle indicators next to title in a row container.

Bar gradient coloring: Parse threshold config JSON, compute position as `(value - min) / (max - min)`, then interpolate between min_color and max_color. Apply to bar indicator style and main value text color.

Theme integration: Read colors from `current_theme` (via `nina_dashboard_internal.h`). Apply `bento_bg`, `bento_border`, `label_color`, `text_color`. Respect `color_brightness` dimming via `app_config_apply_brightness()`.

Update function pattern (similar to `update_nina_dashboard_page`):
- Read `allsky_data_t->field_values[]` for each widget
- Show "--" if field value is empty string or data not connected
- Parse threshold config to compute bar value and gradient color
- Special dew point logic: if dew < ambient + offset, use min_color

**Step 3: Commit**

```bash
git add main/ui/nina_allsky.h main/ui/nina_allsky.c
git commit -m "feat: add LVGL AllSky four-quadrant page with threshold bars"
```

---

### Task 4: WebUI — AllSky Tab

**Files:**
- Modify: `main/config_ui.html:430-435` (top tabs), `main/config_ui.html:444-461` (bottom nav), insert new tab panel

**Step 1: Add AllSky tab button**

In the top tabs div (around line 430), add:
```html
<button class="tab-btn" data-tab="allsky">AllSky</button>
```

In the bottom nav (around line 444), add:
```html
<button class="nav-btn" data-tab="allsky">
  <svg>...</svg> AllSky
</button>
```

**Step 2: Add AllSky tab panel**

Insert new `<div id="tab-allsky" class="tab-panel">` after the last tab panel (before closing `</div>` of the main container). The tab contains three sections:

**Section 1: Connection settings**
- Hostname input field (text, placeholder "allskypi5.lan:8080")
- Update interval dropdown (1s, 2s, 5s, 10s, 30s, 60s, 120s, 300s)
- Dew point offset input (number, step 0.5, default 5.0)

**Section 2: Thresholds**
For each of the 7 threshold fields (cpu_temp, ssd_temp, sqm, ambient, humidity, dew_point, amps):
- Row with: field name label, min input, max input, min color picker, max color picker
- Preview gradient bar showing the configured colors
- Use the existing glassmorphic card styling from the other tabs

**Section 3: Field Mappings with JSON Tree Browser**
- "Fetch JSON" button: when clicked, fetches `http://<hostname>/all` from the browser and renders an expandable tree
- Tree rendering: iterate cJSON modules as collapsible `<details>` elements, leaf values shown with key name and current value
- Field mapping table: 14 rows (thermal main/sub1/sub2, sqm main/sub1/sub2, ambient main/sub1/sub2/dot1/dot2, power main/sub1/sub2)
- Each row: quadrant name, field type, selected key path (text input), custom title/unit/label/suffix inputs
- Clicking a tree leaf populates the currently focused key path input

**Step 3: Add JavaScript for the AllSky tab**

- `loadAllSkyConfig()`: `fetch('/api/allsky-config')` → populate fields
- `saveAllSkyConfig()`: gather all fields → `POST /api/allsky-config` as JSON
- `fetchAllSkyJson()`: fetch from `http://<hostname>/all` directly (browser CORS — may need proxy through ESP32)
- `renderJsonTree(data, container)`: recursive function to build expandable tree HTML
- Wire up the "Fetch JSON" button and tree click handlers

**CORS note**: The AllSky API may not have CORS headers. Two options:
1. Add a proxy endpoint on the ESP32: `GET /api/allsky-proxy` that fetches from the AllSky host and returns the JSON. This is more reliable.
2. Rely on the browser being on the same network. This may fail.

Recommend option 1: add `/api/allsky-proxy` endpoint that the browser calls, which forwards to the AllSky API.

**Step 4: Commit**

```bash
git add main/config_ui.html
git commit -m "feat: add AllSky tab to WebUI with threshold editors and JSON tree browser"
```

---

### Task 5: Dashboard Integration — Page Navigation

**Files:**
- Modify: `main/ui/nina_dashboard.c:37-45,120-167,214-247,621-647,710-784`
- Modify: `main/ui/nina_dashboard_internal.h:86-94`
- Modify: `main/CMakeLists.txt:5-12`

**Step 1: Add allsky_obj to dashboard state**

In `nina_dashboard.c` (around line 37), add:
```c
static lv_obj_t *allsky_obj = NULL;
```

In `nina_dashboard_internal.h`, add extern:
```c
extern lv_obj_t *allsky_obj;
```

**Step 2: Update page index convention comment**

Replace the comment block at lines 120-127:
```c
/*
 * Page index convention:
 *   0                     = AllSky page
 *   1                     = summary page
 *   2 .. page_count + 1   = NINA instance pages  (pages[idx-2])
 *   page_count + 2        = settings page
 *   page_count + 3        = sysinfo page
 *   total_page_count      = page_count + 4
 */
```

**Step 3: Update hide_page_at()**

```c
static void hide_page_at(int idx) {
    if (idx == 0 && allsky_obj)
        lv_obj_add_flag(allsky_obj, LV_OBJ_FLAG_HIDDEN);
    else if (idx == 1 && summary_obj)
        lv_obj_add_flag(summary_obj, LV_OBJ_FLAG_HIDDEN);
    else if (idx >= 2 && idx <= page_count + 1)
        lv_obj_add_flag(pages[idx - 2].page, LV_OBJ_FLAG_HIDDEN);
    else if (idx == page_count + 2 && settings_obj) {
        settings_tabview_destroy();
        settings_obj = NULL;
    } else if (idx == page_count + 3 && sysinfo_obj)
        lv_obj_add_flag(sysinfo_obj, LV_OBJ_FLAG_HIDDEN);
}
```

**Step 4: Update show_page_at()**

```c
static void show_page_at(int idx) {
    if (idx == 0 && allsky_obj)
        lv_obj_clear_flag(allsky_obj, LV_OBJ_FLAG_HIDDEN);
    else if (idx == 1 && summary_obj)
        lv_obj_clear_flag(summary_obj, LV_OBJ_FLAG_HIDDEN);
    else if (idx >= 2 && idx <= page_count + 1)
        lv_obj_clear_flag(pages[idx - 2].page, LV_OBJ_FLAG_HIDDEN);
    else if (idx == page_count + 2) {
        if (!settings_obj)
            settings_obj = settings_tabview_create(main_cont);
        lv_obj_clear_flag(settings_obj, LV_OBJ_FLAG_HIDDEN);
        settings_tabview_refresh();
    }
    else if (idx == page_count + 3 && sysinfo_obj) {
        lv_obj_clear_flag(sysinfo_obj, LV_OBJ_FLAG_HIDDEN);
        sysinfo_page_refresh();
    }
}
```

**Step 5: Update get_page_obj()**

```c
static lv_obj_t *get_page_obj(int idx) {
    if (idx == 0 && allsky_obj) return allsky_obj;
    if (idx == 1 && summary_obj) return summary_obj;
    if (idx >= 2 && idx <= page_count + 1) return pages[idx - 2].page;
    if (idx == page_count + 2 && settings_obj) return settings_obj;
    if (idx == page_count + 3 && sysinfo_obj) return sysinfo_obj;
    return NULL;
}
```

**Step 6: Update gesture_event_cb()**

Settings page is now at `page_count + 2`:
```c
if (active_page == page_count + 2) return;  // was page_count + 1
int settings_idx = page_count + 2;           // was page_count + 1
```

**Step 7: Update create_nina_dashboard()**

Before summary creation (around line 737), add:
```c
    /* AllSky page — always first (page index 0), hidden initially unless it's the default */
    allsky_obj = allsky_page_create(main_cont);
    lv_obj_add_flag(allsky_obj, LV_OBJ_FLAG_HIDDEN);
```

Change `active_page = 0` to `active_page = 1` (summary is now page 1).

Update NINA page creation to use correct index:
```c
    // Pages are now at indices 2..page_count+1 (was 1..page_count)
```

Update total_page_count:
```c
    total_page_count = page_count + 4;  // allsky + summary + NINA pages + settings + sysinfo
```

**Step 8: Update nina_dashboard_apply_theme()**

At line ~235, add:
```c
    allsky_page_apply_theme();
```

**Step 9: Update helper functions**

Update `nina_dashboard_is_summary_page()`, `nina_dashboard_is_sysinfo_page()`, `nina_dashboard_is_settings_page()` to use the new indices. Add `nina_dashboard_is_allsky_page()`.

**Step 10: Update auto-rotate bitmask handling in tasks.c**

In `tasks.c` auto-rotate logic (line ~690), add AllSky page handling:
```c
if (candidate == 0)
    in_mask = (page_mask & 0x20) != 0;             /* AllSky (bit 5) */
else if (candidate == 1)
    in_mask = (page_mask & 0x01) != 0;             /* Summary (bit 0) */
else if (candidate >= 2 && candidate <= ena_page_count + 1) {
    int inst = nina_dashboard_page_to_instance(candidate - 2);  // was candidate - 1
    ...
}
else if (candidate == ena_page_count + 2)
    in_mask = false;                               /* Settings — never */
else if (candidate == ena_page_count + 3)
    in_mask = (page_mask & 0x10) != 0;             /* Sysinfo (bit 4) */
```

**Step 11: Update page_instance_map usage in tasks.c**

In `data_update_task()` (around line 528), update the NINA page detection:
```c
// Old: current_active >= 1 && !on_sysinfo && !on_settings && !on_summary
// New: need to also exclude allsky page
bool on_allsky = nina_dashboard_is_allsky_page();
if (!on_sysinfo && !on_settings && !on_summary && !on_allsky && current_active >= 2) {
    active_page_idx = current_active - 2;  // was current_active - 1
    active_nina_idx = nina_dashboard_page_to_instance(active_page_idx);
}
```

**Step 12: Update CMakeLists.txt**

Add new source files to SRCS:
```
ui/nina_allsky.c allsky_client.c
```

**Step 13: Commit**

```bash
git add main/ui/nina_dashboard.c main/ui/nina_dashboard_internal.h main/tasks.c main/CMakeLists.txt
git commit -m "feat: integrate AllSky page into dashboard navigation (page index 0)"
```

---

### Task 6: Web API — AllSky Config Endpoints

**Files:**
- Create: `main/web_handlers_allsky.c`
- Modify: `main/web_server.c:43,51-72`
- Modify: `main/web_server_internal.h:47-68`
- Modify: `main/CMakeLists.txt:5-12`

**Step 1: Create web_handlers_allsky.c**

```c
#include "web_server_internal.h"
#include <string.h>

esp_err_t allsky_config_get_handler(httpd_req_t *req) {
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    cJSON_AddStringToObject(root, "hostname", cfg->allsky_hostname);
    cJSON_AddNumberToObject(root, "update_interval_s", cfg->allsky_update_interval_s);
    cJSON_AddNumberToObject(root, "dew_offset", cfg->allsky_dew_offset);
    cJSON_AddStringToObject(root, "field_config", cfg->allsky_field_config);
    cJSON_AddStringToObject(root, "thresholds", cfg->allsky_thresholds);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t allsky_config_post_handler(httpd_req_t *req) {
    // Read body (same pattern as config_post_handler)
    int total_len = req->content_len;
    if (total_len > CONFIG_MAX_PAYLOAD)
        return send_400(req, "Payload too large");

    char *buf = malloc(total_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return send_400(req, "Invalid JSON");

    // Validate
    if (!validate_string_len(root, "hostname", sizeof(((app_config_t*)0)->allsky_hostname)))
        { cJSON_Delete(root); return send_400(req, "hostname too long"); }
    if (!validate_string_len(root, "field_config", sizeof(((app_config_t*)0)->allsky_field_config)))
        { cJSON_Delete(root); return send_400(req, "field_config too long"); }
    if (!validate_string_len(root, "thresholds", sizeof(((app_config_t*)0)->allsky_thresholds)))
        { cJSON_Delete(root); return send_400(req, "thresholds too long"); }

    // Apply
    app_config_t cfg = app_config_get_snapshot();
    JSON_TO_STRING(root, "hostname", cfg.allsky_hostname);
    JSON_TO_INT(root, "update_interval_s", cfg.allsky_update_interval_s);
    cJSON *dew = cJSON_GetObjectItem(root, "dew_offset");
    if (cJSON_IsNumber(dew)) cfg.allsky_dew_offset = (float)dew->valuedouble;
    JSON_TO_STRING(root, "field_config", cfg.allsky_field_config);
    JSON_TO_STRING(root, "thresholds", cfg.allsky_thresholds);

    cJSON_Delete(root);
    app_config_save(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Proxy endpoint: browser calls this to fetch AllSky JSON (avoids CORS issues)
esp_err_t allsky_proxy_get_handler(httpd_req_t *req) {
    app_config_t *cfg = app_config_get();
    if (cfg->allsky_hostname[0] == '\0')
        return send_400(req, "AllSky hostname not configured");

    char url[256];
    snprintf(url, sizeof(url), "http://%s/all", cfg->allsky_hostname);

    // Use esp_http_client to fetch from AllSky
    // Allocate response buffer in PSRAM, forward to browser
    // (Follow same HTTP GET pattern as allsky_client.c)
    // On success: httpd_resp_set_type(req, "application/json"); httpd_resp_send(req, data, len);
    // On failure: return send_400(req, "Failed to reach AllSky API");
}
```

**Step 2: Register routes in web_server.c**

Increase `max_uri_handlers` from 22 to 25 (line 43).

Add to routes array (line ~72):
```c
{ "/api/allsky-config",  HTTP_GET,  allsky_config_get_handler, NULL },
{ "/api/allsky-config",  HTTP_POST, allsky_config_post_handler, NULL },
{ "/api/allsky-proxy",   HTTP_GET,  allsky_proxy_get_handler, NULL },
```

**Step 3: Add handler declarations to web_server_internal.h**

```c
esp_err_t allsky_config_get_handler(httpd_req_t *req);
esp_err_t allsky_config_post_handler(httpd_req_t *req);
esp_err_t allsky_proxy_get_handler(httpd_req_t *req);
```

**Step 4: Add to CMakeLists.txt**

Add `web_handlers_allsky.c` to the SRCS list.

**Step 5: Commit**

```bash
git add main/web_handlers_allsky.c main/web_server.c main/web_server_internal.h main/CMakeLists.txt
git commit -m "feat: add AllSky config and proxy web API endpoints"
```

---

### Task 7: Polling Task — AllSky Integration in tasks.c

**Files:**
- Modify: `main/tasks.h:54-60`
- Modify: `main/tasks.c:333-880` (data_update_task)

**Step 1: Add allsky poll task declaration to tasks.h**

```c
/** FreeRTOS task: AllSky API poller (single task, Core 0). */
void allsky_poll_task(void *arg);
```

**Step 2: Add allsky_poll_task implementation in tasks.c**

```c
void allsky_poll_task(void *arg) {
    allsky_data_t *data = (allsky_data_t *)arg;

    // Wait for WiFi
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    while (1) {
        app_config_t *cfg = app_config_get();
        const char *hostname = cfg->allsky_hostname;

        if (hostname[0] != '\0') {
            allsky_client_poll(hostname, cfg->allsky_field_config, data);
        } else {
            // Not configured — mark disconnected
            if (allsky_data_lock(data, 100)) {
                data->connected = false;
                allsky_data_unlock(data);
            }
        }

        uint16_t interval = cfg->allsky_update_interval_s;
        if (interval < 1) interval = 5;
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
    }
}
```

**Step 3: Spawn allsky_poll_task in data_update_task**

After the per-instance poll task spawning loop (line ~492), add:

```c
    /* Spawn AllSky poll task on Core 0 */
    static allsky_data_t allsky_data;
    allsky_data_init(&allsky_data);
    {
        StackType_t *as_stack = heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *as_tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (as_stack && as_tcb) {
            xTaskCreateStaticPinnedToCore(allsky_poll_task, "allsky_poll", 8192,
                                          &allsky_data, 4, as_stack, as_tcb, 0);
        } else {
            ESP_LOGE(TAG, "Failed to allocate AllSky poll task stack");
            if (as_stack) heap_caps_free(as_stack);
            if (as_tcb) heap_caps_free(as_tcb);
        }
    }
```

**Step 4: Add AllSky UI update in main loop**

After the sysinfo update block (line ~783), add:

```c
        /* Update AllSky page when visible */
        if (on_allsky) {
            if (allsky_data_lock(&allsky_data, 100)) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    allsky_page_update(&allsky_data);
                    bsp_display_unlock();
                }
                allsky_data_unlock(&allsky_data);
            }
        }
```

**Step 5: Add `on_allsky` detection in the main loop**

Near line 514 where `on_sysinfo`, `on_settings`, `on_summary` are computed:
```c
        bool on_allsky = nina_dashboard_is_allsky_page();
```

**Step 6: Commit**

```bash
git add main/tasks.h main/tasks.c
git commit -m "feat: add AllSky poll task and UI update integration"
```

---

## Summary of All Files

**New files (4):**
- `main/allsky_client.h` — Data struct, poll function declarations
- `main/allsky_client.c` — HTTP fetch, JSON key extraction
- `main/ui/nina_allsky.h` — LVGL page declarations
- `main/ui/nina_allsky.c` — Four-quadrant LVGL page implementation
- `main/web_handlers_allsky.c` — Config and proxy API endpoints

**Modified files (8):**
- `main/app_config.h` — AllSky fields in struct, version bump
- `main/app_config.c` — Defaults, migration, validation
- `main/ui/nina_dashboard.c` — Page index shift, allsky_obj, theme hook
- `main/ui/nina_dashboard_internal.h` — Extern allsky_obj
- `main/tasks.h` — allsky_poll_task declaration
- `main/tasks.c` — Spawn task, UI update in loop, auto-rotate bitmask
- `main/web_server.c` — Register 3 new routes
- `main/web_server_internal.h` — Handler declarations
- `main/config_ui.html` — AllSky tab with tree browser
- `main/CMakeLists.txt` — Add new .c files to SRCS

## Parallel Execution Strategy (Team)

**Agent 1 (Config + Integration):** Task 1 → Task 5 → Task 7
**Agent 2 (UI):** Task 3 (LVGL page) → waits for Agent 1's Task 5
**Agent 3 (Backend + WebUI):** Task 2 (client) + Task 6 (web API) + Task 4 (WebUI tab)

All agents start after Task 1 completes.
