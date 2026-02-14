#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char api_url_1[128];
    char api_url_2[128];
    char ntp_server[64];
} app_config_t;

void app_config_init(void);
app_config_t *app_config_get(void);
void app_config_save(const app_config_t *config);

#ifdef __cplusplus
}
#endif
