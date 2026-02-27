#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "perf_monitor.h"

static const char *TAG = "app_config";
static app_config_t s_config;
static SemaphoreHandle_t s_config_mutex;
static const char *NVS_NAMESPACE = "app_conf";

// ── Cached parsed JSON trees for hot-path lookups ──
// Invalidated on config save. NULL = needs re-parse on next access.
static cJSON *s_filter_colors_cache[MAX_NINA_INSTANCES] = {NULL};
static cJSON *s_rms_thresholds_cache[MAX_NINA_INSTANCES] = {NULL};
static cJSON *s_hfr_thresholds_cache[MAX_NINA_INSTANCES] = {NULL};

/**
 * @brief Invalidate all cached JSON parse trees.
 * Call after any config mutation (save, sync, factory reset).
 */
static void invalidate_json_caches(void) {
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (s_filter_colors_cache[i]) {
            cJSON_Delete(s_filter_colors_cache[i]);
            s_filter_colors_cache[i] = NULL;
        }
        if (s_rms_thresholds_cache[i]) {
            cJSON_Delete(s_rms_thresholds_cache[i]);
            s_rms_thresholds_cache[i] = NULL;
        }
        if (s_hfr_thresholds_cache[i]) {
            cJSON_Delete(s_hfr_thresholds_cache[i]);
            s_hfr_thresholds_cache[i] = NULL;
        }
    }
}

// Default RMS thresholds: good <= 0.5", ok <= 1.0", bad > 1.0" - DIMMED for Night Vision
static const char *DEFAULT_RMS_THRESHOLDS =
    "{\"good_max\":0.5,\"ok_max\":1.0,"
    "\"good_color\":\"#15803d\",\"ok_color\":\"#ca8a04\",\"bad_color\":\"#b91c1c\"}";

// Default HFR thresholds: good <= 2.0, ok <= 3.5, bad > 3.5 - DIMMED for Night Vision
static const char *DEFAULT_HFR_THRESHOLDS =
    "{\"good_max\":2.0,\"ok_max\":3.5,"
    "\"good_color\":\"#15803d\",\"ok_color\":\"#ca8a04\",\"bad_color\":\"#b91c1c\"}";

// Default filter colors for common astrophotography filters
static const struct {
    const char *name;
    uint32_t    color;
} DEFAULT_FILTER_COLORS[] = {
    { "L",    0xFFFFFF },
    { "R",    0xB91C1C },
    { "G",    0x15803D },
    { "B",    0x1D4ED8 },
    { "Sii",  0xFF00FF },
    { "Ha",   0xCCFF00 },
    { "Oiii", 0x00FFFF },
};

static uint32_t get_default_filter_color(const char *name) {
    for (int i = 0; i < (int)(sizeof(DEFAULT_FILTER_COLORS) / sizeof(DEFAULT_FILTER_COLORS[0])); i++) {
        if (strcasecmp(name, DEFAULT_FILTER_COLORS[i].name) == 0) {
            return DEFAULT_FILTER_COLORS[i].color;
        }
    }
    return 0xFFFFFF;  // White for unknown filters
}

static void get_default_filter_color_hex(const char *name, char *out, size_t out_size) {
    uint32_t color = get_default_filter_color(name);
    snprintf(out, out_size, "#%06x", (unsigned int)color);
}

/* ── Legacy config struct (version 0) — used only for NVS migration ────── */
typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char api_url[3][128];
    char ntp_server[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
} app_config_v0_t;

/* ── Version 1 config struct — used only for NVS migration to v2 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
} app_config_v1_t;

/* ── Version 2 config struct — used only for NVS migration to v3 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
} app_config_v2_t;

/* ── Version 3 config struct — used only for NVS migration to v4 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
} app_config_v3_t;

/* ── Version 4 config struct — used only for NVS migration to v5 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
} app_config_v4_t;

/* ── Version 5 config struct — used only for NVS migration to v6 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
} app_config_v5_t;

/* ── Version 6 config struct — used only for NVS migration to v7 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
} app_config_v6_t;

/* ── Version 7 config struct — used only for NVS migration to v8 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
} app_config_v7_t;

/* ── Version 8 config struct — used only for NVS migration to v9 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
} app_config_v8_t;

/* ── Version 9 config struct — used only for NVS migration to v10 ────── */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
} app_config_v9_t;

static void set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(app_config_t));
    cfg->config_version = APP_CONFIG_VERSION;
    strcpy(cfg->api_url[0], "http://astromele2.lan:1888/v2/api/");
    strcpy(cfg->api_url[1], "http://astromele3.lan:1888/v2/api/");
    strcpy(cfg->ntp_server, "pool.ntp.org");
    cfg->tz_string[0] = '\0';  // Default: UTC (no offset)
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        strcpy(cfg->filter_colors[i], "{}");
        strcpy(cfg->rms_thresholds[i], DEFAULT_RMS_THRESHOLDS);
        strcpy(cfg->hfr_thresholds[i], DEFAULT_HFR_THRESHOLDS);
    }
    cfg->brightness = 50;
    cfg->color_brightness = 100;
    cfg->active_page_override = -1;
    cfg->auto_rotate_enabled = false;
    cfg->auto_rotate_interval_s = 30;
    cfg->auto_rotate_effect = 0;
    cfg->auto_rotate_skip_disconnected = true;
    cfg->auto_rotate_pages = 0x0E;  // Default: all NINA instances (bits 1-3)
    cfg->update_rate_s = 2;
    cfg->graph_update_interval_s = 5;
    cfg->connection_timeout_s = 6;
    cfg->toast_duration_s = 8;
    cfg->debug_mode = false;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        cfg->instance_enabled[i] = true;
    }
    cfg->screen_sleep_enabled = false;
    cfg->screen_sleep_timeout_s = 60;
    cfg->alert_flash_enabled = true;
    strcpy(cfg->mqtt_broker_url, "mqtt://192.168.1.250");
    strcpy(cfg->mqtt_topic_prefix, "ninadisplay");
    cfg->mqtt_port = 1883;
}

/**
 * @brief Migrate a v0 (legacy) config blob into the current struct layout.
 *
 * WiFi credentials from the old config are intentionally dropped — ESP-IDF's
 * WiFi NVS subsystem already persists them from prior esp_wifi_set_config()
 * calls, so they will be auto-restored on boot.
 */
static void migrate_from_v0(const app_config_v0_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_interval_s > 0;  // infer from old interval
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s > 0 ? old->auto_rotate_interval_s : 30;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;

    ESP_LOGI(TAG, "Migrated config from v0 → v%d (WiFi credentials now managed by ESP-IDF WiFi NVS)",
             APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v1 config blob into the current struct layout.
 *
 * v1 → v2 adds: auto_rotate_pages bitmask, auto_rotate_effect range 0-3.
 */
static void migrate_from_v1(const app_config_v1_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_interval_s > 0;  // infer from old interval
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s > 0 ? old->auto_rotate_interval_s : 30;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    /* auto_rotate_pages keeps default 0x0E from set_defaults() */

    ESP_LOGI(TAG, "Migrated config from v1 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v2 config blob into the current struct layout.
 *
 * v2 → v3 adds: update_rate_s (UI/data update interval).
 */
static void migrate_from_v2(const app_config_v2_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    /* update_rate_s keeps default 2 from set_defaults() */

    ESP_LOGI(TAG, "Migrated config from v2 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v3 config blob into the current struct layout.
 *
 * v3 → v4 adds: graph_update_interval_s (graph overlay auto-refresh interval).
 */
static void migrate_from_v3(const app_config_v3_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    cfg->update_rate_s = old->update_rate_s;
    /* graph_update_interval_s keeps default 5 from set_defaults() */

    ESP_LOGI(TAG, "Migrated config from v3 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v4 config blob into the current struct layout.
 *
 * v4 → v5 adds: connection_timeout_s (seconds before marking instance offline).
 */
static void migrate_from_v4(const app_config_v4_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    cfg->update_rate_s = old->update_rate_s;
    cfg->graph_update_interval_s = old->graph_update_interval_s;
    /* connection_timeout_s keeps default 6 from set_defaults() */

    ESP_LOGI(TAG, "Migrated config from v4 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v5 config blob into the current struct layout.
 *
 * v5 → v6 adds: toast_duration_s (toast notification display duration).
 */
static void migrate_from_v5(const app_config_v5_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    cfg->update_rate_s = old->update_rate_s;
    cfg->graph_update_interval_s = old->graph_update_interval_s;
    cfg->connection_timeout_s = old->connection_timeout_s;
    /* toast_duration_s keeps default 8 from set_defaults() */

    ESP_LOGI(TAG, "Migrated config from v5 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v6 config blob into the current struct layout.
 *
 * v6 → v7 adds: debug_mode (runtime debug/perf profiling toggle).
 */
static void migrate_from_v6(const app_config_v6_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    cfg->update_rate_s = old->update_rate_s;
    cfg->graph_update_interval_s = old->graph_update_interval_s;
    cfg->connection_timeout_s = old->connection_timeout_s;
    cfg->toast_duration_s = old->toast_duration_s;
    /* debug_mode keeps default false from set_defaults() */

    ESP_LOGI(TAG, "Migrated config from v6 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v7 config blob into the current struct layout.
 *
 * v7 → v8 adds: instance_enabled[3] (per-instance enable/disable toggle).
 */
static void migrate_from_v7(const app_config_v7_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    cfg->update_rate_s = old->update_rate_s;
    cfg->graph_update_interval_s = old->graph_update_interval_s;
    cfg->connection_timeout_s = old->connection_timeout_s;
    cfg->toast_duration_s = old->toast_duration_s;
    cfg->debug_mode = old->debug_mode;
    /* instance_enabled: default to enabled for instances with a configured URL */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        cfg->instance_enabled[i] = (old->api_url[i][0] != '\0');
    }

    ESP_LOGI(TAG, "Migrated config from v7 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v8 config blob into the current struct layout.
 *
 * v8 → v9 adds: screen_sleep_enabled, screen_sleep_timeout_s.
 */
static void migrate_from_v8(const app_config_v8_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    cfg->update_rate_s = old->update_rate_s;
    cfg->graph_update_interval_s = old->graph_update_interval_s;
    cfg->connection_timeout_s = old->connection_timeout_s;
    cfg->toast_duration_s = old->toast_duration_s;
    cfg->debug_mode = old->debug_mode;
    memcpy(cfg->instance_enabled, old->instance_enabled, sizeof(cfg->instance_enabled));
    /* screen_sleep: defaults from set_defaults() (disabled, 60s timeout) */

    ESP_LOGI(TAG, "Migrated config from v8 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v9 config blob into the current struct layout.
 *
 * v9 → v10 adds: alert_flash_enabled.
 */
static void migrate_from_v9(const app_config_v9_t *old, app_config_t *cfg) {
    set_defaults(cfg);

    memcpy(cfg->api_url, old->api_url, sizeof(cfg->api_url));
    memcpy(cfg->ntp_server, old->ntp_server, sizeof(cfg->ntp_server));
    memcpy(cfg->tz_string, old->tz_string, sizeof(cfg->tz_string));
    memcpy(cfg->filter_colors, old->filter_colors, sizeof(cfg->filter_colors));
    memcpy(cfg->rms_thresholds, old->rms_thresholds, sizeof(cfg->rms_thresholds));
    memcpy(cfg->hfr_thresholds, old->hfr_thresholds, sizeof(cfg->hfr_thresholds));
    cfg->theme_index = old->theme_index;
    cfg->brightness = old->brightness;
    cfg->color_brightness = old->color_brightness;
    cfg->mqtt_enabled = old->mqtt_enabled;
    memcpy(cfg->mqtt_broker_url, old->mqtt_broker_url, sizeof(cfg->mqtt_broker_url));
    memcpy(cfg->mqtt_username, old->mqtt_username, sizeof(cfg->mqtt_username));
    memcpy(cfg->mqtt_password, old->mqtt_password, sizeof(cfg->mqtt_password));
    memcpy(cfg->mqtt_topic_prefix, old->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix));
    cfg->mqtt_port = old->mqtt_port;
    cfg->active_page_override = old->active_page_override;
    cfg->auto_rotate_enabled = old->auto_rotate_enabled;
    cfg->auto_rotate_interval_s = old->auto_rotate_interval_s;
    cfg->auto_rotate_effect = old->auto_rotate_effect;
    cfg->auto_rotate_skip_disconnected = old->auto_rotate_skip_disconnected;
    cfg->auto_rotate_pages = old->auto_rotate_pages;
    cfg->update_rate_s = old->update_rate_s;
    cfg->graph_update_interval_s = old->graph_update_interval_s;
    cfg->connection_timeout_s = old->connection_timeout_s;
    cfg->toast_duration_s = old->toast_duration_s;
    cfg->debug_mode = old->debug_mode;
    memcpy(cfg->instance_enabled, old->instance_enabled, sizeof(cfg->instance_enabled));
    cfg->screen_sleep_enabled = old->screen_sleep_enabled;
    cfg->screen_sleep_timeout_s = old->screen_sleep_timeout_s;
    /* alert_flash_enabled: defaults from set_defaults() (enabled) */

    ESP_LOGI(TAG, "Migrated config from v9 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Validate and clamp config fields to sane ranges.
 * @return true if any field was corrected.
 */
static bool validate_config(app_config_t *cfg) {
    bool fixed = false;

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (cfg->rms_thresholds[i][0] == '\0') {
            strcpy(cfg->rms_thresholds[i], DEFAULT_RMS_THRESHOLDS);
            fixed = true;
        }
        if (cfg->hfr_thresholds[i][0] == '\0') {
            strcpy(cfg->hfr_thresholds[i], DEFAULT_HFR_THRESHOLDS);
            fixed = true;
        }
    }
    if (cfg->color_brightness < 0 || cfg->color_brightness > 100) {
        cfg->color_brightness = 100;
        fixed = true;
    }
    if (cfg->theme_index < 0 || cfg->theme_index > 20) {
        cfg->theme_index = 0;
        fixed = true;
    }
    if (cfg->brightness < 0 || cfg->brightness > 100) {
        cfg->brightness = 50;
        fixed = true;
    }
    if (cfg->mqtt_topic_prefix[0] == '\0') {
        strcpy(cfg->mqtt_topic_prefix, "ninadisplay");
        fixed = true;
    }
    if (cfg->mqtt_port == 0) {
        cfg->mqtt_port = 1883;
        fixed = true;
    }
    if (cfg->active_page_override < -1 || cfg->active_page_override > MAX_NINA_INSTANCES + 2) {
        cfg->active_page_override = -1;
        fixed = true;
    }
    if (cfg->auto_rotate_interval_s == 0 || cfg->auto_rotate_interval_s > 3600) {
        cfg->auto_rotate_interval_s = 30;
        fixed = true;
    }
    if (cfg->auto_rotate_effect > 3) {
        cfg->auto_rotate_effect = 0;
        fixed = true;
    }
    if (cfg->auto_rotate_pages == 0) {
        cfg->auto_rotate_pages = 0x0E;  // Default: all NINA instances
        fixed = true;
    }
    if (cfg->update_rate_s < 1 || cfg->update_rate_s > 10) {
        cfg->update_rate_s = 2;
        fixed = true;
    }
    if (cfg->graph_update_interval_s < 2 || cfg->graph_update_interval_s > 30) {
        cfg->graph_update_interval_s = 5;
        fixed = true;
    }
    if (cfg->connection_timeout_s < 2 || cfg->connection_timeout_s > 30) {
        cfg->connection_timeout_s = 6;
        fixed = true;
    }
    if (cfg->toast_duration_s < 3 || cfg->toast_duration_s > 30) {
        cfg->toast_duration_s = 8;
        fixed = true;
    }
    if (cfg->screen_sleep_timeout_s < 10 || cfg->screen_sleep_timeout_s > 3600) {
        cfg->screen_sleep_timeout_s = 60;
        fixed = true;
    }

    return fixed;
}

void app_config_init(void) {
    s_config_mutex = xSemaphoreCreateMutex();
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle! Using defaults.", esp_err_to_name(err));
        set_defaults(&s_config);
        return;
    }

    /* Determine stored blob size without reading */
    size_t stored_size = 0;
    err = nvs_get_blob(handle, "config", NULL, &stored_size);
    if (err != ESP_OK || stored_size == 0) {
        ESP_LOGW(TAG, "Config not found in NVS, using defaults");
        set_defaults(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
        nvs_close(handle);
        return;
    }

    /* Read raw blob into a temporary buffer for version detection */
    void *raw = heap_caps_malloc(stored_size, MALLOC_CAP_SPIRAM);
    if (!raw) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for config read", (int)stored_size);
        set_defaults(&s_config);
        nvs_close(handle);
        return;
    }
    nvs_get_blob(handle, "config", raw, &stored_size);

    /*
     * Version detection strategy:
     *   - Current format: first uint32_t == APP_CONFIG_VERSION and size matches.
     *   - Legacy v0: first bytes are the wifi_ssid field (ASCII text), so the
     *     first uint32_t will be a large value (printable ASCII) or zero (empty SSID).
     *     In either case it won't equal APP_CONFIG_VERSION (1).
     */
    uint32_t version_check = 0;
    memcpy(&version_check, raw, sizeof(uint32_t));

    if (version_check == APP_CONFIG_VERSION && stored_size == sizeof(app_config_t)) {
        /* Current version — load directly */
        memcpy(&s_config, raw, sizeof(app_config_t));
        ESP_LOGI(TAG, "Config v%d loaded (%d bytes)", APP_CONFIG_VERSION, (int)stored_size);

        if (validate_config(&s_config)) {
            nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
            nvs_commit(handle);
        }
    } else if (version_check == 9 && stored_size >= sizeof(app_config_v9_t)) {
        /* Version 9 blob — migrate to v10 */
        app_config_v9_t old;
        memcpy(&old, raw, sizeof(app_config_v9_t));
        migrate_from_v9(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 8 && stored_size >= sizeof(app_config_v8_t)) {
        /* Version 8 blob — migrate to v9 */
        app_config_v8_t old;
        memcpy(&old, raw, sizeof(app_config_v8_t));
        migrate_from_v8(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 7 && stored_size >= sizeof(app_config_v7_t)) {
        /* Version 7 blob — migrate to v8 */
        app_config_v7_t old;
        memcpy(&old, raw, sizeof(app_config_v7_t));
        migrate_from_v7(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 6 && stored_size >= sizeof(app_config_v6_t)) {
        /* Version 6 blob — migrate to v7 */
        app_config_v6_t old;
        memcpy(&old, raw, sizeof(app_config_v6_t));
        migrate_from_v6(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 5 && stored_size >= sizeof(app_config_v5_t)) {
        /* Version 5 blob — migrate to v6 */
        app_config_v5_t old;
        memcpy(&old, raw, sizeof(app_config_v5_t));
        migrate_from_v5(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 4 && stored_size >= sizeof(app_config_v4_t)) {
        /* Version 4 blob — migrate to v5 */
        app_config_v4_t old;
        memcpy(&old, raw, sizeof(app_config_v4_t));
        migrate_from_v4(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 3 && stored_size >= sizeof(app_config_v3_t)) {
        /* Version 3 blob — migrate to v4 */
        app_config_v3_t old;
        memcpy(&old, raw, sizeof(app_config_v3_t));
        migrate_from_v3(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 2 && stored_size >= sizeof(app_config_v2_t)) {
        /* Version 2 blob — migrate to v3 */
        app_config_v2_t old;
        memcpy(&old, raw, sizeof(app_config_v2_t));
        migrate_from_v2(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 1 && stored_size >= sizeof(app_config_v1_t)) {
        /* Version 1 blob — migrate to v2 */
        app_config_v1_t old;
        memcpy(&old, raw, sizeof(app_config_v1_t));
        migrate_from_v1(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (stored_size >= sizeof(app_config_v0_t)) {
        /* Likely legacy v0 blob — attempt migration */
        app_config_v0_t old;
        memcpy(&old, raw, sizeof(app_config_v0_t));
        migrate_from_v0(&old, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else {
        ESP_LOGW(TAG, "Unknown config blob (size=%d, ver=0x%08x), using defaults",
                 (int)stored_size, (unsigned)version_check);
        set_defaults(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    }

    free(raw);
    nvs_close(handle);
}

app_config_t *app_config_get(void) {
    return &s_config;
}

void app_config_save(const app_config_t *config) {
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    invalidate_json_caches();

    memcpy(&s_config, config, sizeof(app_config_t));
    s_config.config_version = APP_CONFIG_VERSION;  // Always stamp current version

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for saving: %s", esp_err_to_name(err));
        xSemaphoreGive(s_config_mutex);
        return;
    }

    err = nvs_set_blob(my_handle, "config", &s_config, sizeof(app_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Config saved");
        nvs_commit(my_handle);
    }
    nvs_close(my_handle);

    xSemaphoreGive(s_config_mutex);
}

app_config_t app_config_get_snapshot(void) {
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    app_config_t copy = s_config;
    xSemaphoreGive(s_config_mutex);
    return copy;
}

void app_config_factory_reset(void) {
    invalidate_json_caches();
    ESP_LOGW(TAG, "Performing factory reset - erasing NVS partition");

    // Erase the entire NVS partition
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
        return;
    }

    // Re-initialize NVS
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-initialize NVS: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Factory reset complete - all settings erased");

    // Reinitialize config with defaults
    app_config_init();
}

/**
 * @brief Parse a hex color string from a cJSON object field
 */
static uint32_t parse_color_field(cJSON *root, const char *field, uint32_t fallback) {
    cJSON *item = cJSON_GetObjectItem(root, field);
    if (item && cJSON_IsString(item) && item->valuestring) {
        const char *hex = item->valuestring;
        if (hex[0] == '#') hex++;
        return (uint32_t)strtol(hex, NULL, 16);
    }
    return fallback;
}

/**
 * @brief Get pointer to the filter_colors field for a given instance index
 */
static char *get_filter_colors_field(int instance_index) {
    if (instance_index < 0 || instance_index >= MAX_NINA_INSTANCES) instance_index = 0;
    return s_config.filter_colors[instance_index];
}

/**
 * @brief Apply brightness scaling to a color
 * @param color 0xRRGGBB color value
 * @param brightness Brightness percentage 0-100
 * @return Adjusted color
 */
uint32_t app_config_apply_brightness(uint32_t color, int brightness) {
    if (brightness >= 100) return color;
    if (brightness <= 0) return 0x000000;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    r = (uint8_t)((r * brightness) / 100);
    g = (uint8_t)((g * brightness) / 100);
    b = (uint8_t)((b * brightness) / 100);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/**
 * @brief Get the color for a specific filter
 * @param filter_name Name of the filter (e.g., "Ha", "L", "R")
 * @return 32-bit color value (0xRRGGBB) or default blue if not found
 */
uint32_t app_config_get_filter_color(const char *filter_name, int instance_index) {
    if (!filter_name || filter_name[0] == '\0' || strcmp(filter_name, "--") == 0) {
        return 0xFFFFFF;  // White for empty/unknown filter
    }

    int idx = instance_index;
    if (idx < 0 || idx >= MAX_NINA_INSTANCES) idx = 0;

    xSemaphoreTake(s_config_mutex, portMAX_DELAY);

    // Use cached parse tree, re-parse only on first access or after invalidation
    if (!s_filter_colors_cache[idx]) {
        const char *json = get_filter_colors_field(idx);
        perf_timer_start(&g_perf.json_config_color_parse);
        s_filter_colors_cache[idx] = cJSON_Parse(json);
        perf_timer_stop(&g_perf.json_config_color_parse);
        perf_counter_increment(&g_perf.json_parse_count);
    }
    cJSON *root = s_filter_colors_cache[idx];
    if (!root) {
        xSemaphoreGive(s_config_mutex);
        return 0xFFFFFF;
    }

    cJSON *color_item = cJSON_GetObjectItem(root, filter_name);
    uint32_t color;

    if (color_item && cJSON_IsString(color_item) && color_item->valuestring) {
        const char *hex = color_item->valuestring;
        if (hex[0] == '#') hex++;
        color = (uint32_t)strtol(hex, NULL, 16);
    } else {
        color = get_default_filter_color(filter_name);
    }

    // DO NOT call cJSON_Delete(root) — it's cached!

    // Apply global color brightness
    int gb = s_config.color_brightness;
    if (gb < 0 || gb > 100) gb = 100;

    xSemaphoreGive(s_config_mutex);

    color = app_config_apply_brightness(color, gb);

    return color;
}

/**
 * @brief Shared helper: parse threshold JSON, compare value, return brightness-adjusted color.
 */
static uint32_t get_threshold_color(float value, const char *json,
                                    cJSON **cache,
                                    float default_good_max, float default_ok_max) {
    // Use cached parse tree, re-parse only on first access or after invalidation
    if (!*cache) {
        perf_timer_start(&g_perf.json_config_color_parse);
        *cache = cJSON_Parse(json);
        perf_timer_stop(&g_perf.json_config_color_parse);
        perf_counter_increment(&g_perf.json_parse_count);
    }
    cJSON *root = *cache;
    if (!root) {
        int gb = s_config.color_brightness;
        if (gb < 0 || gb > 100) gb = 100;
        return app_config_apply_brightness(0xef4444, gb);
    }

    uint32_t good_color = parse_color_field(root, "good_color", 0x10b981);
    uint32_t ok_color   = parse_color_field(root, "ok_color",   0xeab308);
    uint32_t bad_color  = parse_color_field(root, "bad_color",  0xef4444);

    cJSON *gm = cJSON_GetObjectItem(root, "good_max");
    cJSON *om = cJSON_GetObjectItem(root, "ok_max");
    float good_max = (gm && cJSON_IsNumber(gm)) ? (float)gm->valuedouble : default_good_max;
    float ok_max   = (om && cJSON_IsNumber(om)) ? (float)om->valuedouble : default_ok_max;

    uint32_t result = (value <= good_max) ? good_color
                    : (value <= ok_max)   ? ok_color
                    :                       bad_color;

    // DO NOT call cJSON_Delete(root) — it's cached!

    int gb = s_config.color_brightness;
    if (gb < 0 || gb > 100) gb = 100;
    return app_config_apply_brightness(result, gb);
}

/**
 * @brief Get the color for a guiding RMS value based on per-instance configured thresholds.
 */
uint32_t app_config_get_rms_color(float rms_value, int instance_index) {
    if (instance_index < 0 || instance_index >= MAX_NINA_INSTANCES) instance_index = 0;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    uint32_t color = get_threshold_color(rms_value, s_config.rms_thresholds[instance_index],
                                         &s_rms_thresholds_cache[instance_index], 0.5f, 1.0f);
    xSemaphoreGive(s_config_mutex);
    return color;
}

/**
 * @brief Get the color for an HFR value based on per-instance configured thresholds.
 */
uint32_t app_config_get_hfr_color(float hfr_value, int instance_index) {
    if (instance_index < 0 || instance_index >= MAX_NINA_INSTANCES) instance_index = 0;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    uint32_t color = get_threshold_color(hfr_value, s_config.hfr_thresholds[instance_index],
                                         &s_hfr_thresholds_cache[instance_index], 2.0f, 3.5f);
    xSemaphoreGive(s_config_mutex);
    return color;
}

static void fill_threshold_config(const char *json, cJSON **cache,
                                   float default_good_max, float default_ok_max,
                                   threshold_config_t *out) {
    if (!*cache) {
        *cache = cJSON_Parse(json);
    }
    cJSON *root = *cache;
    if (!root) {
        out->good_max = default_good_max;
        out->ok_max = default_ok_max;
        out->good_color = 0x10b981;
        out->ok_color = 0xeab308;
        out->bad_color = 0xef4444;
        return;
    }
    out->good_color = parse_color_field(root, "good_color", 0x10b981);
    out->ok_color = parse_color_field(root, "ok_color", 0xeab308);
    out->bad_color = parse_color_field(root, "bad_color", 0xef4444);
    cJSON *gm = cJSON_GetObjectItem(root, "good_max");
    cJSON *om = cJSON_GetObjectItem(root, "ok_max");
    out->good_max = (gm && cJSON_IsNumber(gm)) ? (float)gm->valuedouble : default_good_max;
    out->ok_max = (om && cJSON_IsNumber(om)) ? (float)om->valuedouble : default_ok_max;
}

void app_config_get_rms_threshold_config(int instance_index, threshold_config_t *out) {
    if (instance_index < 0 || instance_index >= MAX_NINA_INSTANCES) instance_index = 0;
    fill_threshold_config(s_config.rms_thresholds[instance_index],
                          &s_rms_thresholds_cache[instance_index],
                          0.5f, 1.0f, out);
}

void app_config_get_hfr_threshold_config(int instance_index, threshold_config_t *out) {
    if (instance_index < 0 || instance_index >= MAX_NINA_INSTANCES) instance_index = 0;
    fill_threshold_config(s_config.hfr_thresholds[instance_index],
                          &s_hfr_thresholds_cache[instance_index],
                          2.0f, 3.5f, out);
}

/**
 * @brief Check if a filter name exists in the API filter list
 */
static bool filter_in_api_list(const char *name, const char *filter_names[], int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(name, filter_names[i]) == 0) return true;
    }
    return false;
}

/**
 * @brief Sync NVS filter_colors and filter_brightness with the actual filter list from the NINA API.
 *
 * - Adds entries for any API filters missing from the config (default color / brightness)
 * - Removes stale entries for filters no longer in the API
 * - Saves to NVS only if something changed
 */
void app_config_sync_filters(const char *filter_names[], int count, int instance_index) {
    if (count <= 0) return;

    bool needs_save = false;
    char *field = get_filter_colors_field(instance_index);

    // --- Sync filter_colors for this instance ---
    cJSON *colors = cJSON_Parse(field);
    if (!colors) colors = cJSON_CreateObject();

    // Add missing filters with per-filter default colors
    for (int i = 0; i < count; i++) {
        if (!cJSON_GetObjectItem(colors, filter_names[i])) {
            char hex_buf[8];
            get_default_filter_color_hex(filter_names[i], hex_buf, sizeof(hex_buf));
            cJSON_AddStringToObject(colors, filter_names[i], hex_buf);
            ESP_LOGI(TAG, "Filter sync: added color for '%s' (default %s)", filter_names[i], hex_buf);
            needs_save = true;
        }
    }

    // Remove stale filters
    cJSON *child = colors->child;
    while (child) {
        cJSON *next = child->next;
        if (!filter_in_api_list(child->string, filter_names, count)) {
            ESP_LOGI(TAG, "Filter sync: removed stale color for '%s'", child->string);
            cJSON_DeleteItemFromObject(colors, child->string);
            needs_save = true;
        }
        child = next;
    }

    if (needs_save) {
        char *json_str = cJSON_PrintUnformatted(colors);
        if (json_str) {
            if (strlen(json_str) < 512) {
                strcpy(field, json_str);
            } else {
                ESP_LOGW(TAG, "Filter colors JSON too large (%d bytes), not updating", (int)strlen(json_str));
            }
            free(json_str);
        }
    }
    cJSON_Delete(colors);

    if (needs_save) {
        invalidate_json_caches();
        app_config_save(&s_config);
        ESP_LOGI(TAG, "Filter config synced with API and saved to NVS");
    } else {
        ESP_LOGI(TAG, "Filter config already in sync with API");
    }
}

int app_config_get_instance_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (s_config.api_url[i][0] != '\0') count++;
    }
    return count > 0 ? count : 1;  // Always at least 1
}

const char *app_config_get_instance_url(int index) {
    if (index < 0 || index >= MAX_NINA_INSTANCES) return "";
    return s_config.api_url[index];
}

bool app_config_is_instance_enabled(int index) {
    if (index < 0 || index >= MAX_NINA_INSTANCES) return false;
    return s_config.instance_enabled[index];
}

int app_config_get_enabled_instance_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (s_config.api_url[i][0] != '\0' && s_config.instance_enabled[i]) count++;
    }
    return count > 0 ? count : 1;
}
