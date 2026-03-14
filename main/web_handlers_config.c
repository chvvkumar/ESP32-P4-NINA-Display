#include "web_server_internal.h"
#include "mqtt_ha.h"
#include "esp_wifi.h"
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "build_version.h"
#include "esp_mac.h"

extern const uint8_t config_html_start[] asm("_binary_config_ui_html_start");
extern const uint8_t config_html_end[]   asm("_binary_config_ui_html_end");
extern const uint8_t favicon_png_start[] asm("_binary_favicon_png_start");
extern const uint8_t favicon_png_end[]   asm("_binary_favicon_png_end");

// Handler for root URL
esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)config_html_start,
                    config_html_end - config_html_start);
    return ESP_OK;
}

// Handler for favicon
esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    httpd_resp_send(req, (const char *)favicon_png_start,
                    favicon_png_end - favicon_png_start);
    return ESP_OK;
}

/*
 * Serialize all app_config_t fields to a cJSON object.
 * Includes config_version but NOT ssid (WiFi stack) or _dirty (runtime state).
 * Returns a new cJSON object on success, NULL on allocation failure.
 * Caller must cJSON_Delete() the result.
 */
static cJSON *serialize_config_to_json(const app_config_t *cfg)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "config_version", APP_CONFIG_VERSION);

    cJSON_AddStringToObject(obj, "hostname", cfg->hostname);
    cJSON_AddStringToObject(obj, "url1", cfg->api_url[0]);
    cJSON_AddStringToObject(obj, "url2", cfg->api_url[1]);
    cJSON_AddStringToObject(obj, "url3", cfg->api_url[2]);
    cJSON_AddStringToObject(obj, "ntp", cfg->ntp_server);
    cJSON_AddStringToObject(obj, "timezone", cfg->tz_string);
    cJSON_AddStringToObject(obj, "filter_colors_1", cfg->filter_colors[0]);
    cJSON_AddStringToObject(obj, "filter_colors_2", cfg->filter_colors[1]);
    cJSON_AddStringToObject(obj, "filter_colors_3", cfg->filter_colors[2]);
    cJSON_AddStringToObject(obj, "rms_thresholds_1", cfg->rms_thresholds[0]);
    cJSON_AddStringToObject(obj, "rms_thresholds_2", cfg->rms_thresholds[1]);
    cJSON_AddStringToObject(obj, "rms_thresholds_3", cfg->rms_thresholds[2]);
    cJSON_AddStringToObject(obj, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
    cJSON_AddStringToObject(obj, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
    cJSON_AddStringToObject(obj, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
    cJSON_AddNumberToObject(obj, "theme_index", cfg->theme_index);
    cJSON_AddNumberToObject(obj, "widget_style", cfg->widget_style);
    cJSON_AddNumberToObject(obj, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(obj, "color_brightness", cfg->color_brightness);
    cJSON_AddBoolToObject(obj, "mqtt_enabled", cfg->mqtt_enabled);
    cJSON_AddStringToObject(obj, "mqtt_broker_url", cfg->mqtt_broker_url);
    cJSON_AddNumberToObject(obj, "mqtt_port", cfg->mqtt_port);
    cJSON_AddStringToObject(obj, "mqtt_username", cfg->mqtt_username);
    cJSON_AddStringToObject(obj, "mqtt_password", cfg->mqtt_password);
    cJSON_AddStringToObject(obj, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);
    cJSON_AddNumberToObject(obj, "active_page_override", cfg->active_page_override);
    cJSON_AddBoolToObject(obj, "auto_rotate_enabled", cfg->auto_rotate_enabled);
    cJSON_AddNumberToObject(obj, "auto_rotate_interval_s", cfg->auto_rotate_interval_s);
    cJSON_AddNumberToObject(obj, "auto_rotate_effect", cfg->auto_rotate_effect);
    cJSON_AddBoolToObject(obj, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);
    cJSON_AddNumberToObject(obj, "auto_rotate_pages", cfg->auto_rotate_pages);
    cJSON_AddNumberToObject(obj, "update_rate_s", cfg->update_rate_s);
    cJSON_AddNumberToObject(obj, "graph_update_interval_s", cfg->graph_update_interval_s);
    cJSON_AddNumberToObject(obj, "connection_timeout_s", cfg->connection_timeout_s);
    cJSON_AddNumberToObject(obj, "toast_duration_s", cfg->toast_duration_s);
    cJSON_AddBoolToObject(obj, "debug_mode", cfg->debug_mode);
    cJSON_AddBoolToObject(obj, "demo_mode", cfg->demo_mode);
    cJSON_AddBoolToObject(obj, "instance_enabled_1", cfg->instance_enabled[0]);
    cJSON_AddBoolToObject(obj, "instance_enabled_2", cfg->instance_enabled[1]);
    cJSON_AddBoolToObject(obj, "instance_enabled_3", cfg->instance_enabled[2]);
    cJSON_AddBoolToObject(obj, "screen_sleep_enabled", cfg->screen_sleep_enabled);
    cJSON_AddNumberToObject(obj, "screen_sleep_timeout_s", cfg->screen_sleep_timeout_s);
    cJSON_AddBoolToObject(obj, "alert_flash_enabled", cfg->alert_flash_enabled);
    cJSON_AddNumberToObject(obj, "idle_poll_interval_s", cfg->idle_poll_interval_s);
    cJSON_AddBoolToObject(obj, "wifi_power_save", cfg->wifi_power_save);
    cJSON_AddBoolToObject(obj, "deep_sleep_enabled", cfg->deep_sleep_enabled);
    cJSON_AddNumberToObject(obj, "deep_sleep_wake_timer_s", cfg->deep_sleep_wake_timer_s);
    cJSON_AddBoolToObject(obj, "deep_sleep_on_idle", cfg->deep_sleep_on_idle);
    cJSON_AddBoolToObject(obj, "auto_update_check", cfg->auto_update_check);
    cJSON_AddNumberToObject(obj, "update_channel", cfg->update_channel);
    cJSON_AddNumberToObject(obj, "screen_rotation", cfg->screen_rotation);
    cJSON_AddBoolToObject(obj, "allsky_enabled", cfg->allsky_enabled);
    cJSON_AddStringToObject(obj, "allsky_hostname", cfg->allsky_hostname);
    cJSON_AddNumberToObject(obj, "allsky_update_interval_s", cfg->allsky_update_interval_s);
    cJSON_AddNumberToObject(obj, "allsky_dew_offset", (double)cfg->allsky_dew_offset);
    cJSON_AddStringToObject(obj, "allsky_field_config", cfg->allsky_field_config);
    cJSON_AddStringToObject(obj, "allsky_thresholds", cfg->allsky_thresholds);
    cJSON_AddBoolToObject(obj, "spotify_enabled", cfg->spotify_enabled);
    cJSON_AddStringToObject(obj, "spotify_client_id", cfg->spotify_client_id);
    cJSON_AddNumberToObject(obj, "spotify_poll_interval_ms", cfg->spotify_poll_interval_ms);
    cJSON_AddBoolToObject(obj, "spotify_show_progress_bar", cfg->spotify_show_progress_bar);
    cJSON_AddBoolToObject(obj, "spotify_minimal_mode", cfg->spotify_minimal_mode);
    cJSON_AddNumberToObject(obj, "spotify_overlay_timeout_s", cfg->spotify_overlay_timeout_s);

    return obj;
}

// Handler for getting config
esp_err_t config_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    cJSON *root = serialize_config_to_json(cfg);
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* WiFi SSID: read from the WiFi stack (not stored in app_config).
     * Password is never exposed via the API. */
    wifi_config_t sta_cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", (const char *)sta_cfg.sta.ssid);
    } else {
        cJSON_AddStringToObject(root, "ssid", "");
    }

    cJSON_AddBoolToObject(root, "_dirty", app_config_is_dirty());

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

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

/* ---- Shared config parsing helpers ---- */

// Validate all config string field lengths and URL formats.
// Returns true if valid; sends 400 and returns false if invalid.
static bool validate_config_fields(cJSON *root, httpd_req_t *req)
{
    if (!validate_string_len(root, "hostname", 32) ||
        !validate_string_len(root, "url1", 128) ||
        !validate_string_len(root, "url2", 128) ||
        !validate_string_len(root, "url3", 128) ||
        !validate_string_len(root, "ntp", 64) ||
        !validate_string_len(root, "timezone", 64) ||
        !validate_string_len(root, "mqtt_broker_url", 128) ||
        !validate_string_len(root, "mqtt_username", 64) ||
        !validate_string_len(root, "mqtt_password", 64) ||
        !validate_string_len(root, "mqtt_topic_prefix", 64) ||
        !validate_string_len(root, "filter_colors_1", 512) ||
        !validate_string_len(root, "filter_colors_2", 512) ||
        !validate_string_len(root, "filter_colors_3", 512) ||
        !validate_string_len(root, "rms_thresholds_1", 256) ||
        !validate_string_len(root, "rms_thresholds_2", 256) ||
        !validate_string_len(root, "rms_thresholds_3", 256) ||
        !validate_string_len(root, "hfr_thresholds_1", 256) ||
        !validate_string_len(root, "hfr_thresholds_2", 256) ||
        !validate_string_len(root, "hfr_thresholds_3", 256) ||
        !validate_string_len(root, "allsky_hostname", 128) ||
        !validate_string_len(root, "allsky_field_config", 1536) ||
        !validate_string_len(root, "allsky_thresholds", 1024)) {
        send_400(req, "String field exceeds maximum length");
        return false;
    }

    cJSON *url_items[] = {
        cJSON_GetObjectItem(root, "url1"),
        cJSON_GetObjectItem(root, "url2"),
        cJSON_GetObjectItem(root, "url3"),
        cJSON_GetObjectItem(root, "mqtt_broker_url"),
    };
    for (int i = 0; i < 4; i++) {
        if (cJSON_IsString(url_items[i]) && !validate_url_format(url_items[i]->valuestring)) {
            send_400(req, "Invalid URL format");
            return false;
        }
    }
    return true;
}

// Parse JSON into a new app_config_t (heap-allocated).
// Starts from a copy of the current config so missing fields are preserved.
// Returns NULL on allocation failure.
static app_config_t *parse_config_from_json(cJSON *root)
{
    app_config_t *cfg = malloc(sizeof(app_config_t));
    if (!cfg) {
        ESP_LOGE(TAG, "parse_config_from_json: malloc failed");
        return NULL;
    }
    memcpy(cfg, app_config_get(), sizeof(app_config_t));

    JSON_TO_STRING(root, "hostname",       cfg->hostname);
    JSON_TO_STRING(root, "url1",           cfg->api_url[0]);
    JSON_TO_STRING(root, "url2",           cfg->api_url[1]);
    JSON_TO_STRING(root, "url3",           cfg->api_url[2]);
    JSON_TO_STRING(root, "ntp",            cfg->ntp_server);
    JSON_TO_STRING(root, "timezone",       cfg->tz_string);
    JSON_TO_INT   (root, "theme_index",    cfg->theme_index);
    cJSON *ws_item = cJSON_GetObjectItem(root, "widget_style");
    if (cJSON_IsNumber(ws_item)) {
        int v = ws_item->valueint;
        if (v < 0) v = 0;
        if (v >= WIDGET_STYLE_COUNT) v = WIDGET_STYLE_COUNT - 1;
        cfg->widget_style = (uint8_t)v;
    }
    JSON_TO_STRING(root, "filter_colors_1", cfg->filter_colors[0]);
    JSON_TO_STRING(root, "filter_colors_2", cfg->filter_colors[1]);
    JSON_TO_STRING(root, "filter_colors_3", cfg->filter_colors[2]);
    JSON_TO_STRING(root, "rms_thresholds_1", cfg->rms_thresholds[0]);
    JSON_TO_STRING(root, "rms_thresholds_2", cfg->rms_thresholds[1]);
    JSON_TO_STRING(root, "rms_thresholds_3", cfg->rms_thresholds[2]);
    JSON_TO_STRING(root, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
    JSON_TO_STRING(root, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
    JSON_TO_STRING(root, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
    JSON_TO_BOOL  (root, "mqtt_enabled",   cfg->mqtt_enabled);
    JSON_TO_STRING(root, "mqtt_broker_url", cfg->mqtt_broker_url);
    JSON_TO_STRING(root, "mqtt_username",  cfg->mqtt_username);
    JSON_TO_STRING(root, "mqtt_password",  cfg->mqtt_password);
    JSON_TO_STRING(root, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);

    // Clamped int fields
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        int val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->brightness = val;
    }

    cJSON *color_brightness = cJSON_GetObjectItem(root, "color_brightness");
    if (cJSON_IsNumber(color_brightness)) {
        int val = color_brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->color_brightness = val;
    }

    cJSON *mqtt_port = cJSON_GetObjectItem(root, "mqtt_port");
    if (cJSON_IsNumber(mqtt_port)) {
        cfg->mqtt_port = (uint16_t)mqtt_port->valueint;
    }

    JSON_TO_BOOL(root, "auto_rotate_enabled", cfg->auto_rotate_enabled);
    JSON_TO_BOOL(root, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);

    cJSON *apo_item = cJSON_GetObjectItem(root, "active_page_override");
    if (cJSON_IsNumber(apo_item)) {
        int v = apo_item->valueint;
        if (v < -1) v = -1;
        if (v > MAX_NINA_INSTANCES + 3) v = MAX_NINA_INSTANCES + 3;
        cfg->active_page_override = (int8_t)v;
    }

    cJSON *ari_item = cJSON_GetObjectItem(root, "auto_rotate_interval_s");
    if (cJSON_IsNumber(ari_item)) {
        int v = ari_item->valueint;
        if (v < 0) v = 0;
        if (v > 3600) v = 3600;
        cfg->auto_rotate_interval_s = (uint16_t)v;
    }

    cJSON *are_item = cJSON_GetObjectItem(root, "auto_rotate_effect");
    if (cJSON_IsNumber(are_item)) {
        int v = are_item->valueint;
        if (v < 0) v = 0;
        if (v > 3) v = 3;
        cfg->auto_rotate_effect = (uint8_t)v;
    }

    cJSON *arp_item = cJSON_GetObjectItem(root, "auto_rotate_pages");
    if (cJSON_IsNumber(arp_item)) {
        int v = arp_item->valueint;
        if (v < 0) v = 0;
        if (v > 0x3F) v = 0x3F;
        cfg->auto_rotate_pages = (uint8_t)v;
    }

    cJSON *ur_item = cJSON_GetObjectItem(root, "update_rate_s");
    if (cJSON_IsNumber(ur_item)) {
        int v = ur_item->valueint;
        if (v < 1) v = 1;
        if (v > 10) v = 10;
        cfg->update_rate_s = (uint8_t)v;
    }

    cJSON *gui_item = cJSON_GetObjectItem(root, "graph_update_interval_s");
    if (cJSON_IsNumber(gui_item)) {
        int v = gui_item->valueint;
        if (v < 2) v = 2;
        if (v > 30) v = 30;
        cfg->graph_update_interval_s = (uint8_t)v;
    }

    cJSON *ct_item = cJSON_GetObjectItem(root, "connection_timeout_s");
    if (cJSON_IsNumber(ct_item)) {
        int v = ct_item->valueint;
        if (v < 2) v = 2;
        if (v > 30) v = 30;
        cfg->connection_timeout_s = (uint8_t)v;
    }

    cJSON *td_item = cJSON_GetObjectItem(root, "toast_duration_s");
    if (cJSON_IsNumber(td_item)) {
        int v = td_item->valueint;
        if (v < 3) v = 3;
        if (v > 30) v = 30;
        cfg->toast_duration_s = (uint8_t)v;
    }

    JSON_TO_BOOL(root, "debug_mode", cfg->debug_mode);
    JSON_TO_BOOL(root, "demo_mode", cfg->demo_mode);
    JSON_TO_BOOL(root, "instance_enabled_1", cfg->instance_enabled[0]);
    JSON_TO_BOOL(root, "instance_enabled_2", cfg->instance_enabled[1]);
    JSON_TO_BOOL(root, "instance_enabled_3", cfg->instance_enabled[2]);

    JSON_TO_BOOL(root, "screen_sleep_enabled", cfg->screen_sleep_enabled);
    JSON_TO_BOOL(root, "alert_flash_enabled", cfg->alert_flash_enabled);
    cJSON *sst_item = cJSON_GetObjectItem(root, "screen_sleep_timeout_s");
    if (cJSON_IsNumber(sst_item)) {
        int v = sst_item->valueint;
        if (v < 10) v = 10;
        if (v > 3600) v = 3600;
        cfg->screen_sleep_timeout_s = (uint16_t)v;
    }

    cJSON *ipi_item = cJSON_GetObjectItem(root, "idle_poll_interval_s");
    if (cJSON_IsNumber(ipi_item)) {
        int v = ipi_item->valueint;
        if (v < 5) v = 5;
        if (v > 120) v = 120;
        cfg->idle_poll_interval_s = (uint8_t)v;
    }

    JSON_TO_BOOL(root, "wifi_power_save", cfg->wifi_power_save);

    JSON_TO_BOOL(root, "deep_sleep_enabled", cfg->deep_sleep_enabled);
    JSON_TO_BOOL(root, "deep_sleep_on_idle", cfg->deep_sleep_on_idle);

    cJSON *dswt_item = cJSON_GetObjectItem(root, "deep_sleep_wake_timer_s");
    if (cJSON_IsNumber(dswt_item)) {
        int v = dswt_item->valueint;
        if (v < 0) v = 0;
        if (v > 259200) v = 259200;  // max 72 hours in seconds
        cfg->deep_sleep_wake_timer_s = (uint32_t)v;
    }

    cJSON *auto_update = cJSON_GetObjectItem(root, "auto_update_check");
    if (cJSON_IsBool(auto_update)) {
        cfg->auto_update_check = cJSON_IsTrue(auto_update) ? 1 : 0;
    }
    cJSON *update_ch = cJSON_GetObjectItem(root, "update_channel");
    if (cJSON_IsNumber(update_ch)) {
        int v = update_ch->valueint;
        cfg->update_channel = (v == 1) ? 1 : 0;
    }

    cJSON *rot_item = cJSON_GetObjectItem(root, "screen_rotation");
    if (cJSON_IsNumber(rot_item)) {
        int v = rot_item->valueint;
        if (v < 0) v = 0;
        if (v > 3) v = 3;
        cfg->screen_rotation = (uint8_t)v;
    }

    JSON_TO_BOOL  (root, "allsky_enabled",      cfg->allsky_enabled);
    JSON_TO_STRING(root, "allsky_hostname",      cfg->allsky_hostname);
    JSON_TO_STRING(root, "allsky_field_config",  cfg->allsky_field_config);
    JSON_TO_STRING(root, "allsky_thresholds",    cfg->allsky_thresholds);

    cJSON *as_interval = cJSON_GetObjectItem(root, "allsky_update_interval_s");
    if (cJSON_IsNumber(as_interval)) {
        int v = as_interval->valueint;
        if (v < 1) v = 1;
        if (v > 300) v = 300;
        cfg->allsky_update_interval_s = (uint16_t)v;
    }

    cJSON *as_dew = cJSON_GetObjectItem(root, "allsky_dew_offset");
    if (cJSON_IsNumber(as_dew)) {
        float v = (float)as_dew->valuedouble;
        if (v < -50.0f) v = -50.0f;
        if (v > 50.0f) v = 50.0f;
        cfg->allsky_dew_offset = v;
    }

    JSON_TO_BOOL  (root, "spotify_enabled",           cfg->spotify_enabled);
    JSON_TO_STRING(root, "spotify_client_id",          cfg->spotify_client_id);
    JSON_TO_INT   (root, "spotify_poll_interval_ms",   cfg->spotify_poll_interval_ms);
    JSON_TO_BOOL  (root, "spotify_show_progress_bar",  cfg->spotify_show_progress_bar);
    JSON_TO_BOOL  (root, "spotify_minimal_mode",       cfg->spotify_minimal_mode);
    JSON_TO_INT   (root, "spotify_overlay_timeout_s",  cfg->spotify_overlay_timeout_s);

    return cfg;
}

// Receive and parse JSON body from a POST request.
// Returns parsed cJSON root on success, NULL on failure (error response already sent).
static cJSON *receive_json_body(httpd_req_t *req, int max_size)
{
    int remaining = req->content_len;
    if (remaining >= max_size) {
        send_400(req, "Payload too large");
        return NULL;
    }

    char *buf = heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Config handler: malloc failed for payload buffer");
        httpd_resp_send_500(req);
        return NULL;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, buf + received, remaining - received);
        if (ret <= 0) {
            free(buf);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Config handler: recv timeout (got %d/%d bytes)", received, remaining);
                httpd_resp_send_408(req);
            } else {
                ESP_LOGW(TAG, "Config handler: recv error %d (got %d/%d bytes)", ret, received, remaining);
            }
            return NULL;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        send_400(req, "Invalid JSON");
        return NULL;
    }
    return root;
}

// Handler for saving config (persists to NVS)
esp_err_t config_post_handler(httpd_req_t *req)
{
    cJSON *root = receive_json_body(req, CONFIG_MAX_PAYLOAD);
    if (!root) return ESP_OK;

    if (!validate_config_fields(root, req)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    // WiFi credentials: pass directly to ESP-IDF WiFi NVS
    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(ssid_item) && ssid_item->valuestring[0] != '\0') {
        if (strlen(ssid_item->valuestring) >= 32) {
            cJSON_Delete(root);
            return send_400(req, "SSID too long (max 31 chars)");
        }
        if (cJSON_IsString(pass_item) && strlen(pass_item->valuestring) >= 64) {
            cJSON_Delete(root);
            return send_400(req, "WiFi password too long (max 63 chars)");
        }

        wifi_config_t sta_cfg = {0};
        esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        strncpy((char *)sta_cfg.sta.ssid, ssid_item->valuestring,
                sizeof(sta_cfg.sta.ssid) - 1);
        if (cJSON_IsString(pass_item) && pass_item->valuestring[0] != '\0') {
            memset(sta_cfg.sta.password, 0, sizeof(sta_cfg.sta.password));
            strncpy((char *)sta_cfg.sta.password, pass_item->valuestring,
                    sizeof(sta_cfg.sta.password) - 1);
        }
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        ESP_LOGI(TAG, "WiFi STA credentials updated via esp_wifi_set_config");
    }

    app_config_t *cfg = parse_config_from_json(root);
    cJSON_Delete(root);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    app_config_t *old_cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!old_cfg) {
        ESP_LOGE(TAG, "config_post: malloc failed for old_cfg");
        free(cfg);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    memcpy(old_cfg, app_config_get(), sizeof(app_config_t));
    app_config_save(cfg);
    config_trigger_side_effects(old_cfg, cfg);
    free(old_cfg);
    free(cfg);

    ESP_LOGI(TAG, "Config saved to NVS");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live-applying config (in-memory only, no NVS)
esp_err_t config_apply_handler(httpd_req_t *req)
{
    cJSON *root = receive_json_body(req, CONFIG_MAX_PAYLOAD);
    if (!root) return ESP_OK;

    if (!validate_config_fields(root, req)) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    app_config_t *cfg = parse_config_from_json(root);
    cJSON_Delete(root);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    app_config_t *old_cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!old_cfg) {
        ESP_LOGE(TAG, "config_apply: malloc failed for old_cfg");
        free(cfg);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    memcpy(old_cfg, app_config_get(), sizeof(app_config_t));
    app_config_apply(cfg);
    config_trigger_side_effects(old_cfg, cfg);
    free(old_cfg);
    free(cfg);

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for reverting config to NVS-saved state
esp_err_t config_revert_handler(httpd_req_t *req)
{
    app_config_t *old_cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!old_cfg) {
        ESP_LOGE(TAG, "config_revert: malloc failed for old_cfg");
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    memcpy(old_cfg, app_config_get(), sizeof(app_config_t));

    esp_err_t err = app_config_revert();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config revert failed");
        free(old_cfg);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    config_trigger_side_effects(old_cfg, app_config_get());
    free(old_cfg);

    ESP_LOGI(TAG, "Config reverted from NVS");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

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
    cJSON *full_config = serialize_config_to_json(cfg);
    if (!full_config) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

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
             "attachment; filename=\"%s_%s_v%d_%s%s.json\"",
             safe_host, mac_file, APP_CONFIG_VERSION, date_short,
             include_sensitive ? "_secrets" : "");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", filename);
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free((void *)json_str);
    return ESP_OK;
}

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
        cJSON *current_json = serialize_config_to_json(app_config_get());
        if (!current_json) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_OK;
        }

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
