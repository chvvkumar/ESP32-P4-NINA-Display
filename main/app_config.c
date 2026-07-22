#include "app_config.h"
#include "app_config_forward.h"
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
#include "settings_table.h"
#include "themes.h"
#include "ui/page_registry.h"

static const char *TAG = "app_config";
static app_config_t s_config;
static SemaphoreHandle_t s_config_mutex;
static bool s_config_dirty = false;
static const char *NVS_NAMESPACE = "app_conf";

/* ── Tiles-config caches (v52 config split) ──
 * json_tiles_config / ha_tiles_config were removed from app_config_t and moved
 * to dedicated NVS string keys, each mirrored in a fixed 6144-byte PSRAM buffer
 * allocated once and NEVER freed/realloc'd (see accessor contract in the .h). */
#define TILES_CACHE_BYTES 6144
static char *s_json_tiles_cache = NULL;   /* PSRAM, allocated once, never freed */
static char *s_ha_tiles_cache   = NULL;   /* PSRAM, allocated once, never freed */

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

/* Allocate the two tiles caches once. Alloc-guarded so a factory-reset re-init
 * (which calls app_config_init again) neither leaks nor double-allocs — the
 * buffers persist across the erase; only their contents reset to "". */
static void tiles_caches_alloc(void) {
    if (!s_json_tiles_cache) {
        s_json_tiles_cache = heap_caps_malloc(TILES_CACHE_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!s_ha_tiles_cache) {
        s_ha_tiles_cache = heap_caps_malloc(TILES_CACHE_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (s_json_tiles_cache) {
        s_json_tiles_cache[0] = '\0';
    }
    if (s_ha_tiles_cache) {
        s_ha_tiles_cache[0] = '\0';
    }
    /* On alloc failure the pointer stays NULL; getters return "". */
}

/* Load one NVS string key into its fixed cache buffer; missing key -> "".
 * Handle must be open. Buffer is always NUL-terminated on return. */
static void tiles_cache_load_key(nvs_handle_t h, const char *key, char *cache) {
    if (!cache) {
        return;
    }
    size_t len = TILES_CACHE_BYTES;
    esp_err_t e = nvs_get_str(h, key, cache, &len);
    if (e != ESP_OK) {
        cache[0] = '\0';
    }
    cache[TILES_CACHE_BYTES - 1] = '\0';   /* defensive */
}

const char *app_config_get_json_tiles(void) {
    return s_json_tiles_cache ? s_json_tiles_cache : "";
}

const char *app_config_get_ha_tiles(void) {
    return s_ha_tiles_cache ? s_ha_tiles_cache : "";
}

/* Shared setter: validate length, update cache + NVS key under the config mutex.
 * Read is lock-free by contract; the write is serialized against app_config_save
 * (same mutex) so shared state is never torn between a save and a tiles-set. */
static esp_err_t tiles_set(const char *key, char **cache, const char *s) {
    if (!s) {
        s = "";
    }
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    if (!*cache) {   /* first-ever set before init alloc, or a prior OOM */
        *cache = heap_caps_malloc(TILES_CACHE_BYTES, MALLOC_CAP_SPIRAM);
        if (!*cache) {
            xSemaphoreGive(s_config_mutex);
            return ESP_ERR_NO_MEM;
        }
    }
    strlcpy(*cache, s, TILES_CACHE_BYTES);   /* clamps to 6143 + NUL, always terminated */

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, key, *cache);
        if (err == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_config_mutex);
    return err;   /* cache updated regardless; a failed NVS write is logged by caller */
}

esp_err_t app_config_set_json_tiles(const char *s) {
    return tiles_set("json_tiles", &s_json_tiles_cache, s);
}

esp_err_t app_config_set_ha_tiles(const char *s) {
    return tiles_set("ha_tiles", &s_ha_tiles_cache, s);
}

// Default RMS thresholds: good <= 0.5", ok <= 1.0", bad > 1.0" - DIMMED for Night Vision
static const char *DEFAULT_RMS_THRESHOLDS =
    "{\"good_max\":0.5,\"ok_max\":1.0,"
    "\"good_color\":\"#15803d\",\"ok_color\":\"#ca8a04\",\"bad_color\":\"#b91c1c\"}";

// Default HFR thresholds: good <= 2.0, ok <= 3.5, bad > 3.5 - DIMMED for Night Vision
static const char *DEFAULT_HFR_THRESHOLDS =
    "{\"good_max\":2.0,\"ok_max\":3.5,"
    "\"good_color\":\"#15803d\",\"ok_color\":\"#ca8a04\",\"bad_color\":\"#b91c1c\"}";

// Default AllSky field configuration — empty structure, user must configure via web UI
static const char *DEFAULT_ALLSKY_FIELD_CONFIG =
    "{\"thermal\":{\"main\":{\"key\":\"\",\"unit\":\"\"},"
    "\"sub1\":{\"label\":\"\",\"key\":\"\",\"suffix\":\"\"}},"
    "\"sqm\":{\"main\":{\"key\":\"\",\"unit\":\"\"},"
    "\"sub1\":{\"label\":\"\",\"key\":\"\",\"suffix\":\"\"},"
    "\"sub2\":{\"label\":\"\",\"key\":\"\",\"suffix\":\"\"}},"
    "\"ambient\":{\"main\":{\"key\":\"\",\"unit\":\"\"},"
    "\"sub1\":{\"label\":\"\",\"key\":\"\",\"suffix\":\"\"},"
    "\"sub2\":{\"label\":\"\",\"key\":\"\",\"suffix\":\"\"},"
    "\"dot1\":{\"key\":\"\",\"on_value\":\"\"},"
    "\"dot2\":{\"key\":\"\",\"on_value\":\"\"}},"
    "\"power\":{\"main\":{\"key\":\"\",\"unit\":\"\"},"
    "\"sub1\":{\"label\":\"\",\"key\":\"\",\"suffix\":\"\"},"
    "\"sub2\":{\"label\":\"\",\"key\":\"\",\"suffix\":\"\"}}}";

// Default AllSky thresholds — min/max ranges with gradient colors per field
// Keys use positional format: {quadrant}_{field} (e.g., thermal_main, ambient_sub1)
static const char *DEFAULT_ALLSKY_THRESHOLDS =
    "{\"thermal_main\":{\"min\":0,\"max\":80,\"color_min\":\"#3b82f6\",\"color_max\":\"#ef4444\"},"
    "\"thermal_sub1\":{\"min\":0,\"max\":70,\"color_min\":\"#3b82f6\",\"color_max\":\"#ef4444\"},"
    "\"sqm_main\":{\"min\":16,\"max\":22,\"color_min\":\"#3b82f6\",\"color_max\":\"#ef4444\"},"
    "\"ambient_main\":{\"min\":-30,\"max\":40,\"color_min\":\"#3b82f6\",\"color_max\":\"#ef4444\"},"
    "\"ambient_sub1\":{\"min\":0,\"max\":100,\"color_min\":\"#3b82f6\",\"color_max\":\"#ef4444\"},"
    "\"ambient_sub2\":{\"min\":-30,\"max\":30,\"color_min\":\"#3b82f6\",\"color_max\":\"#ef4444\"},"
    "\"power_main\":{\"min\":0,\"max\":5,\"color_min\":\"#3b82f6\",\"color_max\":\"#ef4444\"}}";

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

/* ── Version 10 config struct — used only for NVS migration to v11 ────── */
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
    bool     alert_flash_enabled;
} app_config_v10_t;

/* ── Version 11 config struct — used only for NVS migration to v12 ────── */
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
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
} app_config_v11_t;

/* ── Version 12 config struct — used only for NVS migration to v13 ────── */
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
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
} app_config_v12_t;

/* ── Version 13 config struct — used only for NVS migration to v14 ────── */
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
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
} app_config_v13_t;

/* ── Version 14 config struct — used only for NVS migration to v15 ────── */
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
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
} app_config_v14_t;

/* ── Version 15 config struct — used only for NVS migration to v16 ────── */
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
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
} app_config_v15_t;

/* ── Version 16 config struct — used only for NVS migration to v17 ────── */
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
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
} app_config_v16_t;

static void set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(app_config_t));
    settings_defaults_apply(cfg);   /* every "simple" field's default — see settings_table.h */
    cfg->config_version = APP_CONFIG_VERSION;
    strcpy(cfg->api_url[0], "http://astromele1.lan:1888/v2/api/");
    strcpy(cfg->api_url[1], "http://astromele2.lan:1888/v2/api/");
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        strcpy(cfg->filter_colors[i], "{}");
        strcpy(cfg->rms_thresholds[i], DEFAULT_RMS_THRESHOLDS);
        strcpy(cfg->hfr_thresholds[i], DEFAULT_HFR_THRESHOLDS);
    }
    cfg->active_page_override = PAGE_REF_SUMMARY;  /* Home Page = Summary (page_ref_t registry id) */
    cfg->auto_rotate_pages = 0x0E;  // Default: all NINA instances (bits 1-3)
    cfg->auto_rotate_pages_hi = 0;  // bits 8-15 off by default (Image Display opt-in, like AllSky/Spotify/Clock)
    /* Default rotation order: Summary, AllSky, Spotify, Clock, NINA1, NINA2, NINA3, SysInfo */
    for (int i = 0; i < 8; i++) cfg->auto_rotate_order[i] = (uint8_t)i;
    cfg->auto_rotate_order_ext = 8;  // 9th slot = bit index 8 (Image Display)
    /* Default flat slideshow order (auto_rotate_order2): ARP_IDX_* values 0..7
     * only. Image Display (ARP_IDX_IMG_GOES, index 8) stays OPT-IN, excluded from
     * the fresh-install default rotation. memset() above already cleared the
     * array, so set the explicit defaults and pad the tail (8..15) with 0xFF. */
    for (int i = 0; i < 8; i++) cfg->auto_rotate_order2[i] = (uint8_t)i;   // 0..7 (Clock is last)
    for (int i = 8; i < ARP_ORDER_CAPACITY; i++) cfg->auto_rotate_order2[i] = 0xFF;
    /* update_rate_s / graph_update_interval_s: NOT table-driven — their
     * validate_config() out-of-range reset target differs from this default
     * (5->2 and 10->5 respectively), so one table row can't represent both
     * constants without lying about one of them. See settings_table.h. */
    cfg->update_rate_s = 5;
    cfg->graph_update_interval_s = 10;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        cfg->instance_enabled[i] = true;
    }

    // AllSky defaults (field_config/thresholds are JSON blobs, not table-driven)
    strncpy(cfg->allsky_field_config, DEFAULT_ALLSKY_FIELD_CONFIG, sizeof(cfg->allsky_field_config) - 1);
    cfg->allsky_field_config[sizeof(cfg->allsky_field_config) - 1] = '\0';
    strncpy(cfg->allsky_thresholds, DEFAULT_ALLSKY_THRESHOLDS, sizeof(cfg->allsky_thresholds) - 1);
    cfg->allsky_thresholds[sizeof(cfg->allsky_thresholds) - 1] = '\0';

    // JSON Display defaults (tiles now live in NVS key "json_tiles", see accessors)
    cfg->json_enabled = false;
    cfg->json_url[0] = '\0';
    cfg->json_auth_header[0] = '\0';
    cfg->json_update_interval_s = 30;

    // Home Assistant defaults (tiles now live in NVS key "ha_tiles", see accessors)
    cfg->ha_enabled = false;
    cfg->ha_base_url[0] = '\0';
    cfg->ha_token[0] = '\0';
    cfg->ha_update_interval_s = 30;

    // Spotify client ID: secret-like sentinel, not table-driven
    cfg->spotify_client_id[0] = '\0';

    memset(cfg->wifi_networks, 0, sizeof(cfg->wifi_networks));

    // Toast notification overhaul defaults (mask/per-instance mute are not table-driven)
    cfg->toast_notify_mask = 0xFFFFFFFF;  // all categories enabled
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        cfg->toast_instance_muted[i] = false;
    }

    cfg->idle_page_override_target = PAGE_REF_SUMMARY;
    cfg->home_page_lock = false;       // hold the Home Page regardless of connection state; explicit bool canonicalization in validate_config(), not table-driven

    // Admin password default — change via web UI on first boot
    strcpy(cfg->admin_password, "changeme123!");

    /* moon_drag_light_mode: NOT table-driven — its validate_config()
     * out-of-range reset target (0) differs from this default (2), so one
     * table row can't represent both constants. See settings_table.h. */
    cfg->moon_drag_light_mode = 2;   // 0=true phase, 1=explore, 2=locked to surface (default) (moon drag-to-rotate lighting)
}

/* Convert the two page-target fields from their pre-v47 encodings
 * (active_page_override = raw page index, idle_page_override_target =
 * idle_target_t enum) to page_ref registry ids. Must run on EVERY
 * migration path from a pre-v47 config, since the dispatcher does not
 * chain migrations.
 *
 * Known limitation: the active_page_override remap below assumes the
 * v44/v45-era raw-page-index layout. Pre-v43 configs used an older page
 * layout, so an ancient stored value may not map perfectly here; however
 * validate_config() (registry-aware, runs after every migration) bounds
 * any unmappable value to Summary, which is acceptable.
 *
 * The from_version parameter is the config version the caller is migrating
 * FROM. The active_page_override field has existed since v0 and is always
 * stored as a raw page index, so its remap is unconditional. The
 * idle_page_override_target field was INTRODUCED at config v29; for sources
 * older than v29 the field is not present in the old blob and therefore holds
 * the set_defaults() value (PAGE_REF_SUMMARY = 0), which is already a correct
 * page_ref id. Remapping that 0 as if it were the old idle_target_t enum would
 * wrongly turn it into Clock (id 7), so the idle remap is gated on
 * from_version >= 29. */
static void normalize_legacy_page_targets(app_config_t *cfg, uint32_t from_version)
{
    /* Remap idle_page_override_target: old idle_target_t enum -> page_ref id.
     * Only applies when migrating from v29 or later, where the field actually
     * existed in the old blob. For pre-v29 sources the field already holds the
     * set_defaults() PAGE_REF_SUMMARY value and must be left untouched.
     * Use an int temporary so the negative-capable int8_t does not sign-extend
     * unexpectedly inside the switch. */
    if (from_version >= 29) {
        int isrc = cfg->image_display_source;
        if (isrc < 0) isrc = 0;
        if (isrc > 3) isrc = 3;
        int old = (int)cfg->idle_page_override_target;
        switch (old) {
            case -1: cfg->idle_page_override_target = 0;  break;  /* Summary  */
            case  0: cfg->idle_page_override_target = 7;  break;  /* Clock    */
            case  1: cfg->idle_page_override_target = 5;  break;  /* AllSky   */
            case  2: cfg->idle_page_override_target = 6;  break;  /* Spotify  */
            case  3: cfg->idle_page_override_target = (int8_t)(8 + isrc); break;  /* Image -> concrete source (8..11) */
            case  4: cfg->idle_page_override_target = 4;  break;  /* SysInfo  */
            case  5: cfg->idle_page_override_target = 1;  break;  /* NINA1    */
            case  6: cfg->idle_page_override_target = 2;  break;  /* NINA2    */
            case  7: cfg->idle_page_override_target = 3;  break;  /* NINA3    */
            default: cfg->idle_page_override_target = 0;  break;  /* Summary  */
        }
    }

    /* Remap active_page_override: old RAW PAGE INDEX -> page_ref id.
     * Same int-temporary guard for the int8_t -1 (legacy Auto) case. */
    int asrc = cfg->image_display_source;
    if (asrc < 0) asrc = 0;
    if (asrc > 3) asrc = 3;
    int old2 = (int)cfg->active_page_override;
    switch (old2) {
        case -1: cfg->active_page_override = 0;  break;  /* legacy Auto -> Summary */
        case  0: cfg->active_page_override = 5;  break;  /* AllSky   */
        case  1: cfg->active_page_override = 6;  break;  /* Spotify  */
        case  2: cfg->active_page_override = 7;  break;  /* Clock    */
        case  3: cfg->active_page_override = (int8_t)(8 + asrc); break;  /* Image -> concrete source (8..11) */
        case  4: cfg->active_page_override = 0;  break;  /* Summary  */
        case  5: cfg->active_page_override = 1;  break;  /* NINA1    */
        case  6: cfg->active_page_override = 2;  break;  /* NINA2    */
        case  7: cfg->active_page_override = 3;  break;  /* NINA3    */
        case  8: cfg->active_page_override = 13; break;  /* Settings (home_page resolves to Summary at runtime) */
        case  9: cfg->active_page_override = 4;  break;  /* SysInfo  */
        default: cfg->active_page_override = 0;  break;  /* Summary  */
    }
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

    normalize_legacy_page_targets(cfg, 0);

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

    normalize_legacy_page_targets(cfg, 1);

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

    normalize_legacy_page_targets(cfg, 2);

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

    normalize_legacy_page_targets(cfg, 3);

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

    normalize_legacy_page_targets(cfg, 4);

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

    normalize_legacy_page_targets(cfg, 5);

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

    normalize_legacy_page_targets(cfg, 6);

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

    normalize_legacy_page_targets(cfg, 7);

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

    normalize_legacy_page_targets(cfg, 8);

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

    normalize_legacy_page_targets(cfg, 9);

    ESP_LOGI(TAG, "Migrated config from v9 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v10 config blob into the current struct layout.
 *
 * v10 → v11 adds: idle_poll_interval_s, wifi_power_save.
 */
static void migrate_from_v10(const app_config_v10_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    /* idle_poll_interval_s, wifi_power_save: defaults from set_defaults() */

    normalize_legacy_page_targets(cfg, 10);

    ESP_LOGI(TAG, "Migrated config from v10 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v11 config blob into the current struct layout.
 *
 * v11 → v12 adds: widget_style.
 */
static void migrate_from_v11(const app_config_v11_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    /* widget_style: defaults from set_defaults() */

    normalize_legacy_page_targets(cfg, 11);

    ESP_LOGI(TAG, "Migrated config from v11 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v12 config blob into the current struct layout.
 *
 * v12 → v13 adds: auto_update_check, update_channel.
 */
static void migrate_from_v12(const app_config_v12_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    /* auto_update_check, update_channel: defaults from set_defaults() */

    normalize_legacy_page_targets(cfg, 12);

    ESP_LOGI(TAG, "Migrated config from v12 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v13 config blob into the current struct layout.
 *
 * v13 → v14 adds: deep_sleep_enabled, deep_sleep_wake_timer_s, deep_sleep_on_idle.
 */
static void migrate_from_v13(const app_config_v13_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    /* deep_sleep_enabled, deep_sleep_wake_timer_s, deep_sleep_on_idle: defaults from set_defaults() */

    normalize_legacy_page_targets(cfg, 13);

    ESP_LOGI(TAG, "Migrated config from v13 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v14 config blob into the current struct layout.
 *
 * v14 → v15 adds: screen_rotation.
 */
static void migrate_from_v14(const app_config_v14_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    /* screen_rotation: defaults from set_defaults() */

    normalize_legacy_page_targets(cfg, 14);

    ESP_LOGI(TAG, "Migrated config from v14 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v15 config blob into the current struct layout.
 *
 * v15 → v16 adds: hostname.
 */
static void migrate_from_v15(const app_config_v15_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    /* hostname: defaults from set_defaults() */

    normalize_legacy_page_targets(cfg, 15);

    ESP_LOGI(TAG, "Migrated config from v15 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v17 config blob into the current struct layout.
 *
 * v17 → v18 adds: allsky_enabled flag (default true).
 */
static void migrate_from_v17(const app_config_v17_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = false;  /* new in v18 — default off, user must configure fields first */

    normalize_legacy_page_targets(cfg, 17);

    ESP_LOGI(TAG, "Migrated config from v17 → v%d", APP_CONFIG_VERSION);
}

/**
 * v18 → v19 adds: demo_mode flag (default false).
 */
static void migrate_from_v18(const app_config_v18_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = old->allsky_enabled;
    cfg->demo_mode = false;  /* new in v19 — default off */

    normalize_legacy_page_targets(cfg, 18);

    ESP_LOGI(TAG, "Migrated config from v18 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v19 config blob into the current struct layout.
 *
 * v19 → v20 adds: Spotify integration fields.
 */
static void migrate_from_v19(const app_config_v19_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = old->allsky_enabled;
    cfg->demo_mode = old->demo_mode;
    /* Spotify fields: new in v20 — defaults already set by set_defaults() */

    normalize_legacy_page_targets(cfg, 19);

    ESP_LOGI(TAG, "Migrated config from v19 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v22 config blob into the current struct layout.
 *
 * v22 → v23 adds: spotify_scroll_text (appended at end).
 */
static void migrate_from_v22(const app_config_v22_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = old->allsky_enabled;
    cfg->demo_mode = old->demo_mode;
    cfg->spotify_enabled = old->spotify_enabled;
    memcpy(cfg->spotify_client_id, old->spotify_client_id, sizeof(cfg->spotify_client_id));
    cfg->spotify_poll_interval_ms = old->spotify_poll_interval_ms;
    cfg->spotify_show_progress_bar = old->spotify_show_progress_bar;
    cfg->spotify_overlay_timeout_s = old->spotify_overlay_timeout_s;
    cfg->spotify_minimal_mode = old->spotify_minimal_mode;
    /* spotify_scroll_text: new in v23 — defaults already set by set_defaults() */

    normalize_legacy_page_targets(cfg, 22);

    ESP_LOGI(TAG, "Migrated config from v22 → v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v23(const app_config_v23_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = old->allsky_enabled;
    cfg->demo_mode = old->demo_mode;
    cfg->spotify_enabled = old->spotify_enabled;
    memcpy(cfg->spotify_client_id, old->spotify_client_id, sizeof(cfg->spotify_client_id));
    cfg->spotify_poll_interval_ms = old->spotify_poll_interval_ms;
    cfg->spotify_show_progress_bar = old->spotify_show_progress_bar;
    cfg->spotify_overlay_timeout_s = old->spotify_overlay_timeout_s;
    cfg->spotify_minimal_mode = old->spotify_minimal_mode;
    cfg->spotify_scroll_text = old->spotify_scroll_text;

    /* wifi_networks stays zeroed from set_defaults() */

    normalize_legacy_page_targets(cfg, 23);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v23 to v%d", APP_CONFIG_VERSION);
}

/* Reconstruct the flat slideshow list from the legacy rotation mask+order for
 * pre-v43 migrations (defined after build_order2_from_legacy). */
static void rebuild_slideshow_from_legacy_mask(app_config_t *cfg);

/* --- v37 → v40 migration (added moon orientation tuning, moon_north_up, moon touch-spin) --- */
static void migrate_from_v37(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v37_t) ? raw_size : sizeof(app_config_v37_t);
    memcpy(cfg, raw, copy);

    /* Moon orientation tuning fields: new in v38 — defaults already set by set_defaults() */
    cfg->moon_flip_u = 0;
    cfg->moon_flip_v = 0;
    cfg->moon_roll_offset = -7.0f;
    cfg->moon_yaw_offset = 0.0f;
    cfg->moon_pitch_offset = -5.0f;

    /* moon_north_up field: new in v39 — default already set by set_defaults() */
    cfg->moon_north_up = 1;

    /* Moon touch-spin fields: new in v40 — defaults already set by set_defaults() */
    cfg->moon_spin_mode = 0;
    cfg->moon_spin_return_s = 3;

    /* crash_log_retention_days: new in v41 — default already set by set_defaults() */
    cfg->crash_log_retention_days = 30;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 37);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v37 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v38(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v38_t) ? raw_size : sizeof(app_config_v38_t);
    memcpy(cfg, raw, copy);

    /* moon_north_up field: new in v39 — default already set by set_defaults() */
    cfg->moon_north_up = 1;

    /* Moon touch-spin fields: new in v40 — defaults already set by set_defaults() */
    cfg->moon_spin_mode = 0;
    cfg->moon_spin_return_s = 3;

    /* crash_log_retention_days: new in v41 — default already set by set_defaults() */
    cfg->crash_log_retention_days = 30;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 38);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v38 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v39(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v39_t) ? raw_size : sizeof(app_config_v39_t);
    memcpy(cfg, raw, copy);

    /* Moon touch-spin fields: new in v40 — defaults already set by set_defaults() */
    cfg->moon_spin_mode = 0;
    cfg->moon_spin_return_s = 3;

    /* crash_log_retention_days: new in v41 — default already set by set_defaults() */
    cfg->crash_log_retention_days = 30;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 39);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v39 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v40(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v40_t) ? raw_size : sizeof(app_config_v40_t);
    memcpy(cfg, raw, copy);

    /* crash_log_retention_days: new in v41 — default already set by set_defaults() */
    cfg->crash_log_retention_days = 30;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 40);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v40 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v41(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v41_t) ? raw_size : sizeof(app_config_v41_t);
    memcpy(cfg, raw, copy);

    /* auto_rotate_pages_hi / auto_rotate_order_ext: new in v42, appended past the
     * v41 snapshot — keep the set_defaults() values (Image Display bit off, 9th
     * order slot = bit 8). */
    cfg->auto_rotate_pages_hi = 0;
    cfg->auto_rotate_order_ext = 8;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 41);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v41 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v42(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v42_t) ? raw_size : sizeof(app_config_v42_t);
    memcpy(cfg, raw, copy);

    /* goes_orientation / solar_orientation: new in v43, appended past the v42
     * snapshot — keep the set_defaults() values (both 0 = 0° rotation). */
    cfg->goes_orientation = 0;
    cfg->solar_orientation = 0;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 42);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v42 to v%d", APP_CONFIG_VERSION);
}

/* --- v43 → v44 migration: rename active_page_override semantics to "Home Page";
 *     collapse slideshow mask+order into one ordered list; add nav_grace_s;
 *     drop idle_page_persistent; normalize mode exclusivity. --- */
/* Build the flat auto_rotate_order2[] list from the legacy 9-slot order
 * already present in cfg (auto_rotate_order[0..7] + auto_rotate_order_ext).
 * Bit-indices 0-7 copy through unchanged; the single legacy Image Display
 * stop (bit-index 8) maps to the per-source ARP_IDX_IMG_* matching
 * cfg->image_display_source (0->GOES,1->Moon,2->Solar,3->Custom). 0xFF
 * terminators are skipped; order is preserved; the result is 0xFF-padded. */
static void build_order2_from_legacy(app_config_t *cfg)
{
    for (int i = 0; i < ARP_ORDER_CAPACITY; i++) cfg->auto_rotate_order2[i] = 0xFF;

    int dn = 0;
    for (int i = 0; i < 9 && dn < ARP_ORDER_CAPACITY; i++) {
        uint8_t v = (i < 8) ? cfg->auto_rotate_order[i] : cfg->auto_rotate_order_ext;
        if (v == 0xFF) continue;
        uint8_t mapped;
        if (v == 8) {
            /* Legacy single Image Display stop -> per-source stop. */
            switch (cfg->image_display_source) {
                case 1:  mapped = ARP_IDX_IMG_MOON;   break;
                case 2:  mapped = ARP_IDX_IMG_SOLAR;  break;
                case 3:  mapped = ARP_IDX_IMG_CUSTOM; break;
                default: mapped = ARP_IDX_IMG_GOES;   break;
            }
        } else if (v <= 7) {
            mapped = v;   /* indices 0-7 are identical across schemes */
        } else {
            continue;     /* skip invalid legacy index (9-254) */
        }
        cfg->auto_rotate_order2[dn++] = mapped;
    }
}

/* Pre-v43 migrations otherwise leave auto_rotate_order2[] at its set_defaults()
 * value, discarding the user's saved slideshow selection. Derive that selection
 * the same way migrate_from_v43 does: filter the legacy 9-slot order by the
 * effective rotation mask (auto_rotate_pages | pages_hi<<8 — pages_hi is 0 for
 * vintages predating it), rewrite the legacy order, then rebuild the flat list
 * and settle nav-mode exclusivity. Operates on the fields already present in
 * cfg after the migration's copy/fixups. */
static void rebuild_slideshow_from_legacy_mask(app_config_t *cfg)
{
    uint16_t eff_mask = (uint16_t)cfg->auto_rotate_pages
                      | ((uint16_t)cfg->auto_rotate_pages_hi << 8);
    uint8_t derived[9];
    int dn = 0;
    for (int i = 0; i < 9; i++) {
        uint8_t bit = (i < 8) ? cfg->auto_rotate_order[i] : cfg->auto_rotate_order_ext;
        if (bit == 0xFF) continue;
        if (bit > 8) continue;
        if (!(eff_mask & (1u << bit))) continue;
        /* de-dup */
        bool seen = false;
        for (int k = 0; k < dn; k++) if (derived[k] == bit) { seen = true; break; }
        if (seen) continue;
        derived[dn++] = bit;
    }
    for (int i = 0; i < 8; i++) cfg->auto_rotate_order[i] = (i < dn) ? derived[i] : 0xFF;
    cfg->auto_rotate_order_ext = (dn > 8) ? derived[8] : 0xFF;

    build_order2_from_legacy(cfg);
    app_config_normalize_nav_exclusivity(cfg);
}

static void migrate_from_v43(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v43_t) ? raw_size : sizeof(app_config_v43_t);
    memcpy(cfg, raw, copy);

    const app_config_v43_t *old = (const app_config_v43_t *)raw;

    /* Home Page = old active_page_override. -1 (auto) maps to Summary, matching
     * the prior boot default (active_page = PAGE_IDX_SUMMARY when override < 0).
     * PAGE_IDX_SUMMARY == 4 per nina_dashboard_internal.h (not includable here). */
    if (old->active_page_override < 0) {
        cfg->active_page_override = 4;   /* PAGE_IDX_SUMMARY */
    } else {
        cfg->active_page_override = old->active_page_override;
    }

    /* Derive the single ordered slideshow list: walk the old 9-slot order
     * (auto_rotate_order[0..7] + auto_rotate_order_ext), keep each entry whose
     * bit is set in the old effective mask, drop 0xFF terminators and bits > 8.
     * Result is written back into auto_rotate_order[] + auto_rotate_order_ext as
     * the new membership-equals-order list (bit-index values, 0xFF padded). */
    uint16_t eff_mask = (uint16_t)old->auto_rotate_pages
                      | ((uint16_t)old->auto_rotate_pages_hi << 8);
    uint8_t derived[9];
    int dn = 0;
    for (int i = 0; i < 9; i++) {
        uint8_t bit = (i < 8) ? old->auto_rotate_order[i] : old->auto_rotate_order_ext;
        if (bit == 0xFF) continue;
        if (bit > 8) continue;
        if (!(eff_mask & (1u << bit))) continue;
        /* de-dup */
        bool seen = false;
        for (int k = 0; k < dn; k++) if (derived[k] == bit) { seen = true; break; }
        if (seen) continue;
        derived[dn++] = bit;
    }
    for (int i = 0; i < 8; i++) cfg->auto_rotate_order[i] = (i < dn) ? derived[i] : 0xFF;
    cfg->auto_rotate_order_ext = (dn > 8) ? derived[8] : 0xFF;

    /* Translate the derived legacy 9-slot order into the flat auto_rotate_order2[]
     * list so v43 users keep their saved slideshow order (consistent with
     * v44/v45). build_order2_from_legacy() splits the single Image Display stop
     * into the per-source ARP_IDX_IMG_* matching this device's image_display_source. */
    build_order2_from_legacy(cfg);

    /* nav_grace_s default. */
    cfg->nav_grace_s = 10;

    /* Idle target + idle enabled carried forward (already memcpy'd). */
    /* idle_page_persistent dropped: its "return to idle" job is now the grace window. */
    cfg->idle_page_persistent = false;

    /* Exclusivity: auto-rotate wins the tie-break. */
    app_config_normalize_nav_exclusivity(cfg);

    normalize_legacy_page_targets(cfg, 43);

    cfg->config_version = APP_CONFIG_VERSION;

    /* Single migration summary line. */
    ESP_LOGI(TAG,
        "Migrated config v43->v44: HomePage=%d, slideshow=[%d slots], "
        "nav_grace_s=%d, dropped idle_page_persistent, dropped rotation bitmask",
        (int)cfg->active_page_override, dn, (int)cfg->nav_grace_s);
}

/* --- v44 → v46 migration: add Custom Image URL source (Image Display source
 *     index 3) — custom_image_url, custom_orientation, custom_update_interval_s. --- */
static void migrate_from_v44(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v44_t) ? raw_size : sizeof(app_config_v44_t);
    memcpy(cfg, raw, copy);

    /* custom_image_url / custom_orientation / custom_update_interval_s: new in
     * v45, appended past the v44 snapshot — apply the set_defaults() values. */
    strlcpy(cfg->custom_image_url, "https://picsum.photos/720", sizeof(cfg->custom_image_url));
    cfg->custom_orientation = 0;
    cfg->custom_update_interval_s = 60;

    /* The legacy 9-slot order (auto_rotate_order[0..7] + auto_rotate_order_ext)
     * lives at this struct offset and was already copied above. Translate it
     * into the new flat auto_rotate_order2[] list, splitting the single
     * Image Display stop into the per-source ARP_IDX_IMG_* matching this
     * device's saved image_display_source. */
    build_order2_from_legacy(cfg);

    normalize_legacy_page_targets(cfg, 44);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v44 to v%d", APP_CONFIG_VERSION);
}

/* --- v45 → v46 migration: append auto_rotate_order2[16] (each image source is
 *     now its own distinct slideshow stop). The legacy auto_rotate_order[8] +
 *     auto_rotate_order_ext encoding is preserved for binary stability but no
 *     longer drives the slideshow. --- */
static void migrate_from_v45(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v45_t) ? raw_size : sizeof(app_config_v45_t);
    memcpy(cfg, raw, copy);

    /* Translate the old 9-slot order into the new flat auto_rotate_order2[],
     * mapping the single old Image Display stop (bit-index 8) to the matching
     * per-source ARP_IDX_IMG_* based on cfg->image_display_source. */
    build_order2_from_legacy(cfg);

    normalize_legacy_page_targets(cfg, 45);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v45 to v%d", APP_CONFIG_VERSION);
}

/* --- v46 → v47 migration: remap idle_page_override_target (old idle_target_t
 *     enum, -1..7) and active_page_override (old RAW PAGE INDEX) onto the
 *     page_ref_t registry ids defined in ui/page_registry.h. The struct layout
 *     is unchanged; only the stored VALUES of these two int8_t fields move. --- */
static void migrate_from_v46(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v46_t) ? raw_size : sizeof(app_config_v46_t);
    memcpy(cfg, raw, copy);

    /* Remap idle_page_override_target + active_page_override from their old
     * pre-v47 encodings onto page_ref registry ids. */
    normalize_legacy_page_targets(cfg, 46);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config v46 -> v%d (page-ref remap)", APP_CONFIG_VERSION);
}

/* --- v47 → v48 migration: appends 6 per-source Image Display mirror-flip
 *     bytes (goes/solar/custom v/hflip). Layout ahead is unchanged; the new
 *     fields are additive and default 0 (no flip = current behavior). --- */
static void migrate_from_v47(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v47_t) ? raw_size : sizeof(app_config_v47_t);
    memcpy(cfg, raw, copy);

    /* The 6 flip fields are appended past the v47 snapshot, so memcpy(copy)
     * never touches them. set_defaults() already zeroed the whole struct, but
     * set them explicitly for documentation (0 = no flip = current behavior). */
    cfg->goes_vflip   = 0;
    cfg->goes_hflip   = 0;
    cfg->solar_vflip  = 0;
    cfg->solar_hflip  = 0;
    cfg->custom_vflip = 0;
    cfg->custom_hflip = 0;

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v47 to v%d", APP_CONFIG_VERSION);
}

/* --- v48 → v49 migration: appends the home_page_lock flag (always show the
 *     Home Page regardless of connection state). Layout ahead is unchanged;
 *     the new field is additive and defaults false (current behavior). --- */
static void migrate_from_v48(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v48_t) ? raw_size : sizeof(app_config_v48_t);
    memcpy(cfg, raw, copy);

    /* home_page_lock is appended past the v48 snapshot, so memcpy(copy) never
     * touches it. set_defaults() already cleared it, but set it explicitly for
     * documentation (false = no lock = current behavior). */
    cfg->home_page_lock = false;

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v48 to v%d", APP_CONFIG_VERSION);
}

/* --- v49 → v50 migration: appends the JSON Display fields. Layout ahead is
 *     unchanged; the new fields are additive and default to disabled. --- */
static void migrate_from_v49(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v49_t) ? raw_size : sizeof(app_config_v49_t);
    memcpy(cfg, raw, copy);

    /* JSON scalar fields are appended past the v49 snapshot, so memcpy(copy)
     * never touches them. set_defaults() already applied them (json_enabled=false,
     * url/auth empty, interval=30); set them explicitly for documentation. The
     * tiles blob no longer lives in the struct — its cache is "" (v49 had none). */
    cfg->json_enabled = false;
    cfg->json_url[0] = '\0';
    cfg->json_auth_header[0] = '\0';
    cfg->json_update_interval_s = 30;

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v49 to v%d", APP_CONFIG_VERSION);
}

/* --- v50 → v52 migration. v50 predates HA and has json_tiles_config as its
 *     LAST field. The v52 struct removed json_tiles_config mid-struct, so a
 *     blind memcpy of the v50 blob would overflow the smaller dest and land the
 *     6144-byte json blob on top of the HA scalars. Copy the shared prefix only,
 *     then lift json_tiles_config out to the "json_tiles" NVS key/cache. --- */
static void migrate_from_v50(const void *raw, size_t raw_size,
                             app_config_t *cfg, nvs_handle_t handle)
{
    const app_config_v50_t *old = (const app_config_v50_t *)raw;
    set_defaults(cfg);   /* also sets ha_* defaults (disabled/empty/30) */

    /* Copy shared prefix [config_version .. json_update_interval_s]. */
    memcpy(cfg, raw, offsetof(app_config_t, ha_enabled));
    /* HA scalars keep their set_defaults() values (v50 had no HA). */

    /* json tiles -> cache + key; ha tiles -> "" (none in v50). */
    if (s_json_tiles_cache) {
        memcpy(s_json_tiles_cache, old->json_tiles_config, TILES_CACHE_BYTES);
        s_json_tiles_cache[TILES_CACHE_BYTES - 1] = '\0';
        nvs_set_str(handle, "json_tiles", s_json_tiles_cache);
    }
    if (s_ha_tiles_cache) {
        s_ha_tiles_cache[0] = '\0';
        nvs_set_str(handle, "ha_tiles", "");
    }

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v50 to v%d", APP_CONFIG_VERSION);
    (void)raw_size;
}

/* --- v51 → v52 migration: split the two inline 6144-byte tiles blobs out to
 *     the "json_tiles"/"ha_tiles" NVS keys. json_tiles_config sat mid-struct
 *     (ahead of the HA scalars), so a blind memcpy would misplace the HA
 *     scalars and overflow the smaller dest. Copy the shared prefix verbatim,
 *     copy the HA scalars by name (their offsets shifted), lift both tiles. --- */
static void migrate_from_v51(const void *raw, size_t raw_size,
                             app_config_t *cfg, nvs_handle_t handle)
{
    const app_config_v51_t *old = (const app_config_v51_t *)raw;
    set_defaults(cfg);

    /* Shared prefix [config_version .. json_update_interval_s] is byte-identical
     * to the old blob (asserted in the header). Copy it verbatim. */
    memcpy(cfg, raw, offsetof(app_config_t, ha_enabled));

    /* HA scalars: copy by name (their offsets differ between old blob and new
     * struct because json_tiles_config was removed ahead of them). */
    cfg->ha_enabled = old->ha_enabled;
    memcpy(cfg->ha_base_url, old->ha_base_url, sizeof(cfg->ha_base_url));
    memcpy(cfg->ha_token, old->ha_token, sizeof(cfg->ha_token));
    cfg->ha_update_interval_s = old->ha_update_interval_s;

    /* Tiles -> caches + NVS keys. Force-terminate (raw blob may lack a NUL). */
    if (s_json_tiles_cache) {
        memcpy(s_json_tiles_cache, old->json_tiles_config, TILES_CACHE_BYTES);
        s_json_tiles_cache[TILES_CACHE_BYTES - 1] = '\0';
        nvs_set_str(handle, "json_tiles", s_json_tiles_cache);
    }
    if (s_ha_tiles_cache) {
        memcpy(s_ha_tiles_cache, old->ha_tiles_config, TILES_CACHE_BYTES);
        s_ha_tiles_cache[TILES_CACHE_BYTES - 1] = '\0';
        nvs_set_str(handle, "ha_tiles", s_ha_tiles_cache);
    }

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v51 to v%d", APP_CONFIG_VERSION);
    (void)raw_size;
}

static void migrate_from_v36(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v36_t) ? raw_size : sizeof(app_config_v36_t);
    memcpy(cfg, raw, copy);

    /* NOTE: this and the older migrations below only memcpy `copy` bytes — the
     * size of their snapshot struct. Any field appended to app_config_t *after*
     * that snapshot struct ends (e.g. crash_log_retention_days, added in v41)
     * lies past `copy`, so it is never overwritten and keeps the value
     * set_defaults() assigned. Those trailing fields therefore need no explicit
     * per-migration assignment here. The newer migrations (v37+) set them
     * explicitly only for documentation. */

    /* moon_drag_light_mode field: new in v37 — defaults already set by set_defaults() */
    cfg->moon_drag_light_mode = 2;

    /* Moon orientation tuning fields: new in v38 — defaults already set by set_defaults() */
    cfg->moon_flip_u = 0;
    cfg->moon_flip_v = 0;
    cfg->moon_roll_offset = -7.0f;
    cfg->moon_yaw_offset = 0.0f;
    cfg->moon_pitch_offset = -5.0f;

    /* moon_north_up field: new in v39 — default already set by set_defaults() */
    cfg->moon_north_up = 1;

    /* Moon touch-spin fields: new in v40 — defaults already set by set_defaults() */
    cfg->moon_spin_mode = 0;
    cfg->moon_spin_return_s = 3;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 36);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v36 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v35(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v35_t) ? raw_size : sizeof(app_config_v35_t);
    memcpy(cfg, raw, copy);

    /* image_display_crop field: new in v36 — defaults already set by set_defaults() */
    cfg->image_display_crop = false;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 35);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v35 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v34(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v34_t) ? raw_size : sizeof(app_config_v34_t);
    memcpy(cfg, raw, copy);

    /* Solar band field: new in v35 — defaults already set by set_defaults() */
    cfg->solar_band = 0;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 34);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v34 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v33(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v33_t) ? raw_size : sizeof(app_config_v33_t);
    memcpy(cfg, raw, copy);

    /* Moon phase fields: new in v34 — defaults already set by set_defaults() */
    cfg->image_display_source = 0;
    cfg->moon_bg_style = 0;
    cfg->moon_lat = cfg->weather_lat;   /* prefill from weather */
    cfg->moon_lon = cfg->weather_lon;

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 33);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v33 to v%d", APP_CONFIG_VERSION);
}

static void migrate_from_v32(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v32_t) ? raw_size : sizeof(app_config_v32_t);
    memcpy(cfg, raw, copy);

    /* Image Display fields: new in v33 — defaults already set by set_defaults() */

    /* v32→v33: IDLE_TARGET_SYSINFO shifted from 3 to 4 */
    if (cfg->idle_page_override_target == 3) {
        cfg->idle_page_override_target = IDLE_TARGET_SYSINFO;  // now 4
    }

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 32);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v32 to v%d", APP_CONFIG_VERSION);
}

/* --- v31 → v33 migration (added auth_enabled) --- */
static void migrate_from_v31(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v31_t) ? raw_size : sizeof(app_config_v31_t);
    memcpy(cfg, raw, copy);

    /* auth_enabled: existing devices default to ON so upgrade stays locked down */
    cfg->auth_enabled = true;

    /* v32→v33: IDLE_TARGET_SYSINFO shifted from 3 to 4 */
    if (cfg->idle_page_override_target == 3) {
        cfg->idle_page_override_target = IDLE_TARGET_SYSINFO;  // now 4
    }

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 31);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v31 to v%d", APP_CONFIG_VERSION);
}

/* --- v30 → v33 migration (added admin_password, auth_enabled) --- */
static void migrate_from_v30(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v30_t) ? raw_size : sizeof(app_config_v30_t);
    memcpy(cfg, raw, copy);

    /* admin_password gets default ("changeme123!") from set_defaults() */
    strcpy(cfg->admin_password, "changeme123!");
    /* auth_enabled defaults to true from set_defaults() */
    cfg->auth_enabled = true;

    /* v32→v33: IDLE_TARGET_SYSINFO shifted from 3 to 4 */
    if (cfg->idle_page_override_target == 3) {
        cfg->idle_page_override_target = IDLE_TARGET_SYSINFO;  // now 4
    }

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 30);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v30 to v%d", APP_CONFIG_VERSION);
}

/* --- v29 → v33 migration (added idle_indicator_enabled, admin_password, auth_enabled) --- */
static void migrate_from_v29(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v29_t) ? raw_size : sizeof(app_config_v29_t);
    memcpy(cfg, raw, copy);

    /* New fields get their defaults from set_defaults() */
    cfg->idle_indicator_enabled = true;
    strcpy(cfg->admin_password, "changeme123!");
    cfg->auth_enabled = true;

    /* v32→v33: IDLE_TARGET_SYSINFO shifted from 3 to 4 */
    if (cfg->idle_page_override_target == 3) {
        cfg->idle_page_override_target = IDLE_TARGET_SYSINFO;  // now 4
    }

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 29);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v29 to v%d", APP_CONFIG_VERSION);
}

/* --- v28 → v29 migration (added weather/idle-override fields, clock page insertion) --- */
static void migrate_from_v28(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    if (raw_size < sizeof(app_config_v28_t)) {
        ESP_LOGW(TAG, "v28 blob too small (%d < %d)", (int)raw_size, (int)sizeof(app_config_v28_t));
        return;
    }
    const app_config_v28_t *old = (const app_config_v28_t *)raw;

    /* Copy all v28 fields */
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

    /* Migrate active_page_override: clock page inserted at index 2,
     * so existing indices >= 2 shift up by 1. */
    if (old->active_page_override >= 2) {
        cfg->active_page_override = old->active_page_override + 1;
    } else {
        cfg->active_page_override = old->active_page_override;
    }

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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = old->allsky_enabled;
    cfg->demo_mode = old->demo_mode;
    cfg->spotify_enabled = old->spotify_enabled;
    memcpy(cfg->spotify_client_id, old->spotify_client_id, sizeof(cfg->spotify_client_id));
    cfg->spotify_poll_interval_ms = old->spotify_poll_interval_ms;
    cfg->spotify_show_progress_bar = old->spotify_show_progress_bar;
    cfg->spotify_overlay_timeout_s = old->spotify_overlay_timeout_s;
    cfg->spotify_minimal_mode = old->spotify_minimal_mode;
    cfg->spotify_scroll_text = old->spotify_scroll_text;
    memcpy(cfg->wifi_networks, old->wifi_networks, sizeof(cfg->wifi_networks));
    cfg->spotify_overlay_visible = old->spotify_overlay_visible;

    /* Copy 7 elements of auto_rotate_order, set 8th to 0 */
    memcpy(cfg->auto_rotate_order, old->auto_rotate_order, 7);
    cfg->auto_rotate_order[7] = 0;

    cfg->toast_aggregation_window_s = old->toast_aggregation_window_s;
    cfg->toast_notify_mask = old->toast_notify_mask;
    memcpy(cfg->toast_instance_muted, old->toast_instance_muted, sizeof(cfg->toast_instance_muted));

    /* New weather/idle fields stay at defaults from set_defaults() */

    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 28);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v28 to v%d", APP_CONFIG_VERSION);
}

/* --- v27 → v28 migration (added toast notification overhaul fields) --- */
static void migrate_from_v27(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_v27_t) ? raw_size : sizeof(app_config_v27_t);
    memcpy(cfg, raw, copy);
    cfg->toast_aggregation_window_s = 5;
    cfg->toast_notify_mask = 0xFFFFFFFF;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        cfg->toast_instance_muted[i] = false;
    }
    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 27);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v27 to v%d", APP_CONFIG_VERSION);
}

/* --- v26 → v27 migration (added auto_rotate_order) --- */
static void migrate_from_v26(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    /* v26 blob is identical layout minus trailing auto_rotate_order[7].
     * Copy old data — new trailing field stays at default from set_defaults().
     * No app_config_v26_t snapshot exists, so bound the copy by the largest
     * pre-v33 layout (app_config_v32_t) to ensure the v33 image_display fields
     * always come from set_defaults(), never from copied blob bytes. */
    size_t copy = raw_size < sizeof(app_config_v32_t) ? raw_size : sizeof(app_config_v32_t);
    memcpy(cfg, raw, copy);
    for (int i = 0; i < 8; i++) cfg->auto_rotate_order[i] = (uint8_t)i;
    rebuild_slideshow_from_legacy_mask(cfg);
    normalize_legacy_page_targets(cfg, 26);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v26 to v%d", APP_CONFIG_VERSION);
}

/* --- v25 → v26 migration (added spotify_overlay_visible) --- */
static void migrate_from_v25(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    /* v25 blob is identical layout minus trailing spotify_overlay_visible.
     * Copy old data — new trailing field stays at default from set_defaults(). */
    size_t copy = raw_size < sizeof(app_config_t) ? raw_size : sizeof(app_config_t);
    memcpy(cfg, raw, copy);
    cfg->spotify_overlay_visible = false;
    normalize_legacy_page_targets(cfg, 25);

    cfg->config_version = APP_CONFIG_VERSION;
    ESP_LOGI(TAG, "Migrated config from v25 to v%d", APP_CONFIG_VERSION);
}

/* --- v24 → v26 migration (theme/widget reindex + added spotify_overlay_visible) --- */
static void migrate_from_v24(const void *raw, size_t raw_size, app_config_t *cfg)
{
    set_defaults(cfg);
    size_t copy = raw_size < sizeof(app_config_t) ? raw_size : sizeof(app_config_t);
    memcpy(cfg, raw, copy);
    cfg->config_version = APP_CONFIG_VERSION;
    cfg->spotify_overlay_visible = false;

    /* Remap theme indices: old → new */
    uint8_t old_theme = cfg->theme_index;
    if (old_theme == 0 || old_theme == 1) {
        /* keep */
    } else if (old_theme == 11) {
        cfg->theme_index = 2;
    } else {
        cfg->theme_index = 0; /* Removed themes → Default */
    }

    /* Remap widget style indices: old → new */
    switch (cfg->widget_style) {
        case 0: case 1: break; /* Default, Subtle Border — keep */
        case 3: cfg->widget_style = 2; break; /* Soft Inset */
        case 4: cfg->widget_style = 3; break; /* Frosted Glass */
        case 6: cfg->widget_style = 4; break; /* Chamfered */
        default: cfg->widget_style = 0; break; /* Wireframe/Accent Bar → Default */
    }
    normalize_legacy_page_targets(cfg, 24);
    ESP_LOGI(TAG, "Migrated config from v24 to v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v21 config blob into the current struct layout.
 *
 * v21 → v23 adds: spotify_minimal_mode, spotify_scroll_text.
 */
static void migrate_from_v21(const app_config_v21_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = old->allsky_enabled;
    cfg->demo_mode = old->demo_mode;
    cfg->spotify_enabled = old->spotify_enabled;
    memcpy(cfg->spotify_client_id, old->spotify_client_id, sizeof(cfg->spotify_client_id));
    cfg->spotify_poll_interval_ms = old->spotify_poll_interval_ms;
    cfg->spotify_show_progress_bar = old->spotify_show_progress_bar;
    cfg->spotify_overlay_timeout_s = old->spotify_overlay_timeout_s;
    /* spotify_minimal_mode: new in v22 — defaults already set by set_defaults() */
    /* spotify_scroll_text: new in v23 — defaults already set by set_defaults() */

    normalize_legacy_page_targets(cfg, 21);

    ESP_LOGI(TAG, "Migrated config from v21 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v20 config blob into the current struct layout.
 *
 * v20 → v23 adds: spotify_overlay_timeout_s, spotify_minimal_mode, spotify_scroll_text.
 */
static void migrate_from_v20(const app_config_v20_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    memcpy(cfg->allsky_hostname, old->allsky_hostname, sizeof(cfg->allsky_hostname));
    cfg->allsky_update_interval_s = old->allsky_update_interval_s;
    cfg->allsky_dew_offset = old->allsky_dew_offset;
    memcpy(cfg->allsky_field_config, old->allsky_field_config, sizeof(cfg->allsky_field_config));
    memcpy(cfg->allsky_thresholds, old->allsky_thresholds, sizeof(cfg->allsky_thresholds));
    cfg->allsky_enabled = old->allsky_enabled;
    cfg->demo_mode = old->demo_mode;
    cfg->spotify_enabled = old->spotify_enabled;
    memcpy(cfg->spotify_client_id, old->spotify_client_id, sizeof(cfg->spotify_client_id));
    cfg->spotify_poll_interval_ms = old->spotify_poll_interval_ms;
    cfg->spotify_show_progress_bar = old->spotify_show_progress_bar;
    /* spotify_overlay_timeout_s: new in v21 — defaults already set by set_defaults() */

    /* spotify_overlay_timeout_s + spotify_minimal_mode: new — defaults set by set_defaults() */

    normalize_legacy_page_targets(cfg, 20);

    ESP_LOGI(TAG, "Migrated config from v20 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Migrate a v16 config blob into the current struct layout.
 *
 * v16 → v17 adds: AllSky integration fields.
 */
static void migrate_from_v16(const app_config_v16_t *old, app_config_t *cfg) {
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
    cfg->alert_flash_enabled = old->alert_flash_enabled;
    cfg->idle_poll_interval_s = old->idle_poll_interval_s;
    cfg->wifi_power_save = old->wifi_power_save;
    cfg->widget_style = old->widget_style;
    cfg->auto_update_check = old->auto_update_check;
    cfg->update_channel = old->update_channel;
    cfg->deep_sleep_enabled = old->deep_sleep_enabled;
    cfg->deep_sleep_wake_timer_s = old->deep_sleep_wake_timer_s;
    cfg->deep_sleep_on_idle = old->deep_sleep_on_idle;
    cfg->screen_rotation = old->screen_rotation;
    memcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    /* AllSky fields: defaults from set_defaults() */

    normalize_legacy_page_targets(cfg, 16);

    ESP_LOGI(TAG, "Migrated config from v16 → v%d", APP_CONFIG_VERSION);
}

/**
 * @brief Validate and clamp config fields to sane ranges.
 * @return true if any field was corrected.
 */
static bool validate_config(app_config_t *cfg) {
    bool fixed = false;

    /* A raw NVS blob (or a truncated/corrupt one) can leave a char-array field
     * without its terminating NUL; every reader below treats these as C
     * strings. Force-terminate every char array before any of them is used. */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        cfg->api_url[i][sizeof(cfg->api_url[i]) - 1] = '\0';
        cfg->filter_colors[i][sizeof(cfg->filter_colors[i]) - 1] = '\0';
        cfg->rms_thresholds[i][sizeof(cfg->rms_thresholds[i]) - 1] = '\0';
        cfg->hfr_thresholds[i][sizeof(cfg->hfr_thresholds[i]) - 1] = '\0';
        cfg->wifi_networks[i].ssid[sizeof(cfg->wifi_networks[i].ssid) - 1] = '\0';
        cfg->wifi_networks[i].password[sizeof(cfg->wifi_networks[i].password) - 1] = '\0';
    }
    cfg->ntp_server[sizeof(cfg->ntp_server) - 1] = '\0';
    cfg->tz_string[sizeof(cfg->tz_string) - 1] = '\0';
    cfg->mqtt_broker_url[sizeof(cfg->mqtt_broker_url) - 1] = '\0';
    cfg->mqtt_username[sizeof(cfg->mqtt_username) - 1] = '\0';
    cfg->mqtt_password[sizeof(cfg->mqtt_password) - 1] = '\0';
    cfg->mqtt_topic_prefix[sizeof(cfg->mqtt_topic_prefix) - 1] = '\0';
    cfg->hostname[sizeof(cfg->hostname) - 1] = '\0';
    cfg->allsky_hostname[sizeof(cfg->allsky_hostname) - 1] = '\0';
    cfg->allsky_field_config[sizeof(cfg->allsky_field_config) - 1] = '\0';
    cfg->allsky_thresholds[sizeof(cfg->allsky_thresholds) - 1] = '\0';
    cfg->spotify_client_id[sizeof(cfg->spotify_client_id) - 1] = '\0';
    cfg->weather_api_key[sizeof(cfg->weather_api_key) - 1] = '\0';
    cfg->weather_location_name[sizeof(cfg->weather_location_name) - 1] = '\0';
    cfg->admin_password[sizeof(cfg->admin_password) - 1] = '\0';
    cfg->goes_region[sizeof(cfg->goes_region) - 1] = '\0';
    cfg->custom_image_url[sizeof(cfg->custom_image_url) - 1] = '\0';
    cfg->json_url[sizeof(cfg->json_url) - 1] = '\0';
    cfg->json_auth_header[sizeof(cfg->json_auth_header) - 1] = '\0';
    /* json/ha tiles no longer live in the struct — NUL-termination is handled by
     * the tiles cache setter (strlcpy) and loader (tiles_cache_load_key). */
    cfg->ha_base_url[sizeof(cfg->ha_base_url) - 1] = '\0';
    cfg->ha_token[sizeof(cfg->ha_token) - 1] = '\0';

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
    if (settings_clamp_apply(cfg)) {
        fixed = true;
    }
    /* Home Page stores a page_ref_t registry id (see ui/page_registry.h). Validate
     * against the registry: an unknown id (including negative int8 values, which
     * wrap to a high page_ref_t and miss the table) or a non-targetable entry
     * (Settings=13, overlays 14..23) collapses to Summary. Registry-aware so
     * appending new targetable pages needs no change here. */
    if (cfg->active_page_override == 12) {   /* PAGE_REF_IMG_DEFAULT retired as a target -> concrete source */
        int src = cfg->image_display_source;
        if (src < 0) src = 0;
        if (src > 3) src = 3;
        cfg->active_page_override = (int8_t)(8 + src);
    }
    {
        const page_ref_entry_t *e = page_ref_by_id((page_ref_t)cfg->active_page_override);
        if (e == NULL || !e->targetable) {
            cfg->active_page_override = PAGE_REF_SUMMARY;   /* unknown or non-targetable -> Summary */
            fixed = true;
        }
    }
    /* Idle page target is likewise a page_ref_t id; same registry-aware validation. */
    if (cfg->idle_page_override_target == 12) {   /* PAGE_REF_IMG_DEFAULT retired as a target -> concrete source */
        int src = cfg->image_display_source;
        if (src < 0) src = 0;
        if (src > 3) src = 3;
        cfg->idle_page_override_target = (int8_t)(8 + src);
    }
    {
        const page_ref_entry_t *e = page_ref_by_id((page_ref_t)cfg->idle_page_override_target);
        if (e == NULL || !e->targetable) {
            cfg->idle_page_override_target = PAGE_REF_SUMMARY;   /* unknown or non-targetable -> Summary */
            fixed = true;
        }
    }
    if (cfg->auto_rotate_pages == 0) {
        cfg->auto_rotate_pages = 0x0E;  // Default: all NINA instances
        fixed = true;
    }
    /* Validate rotation order — reset to default if first entry is 0xFF (uninitialised) */
    if (cfg->auto_rotate_order[0] == 0xFF) {
        for (int i = 0; i < 8; i++) cfg->auto_rotate_order[i] = (uint8_t)i;
        cfg->auto_rotate_order_ext = 8;  // 9th slot = Image Display bit index
        fixed = true;
    }
    /* Validate the flat slideshow order: any entry that is not the 0xFF
     * terminator and not a valid stop id is dropped to 0xFF. */
    for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
        if (cfg->auto_rotate_order2[i] != 0xFF &&
            !ARP_STOP_IS_VALID(cfg->auto_rotate_order2[i])) {
            cfg->auto_rotate_order2[i] = 0xFF;
            fixed = true;
        }
    }
    if (cfg->update_rate_s < 1 || cfg->update_rate_s > 10) {
        cfg->update_rate_s = 2;
        fixed = true;
    }
    if (cfg->graph_update_interval_s < 2 || cfg->graph_update_interval_s > 30) {
        cfg->graph_update_interval_s = 5;
        fixed = true;
    }
    if (cfg->moon_drag_light_mode > 2) {
        cfg->moon_drag_light_mode = 0;
        fixed = true;
    }
    if (cfg->allsky_thresholds[0] == '\0' ||
        strstr(cfg->allsky_thresholds, "thermal_main") == NULL) {
        /* Empty or contains old-format keys (cpu_temp, sqm, etc.) — reset to new positional format */
        strncpy(cfg->allsky_thresholds, DEFAULT_ALLSKY_THRESHOLDS, sizeof(cfg->allsky_thresholds) - 1);
        cfg->allsky_thresholds[sizeof(cfg->allsky_thresholds) - 1] = '\0';
        fixed = true;
    }
    /* bool loaded from a raw NVS blob may hold any byte value; force canonical 0/1 */
    cfg->home_page_lock = cfg->home_page_lock ? true : false;
    /* spotify_overlay_timeout_s is uint8_t (0-255); 0 means never hide, all values valid */

    /* JSON Display: clamp poll interval; canonicalize bool. (Char arrays already
     * NUL-terminated above; tiles config lives in NVS key "json_tiles".) */
    if (cfg->json_update_interval_s < 5 || cfg->json_update_interval_s > 300) {
        cfg->json_update_interval_s = 30;
        fixed = true;
    }
    cfg->json_enabled = cfg->json_enabled ? true : false;

    /* Home Assistant: clamp poll interval; canonicalize bool. (Char arrays
     * already NUL-terminated above; tiles config lives in NVS key "ha_tiles".) */
    if (cfg->ha_update_interval_s < 5 || cfg->ha_update_interval_s > 300) {
        cfg->ha_update_interval_s = 30;
        fixed = true;
    }
    cfg->ha_enabled = cfg->ha_enabled ? true : false;

    return fixed;
}

void app_config_init(void) {
    s_config_mutex = xSemaphoreCreateMutex();
    /* Allocate the tiles caches early so getters are safe ("") on every
     * early-return path below (NVS-open fail, fresh install). Alloc-guarded, so
     * a factory-reset re-init reuses the existing buffers without leaking. */
    tiles_caches_alloc();
    bool tiles_loaded = false;   /* migrations that source inline tiles set this true */
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
        /* tiles_loaded stays false -> tail loads "json_tiles"/"ha_tiles" keys */
    } else if (version_check == 51 && stored_size == sizeof(app_config_v51_t)) {
        /* v51 → v52: split the two inline tiles blobs out to NVS keys. Guard on
         * exact v51 size so a truncated blob falls through to forward/unknown
         * rather than reading the old HA and tiles fields past the buffer. */
        migrate_from_v51(raw, stored_size, &s_config, handle);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
        tiles_loaded = true;
    } else if (version_check == 50 && stored_size == sizeof(app_config_v50_t)) {
        /* v50 → v52: split json tiles out to its NVS key; HA fields defaulted.
         * Guard on exact v50 size (parity with the v51 branch) so a truncated
         * blob does not read old->json_tiles_config past the raw buffer; it
         * falls through to the forward/unknown-version path instead. */
        migrate_from_v50(raw, stored_size, &s_config, handle);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
        tiles_loaded = true;
    } else if (version_check == 49) {
        /* v49 → v50: added JSON Display fields */
        migrate_from_v49(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 48) {
        /* v48 → v49: added home_page_lock (always show the Home Page) */
        migrate_from_v48(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 47) {
        /* v47 → v48: added per-source Image Display mirror flips (goes/solar/custom v/hflip) */
        migrate_from_v47(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 46) {
        /* v46 → v47: remap active_page_override + idle_page_override_target onto page_registry ids */
        migrate_from_v46(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 45) {
        /* v45 → v46: added auto_rotate_order2 (each image source is a distinct slideshow stop) */
        migrate_from_v45(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 44) {
        /* v44 → v46: added Custom Image URL source, then auto_rotate_order2 */
        migrate_from_v44(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 43) {
        /* v43 → v44: Home Page rename, single slideshow list, nav_grace_s, drop persistent */
        migrate_from_v43(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 42) {
        /* v42 → v43: added goes_orientation + solar_orientation (per-source Image Display rotation) */
        migrate_from_v42(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 41) {
        /* v41 → v42: added auto_rotate_pages_hi + auto_rotate_order_ext (Image Display in rotation) */
        migrate_from_v41(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 40) {
        /* v40 → v42: added crash_log_retention_days, then Image Display rotation fields */
        migrate_from_v40(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 39) {
        /* v39 → v41: added moon_spin fields + crash_log_retention_days */
        migrate_from_v39(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 38) {
        /* v38 → v41: added moon_north_up + moon touch-spin + crash_log_retention_days */
        migrate_from_v38(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 37) {
        /* v37 → v38: added moon orientation tuning fields */
        migrate_from_v37(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 36) {
        /* v36 → v38: added moon_drag_light_mode + moon orientation tuning */
        migrate_from_v36(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 35) {
        /* v35 → v36: added image_display_crop */
        migrate_from_v35(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 34) {
        /* v34 → v35: added solar_band */
        migrate_from_v34(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 33) {
        /* v33 → v34: added Moon phase fields (prefill moon lat/lon from weather) */
        migrate_from_v33(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 32) {
        /* v32 → v33: added Image Display fields; IDLE_TARGET_SYSINFO 3→4 */
        migrate_from_v32(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 31) {
        /* v31 → v32: added auth_enabled */
        migrate_from_v31(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 30) {
        /* v30 → v32: added admin_password, auth_enabled */
        migrate_from_v30(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 29) {
        /* v29 → v32: added idle_indicator_enabled, admin_password, auth_enabled */
        migrate_from_v29(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 28) {
        /* v28 → v29: added weather/idle-override fields, clock page insertion */
        migrate_from_v28(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 27) {
        /* v27 → v28: added toast notification overhaul fields */
        migrate_from_v27(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 26) {
        /* v26 → v27: added auto_rotate_order at end */
        migrate_from_v26(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 25) {
        /* v25 → v26: added spotify_overlay_visible at end */
        migrate_from_v25(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 24) {
        /* Version 24 blob — migrate to v26 (theme/widget reindex + overlay_visible) */
        migrate_from_v24(raw, stored_size, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 23 && stored_size >= sizeof(app_config_v23_t)) {
        /* Version 23 blob — migrate to v24 */
        migrate_from_v23((const app_config_v23_t *)raw, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 22 && stored_size >= sizeof(app_config_v22_t)) {
        /* Version 22 blob — migrate to v23 */
        migrate_from_v22((const app_config_v22_t *)raw, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 21 && stored_size >= sizeof(app_config_v21_t)) {
        /* Version 21 blob — migrate to v23 */
        migrate_from_v21((const app_config_v21_t *)raw, &s_config);
        validate_config(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 20 && stored_size >= sizeof(app_config_v20_t)) {
        /* Version 20 blob — migrate to v23 */
        migrate_from_v20((const app_config_v20_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 19 && stored_size >= sizeof(app_config_v19_t)) {
        /* Version 19 blob — migrate to v21 */
        migrate_from_v19((const app_config_v19_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 18 && stored_size >= sizeof(app_config_v18_t)) {
        /* Version 18 blob — migrate to v20 */
        migrate_from_v18((const app_config_v18_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 17 && stored_size >= sizeof(app_config_v17_t)) {
        /* Version 17 blob — migrate to v20 */
        migrate_from_v17((const app_config_v17_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 16 && stored_size >= sizeof(app_config_v16_t)) {
        /* Version 16 blob — migrate to v17 */
        migrate_from_v16((const app_config_v16_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 15 && stored_size >= sizeof(app_config_v15_t)) {
        /* Version 15 blob — migrate to v17 */
        migrate_from_v15((const app_config_v15_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 14 && stored_size >= sizeof(app_config_v14_t)) {
        /* Version 14 blob — migrate to v16 */
        migrate_from_v14((const app_config_v14_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 13 && stored_size >= sizeof(app_config_v13_t)) {
        /* Version 13 blob — migrate to v15 */
        migrate_from_v13((const app_config_v13_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 12 && stored_size >= sizeof(app_config_v12_t)) {
        /* Version 12 blob — migrate to v14 */
        migrate_from_v12((const app_config_v12_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 11 && stored_size >= sizeof(app_config_v11_t)) {
        /* Version 11 blob — migrate to v12 */
        migrate_from_v11((const app_config_v11_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 10 && stored_size >= sizeof(app_config_v10_t)) {
        /* Version 10 blob — migrate to v11 */
        migrate_from_v10((const app_config_v10_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 9 && stored_size >= sizeof(app_config_v9_t)) {
        /* Version 9 blob — migrate to v10 */
        migrate_from_v9((const app_config_v9_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 8 && stored_size >= sizeof(app_config_v8_t)) {
        /* Version 8 blob — migrate to v9 */
        migrate_from_v8((const app_config_v8_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 7 && stored_size >= sizeof(app_config_v7_t)) {
        /* Version 7 blob — migrate to v8 */
        migrate_from_v7((const app_config_v7_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 6 && stored_size >= sizeof(app_config_v6_t)) {
        /* Version 6 blob — migrate to v7 */
        migrate_from_v6((const app_config_v6_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 5 && stored_size >= sizeof(app_config_v5_t)) {
        /* Version 5 blob — migrate to v6 */
        migrate_from_v5((const app_config_v5_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 4 && stored_size >= sizeof(app_config_v4_t)) {
        /* Version 4 blob — migrate to v5 */
        migrate_from_v4((const app_config_v4_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 3 && stored_size >= sizeof(app_config_v3_t)) {
        /* Version 3 blob — migrate to v4 */
        migrate_from_v3((const app_config_v3_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 2 && stored_size >= sizeof(app_config_v2_t)) {
        /* Version 2 blob — migrate to v3 */
        migrate_from_v2((const app_config_v2_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (version_check == 1 && stored_size >= sizeof(app_config_v1_t)) {
        /* Version 1 blob — migrate to v2 */
        migrate_from_v1((const app_config_v1_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (stored_size >= sizeof(app_config_v0_t) &&
               !(version_check > APP_CONFIG_VERSION && version_check < 0x1000)) {
        /* Likely legacy v0 blob — its first bytes are the wifi_ssid text, so
         * version_check is either 0 (empty SSID) or a large printable-ASCII
         * value (>= 0x1000). A small integer just above APP_CONFIG_VERSION is a
         * future/unknown version (e.g. a firmware downgrade), not a v0 blob:
         * let it fall through to the forward-tolerant branch below instead of
         * mis-migrating it as v0. */
        migrate_from_v0((const app_config_v0_t *)raw, &s_config);
        validate_config(&s_config);

        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    } else if (config_accept_forward(version_check, stored_size, APP_CONFIG_VERSION, sizeof(app_config_t))) {
        /* Forward-tolerant load — firmware downgrade case. The stored blob was
         * written by a NEWER firmware (version_check > APP_CONFIG_VERSION).
         * Because app_config_t fields are strictly append-only, this
         * firmware's entire struct layout is a known prefix of that blob, so
         * read it directly rather than falling through to the terminal
         * "unknown blob" branch, which would wipe the whole fleet's settings
         * back to factory defaults on every downgrade.
         *
         * Deliberately do NOT nvs_set_blob() here: writing back would
         * truncate the newer blob to this firmware's smaller struct size,
         * discarding the fields added after APP_CONFIG_VERSION. Leaving NVS
         * untouched means a subsequent re-upgrade finds the original newer
         * blob still intact and loads it with no data loss. */
        ESP_LOGW(TAG, "Config blob v%u is newer than firmware v%u; loading known prefix "
                      "(fields added after v%u unavailable until re-upgrade)",
                 (unsigned)version_check, (unsigned)APP_CONFIG_VERSION, (unsigned)APP_CONFIG_VERSION);
        memcpy(&s_config, raw, sizeof(app_config_t));
        s_config.config_version = APP_CONFIG_VERSION;
        validate_config(&s_config);
    } else {
        ESP_LOGW(TAG, "Unknown config blob (size=%d, ver=0x%08x), using defaults",
                 (int)stored_size, (unsigned)version_check);
        set_defaults(&s_config);
        nvs_set_blob(handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(handle);
    }

    /* Load the tiles caches from their NVS keys unless a migration branch above
     * already populated them (v50/v51). Missing keys -> "" (fresh device, or a
     * device from v49-and-earlier that never had tiles). */
    if (!tiles_loaded) {
        tiles_cache_load_key(handle, "json_tiles", s_json_tiles_cache);
        tiles_cache_load_key(handle, "ha_tiles",   s_ha_tiles_cache);
    }

    free(raw);
    nvs_close(handle);

    /* Warn at boot if admin password is still the factory default. */
    if (strcmp(s_config.admin_password, "changeme123!") == 0) {
        ESP_LOGW(TAG, "Admin password is set to factory default. "
                      "Change it via the web UI (Settings -> System -> Admin Password).");
    }
}

app_config_t *app_config_get(void) {
    return &s_config;
}

void app_config_normalize_nav_exclusivity(app_config_t *cfg) {
    /* Home-page-lock wins over both automatic modes: it holds the Home Page
     * unconditionally, so slideshow and idle override are cleared while set. */
    if (cfg->home_page_lock) {
        cfg->auto_rotate_enabled = false;
        cfg->idle_page_override_enabled = false;
    }
    if (cfg->auto_rotate_enabled && cfg->idle_page_override_enabled) {
        cfg->idle_page_override_enabled = false;
    }
}

void app_config_save(const app_config_t *config) {
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    invalidate_json_caches();

    memcpy(&s_config, config, sizeof(app_config_t));
    s_config.config_version = APP_CONFIG_VERSION;  // Always stamp current version
    validate_config(&s_config);

    app_config_normalize_nav_exclusivity(&s_config);

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
        s_config_dirty = false;
    }
    nvs_close(my_handle);

    xSemaphoreGive(s_config_mutex);
}

void app_config_get_snapshot_into(app_config_t *dst) {
    if (dst == NULL) {
        return;
    }
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    memcpy(dst, &s_config, sizeof(*dst));
    xSemaphoreGive(s_config_mutex);
}

void app_config_apply(const app_config_t *config) {
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    invalidate_json_caches();
    memcpy(&s_config, config, sizeof(app_config_t));
    s_config.config_version = APP_CONFIG_VERSION;
    validate_config(&s_config);
    s_config_dirty = true;
    xSemaphoreGive(s_config_mutex);
}

esp_err_t app_config_revert(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_config_revert: NVS open failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    size_t sz = sizeof(app_config_t);
    app_config_t *tmp = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!tmp) {
        nvs_close(handle);
        ESP_LOGE(TAG, "app_config_revert: malloc failed");
        return ESP_FAIL;
    }

    err = nvs_get_blob(handle, "config", tmp, &sz);
    nvs_close(handle);

    if (err != ESP_OK || sz != sizeof(app_config_t)) {
        free(tmp);
        ESP_LOGE(TAG, "app_config_revert: NVS read failed or size mismatch");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    invalidate_json_caches();
    memcpy(&s_config, tmp, sizeof(app_config_t));
    s_config_dirty = false;
    /* Reload tiles caches from NVS (revert to last-persisted tiles). The tiles
     * setter writes the NVS key immediately on POST, so the keys hold the
     * last-saved values; reload so cache == NVS after a revert. */
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            tiles_cache_load_key(h, "json_tiles", s_json_tiles_cache);
            tiles_cache_load_key(h, "ha_tiles",   s_ha_tiles_cache);
            nvs_close(h);
        }
    }
    xSemaphoreGive(s_config_mutex);
    free(tmp);

    ESP_LOGI(TAG, "Config reverted from NVS");
    return ESP_OK;
}

bool app_config_is_dirty(void) {
    return s_config_dirty;
}

void app_config_factory_reset(void) {
    invalidate_json_caches();
    ESP_LOGW(TAG, "Performing factory reset - erasing NVS partition");

    /* Erase the entire NVS partition. This also removes the tiles keys
     * "json_tiles"/"ha_tiles" along with "config"; the app_config_init() below
     * re-runs tiles_caches_alloc() (alloc-guarded: reuses buffers, resets to "")
     * and finds no keys, so both caches come up empty. */
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
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    fill_threshold_config(s_config.rms_thresholds[instance_index],
                          &s_rms_thresholds_cache[instance_index],
                          0.5f, 1.0f, out);
    xSemaphoreGive(s_config_mutex);
}

void app_config_get_hfr_threshold_config(int instance_index, threshold_config_t *out) {
    if (instance_index < 0 || instance_index >= MAX_NINA_INSTANCES) instance_index = 0;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    fill_threshold_config(s_config.hfr_thresholds[instance_index],
                          &s_hfr_thresholds_cache[instance_index],
                          2.0f, 3.5f, out);
    xSemaphoreGive(s_config_mutex);
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
    if (instance_index < 0 || instance_index >= MAX_NINA_INSTANCES) instance_index = 0;

    /* Runs on a poll-task stack: snapshot the config into a PSRAM heap copy
     * (~7.6 KB) rather than mutating live s_config field-by-field. The merge
     * works against the snapshot; app_config_save() commits it under the mutex
     * (which also invalidates the JSON caches). */
    app_config_t *snap = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!snap) {
        ESP_LOGE(TAG, "Filter sync: failed to allocate config snapshot");
        return;
    }
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    memcpy(snap, &s_config, sizeof(app_config_t));
    xSemaphoreGive(s_config_mutex);

    bool needs_save = false;
    char *field = snap->filter_colors[instance_index];

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
        app_config_save(snap);
        ESP_LOGI(TAG, "Filter config synced with API and saved to NVS");
    } else {
        ESP_LOGI(TAG, "Filter config already in sync with API");
    }
    free(snap);
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
