#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of NINA instances supported
#define MAX_NINA_INSTANCES 3

// Standardized LVGL display lock timeout (ms).
// All callers should use this instead of 0 (infinite) to prevent deadlocks.
#define LVGL_LOCK_TIMEOUT_MS 1000

// Current config struct version — bump on every layout change.
#define APP_CONFIG_VERSION 3

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
} app_config_t;

// WiFi credentials are NOT stored in app_config_t. They are managed by
// ESP-IDF's WiFi NVS subsystem via esp_wifi_set_config() and auto-restored
// on boot. The AP provides headless access for initial/ongoing configuration.

void app_config_init(void);
app_config_t *app_config_get(void);
app_config_t app_config_get_snapshot(void);
void app_config_save(const app_config_t *config);
int app_config_get_instance_count(void);
const char *app_config_get_instance_url(int index);
void app_config_factory_reset(void);
uint32_t app_config_get_filter_color(const char *filter_name, int instance_index);
uint32_t app_config_get_rms_color(float rms_value, int instance_index);
uint32_t app_config_get_hfr_color(float hfr_value, int instance_index);
void app_config_sync_filters(const char *filter_names[], int count, int instance_index);
uint32_t app_config_apply_brightness(uint32_t color, int brightness);

#ifdef __cplusplus
}
#endif
