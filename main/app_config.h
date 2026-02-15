#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char api_url_1[128];
    char api_url_2[128];
    char ntp_server[64];
    char filter_colors[512];  // JSON string: {"L":"#60a5fa","R":"#ef4444","G":"#10b981","B":"#3b82f6","Ha":"#f43f5e","Sii":"#a855f7","Oiii":"#06b6d4"}
    char rms_thresholds[256]; // JSON: {"good_max":0.5,"ok_max":1.0,"good_color":"#10b981","ok_color":"#eab308","bad_color":"#ef4444"}
    char hfr_thresholds[256]; // JSON: {"good_max":2.0,"ok_max":3.5,"good_color":"#10b981","ok_color":"#eab308","bad_color":"#ef4444"}
    char filter_brightness[256]; // JSON: {"L":100,"R":80,...} per-filter brightness 0-100
    int theme_index;          // Index of the selected theme
    int brightness;           // Display brightness 0-100 (default 50)
    int color_brightness;     // Global color brightness for dynamic elements 0-100 (default 100)
} app_config_t;

void app_config_init(void);
app_config_t *app_config_get(void);
void app_config_save(const app_config_t *config);
void app_config_factory_reset(void);
uint32_t app_config_get_filter_color(const char *filter_name);
uint32_t app_config_get_rms_color(float rms_value);
uint32_t app_config_get_hfr_color(float hfr_value);
void app_config_sync_filters(const char *filter_names[], int count);
uint32_t app_config_apply_brightness(uint32_t color, int brightness);

#ifdef __cplusplus
}
#endif
