#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of NINA instances supported
#define MAX_NINA_INSTANCES 3

typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char api_url_1[128];
    char api_url_2[128];
    char api_url_3[128];
    char ntp_server[64];
    char filter_colors[512];     // JSON string: {"L":"#60a5fa","R":"#ef4444","G":"#10b981","B":"#3b82f6","Ha":"#f43f5e","Sii":"#a855f7","Oiii":"#06b6d4"}
    char rms_thresholds_1[256];  // JSON per instance 1: {"good_max":0.5,"ok_max":1.0,"good_color":"#10b981","ok_color":"#eab308","bad_color":"#ef4444"}
    char rms_thresholds_2[256];  // JSON per instance 2
    char rms_thresholds_3[256];  // JSON per instance 3
    char hfr_thresholds_1[256];  // JSON per instance 1: {"good_max":2.0,"ok_max":3.5,"good_color":"#10b981","ok_color":"#eab308","bad_color":"#ef4444"}
    char hfr_thresholds_2[256];  // JSON per instance 2
    char hfr_thresholds_3[256];  // JSON per instance 3
    int theme_index;             // Index of the selected theme
    int brightness;              // Display brightness 0-100 (default 50)
    int color_brightness;        // Global color brightness for dynamic elements 0-100 (default 100)
} app_config_t;

void app_config_init(void);
app_config_t *app_config_get(void);
void app_config_save(const app_config_t *config);
int app_config_get_instance_count(void);
const char *app_config_get_instance_url(int index);
void app_config_factory_reset(void);
uint32_t app_config_get_filter_color(const char *filter_name);
uint32_t app_config_get_rms_color(float rms_value, int instance_index);
uint32_t app_config_get_hfr_color(float hfr_value, int instance_index);
void app_config_sync_filters(const char *filter_names[], int count);
uint32_t app_config_apply_brightness(uint32_t color, int brightness);

#ifdef __cplusplus
}
#endif
