#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app_config";
static app_config_t s_config;
static const char *NVS_NAMESPACE = "app_conf";

void app_config_init(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle! Using defaults.", esp_err_to_name(err));
        // Set defaults
        memset(&s_config, 0, sizeof(app_config_t));
        strcpy(s_config.wifi_ssid, "");
        strcpy(s_config.wifi_pass, "");
        strcpy(s_config.api_url_1, "http://astromele2.lan:1888/v2/api/");
        strcpy(s_config.api_url_2, "http://astromele3.lan:1888/v2/api/");
        strcpy(s_config.ntp_server, "pool.ntp.org");
        return;
    }

    size_t required_size = sizeof(app_config_t);
    err = nvs_get_blob(my_handle, "config", &s_config, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config not found in NVS, using defaults");
        memset(&s_config, 0, sizeof(app_config_t));
        strcpy(s_config.wifi_ssid, "");
        strcpy(s_config.wifi_pass, "");
        strcpy(s_config.api_url_1, "http://astromele2.lan:1888/v2/api/");
        strcpy(s_config.api_url_2, "http://astromele3.lan:1888/v2/api/");
        strcpy(s_config.ntp_server, "pool.ntp.org");
        
        // Save defaults so we have them next time
        nvs_set_blob(my_handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(my_handle);
    }

    nvs_close(my_handle);
}

app_config_t *app_config_get(void) {
    return &s_config;
}

void app_config_save(const app_config_t *config) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for saving: %s", esp_err_to_name(err));
        return;
    }

    // Update static copy
    memcpy(&s_config, config, sizeof(app_config_t));

    err = nvs_set_blob(my_handle, "config", &s_config, sizeof(app_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Config saved");
        nvs_commit(my_handle);
    }
    nvs_close(my_handle);
}
