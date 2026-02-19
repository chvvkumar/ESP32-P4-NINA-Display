#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "ui/nina_dashboard.h"
#include "app_config.h"
#include "web_server.h"
#include "mqtt_ha.h"
#include "tasks.h"

#include <string.h>

static const char *TAG = "main";

/* Shared globals — used by event_handler() and tasks.c */
EventGroupHandle_t s_wifi_event_group;
int instance_count = 1;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "Initializing SNTP");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        const char *ntp = app_config_get()->ntp_server;
        esp_sntp_setservername(0, (ntp[0] != '\0') ? ntp : "pool.ntp.org");
        esp_sntp_init();

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    app_config_t *app_cfg = app_config_get();

    wifi_config_t wifi_config_sta = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(app_cfg->wifi_ssid) > 0) {
        strncpy((char *)wifi_config_sta.sta.ssid,     app_cfg->wifi_ssid, sizeof(wifi_config_sta.sta.ssid));
        strncpy((char *)wifi_config_sta.sta.password, app_cfg->wifi_pass, sizeof(wifi_config_sta.sta.password));
    }

    wifi_config_t wifi_config_ap = {
        .ap = {
            .ssid = "AllSky-Config",
            .ssid_len = strlen("AllSky-Config"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config_sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init finished.");
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    app_config_init();

    instance_count = app_config_get_instance_count();
    ESP_LOGI(TAG, "Configured instances: %d", instance_count);

    wifi_init();
    start_web_server();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    bsp_display_brightness_set(app_config_get()->brightness);

    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        lv_obj_t *scr = lv_scr_act();
        create_nina_dashboard(scr, instance_count);
        {
            /* Apply persisted page override immediately on boot.
             * Page 0 = summary (default), NINA pages start at 1, so offset by 1. */
            app_config_t *cfg = app_config_get();
            if (cfg->active_page_override >= 0 && cfg->active_page_override < instance_count) {
                nina_dashboard_show_page(cfg->active_page_override + 1, instance_count);
            }
        }
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire display lock during init!");
    }

    nina_dashboard_set_page_change_cb(on_page_changed);

    xTaskCreate(input_task,       "input_task", 4096,  NULL, 5, NULL);
    xTaskCreate(data_update_task, "data_task",  20480, NULL, 5, NULL);
}
