# Config Backup & Restore Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add config export/import functionality via the web UI with version-aware restore, sensitive field opt-in, and a preview-then-confirm flow.

**Architecture:** Two new HTTP endpoints (`GET /api/config/backup`, `POST /api/config/restore`) handle the server-side logic. The restore endpoint serves double duty — preview mode returns a categorized diff, confirm mode applies and saves. A new "Backup" tab in `config_ui.html` provides the UI. A static field registry array drives diff computation, missing/unknown field detection, and category grouping.

**Tech Stack:** ESP-IDF HTTP server, cJSON (PSRAM-hooked), NVS, vanilla HTML/CSS/JS in embedded config page.

**Spec:** `docs/superpowers/specs/2026-03-14-config-backup-restore-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `main/web_server_internal.h` | Modify | Add `CONFIG_MAX_RESTORE_PAYLOAD` define, declare `backup_get_handler` and `restore_post_handler` |
| `main/web_server.c` | Modify | Register 2 new routes in `routes[]` array |
| `main/web_handlers_config.c` | Modify | Add `max_size` param to `receive_json_body()`, add `backup_get_handler()`, `restore_post_handler()`, field registry, diff logic |
| `main/config_ui.html` | Modify | Add "Backup" tab button + nav button + tab panel with export/import UI |

No new files needed. All changes follow existing patterns in these files.

---

## Task 1: Add `receive_json_body()` max_size parameter

**Files:**
- Modify: `main/web_server_internal.h` (line 17, add new define)
- Modify: `main/web_handlers_config.c` (lines 394-432, update function signature and all callers)

- [ ] **Step 1: Add `CONFIG_MAX_RESTORE_PAYLOAD` define**

In `main/web_server_internal.h`, after line 17 (`#define CONFIG_MAX_PAYLOAD 8192`), add:

```c
#define CONFIG_MAX_RESTORE_PAYLOAD 16384
```

- [ ] **Step 2: Update `receive_json_body()` signature to accept `max_size`**

In `main/web_handlers_config.c`, change the function at line 394 from:

```c
static cJSON *receive_json_body(httpd_req_t *req)
```

to:

```c
static cJSON *receive_json_body(httpd_req_t *req, int max_size)
```

Update the body to use `max_size` instead of `CONFIG_MAX_PAYLOAD`:
- Line 397: `if (remaining >= max_size)` (was `CONFIG_MAX_PAYLOAD`)
- Line 402: `heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM)` (was `CONFIG_MAX_PAYLOAD`)

- [ ] **Step 3: Update all existing callers to pass `CONFIG_MAX_PAYLOAD`**

Search for all calls to `receive_json_body(req)` in `web_handlers_config.c` and change them to `receive_json_body(req, CONFIG_MAX_PAYLOAD)`. There are 3 callers:
- `config_post_handler()` (line 438)
- `config_apply_handler()` (line 501)
- `config_revert_handler()` — if it calls it (check; the revert handler at line 533 does NOT call `receive_json_body`, it just reloads from NVS)

Also check `web_handlers_allsky.c` and other handler files for any callers — `receive_json_body` is `static` to `web_handlers_config.c` so only callers in that file need updating.

- [ ] **Step 4: Commit**

```bash
git add main/web_server_internal.h main/web_handlers_config.c
git commit -m "refactor: add max_size param to receive_json_body for larger payloads"
```

---

## Task 2: Add field registry and diff computation

**Files:**
- Modify: `main/web_handlers_config.c` (add field registry array and diff helper functions)

The field registry is the core data structure that drives diff computation, category grouping, and missing/unknown field detection. It must be placed before the backup/restore handlers.

- [ ] **Step 1: Add the field registry struct and static array**

Add after the existing `#include` block and before the handler functions in `web_handlers_config.c`:

```c
/* ---- Backup/Restore Field Registry ---- */

typedef struct {
    const char *json_key;      /* JSON key as used in GET /api/config */
    const char *label;         /* Human-readable label for diff display */
    const char *category;      /* Tab/group name for diff grouping */
    bool        is_sensitive;  /* true = only exported in "sensitive" section */
    bool        is_large;      /* true = truncate value in diff display */
} backup_field_t;

static const backup_field_t s_backup_fields[] = {
    /* Display */
    {"theme_index",          "Theme",              "Display",      false, false},
    {"brightness",           "Brightness",         "Display",      false, false},
    {"color_brightness",     "Color Brightness",   "Display",      false, false},
    {"widget_style",         "Widget Style",       "Display",      false, false},
    {"screen_rotation",      "Screen Rotation",    "Display",      false, false},

    /* Behavior */
    {"auto_rotate_enabled",         "Auto-rotate",              "Behavior", false, false},
    {"auto_rotate_interval_s",      "Rotate Interval",          "Behavior", false, false},
    {"auto_rotate_effect",          "Rotate Effect",            "Behavior", false, false},
    {"auto_rotate_skip_disconnected","Skip Disconnected",       "Behavior", false, false},
    {"auto_rotate_pages",           "Rotate Pages Mask",        "Behavior", false, false},
    {"update_rate_s",               "Update Rate",              "Behavior", false, false},
    {"idle_poll_interval_s",        "Idle Poll Interval",       "Behavior", false, false},
    {"connection_timeout_s",        "Connection Timeout",       "Behavior", false, false},
    {"toast_duration_s",            "Toast Duration",           "Behavior", false, false},
    {"graph_update_interval_s",     "Graph Update Interval",    "Behavior", false, false},
    {"active_page_override",        "Active Page Override",     "Behavior", false, false},
    {"alert_flash_enabled",         "Alert Flash",              "Behavior", false, false},
    {"screen_sleep_enabled",        "Screen Sleep",             "Behavior", false, false},
    {"screen_sleep_timeout_s",      "Screen Sleep Timeout",     "Behavior", false, false},

    /* Nodes & Data */
    {"url1",               "NINA URL 1",         "Nodes & Data", false, false},
    {"url2",               "NINA URL 2",         "Nodes & Data", false, false},
    {"url3",               "NINA URL 3",         "Nodes & Data", false, false},
    {"instance_enabled_1", "Instance 1 Enabled",  "Nodes & Data", false, false},
    {"instance_enabled_2", "Instance 2 Enabled",  "Nodes & Data", false, false},
    {"instance_enabled_3", "Instance 3 Enabled",  "Nodes & Data", false, false},
    {"filter_colors_1",    "Filter Colors 1",     "Nodes & Data", false, true},
    {"filter_colors_2",    "Filter Colors 2",     "Nodes & Data", false, true},
    {"filter_colors_3",    "Filter Colors 3",     "Nodes & Data", false, true},
    {"rms_thresholds_1",   "RMS Thresholds 1",    "Nodes & Data", false, true},
    {"rms_thresholds_2",   "RMS Thresholds 2",    "Nodes & Data", false, true},
    {"rms_thresholds_3",   "RMS Thresholds 3",    "Nodes & Data", false, true},
    {"hfr_thresholds_1",   "HFR Thresholds 1",    "Nodes & Data", false, true},
    {"hfr_thresholds_2",   "HFR Thresholds 2",    "Nodes & Data", false, true},
    {"hfr_thresholds_3",   "HFR Thresholds 3",    "Nodes & Data", false, true},

    /* System */
    {"ntp",                  "NTP Server",          "System", false, false},
    {"timezone",             "Timezone",             "System", false, false},
    {"debug_mode",           "Debug Mode",           "System", false, false},
    {"demo_mode",            "Demo Mode",            "System", false, false},
    {"auto_update_check",    "Auto Update Check",    "System", false, false},
    {"update_channel",       "Update Channel",       "System", false, false},
    {"deep_sleep_enabled",   "Deep Sleep",           "System", false, false},
    {"deep_sleep_wake_timer_s","Deep Sleep Timer",   "System", false, false},
    {"deep_sleep_on_idle",   "Sleep on Idle",        "System", false, false},
    {"wifi_power_save",      "WiFi Power Save",      "System", false, false},

    /* AllSky */
    {"allsky_enabled",            "AllSky Enabled",       "AllSky", false, false},
    {"allsky_hostname",           "AllSky Hostname",      "AllSky", false, false},
    {"allsky_update_interval_s",  "AllSky Update Interval","AllSky", false, false},
    {"allsky_dew_offset",         "AllSky Dew Offset",    "AllSky", false, false},
    {"allsky_field_config",       "AllSky Field Config",  "AllSky", false, true},
    {"allsky_thresholds",         "AllSky Thresholds",    "AllSky", false, true},

    /* Spotify */
    {"spotify_enabled",           "Spotify Enabled",      "Spotify", false, false},
    {"spotify_poll_interval_ms",  "Spotify Poll Interval","Spotify", false, false},
    {"spotify_show_progress_bar", "Progress Bar",         "Spotify", false, false},
    {"spotify_overlay_timeout_s", "Overlay Timeout",      "Spotify", false, false},
    {"spotify_minimal_mode",      "Minimal Mode",         "Spotify", false, false},

    /* MQTT (non-sensitive) */
    {"mqtt_enabled",       "MQTT Enabled",       "MQTT", false, false},
    {"mqtt_port",          "MQTT Port",          "MQTT", false, false},
    {"mqtt_topic_prefix",  "MQTT Topic Prefix",  "MQTT", false, false},

    /* Sensitive */
    {"mqtt_username",      "MQTT Username",      "MQTT",    true, false},
    {"mqtt_password",      "MQTT Password",      "MQTT",    true, false},
    {"mqtt_broker_url",    "MQTT Broker URL",    "MQTT",    true, false},
    {"spotify_client_id",  "Spotify Client ID",  "Spotify", true, false},
    {"hostname",           "Hostname",           "System",  true, false},

    {NULL, NULL, NULL, false, false}  /* sentinel */
};
```

- [ ] **Step 2: Add helper to compare two cJSON values**

```c
/* Returns true if two cJSON values are equal */
static bool cjson_values_equal(const cJSON *a, const cJSON *b)
{
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;

    if (cJSON_IsString(a)) return strcmp(a->valuestring, b->valuestring) == 0;
    if (cJSON_IsNumber(a)) return a->valuedouble == b->valuedouble;
    if (cJSON_IsBool(a))   return cJSON_IsTrue(a) == cJSON_IsTrue(b);
    return false;  /* objects/arrays not compared field-by-field */
}
```

- [ ] **Step 3: Add helper to build the diff/preview response JSON**

This function takes the backup JSON and current config JSON, walks the field registry, and builds the preview response:

```c
/*
 * Build restore preview response.
 * backup_root: the parsed backup JSON (contains "meta", "config", optionally "sensitive")
 * current_json: the current config serialized as JSON (same format as GET /api/config)
 * Returns a cJSON object with the full preview response, or NULL on error.
 * Caller must cJSON_Delete() the result.
 */
static cJSON *build_restore_preview(const cJSON *backup_root, const cJSON *current_json)
{
    const cJSON *meta = cJSON_GetObjectItem(backup_root, "meta");
    const cJSON *backup_config = cJSON_GetObjectItem(backup_root, "config");
    const cJSON *backup_sensitive = cJSON_GetObjectItem(backup_root, "sensitive");

    int backup_ver = 0;
    if (meta) {
        cJSON *ver = cJSON_GetObjectItem(meta, "config_version");
        if (cJSON_IsNumber(ver)) backup_ver = ver->valueint;
    }
    int current_ver = APP_CONFIG_VERSION;

    /* Determine version match type */
    const char *version_match;
    if (backup_ver == current_ver)               version_match = "exact";
    else if (backup_ver > current_ver)           version_match = "newer";
    else if (current_ver - backup_ver > 10)      version_match = "much_older";
    else                                         version_match = "older";

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "preview");
    cJSON_AddStringToObject(resp, "version_match", version_match);
    cJSON_AddNumberToObject(resp, "backup_version", backup_ver);
    cJSON_AddNumberToObject(resp, "current_version", current_ver);

    /* Copy metadata fields */
    if (meta) {
        cJSON *fw = cJSON_GetObjectItem(meta, "firmware_version");
        if (cJSON_IsString(fw)) cJSON_AddStringToObject(resp, "backup_firmware", fw->valuestring);
        cJSON *hn = cJSON_GetObjectItem(meta, "hostname");
        if (cJSON_IsString(hn)) cJSON_AddStringToObject(resp, "backup_hostname", hn->valuestring);
        cJSON *mac = cJSON_GetObjectItem(meta, "mac_address");
        if (cJSON_IsString(mac)) cJSON_AddStringToObject(resp, "backup_mac", mac->valuestring);
        cJSON *dt = cJSON_GetObjectItem(meta, "export_date");
        if (cJSON_IsString(dt)) cJSON_AddStringToObject(resp, "export_date", dt->valuestring);
    }

    /* Build warnings */
    cJSON *warnings = cJSON_CreateArray();
    if (strcmp(version_match, "exact") == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Config version matches (v%d). All settings will be restored.", current_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    } else if (strcmp(version_match, "older") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "Backup is from an older config version (v%d -> v%d). "
            "Settings added since v%d will keep their current values.",
            backup_ver, current_ver, backup_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    } else if (strcmp(version_match, "much_older") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "Backup is from a much older version (v%d -> v%d). "
            "Many settings may have been added since your backup version and will keep current values.",
            backup_ver, current_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    } else if (strcmp(version_match, "newer") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "Backup is from a newer firmware (v%d -> v%d). "
            "Some settings may not be recognized by this firmware and will be skipped.",
            backup_ver, current_ver);
        cJSON_AddItemToArray(warnings, cJSON_CreateString(buf));
    }

    /* Check dirty state */
    if (app_config_is_dirty()) {
        cJSON_AddItemToArray(warnings, cJSON_CreateString(
            "You have unsaved changes that will be overwritten by this restore."));
    }
    cJSON_AddItemToObject(resp, "warnings", warnings);

    /* Walk field registry to compute diff */
    cJSON *changes = cJSON_CreateObject();       /* category -> array of change objects */
    cJSON *missing_fields = cJSON_CreateArray();
    cJSON *unknown_fields = cJSON_CreateArray();
    cJSON *sensitive_excluded = cJSON_CreateArray();
    cJSON *validation_notes = cJSON_CreateArray();
    cJSON *no_changes_arr = cJSON_CreateArray();
    int total_changes = 0;
    bool sensitive_included = (backup_sensitive != NULL);

    /* Track which categories have changes */
    const char *categories[] = {"Display", "Behavior", "Nodes & Data", "System", "AllSky", "Spotify", "MQTT"};
    int cat_counts[7] = {0};
    int num_categories = 7;

    for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
        /* Determine which backup section this field comes from */
        const cJSON *backup_value = NULL;
        if (f->is_sensitive) {
            if (backup_sensitive) {
                backup_value = cJSON_GetObjectItem(backup_sensitive, f->json_key);
            }
            if (!backup_value) {
                /* Sensitive field not in backup */
                cJSON_AddItemToArray(sensitive_excluded, cJSON_CreateString(f->json_key));
                continue;
            }
        } else {
            backup_value = cJSON_GetObjectItem(backup_config, f->json_key);
        }

        if (!backup_value) {
            /* Field missing from backup (older version) */
            cJSON_AddItemToArray(missing_fields, cJSON_CreateString(f->json_key));
            continue;
        }

        /* Compare with current value */
        const cJSON *current_value = cJSON_GetObjectItem(current_json, f->json_key);
        if (cjson_values_equal(backup_value, current_value)) {
            continue;  /* No change */
        }

        /* Record the change */
        total_changes++;

        /* Find or create category array */
        cJSON *cat_arr = cJSON_GetObjectItem(changes, f->category);
        if (!cat_arr) {
            cat_arr = cJSON_CreateArray();
            cJSON_AddItemToObject(changes, f->category, cat_arr);
        }

        cJSON *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change, "field", f->json_key);
        cJSON_AddStringToObject(change, "label", f->label);

        /* Add from/to values — truncate large strings */
        if (f->is_large && cJSON_IsString(backup_value)) {
            char trunc[80];
            snprintf(trunc, sizeof(trunc), "Modified (%d chars)", (int)strlen(backup_value->valuestring));
            cJSON_AddStringToObject(change, "to", trunc);
            if (current_value && cJSON_IsString(current_value)) {
                snprintf(trunc, sizeof(trunc), "Current (%d chars)", (int)strlen(current_value->valuestring));
                cJSON_AddStringToObject(change, "from", trunc);
            } else {
                cJSON_AddStringToObject(change, "from", "(empty)");
            }
        } else {
            cJSON_AddItemToObject(change, "from", current_value ? cJSON_Duplicate(current_value, true) : cJSON_CreateNull());
            cJSON_AddItemToObject(change, "to", cJSON_Duplicate(backup_value, true));
        }

        cJSON_AddItemToArray(cat_arr, change);

        /* Track category counts */
        for (int c = 0; c < num_categories; c++) {
            if (strcmp(f->category, categories[c]) == 0) { cat_counts[c]++; break; }
        }
    }

    /* Check for unknown fields in backup (from newer firmware) */
    if (backup_config) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, backup_config) {
            bool found = false;
            for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
                if (!f->is_sensitive && strcmp(f->json_key, item->string) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                cJSON_AddItemToArray(unknown_fields, cJSON_CreateString(item->string));
            }
        }
    }

    /* Build no_changes list */
    for (int c = 0; c < num_categories; c++) {
        if (cat_counts[c] == 0) {
            cJSON_AddItemToArray(no_changes_arr, cJSON_CreateString(categories[c]));
        }
    }

    cJSON_AddItemToObject(resp, "missing_fields", missing_fields);
    cJSON_AddItemToObject(resp, "unknown_fields", unknown_fields);
    cJSON_AddItemToObject(resp, "validation_notes", validation_notes);
    cJSON_AddBoolToObject(resp, "sensitive_included", sensitive_included);
    cJSON_AddItemToObject(resp, "sensitive_excluded", sensitive_excluded);
    cJSON_AddItemToObject(resp, "changes", changes);
    cJSON_AddItemToObject(resp, "no_changes", no_changes_arr);
    cJSON_AddNumberToObject(resp, "total_changes", total_changes);

    return resp;
}
```

- [ ] **Step 4: Commit**

```bash
git add main/web_handlers_config.c
git commit -m "feat: add backup field registry and restore diff computation"
```

---

## Task 3: Add backup GET handler

**Files:**
- Modify: `main/web_server_internal.h` (add handler declaration)
- Modify: `main/web_server.c` (add route)
- Modify: `main/web_handlers_config.c` (add handler function)

This handler serializes the current config to JSON, wraps it with metadata, and sends it as a downloadable file.

- [ ] **Step 1: Declare the handler in `web_server_internal.h`**

Add after the existing handler declarations (around line 76):

```c
esp_err_t backup_get_handler(httpd_req_t *req);
esp_err_t restore_post_handler(httpd_req_t *req);
```

- [ ] **Step 2: Register the routes in `web_server.c`**

Add to the `routes[]` array (around line 78, before the closing `};`):

```c
{ "/api/config/backup",   HTTP_GET,  backup_get_handler,   NULL },
{ "/api/config/restore",  HTTP_POST, restore_post_handler, NULL },
```

Also bump `config.max_uri_handlers` if it's currently 32 and close to full — check the current route count. If adding 2 more stays under 32, no change needed.

- [ ] **Step 3: Implement `backup_get_handler()`**

Add to `web_handlers_config.c`:

```c
#include "build_version.h"
#include "esp_mac.h"

esp_err_t backup_get_handler(httpd_req_t *req)
{
    /* Check include_sensitive query param */
    bool include_sensitive = false;
    {
        char qbuf[32] = {0};
        if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
            char val[4] = {0};
            if (httpd_query_key_value(qbuf, "include_sensitive", val, sizeof(val)) == ESP_OK) {
                include_sensitive = (val[0] == '1');
            }
        }
    }

    /* Build root JSON */
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* ---- Meta section ---- */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "config_version", APP_CONFIG_VERSION);
    cJSON_AddStringToObject(meta, "firmware_version", BUILD_GIT_TAG);
    cJSON_AddStringToObject(meta, "git_sha", BUILD_GIT_SHA);

    /* Hostname */
    app_config_t *cfg = app_config_get();
    cJSON_AddStringToObject(meta, "hostname", cfg->hostname);

    /* MAC address */
    uint8_t mac[6];
    char mac_str[18];   /* "AA:BB:CC:DD:EE:FF" + null */
    char mac_file[13];  /* "AABBCCDDEEFF" + null (for filename) */
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(mac_file, sizeof(mac_file), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strcpy(mac_str, "00:00:00:00:00:00");
        strcpy(mac_file, "000000000000");
    }
    cJSON_AddStringToObject(meta, "mac_address", mac_str);

    /* Export date (ISO-8601 UTC) */
    time_t now;
    time(&now);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    cJSON_AddStringToObject(meta, "export_date", date_str);

    cJSON_AddItemToObject(root, "meta", meta);

    /* ---- Build full config JSON, then split into config + sensitive sections ---- */
    cJSON *full_config = cJSON_CreateObject();
    /* Populate using same logic as config_get_handler */
    cJSON_AddStringToObject(full_config, "hostname", cfg->hostname);
    cJSON_AddStringToObject(full_config, "url1", cfg->api_url[0]);
    cJSON_AddStringToObject(full_config, "url2", cfg->api_url[1]);
    cJSON_AddStringToObject(full_config, "url3", cfg->api_url[2]);
    cJSON_AddStringToObject(full_config, "ntp", cfg->ntp_server);
    cJSON_AddStringToObject(full_config, "timezone", cfg->tz_string);
    cJSON_AddStringToObject(full_config, "filter_colors_1", cfg->filter_colors[0]);
    cJSON_AddStringToObject(full_config, "filter_colors_2", cfg->filter_colors[1]);
    cJSON_AddStringToObject(full_config, "filter_colors_3", cfg->filter_colors[2]);
    cJSON_AddStringToObject(full_config, "rms_thresholds_1", cfg->rms_thresholds[0]);
    cJSON_AddStringToObject(full_config, "rms_thresholds_2", cfg->rms_thresholds[1]);
    cJSON_AddStringToObject(full_config, "rms_thresholds_3", cfg->rms_thresholds[2]);
    cJSON_AddStringToObject(full_config, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
    cJSON_AddStringToObject(full_config, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
    cJSON_AddStringToObject(full_config, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
    cJSON_AddNumberToObject(full_config, "theme_index", cfg->theme_index);
    cJSON_AddNumberToObject(full_config, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(full_config, "color_brightness", cfg->color_brightness);
    cJSON_AddBoolToObject(full_config, "mqtt_enabled", cfg->mqtt_enabled);
    cJSON_AddStringToObject(full_config, "mqtt_broker_url", cfg->mqtt_broker_url);
    cJSON_AddStringToObject(full_config, "mqtt_username", cfg->mqtt_username);
    cJSON_AddStringToObject(full_config, "mqtt_password", cfg->mqtt_password);
    cJSON_AddStringToObject(full_config, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);
    cJSON_AddNumberToObject(full_config, "mqtt_port", cfg->mqtt_port);
    cJSON_AddNumberToObject(full_config, "active_page_override", cfg->active_page_override);
    cJSON_AddBoolToObject(full_config, "auto_rotate_enabled", cfg->auto_rotate_enabled);
    cJSON_AddNumberToObject(full_config, "auto_rotate_interval_s", cfg->auto_rotate_interval_s);
    cJSON_AddNumberToObject(full_config, "auto_rotate_effect", cfg->auto_rotate_effect);
    cJSON_AddBoolToObject(full_config, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);
    cJSON_AddNumberToObject(full_config, "auto_rotate_pages", cfg->auto_rotate_pages);
    cJSON_AddNumberToObject(full_config, "update_rate_s", cfg->update_rate_s);
    cJSON_AddNumberToObject(full_config, "graph_update_interval_s", cfg->graph_update_interval_s);
    cJSON_AddNumberToObject(full_config, "connection_timeout_s", cfg->connection_timeout_s);
    cJSON_AddNumberToObject(full_config, "toast_duration_s", cfg->toast_duration_s);
    cJSON_AddBoolToObject(full_config, "debug_mode", cfg->debug_mode);
    cJSON_AddBoolToObject(full_config, "instance_enabled_1", cfg->instance_enabled[0]);
    cJSON_AddBoolToObject(full_config, "instance_enabled_2", cfg->instance_enabled[1]);
    cJSON_AddBoolToObject(full_config, "instance_enabled_3", cfg->instance_enabled[2]);
    cJSON_AddBoolToObject(full_config, "screen_sleep_enabled", cfg->screen_sleep_enabled);
    cJSON_AddNumberToObject(full_config, "screen_sleep_timeout_s", cfg->screen_sleep_timeout_s);
    cJSON_AddBoolToObject(full_config, "alert_flash_enabled", cfg->alert_flash_enabled);
    cJSON_AddNumberToObject(full_config, "idle_poll_interval_s", cfg->idle_poll_interval_s);
    cJSON_AddBoolToObject(full_config, "wifi_power_save", cfg->wifi_power_save);
    cJSON_AddNumberToObject(full_config, "widget_style", cfg->widget_style);
    cJSON_AddBoolToObject(full_config, "auto_update_check", cfg->auto_update_check);
    cJSON_AddNumberToObject(full_config, "update_channel", cfg->update_channel);
    cJSON_AddBoolToObject(full_config, "deep_sleep_enabled", cfg->deep_sleep_enabled);
    cJSON_AddNumberToObject(full_config, "deep_sleep_wake_timer_s", cfg->deep_sleep_wake_timer_s);
    cJSON_AddBoolToObject(full_config, "deep_sleep_on_idle", cfg->deep_sleep_on_idle);
    cJSON_AddNumberToObject(full_config, "screen_rotation", cfg->screen_rotation);
    cJSON_AddBoolToObject(full_config, "allsky_enabled", cfg->allsky_enabled);
    cJSON_AddStringToObject(full_config, "allsky_hostname", cfg->allsky_hostname);
    cJSON_AddNumberToObject(full_config, "allsky_update_interval_s", cfg->allsky_update_interval_s);
    cJSON_AddNumberToObject(full_config, "allsky_dew_offset", (double)cfg->allsky_dew_offset);
    cJSON_AddStringToObject(full_config, "allsky_field_config", cfg->allsky_field_config);
    cJSON_AddStringToObject(full_config, "allsky_thresholds", cfg->allsky_thresholds);
    cJSON_AddBoolToObject(full_config, "demo_mode", cfg->demo_mode);
    cJSON_AddBoolToObject(full_config, "spotify_enabled", cfg->spotify_enabled);
    cJSON_AddStringToObject(full_config, "spotify_client_id", cfg->spotify_client_id);
    cJSON_AddNumberToObject(full_config, "spotify_poll_interval_ms", cfg->spotify_poll_interval_ms);
    cJSON_AddBoolToObject(full_config, "spotify_show_progress_bar", cfg->spotify_show_progress_bar);
    cJSON_AddNumberToObject(full_config, "spotify_overlay_timeout_s", cfg->spotify_overlay_timeout_s);
    cJSON_AddBoolToObject(full_config, "spotify_minimal_mode", cfg->spotify_minimal_mode);

    /* Split into config and sensitive sections */
    cJSON *config_section = cJSON_CreateObject();
    cJSON *sensitive_section = include_sensitive ? cJSON_CreateObject() : NULL;

    for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
        cJSON *val = cJSON_GetObjectItem(full_config, f->json_key);
        if (!val) continue;

        if (f->is_sensitive) {
            if (sensitive_section) {
                cJSON_AddItemToObject(sensitive_section, f->json_key, cJSON_Duplicate(val, true));
            }
        } else {
            cJSON_AddItemToObject(config_section, f->json_key, cJSON_Duplicate(val, true));
        }
    }

    cJSON_AddItemToObject(root, "config", config_section);
    if (sensitive_section) {
        cJSON_AddItemToObject(root, "sensitive", sensitive_section);
    }

    cJSON_Delete(full_config);

    /* Serialize and send */
    const char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* Build filename: {hostname}_{MAC}_v{version}_{date}.json */
    char filename[128];
    /* Sanitize hostname for filename (alphanumeric + hyphens only) */
    char safe_host[33];
    {
        const char *src = cfg->hostname;
        int j = 0;
        for (int i = 0; src[i] && j < 32; i++) {
            char c = src[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-') {
                safe_host[j++] = c;
            }
        }
        if (j == 0) { safe_host[j++] = 'D'; safe_host[j++] = 'E'; safe_host[j++] = 'V'; }
        safe_host[j] = '\0';
    }
    char date_short[16];
    strftime(date_short, sizeof(date_short), "%Y-%m-%d", &timeinfo);
    snprintf(filename, sizeof(filename),
             "attachment; filename=\"%s_%s_v%d_%s.json\"",
             safe_host, mac_file, APP_CONFIG_VERSION, date_short);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", filename);
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free((void *)json_str);
    return ESP_OK;
}
```

**Important implementation notes for the agent:**
- Check the exact field serialization in the existing `config_get_handler()` (lines 32-127 of `web_handlers_config.c`) and make sure every field uses the same JSON key name. The code above mirrors those keys. Cross-reference carefully — if `config_get_handler` uses `"url1"` for `cfg->api_url[0]`, we must use the same.
- `build_version.h` is auto-generated at build time — just `#include "build_version.h"`.
- Use `ESP_MAC_BASE` not `ESP_MAC_WIFI_STA` (the latter fails on ESP32-P4 remote coprocessor). See `main/mqtt_ha.c:58` for the pattern.
- **Add `config_version` to `config_get_handler` response**: Add `cJSON_AddNumberToObject(root, "config_version", APP_CONFIG_VERSION)` at the start of `config_get_handler`'s JSON building block. This is needed so the web UI Backup tab can display the current config version. The backup tab's `populateConfig` handler reads `data.config_version`.

- [ ] **Step 4: Commit**

```bash
git add main/web_server_internal.h main/web_server.c main/web_handlers_config.c
git commit -m "feat: add GET /api/config/backup endpoint for config export"
```

---

## Task 4: Add restore POST handler

**Files:**
- Modify: `main/web_handlers_config.c` (add `restore_post_handler()`)

- [ ] **Step 1: Implement `restore_post_handler()`**

Add to `web_handlers_config.c`:

```c
esp_err_t restore_post_handler(httpd_req_t *req)
{
    cJSON *root = receive_json_body(req, CONFIG_MAX_RESTORE_PAYLOAD);
    if (!root) return ESP_OK;  /* error already sent */

    /* Validate structure */
    cJSON *backup = cJSON_GetObjectItem(root, "backup");
    cJSON *confirm_item = cJSON_GetObjectItem(root, "confirm");
    if (!backup || !cJSON_IsObject(backup)) {
        cJSON_Delete(root);
        return send_400(req, "Missing 'backup' object in request");
    }

    cJSON *meta = cJSON_GetObjectItem(backup, "meta");
    cJSON *backup_config = cJSON_GetObjectItem(backup, "config");
    if (!meta || !backup_config) {
        cJSON_Delete(root);
        return send_400(req, "Invalid backup file: missing 'meta' or 'config' section");
    }

    cJSON *ver = cJSON_GetObjectItem(meta, "config_version");
    if (!cJSON_IsNumber(ver)) {
        cJSON_Delete(root);
        return send_400(req, "Invalid backup file: no config_version in metadata");
    }

    bool do_confirm = cJSON_IsTrue(confirm_item);

    if (!do_confirm) {
        /* ---- Preview mode ---- */
        /* Serialize current config to JSON for comparison */
        /* Reuse the same serialization as config_get_handler by building it inline */
        app_config_t *cfg = app_config_get();
        cJSON *current_json = cJSON_CreateObject();
        if (!current_json) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        /* Populate current_json with all fields (same keys as config_get_handler) */
        /* NOTE: The implementing agent should extract a shared helper function
         * like serialize_config_to_json(cfg) that both config_get_handler and
         * this handler can call, to avoid duplicating the field list.
         * Alternatively, just duplicate the serialization block from config_get_handler. */
        cJSON_AddStringToObject(current_json, "hostname", cfg->hostname);
        cJSON_AddStringToObject(current_json, "url1", cfg->api_url[0]);
        cJSON_AddStringToObject(current_json, "url2", cfg->api_url[1]);
        cJSON_AddStringToObject(current_json, "url3", cfg->api_url[2]);
        cJSON_AddStringToObject(current_json, "ntp", cfg->ntp_server);
        cJSON_AddStringToObject(current_json, "timezone", cfg->tz_string);
        cJSON_AddStringToObject(current_json, "filter_colors_1", cfg->filter_colors[0]);
        cJSON_AddStringToObject(current_json, "filter_colors_2", cfg->filter_colors[1]);
        cJSON_AddStringToObject(current_json, "filter_colors_3", cfg->filter_colors[2]);
        cJSON_AddStringToObject(current_json, "rms_thresholds_1", cfg->rms_thresholds[0]);
        cJSON_AddStringToObject(current_json, "rms_thresholds_2", cfg->rms_thresholds[1]);
        cJSON_AddStringToObject(current_json, "rms_thresholds_3", cfg->rms_thresholds[2]);
        cJSON_AddStringToObject(current_json, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
        cJSON_AddStringToObject(current_json, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
        cJSON_AddStringToObject(current_json, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
        cJSON_AddNumberToObject(current_json, "theme_index", cfg->theme_index);
        cJSON_AddNumberToObject(current_json, "brightness", cfg->brightness);
        cJSON_AddNumberToObject(current_json, "color_brightness", cfg->color_brightness);
        cJSON_AddBoolToObject(current_json, "mqtt_enabled", cfg->mqtt_enabled);
        cJSON_AddStringToObject(current_json, "mqtt_broker_url", cfg->mqtt_broker_url);
        cJSON_AddStringToObject(current_json, "mqtt_username", cfg->mqtt_username);
        cJSON_AddStringToObject(current_json, "mqtt_password", cfg->mqtt_password);
        cJSON_AddStringToObject(current_json, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);
        cJSON_AddNumberToObject(current_json, "mqtt_port", cfg->mqtt_port);
        cJSON_AddNumberToObject(current_json, "active_page_override", cfg->active_page_override);
        cJSON_AddBoolToObject(current_json, "auto_rotate_enabled", cfg->auto_rotate_enabled);
        cJSON_AddNumberToObject(current_json, "auto_rotate_interval_s", cfg->auto_rotate_interval_s);
        cJSON_AddNumberToObject(current_json, "auto_rotate_effect", cfg->auto_rotate_effect);
        cJSON_AddBoolToObject(current_json, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);
        cJSON_AddNumberToObject(current_json, "auto_rotate_pages", cfg->auto_rotate_pages);
        cJSON_AddNumberToObject(current_json, "update_rate_s", cfg->update_rate_s);
        cJSON_AddNumberToObject(current_json, "graph_update_interval_s", cfg->graph_update_interval_s);
        cJSON_AddNumberToObject(current_json, "connection_timeout_s", cfg->connection_timeout_s);
        cJSON_AddNumberToObject(current_json, "toast_duration_s", cfg->toast_duration_s);
        cJSON_AddBoolToObject(current_json, "debug_mode", cfg->debug_mode);
        cJSON_AddBoolToObject(current_json, "instance_enabled_1", cfg->instance_enabled[0]);
        cJSON_AddBoolToObject(current_json, "instance_enabled_2", cfg->instance_enabled[1]);
        cJSON_AddBoolToObject(current_json, "instance_enabled_3", cfg->instance_enabled[2]);
        cJSON_AddBoolToObject(current_json, "screen_sleep_enabled", cfg->screen_sleep_enabled);
        cJSON_AddNumberToObject(current_json, "screen_sleep_timeout_s", cfg->screen_sleep_timeout_s);
        cJSON_AddBoolToObject(current_json, "alert_flash_enabled", cfg->alert_flash_enabled);
        cJSON_AddNumberToObject(current_json, "idle_poll_interval_s", cfg->idle_poll_interval_s);
        cJSON_AddBoolToObject(current_json, "wifi_power_save", cfg->wifi_power_save);
        cJSON_AddNumberToObject(current_json, "widget_style", cfg->widget_style);
        cJSON_AddBoolToObject(current_json, "auto_update_check", cfg->auto_update_check);
        cJSON_AddNumberToObject(current_json, "update_channel", cfg->update_channel);
        cJSON_AddBoolToObject(current_json, "deep_sleep_enabled", cfg->deep_sleep_enabled);
        cJSON_AddNumberToObject(current_json, "deep_sleep_wake_timer_s", cfg->deep_sleep_wake_timer_s);
        cJSON_AddBoolToObject(current_json, "deep_sleep_on_idle", cfg->deep_sleep_on_idle);
        cJSON_AddNumberToObject(current_json, "screen_rotation", cfg->screen_rotation);
        cJSON_AddBoolToObject(current_json, "allsky_enabled", cfg->allsky_enabled);
        cJSON_AddStringToObject(current_json, "allsky_hostname", cfg->allsky_hostname);
        cJSON_AddNumberToObject(current_json, "allsky_update_interval_s", cfg->allsky_update_interval_s);
        cJSON_AddNumberToObject(current_json, "allsky_dew_offset", cfg->allsky_dew_offset);
        cJSON_AddStringToObject(current_json, "allsky_field_config", cfg->allsky_field_config);
        cJSON_AddStringToObject(current_json, "allsky_thresholds", cfg->allsky_thresholds);
        cJSON_AddBoolToObject(current_json, "demo_mode", cfg->demo_mode);
        cJSON_AddBoolToObject(current_json, "spotify_enabled", cfg->spotify_enabled);
        cJSON_AddStringToObject(current_json, "spotify_client_id", cfg->spotify_client_id);
        cJSON_AddNumberToObject(current_json, "spotify_poll_interval_ms", cfg->spotify_poll_interval_ms);
        cJSON_AddBoolToObject(current_json, "spotify_show_progress_bar", cfg->spotify_show_progress_bar);
        cJSON_AddNumberToObject(current_json, "spotify_overlay_timeout_s", cfg->spotify_overlay_timeout_s);
        cJSON_AddBoolToObject(current_json, "spotify_minimal_mode", cfg->spotify_minimal_mode);

        cJSON *preview = build_restore_preview(backup, current_json);
        cJSON_Delete(current_json);
        cJSON_Delete(root);

        if (!preview) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        const char *json_str = cJSON_PrintUnformatted(preview);
        cJSON_Delete(preview);
        if (!json_str) { httpd_resp_send_500(req); return ESP_OK; }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
        free((void *)json_str);
        return ESP_OK;

    } else {
        /* ---- Confirm mode: apply the backup ---- */
        /* Merge backup config + sensitive into a single JSON object */
        cJSON *merged = cJSON_Duplicate(backup_config, true);
        if (!merged) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        /* Overlay sensitive fields if present */
        cJSON *backup_sensitive = cJSON_GetObjectItem(backup, "sensitive");
        if (backup_sensitive) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, backup_sensitive) {
                /* Only overlay fields we recognize */
                bool known = false;
                for (const backup_field_t *f = s_backup_fields; f->json_key; f++) {
                    if (strcmp(f->json_key, item->string) == 0) { known = true; break; }
                }
                if (known) {
                    cJSON_DeleteItemFromObject(merged, item->string);
                    cJSON_AddItemToObject(merged, item->string, cJSON_Duplicate(item, true));
                }
            }
        }

        /* Validate merged fields */
        if (!validate_config_fields(merged, req)) {
            cJSON_Delete(merged);
            cJSON_Delete(root);
            return ESP_OK;
        }

        /* Parse into config struct using existing parse_config_from_json */
        app_config_t *new_cfg = parse_config_from_json(merged);
        cJSON_Delete(merged);
        cJSON_Delete(root);

        if (!new_cfg) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        /* Save to NVS and trigger side effects */
        app_config_t *old_cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
        if (!old_cfg) {
            free(new_cfg);
            httpd_resp_send_500(req);
            return ESP_OK;
        }
        memcpy(old_cfg, app_config_get(), sizeof(app_config_t));

        app_config_save(new_cfg);
        config_trigger_side_effects(old_cfg, new_cfg);
        free(old_cfg);
        free(new_cfg);

        /* Send success response with change count */
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "applied");
        /* Note: total_changes is approximate — config may have changed between
         * preview and confirm. The UI should not assert on matching counts. */
        cJSON_AddNumberToObject(resp, "total_changes", 0);
        cJSON_AddItemToObject(resp, "validation_notes", cJSON_CreateArray());
        const char *resp_str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp_str ? resp_str : "{\"status\":\"applied\"}", HTTPD_RESP_USE_STRLEN);
        free((void *)resp_str);
        return ESP_OK;
    }
}
```

**Important notes for the agent:**
- The config serialization is duplicated between `config_get_handler`, `backup_get_handler`, and `restore_post_handler`. Consider extracting a shared `static cJSON *serialize_config_to_json(const app_config_t *cfg)` helper. This is strongly recommended to keep things DRY. Look at the existing `config_get_handler` (lines 32-127) and factor the field serialization into this helper.
- `parse_config_from_json()` already starts from a copy of current config and only overlays present fields — this is exactly the JSON overlay behavior we want.
- `config_trigger_side_effects()` handles MQTT reconnection, theme refresh, etc. — it's already called by `config_post_handler` (line 489).
- `validate_config_fields()` checks string lengths and URL formats — reuse as-is.

- [ ] **Step 2: Commit**

```bash
git add main/web_handlers_config.c
git commit -m "feat: add POST /api/config/restore endpoint for config import"
```

---

## Task 5: Add "Backup" tab to web UI

**Files:**
- Modify: `main/config_ui.html`

This is the largest task. The new tab follows the exact same patterns as existing tabs (Spotify, AllSky, etc.).

- [ ] **Step 1: Add tab button in top navigation**

After the Spotify tab button (line 446), add:

```html
<button class="tab-btn" data-tab="backup">Backup</button>
```

- [ ] **Step 2: Add mobile bottom navigation button**

After the Spotify nav button (around line 480), add:

```html
<button class="nav-btn" data-tab="backup">
  <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
    <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
    <polyline points="7 10 12 15 17 10"/>
    <line x1="12" y1="15" x2="12" y2="3"/>
  </svg>
  Backup
</button>
```

- [ ] **Step 3: Add tab accent color**

In the `tabAccents` object (around line 1076), add:

```javascript
backup:  {color:'#f59e0b', rgb:'245, 158, 11'}
```

This gives the Backup tab an amber/gold accent — distinct from existing tabs and thematically appropriate for a "caution/important" feature.

- [ ] **Step 4: Add the tab panel HTML**

After the Spotify tab panel closing `</div>` (after line 974), add the full Backup tab panel:

```html
<div id="tab-backup" class="tab-panel">
  <!-- Export Card -->
  <div class="card">
    <div class="card-title">Export Configuration</div>
    <div style="display:flex;align-items:center;gap:12px;margin-bottom:16px;font-size:0.82rem;color:#a1a1aa">
      <span>Config version: <strong style="color:#f4f4f5" id="backupCurrentVersion">--</strong></span>
      <span>&middot;</span>
      <span>Firmware: <strong style="color:#f4f4f5" id="backupFirmwareVersion">--</strong></span>
    </div>
    <div class="toggle-row" style="border-bottom:1px solid rgba(255,255,255,0.06)">
      <div>
        <span class="toggle-label">Include sensitive data</span>
        <div class="field-hint" style="margin-top:2px">MQTT credentials, Spotify Client ID, Hostname</div>
      </div>
      <label class="toggle"><input type="checkbox" id="backupIncludeSensitive"><span class="toggle-track"></span></label>
    </div>
    <button class="btn btn-primary" id="backupDownloadBtn" onclick="downloadBackup()" style="margin-top:16px;width:100%">Download Backup</button>
  </div>

  <!-- Import Card -->
  <div class="card">
    <div class="card-title">Restore Configuration</div>
    <div class="field">
      <label class="ota-label" id="restoreFileLabel" style="display:block;text-align:center;cursor:pointer">
        Choose backup .json file
        <input type="file" id="restoreFileInput" accept=".json" onchange="restoreFileSelected(this)" style="display:none">
      </label>
    </div>

    <!-- Preview Panel (hidden until file selected) -->
    <div id="restorePreview" style="display:none;margin-top:16px">
      <!-- Metadata Header -->
      <div style="background:rgba(255,255,255,0.04);border:1px solid rgba(255,255,255,0.08);border-radius:10px;padding:14px 16px;margin-bottom:12px;font-size:0.82rem;color:#a1a1aa;line-height:1.8">
        <div>Source: <strong style="color:#f4f4f5" id="restoreHostname">--</strong> (<span id="restoreMac">--</span>)</div>
        <div>Firmware: <strong style="color:#f4f4f5" id="restoreFirmware">--</strong> &middot; Exported: <span id="restoreDate">--</span></div>
      </div>

      <!-- Version Warning Banner -->
      <div id="restoreWarningBanner" style="border-radius:10px;padding:14px 16px;margin-bottom:12px;font-size:0.82rem;line-height:1.6;display:none"></div>

      <!-- Changes Diff -->
      <div id="restoreChanges" style="margin-bottom:12px"></div>

      <!-- No Changes -->
      <div id="restoreNoChanges" style="font-size:0.82rem;color:#71717a;margin-bottom:12px"></div>

      <!-- Missing Fields Notice -->
      <div id="restoreMissing" style="display:none;background:rgba(245,158,11,0.06);border:1px solid rgba(245,158,11,0.15);border-radius:10px;padding:12px 16px;margin-bottom:12px;font-size:0.8rem;color:#d4d4d8"></div>

      <!-- Unknown Fields Notice -->
      <div id="restoreUnknown" style="display:none;background:rgba(245,158,11,0.06);border:1px solid rgba(245,158,11,0.15);border-radius:10px;padding:12px 16px;margin-bottom:12px;font-size:0.8rem;color:#d4d4d8"></div>

      <!-- Sensitive Excluded Notice -->
      <div id="restoreSensitive" style="display:none;background:rgba(255,255,255,0.03);border:1px solid rgba(255,255,255,0.06);border-radius:10px;padding:12px 16px;margin-bottom:12px;font-size:0.8rem;color:#71717a"></div>

      <!-- Validation Notes -->
      <div id="restoreValidation" style="display:none;background:rgba(245,158,11,0.06);border:1px solid rgba(245,158,11,0.15);border-radius:10px;padding:12px 16px;margin-bottom:12px;font-size:0.8rem;color:#d4d4d8"></div>

      <!-- Action Buttons -->
      <div style="display:flex;gap:10px;margin-top:16px">
        <button class="btn btn-outline" onclick="cancelRestore()" style="flex:1">Cancel</button>
        <button class="btn btn-primary" id="restoreConfirmBtn" onclick="confirmRestore()" style="flex:1">Restore</button>
      </div>
    </div>
  </div>
</div>
```

- [ ] **Step 5: Add the JavaScript functions**

Add the following JavaScript before the closing `</script>` tag (around line 1710):

```javascript
/* ---- Backup & Restore ---- */

var _backupData = null;  /* holds parsed backup JSON for confirm step */

function downloadBackup() {
  var btn = $('backupDownloadBtn');
  btn.disabled = true; btn.textContent = 'Downloading...';
  var sensitive = $('backupIncludeSensitive').checked ? '1' : '0';
  fetch('/api/config/backup?include_sensitive=' + sensitive)
    .then(function(r) {
      if (!r.ok) throw new Error('Download failed');
      /* Get filename from Content-Disposition header */
      var cd = r.headers.get('Content-Disposition') || '';
      var match = cd.match(/filename="?([^"]+)"?/);
      var filename = match ? match[1] : 'config-backup.json';
      return r.blob().then(function(blob) { return {blob: blob, filename: filename}; });
    })
    .then(function(result) {
      var a = document.createElement('a');
      a.href = URL.createObjectURL(result.blob);
      a.download = result.filename;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(a.href);
      showToast('Backup downloaded', 'success');
    })
    .catch(function(e) { showToast('Backup failed: ' + e.message, 'error'); })
    .finally(function() { btn.disabled = false; btn.textContent = 'Download Backup'; });
}

function restoreFileSelected(input) {
  var label = $('restoreFileLabel');
  if (!input.files.length) {
    label.textContent = 'Choose backup .json file';
    label.classList.remove('has-file');
    label.appendChild(input);
    $('restorePreview').style.display = 'none';
    return;
  }
  label.textContent = input.files[0].name;
  label.classList.add('has-file');
  label.appendChild(input);

  var reader = new FileReader();
  reader.onload = function(e) {
    try {
      _backupData = JSON.parse(e.target.result);
    } catch (err) {
      showToast('Invalid JSON file', 'error');
      return;
    }
    /* Send preview request */
    postJson('/api/config/restore', {backup: _backupData, confirm: false})
      .then(function(r) {
        if (!r.ok) return r.json().then(function(e) { throw new Error(e.error || 'Preview failed'); });
        return r.json();
      })
      .then(function(preview) { renderRestorePreview(preview); })
      .catch(function(e) { showToast('Preview failed: ' + e.message, 'error'); });
  };
  reader.readAsText(input.files[0]);
}

function renderRestorePreview(p) {
  if (p.error) {
    showToast(p.error, 'error');
    return;
  }

  $('restorePreview').style.display = 'block';

  /* Metadata */
  $('restoreHostname').textContent = p.backup_hostname || 'Unknown';
  $('restoreMac').textContent = p.backup_mac || '--';
  $('restoreFirmware').textContent = p.backup_firmware || '--';
  $('restoreDate').textContent = p.export_date || '--';

  /* Version warning banner */
  var banner = $('restoreWarningBanner');
  if (p.warnings && p.warnings.length > 0) {
    var color;
    if (p.version_match === 'exact')       color = {bg: 'rgba(16,185,129,0.08)', border: 'rgba(16,185,129,0.2)', text: '#10b981'};
    else if (p.version_match === 'newer')  color = {bg: 'rgba(245,158,11,0.08)', border: 'rgba(245,158,11,0.2)', text: '#f59e0b'};
    else if (p.version_match === 'much_older') color = {bg: 'rgba(239,68,68,0.08)', border: 'rgba(239,68,68,0.2)', text: '#ef4444'};
    else                                   color = {bg: 'rgba(245,158,11,0.08)', border: 'rgba(245,158,11,0.2)', text: '#f59e0b'};
    banner.style.background = color.bg;
    banner.style.borderColor = color.border;
    banner.style.border = '1px solid ' + color.border;
    banner.style.color = color.text;
    banner.style.display = 'block';
    banner.innerHTML = '<div style="margin-bottom:4px"><strong>Version: v' + p.backup_version + ' &rarr; v' + p.current_version + '</strong></div>' +
      p.warnings.map(function(w) { return '<div>' + w + '</div>'; }).join('');
  } else {
    banner.style.display = 'none';
  }

  /* Changes diff grouped by category */
  var changesDiv = $('restoreChanges');
  changesDiv.innerHTML = '';
  if (p.changes && p.total_changes > 0) {
    var cats = Object.keys(p.changes);
    cats.forEach(function(cat) {
      var items = p.changes[cat];
      if (!items || !items.length) return;
      var section = document.createElement('div');
      section.style.cssText = 'background:rgba(255,255,255,0.03);border:1px solid rgba(255,255,255,0.06);border-radius:10px;padding:14px 16px;margin-bottom:8px';
      var header = '<div style="font-size:0.85rem;font-weight:600;color:#f4f4f5;margin-bottom:8px;cursor:pointer" onclick="this.nextElementSibling.style.display=this.nextElementSibling.style.display===\'none\'?\'block\':\'none\'">' +
        cat + ' <span style="color:#71717a;font-weight:400">(' + items.length + ' change' + (items.length > 1 ? 's' : '') + ')</span></div>';
      var body = '<div>';
      items.forEach(function(ch) {
        var fromStr = formatDiffValue(ch.from);
        var toStr = formatDiffValue(ch.to);
        body += '<div style="display:flex;justify-content:space-between;padding:4px 0;font-size:0.8rem;border-bottom:1px solid rgba(255,255,255,0.04)">' +
          '<span style="color:#a1a1aa">' + ch.label + '</span>' +
          '<span><span style="color:#71717a;text-decoration:line-through">' + fromStr + '</span> &rarr; <span style="color:var(--accent)">' + toStr + '</span></span>' +
          '</div>';
      });
      body += '</div>';
      section.innerHTML = header + body;
      changesDiv.appendChild(section);
    });
  }

  /* No changes categories */
  var noChanges = $('restoreNoChanges');
  if (p.no_changes && p.no_changes.length > 0) {
    noChanges.textContent = 'No changes: ' + p.no_changes.join(', ');
  } else {
    noChanges.textContent = '';
  }

  /* Missing fields */
  var missingDiv = $('restoreMissing');
  if (p.missing_fields && p.missing_fields.length > 0) {
    missingDiv.style.display = 'block';
    missingDiv.innerHTML = '<strong style="color:#f59e0b">Missing from backup:</strong> ' +
      p.missing_fields.join(', ') + '<br><span style="color:#71717a">These settings will keep their current values.</span>';
  } else {
    missingDiv.style.display = 'none';
  }

  /* Unknown fields */
  var unknownDiv = $('restoreUnknown');
  if (p.unknown_fields && p.unknown_fields.length > 0) {
    unknownDiv.style.display = 'block';
    unknownDiv.innerHTML = '<strong style="color:#f59e0b">Unknown fields (from newer firmware):</strong> ' +
      p.unknown_fields.join(', ') + '<br><span style="color:#71717a">These will be skipped.</span>';
  } else {
    unknownDiv.style.display = 'none';
  }

  /* Sensitive excluded */
  var sensDiv = $('restoreSensitive');
  if (!p.sensitive_included && p.sensitive_excluded && p.sensitive_excluded.length > 0) {
    sensDiv.style.display = 'block';
    sensDiv.innerHTML = '<strong>Sensitive fields not in backup:</strong> ' +
      p.sensitive_excluded.join(', ') + '<br>These will remain unchanged.';
  } else {
    sensDiv.style.display = 'none';
  }

  /* Validation notes */
  var valDiv = $('restoreValidation');
  if (p.validation_notes && p.validation_notes.length > 0) {
    valDiv.style.display = 'block';
    valDiv.innerHTML = '<strong style="color:#f59e0b">Adjusted values:</strong><br>' +
      p.validation_notes.join('<br>');
  } else {
    valDiv.style.display = 'none';
  }

  /* Update confirm button text */
  $('restoreConfirmBtn').textContent = 'Restore (' + p.total_changes + ' change' + (p.total_changes !== 1 ? 's' : '') + ')';
}

function formatDiffValue(val) {
  if (val === null || val === undefined) return '(empty)';
  if (typeof val === 'boolean') return val ? 'On' : 'Off';
  if (typeof val === 'string' && val.length > 64) return val.substring(0, 61) + '...';
  return String(val);
}

function cancelRestore() {
  $('restorePreview').style.display = 'none';
  $('restoreFileLabel').textContent = 'Choose backup .json file';
  $('restoreFileLabel').classList.remove('has-file');
  $('restoreFileInput').value = '';
  _backupData = null;
}

function confirmRestore() {
  if (!_backupData) return;
  var btn = $('restoreConfirmBtn');
  btn.disabled = true; btn.textContent = 'Restoring...';

  postJson('/api/config/restore', {backup: _backupData, confirm: true})
    .then(function(r) {
      if (!r.ok) return r.json().then(function(e) { throw new Error(e.error || 'Restore failed'); });
      return r.json();
    })
    .then(function(result) {
      showToast('Configuration restored successfully', 'success');
      _backupData = null;
      /* Reload config and reset UI after short delay */
      setTimeout(function() { loadConfig(); cancelRestore(); }, 500);
    })
    .catch(function(e) {
      showToast('Restore failed: ' + e.message, 'error');
    })
    .finally(function() {
      btn.disabled = false; btn.textContent = 'Restore';
    });
}
```

- [ ] **Step 6: Update `populateConfig()` to fill backup tab info**

At the end of the `populateConfig()` function (around line 1483), add:

```javascript
/* Backup tab version info */
if ($('backupCurrentVersion')) $('backupCurrentVersion').textContent = 'v' + (data.config_version || '?');
if ($('backupFirmwareVersion')) {
  fetch('/api/version').then(function(r){return r.json()}).then(function(v){
    $('backupFirmwareVersion').textContent = v.git_tag || '--';
  }).catch(function(){});
}
```

**Note:** The current config version is not in the `GET /api/config` response — it's internal. Either:
- (a) Add `config_version` to `config_get_handler` response (easiest — just add `cJSON_AddNumberToObject(root, "config_version", APP_CONFIG_VERSION)` at the start of `config_get_handler`), or
- (b) Fetch it from the `/api/version` endpoint if it includes the config version there.

Option (a) is simpler. The implementing agent should add `config_version` to the existing `config_get_handler` response.

- [ ] **Step 7: Commit**

```bash
git add main/config_ui.html main/web_handlers_config.c
git commit -m "feat: add Backup tab to web UI with export/import and preview diff"
```

---

## Task 6: Extract shared config serialization helper (DRY refactor)

**Files:**
- Modify: `main/web_handlers_config.c`

The config-to-JSON serialization is duplicated across `config_get_handler`, `backup_get_handler`, and `restore_post_handler`. Extract it into a shared static function.

- [ ] **Step 1: Create `serialize_config_to_json()` helper**

```c
/*
 * Serialize app_config_t to a cJSON object using the same keys as GET /api/config.
 * Returns a new cJSON object (caller must cJSON_Delete).
 * Returns NULL on allocation failure.
 *
 * IMPORTANT: This helper serializes ONLY app_config_t fields.
 * It must NOT include:
 *   - "ssid" (fetched from esp_wifi_get_config, not app_config_t)
 *   - "_dirty" (runtime state from app_config_is_dirty())
 * Those are added by config_get_handler separately after calling this helper.
 */
static cJSON *serialize_config_to_json(const app_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    /* Copy the exact serialization from config_get_handler lines 34-112,
     * EXCLUDING ssid (WiFi stack) and _dirty (runtime state) */
    cJSON_AddNumberToObject(root, "config_version", APP_CONFIG_VERSION);
    cJSON_AddStringToObject(root, "hostname", cfg->hostname);
    cJSON_AddStringToObject(root, "url1", cfg->api_url[0]);
    /* ... all app_config_t fields ... */

    return root;
}
```

- [ ] **Step 2: Refactor `config_get_handler` to use the helper**

Replace the manual serialization block with:

```c
esp_err_t config_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    cJSON *root = serialize_config_to_json(cfg);
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* Add fields NOT from app_config_t (these live outside the struct) */
    /* ssid comes from WiFi stack, not app_config_t */
    wifi_config_t sta_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", (const char *)sta_cfg.sta.ssid);
    }
    cJSON_AddBoolToObject(root, "_dirty", app_config_is_dirty());

    const char *json_str = cJSON_PrintUnformatted(root);
    /* ... rest unchanged ... */
}
```

- [ ] **Step 3: Refactor `backup_get_handler` and `restore_post_handler` to use the helper**

Replace their inline serialization blocks with calls to `serialize_config_to_json()`.

- [ ] **Step 4: Commit**

```bash
git add main/web_handlers_config.c
git commit -m "refactor: extract serialize_config_to_json to deduplicate config serialization"
```

---

## Task 7: Final integration and testing

**Files:**
- All modified files

- [ ] **Step 1: Verify all routes are registered and handler count is sufficient**

Check `config.max_uri_handlers` in `web_server.c`. Count all routes in the `routes[]` array. If adding 2 new routes exceeds the limit, bump it.

- [ ] **Step 2: Verify `#include` directives**

Ensure `web_handlers_config.c` has:
```c
#include "build_version.h"
#include "esp_mac.h"
#include "app_config.h"
```

- [ ] **Step 3: Review the complete diff**

Run `git diff HEAD~N` (where N = number of commits in this feature) and verify:
- No duplicate code blocks remain
- All cJSON objects are properly freed on all code paths
- `receive_json_body` callers all pass `max_size`
- Field registry matches all fields in `config_get_handler`
- HTML tab structure is consistent with existing tabs

- [ ] **Step 4: Commit any final fixes**

```bash
git add -A
git commit -m "chore: final cleanup for config backup/restore feature"
```
