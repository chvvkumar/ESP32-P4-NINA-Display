#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "driver/gpio.h"
#include "ui/nina_dashboard.h"
#include "nina_client.h"
#include "esp_wifi.h"
#include "esp_event.h"

#include "app_config.h"
#include "web_server.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_35

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "main";
static EventGroupHandle_t s_wifi_event_group;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void)
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
        strncpy((char*)wifi_config_sta.sta.ssid, app_cfg->wifi_ssid, sizeof(wifi_config_sta.sta.ssid));
        strncpy((char*)wifi_config_sta.sta.password, app_cfg->wifi_pass, sizeof(wifi_config_sta.sta.password));
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
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init finished.");
}

static void input_task(void *arg) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    int last_level = 1;
    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        if (level == 0 && last_level == 1) {
            ESP_LOGI(TAG, "Button pressed");
            // Future: Could toggle between instances or trigger actions
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

static void data_update_task(void *arg) {
    nina_client_t d1 = {0};

    // Wait for WiFi
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi Connected, starting data polling");

    app_config_t *cfg = app_config_get();

    while (1) {
        if (strlen(cfg->api_url_1) > 0) {
            ESP_LOGI(TAG, "Polling NINA instance 1...");
            nina_client_get_data(cfg->api_url_1, &d1);
            ESP_LOGI(TAG, "Instance 1: connected=%d, status=%s, target=%s", d1.connected, d1.status, d1.target_name);
        }

        bsp_display_lock(0);
        update_nina_dashboard_ui(&d1);
        bsp_display_unlock();

        vTaskDelay(pdMS_TO_TICKS(2000)); // Poll every 2 seconds
    }
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

    // Initialize Config
    app_config_init();

    // Init WiFi
    wifi_init();

    // Start Web Server
    start_web_server();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    bsp_display_brightness_set(50);

    bsp_display_lock(0);
    lv_obj_t *scr = lv_scr_act();
    create_nina_dashboard(scr);
    bsp_display_unlock();

    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);
    xTaskCreate(data_update_task, "data_task", 8192, NULL, 5, NULL); // Increased stack for HTTP client
}