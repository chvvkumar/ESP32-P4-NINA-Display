#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "display_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of NINA instances supported
#define MAX_NINA_INSTANCES 3

// Current config struct version — bump on every layout change.
#define APP_CONFIG_VERSION 13

#define WIDGET_STYLE_COUNT 7

typedef struct {
    uint32_t config_version;        // Must be first field — used to detect legacy blobs
    char api_url[3][128];           // API base URLs per instance
    char ntp_server[64];
    char tz_string[64];             // POSIX TZ string (e.g. "EST5EDT,M3.2.0,M11.1.0")
    char filter_colors[3][512];     // JSON filter color map per instance: {"L":"#787878","R":"#991b1b",...}
    char rms_thresholds[3][256];    // JSON RMS threshold config per instance
    char hfr_thresholds[3][256];    // JSON HFR threshold config per instance
    int theme_index;                // Index of the selected theme
    int brightness;                 // Display brightness 0-100 (default 50)
    int color_brightness;           // Global color brightness for dynamic elements 0-100 (default 100)
    bool mqtt_enabled;              // Enable MQTT Home Assistant integration
    char mqtt_broker_url[128];      // MQTT broker URL (e.g. "mqtt://192.168.1.100")
    char mqtt_username[64];         // MQTT broker username
    char mqtt_password[64];         // MQTT broker password
    char mqtt_topic_prefix[64];     // MQTT topic prefix (default "ninadisplay")
    uint16_t mqtt_port;             // MQTT broker port (default 1883)
    int8_t   active_page_override;          // -1 = auto, 0 = summary, 1..N = NINA instance, N+1 = sysinfo
    bool     auto_rotate_enabled;            // enable automatic page rotation
    uint16_t auto_rotate_interval_s;        // seconds between automatic page rotations
    uint8_t  auto_rotate_effect;            // 0 = instant, 1 = fade, 2 = slide-left, 3 = slide-right
    bool     auto_rotate_skip_disconnected; // skip pages where NINA is not connected during auto-rotate
    uint8_t  auto_rotate_pages;            // bitmask: bit0=summary, bit1-3=NINA 1-3, bit4=sysinfo
    uint8_t  update_rate_s;                // UI/data update interval in seconds (1-10, default 2)
    uint8_t  graph_update_interval_s;     // Graph overlay auto-refresh interval in seconds (2-30, default 5)
    uint8_t  connection_timeout_s;        // Seconds without successful poll before marking offline (2-30, default 6)
    uint8_t  toast_duration_s;           // Toast notification display duration in seconds (3-30, default 8)
    bool     debug_mode;                // Runtime debug/perf profiling toggle (default false)
    bool     instance_enabled[3];       // Per-instance enable flag (disabled = skip polling/WS)
    bool     screen_sleep_enabled;     // Turn off display when no NINA instances connected
    uint16_t screen_sleep_timeout_s;   // Seconds with 0 connections before screen off (default 60)
    bool     alert_flash_enabled;     // Enable border flash alerts for RMS/HFR/safety events (default true)
    uint8_t  idle_poll_interval_s;   // Heartbeat poll interval while screen sleeping (5-120, default 30)
    bool     wifi_power_save;        // Enable WiFi modem sleep for power savings (default true)
    uint8_t  widget_style;           // Widget panel style index (0-6, default 0)
    uint8_t  auto_update_check;     // 0=disabled, 1=enabled (check GitHub for firmware updates on boot)
    uint8_t  update_channel;        // 0=stable releases only, 1=include pre-releases
} app_config_t;

// WiFi credentials are NOT stored in app_config_t. They are managed by
// ESP-IDF's WiFi NVS subsystem via esp_wifi_set_config() and auto-restored
// on boot. The AP provides headless access for initial/ongoing configuration.

void app_config_init(void);
app_config_t *app_config_get(void);
app_config_t app_config_get_snapshot(void);
void app_config_save(const app_config_t *config);
void app_config_apply(const app_config_t *config);   // in-memory only, no NVS
esp_err_t app_config_revert(void);                    // reload NVS into memory
bool app_config_is_dirty(void);                       // true if apply called without save
int app_config_get_instance_count(void);
const char *app_config_get_instance_url(int index);
void app_config_factory_reset(void);
bool app_config_is_instance_enabled(int index);
int app_config_get_enabled_instance_count(void);
uint32_t app_config_get_filter_color(const char *filter_name, int instance_index);
uint32_t app_config_get_rms_color(float rms_value, int instance_index);
uint32_t app_config_get_hfr_color(float hfr_value, int instance_index);

// Threshold configuration (values + colors) for graph overlay display
typedef struct {
    float good_max;
    float ok_max;
    uint32_t good_color;
    uint32_t ok_color;
    uint32_t bad_color;
} threshold_config_t;

void app_config_get_rms_threshold_config(int instance_index, threshold_config_t *out);
void app_config_get_hfr_threshold_config(int instance_index, threshold_config_t *out);
void app_config_sync_filters(const char *filter_names[], int count, int instance_index);
uint32_t app_config_apply_brightness(uint32_t color, int brightness);

#ifdef __cplusplus
}
#endif
