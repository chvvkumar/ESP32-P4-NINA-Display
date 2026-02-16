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

#include <string.h>
#include "app_config.h"
#include "web_server.h"
#include "esp_sntp.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_35
#define DEBOUNCE_MS      200

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Background heartbeat interval (slower than active polling)
#define HEARTBEAT_INTERVAL_MS 10000

static const char *TAG = "main";
static EventGroupHandle_t s_wifi_event_group;

// Shared state between input_task and data_update_task
static volatile int active_page = 0;
static volatile bool page_changed = false;
static int instance_count = 1;

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

        // Initialize SNTP for time synchronization
        ESP_LOGI(TAG, "Initializing SNTP");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();

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

/**
 * @brief Callback from dashboard swipe gesture — updates shared page state
 * Called from LVGL context (display lock already held by LVGL)
 */
static void on_page_changed(int new_page) {
    if (new_page != active_page) {
        active_page = new_page;
        page_changed = true;
        ESP_LOGI(TAG, "Swipe: switched to page %d", new_page);
    }
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
    int64_t last_press_ms = 0;

    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        if (level == 0 && last_level == 1) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_press_ms > DEBOUNCE_MS) {
                last_press_ms = now_ms;

                if (instance_count > 1) {
                    int new_page = (active_page + 1) % instance_count;
                    ESP_LOGI(TAG, "Button: switching to page %d", new_page);

                    // Update dashboard visibility (must hold display lock)
                    bsp_display_lock(0);
                    nina_dashboard_show_page(new_page, instance_count);
                    bsp_display_unlock();

                    active_page = new_page;
                    page_changed = true;
                }
            }
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Fetch prepared image from NINA, decode JPEG via hardware, and display as thumbnail
 * @return true if image was successfully fetched and displayed
 */
static bool fetch_and_show_thumbnail(const char *base_url) {
    size_t jpeg_size = 0;
    uint8_t *jpeg_buf = nina_client_fetch_prepared_image(base_url, 640, 640, 70, &jpeg_size);
    if (!jpeg_buf || jpeg_size == 0) {
        return false;
    }

    bool success = false;
    jpeg_decode_picture_info_t pic_info = {0};
    esp_err_t err = jpeg_decoder_get_info(jpeg_buf, jpeg_size, &pic_info);
    if (err == ESP_OK && pic_info.width > 0 && pic_info.height > 0) {
        bool is_gray = (pic_info.sample_method == JPEG_DOWN_SAMPLING_GRAY);
        // Output dimensions rounded up to multiples of 16 (JPEG MCU requirement)
        uint32_t out_w = ((pic_info.width + 15) / 16) * 16;
        uint32_t out_h = ((pic_info.height + 15) / 16) * 16;
        // Grayscale: 1 byte/pixel decode buffer; RGB565: 2 bytes/pixel
        uint32_t decode_buf_size = out_w * out_h * (is_gray ? 1 : 2);

        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t allocated_size = 0;
        uint8_t *decode_buf = (uint8_t *)jpeg_alloc_decoder_mem(decode_buf_size, &mem_cfg, &allocated_size);

        if (decode_buf) {
            jpeg_decoder_handle_t decoder = NULL;
            jpeg_decode_engine_cfg_t engine_cfg = {
                .intr_priority = 0,
                .timeout_ms = 5000,
            };
            err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
            if (err == ESP_OK && decoder) {
                jpeg_decode_cfg_t decode_cfg = {
                    .output_format = is_gray ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565,
                    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                };
                uint32_t out_size = 0;
                err = jpeg_decoder_process(decoder, &decode_cfg,
                    jpeg_buf, jpeg_size,
                    decode_buf, allocated_size, &out_size);
                jpeg_del_decoder_engine(decoder);

                if (err == ESP_OK && out_size > 0) {
                    uint8_t *rgb_buf = decode_buf;
                    uint32_t rgb_size = out_size;

                    if (is_gray) {
                        // Convert grayscale to RGB565 for LVGL display
                        uint32_t pixel_count = out_w * out_h;
                        rgb_size = pixel_count * 2;
                        rgb_buf = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
                        if (rgb_buf) {
                            uint16_t *dst = (uint16_t *)rgb_buf;
                            for (uint32_t i = 0; i < pixel_count; i++) {
                                uint8_t g = decode_buf[i];
                                // RGB565: 5-bit R, 6-bit G, 5-bit B
                                dst[i] = ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3);
                            }
                            free(decode_buf);
                            decode_buf = NULL;
                        } else {
                            ESP_LOGE(TAG, "Failed to alloc gray->RGB565 buffer");
                            free(decode_buf);
                            decode_buf = NULL;
                            free(jpeg_buf);
                            return false;
                        }
                    }

                    ESP_LOGI(TAG, "JPEG decoded: %lux%lu %s -> %lu bytes",
                        (unsigned long)pic_info.width, (unsigned long)pic_info.height,
                        is_gray ? "gray" : "color", (unsigned long)rgb_size);
                    bsp_display_lock(0);
                    nina_dashboard_set_thumbnail(rgb_buf, out_w, out_h, rgb_size);
                    bsp_display_unlock();
                    rgb_buf = NULL;  // ownership transferred
                    decode_buf = NULL;
                    success = true;
                } else {
                    ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
                }
            }
            if (decode_buf) free(decode_buf);
        }
    }

    free(jpeg_buf);
    return success;
}

static void data_update_task(void *arg) {
    nina_client_t instances[MAX_NINA_INSTANCES] = {0};
    nina_poll_state_t poll_states[MAX_NINA_INSTANCES];
    bool filters_synced[MAX_NINA_INSTANCES] = {false};
    int64_t last_heartbeat_ms[MAX_NINA_INSTANCES] = {0};

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        nina_poll_state_init(&poll_states[i]);
    }

    // Wait for WiFi
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi Connected, waiting for time sync...");

    // Wait for time to be set (up to 30 seconds)
    time_t now = 0;
    int retry = 0;
    const int max_retry = 30;
    while (now < 1577836800 && ++retry < max_retry) {  // Jan 1, 2020
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, max_retry);
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
    }

    if (now < 1577836800) {
        ESP_LOGW(TAG, "Time not set after %d seconds, continuing anyway", max_retry);
    } else {
        char strftime_buf[64];
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "System time set: %s", strftime_buf);
    }

    ESP_LOGI(TAG, "Starting data polling with %d instances", instance_count);

    // Start WebSocket for the initial active page
    const char *active_url = app_config_get_instance_url(active_page);
    if (strlen(active_url) > 0) {
        nina_websocket_start(active_url, &instances[active_page]);
    }

    int ws_instance = active_page;  // Track which instance has the WS connection

    while (1) {
        int current_active = active_page;  // Snapshot to avoid races

        // Handle page change: stop/start WebSocket, trigger immediate full poll
        if (page_changed) {
            page_changed = false;

            // Stop WebSocket for old instance
            nina_websocket_stop();

            // Start WebSocket for new active instance
            const char *new_url = app_config_get_instance_url(current_active);
            if (strlen(new_url) > 0) {
                nina_websocket_start(new_url, &instances[current_active]);
            }
            ws_instance = current_active;

            // Reset poll state for newly active instance to force full re-fetch
            nina_poll_state_init(&poll_states[current_active]);

            ESP_LOGI(TAG, "Page switched to %d, WebSocket restarted", current_active);
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        for (int i = 0; i < instance_count; i++) {
            const char *url = app_config_get_instance_url(i);
            if (strlen(url) == 0) continue;

            if (i == current_active) {
                // Active page: full tiered polling
                nina_client_poll(url, &instances[i], &poll_states[i]);
                ESP_LOGI(TAG, "Instance %d (active): connected=%d, status=%s, target=%s, ws=%d",
                    i + 1, instances[i].connected, instances[i].status,
                    instances[i].target_name, instances[i].websocket_connected);

                // Sync filters on first successful fetch (merge into global colors)
                if (!filters_synced[i] && instances[i].filter_count > 0) {
                    const char *names[MAX_FILTERS];
                    for (int f = 0; f < instances[i].filter_count; f++) {
                        names[f] = instances[i].filters[f].name;
                    }
                    app_config_sync_filters(names, instances[i].filter_count);
                    filters_synced[i] = true;
                }
            } else {
                // Background page: heartbeat-only polling (every 10s)
                if (now_ms - last_heartbeat_ms[i] >= HEARTBEAT_INTERVAL_MS) {
                    nina_client_poll_heartbeat(url, &instances[i]);
                    last_heartbeat_ms[i] = now_ms;
                    ESP_LOGD(TAG, "Instance %d (background): connected=%d",
                        i + 1, instances[i].connected);
                }
            }
        }

        // Update only the active page's UI
        bsp_display_lock(0);
        update_nina_dashboard_page(current_active, &instances[current_active]);
        bsp_display_unlock();

        // Handle thumbnail: initial request or auto-refresh on new image
        bool want_thumbnail = nina_dashboard_thumbnail_requested();
        bool auto_refresh = nina_dashboard_thumbnail_visible()
                            && instances[current_active].new_image_available;

        if (want_thumbnail || auto_refresh) {
            if (want_thumbnail) nina_dashboard_clear_thumbnail_request();
            if (auto_refresh) instances[current_active].new_image_available = false;

            const char *thumb_url = app_config_get_instance_url(current_active);
            if (strlen(thumb_url) > 0 && instances[current_active].connected) {
                if (!fetch_and_show_thumbnail(thumb_url) && want_thumbnail) {
                    // Only hide overlay on failure for manual requests, not auto-refresh
                    bsp_display_lock(0);
                    nina_dashboard_hide_thumbnail();
                    bsp_display_unlock();
                }
            }
        }

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

    // Determine instance count from config
    instance_count = app_config_get_instance_count();
    ESP_LOGI(TAG, "Configured instances: %d", instance_count);

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
    bsp_display_brightness_set(app_config_get()->brightness);

    bsp_display_lock(0);
    lv_obj_t *scr = lv_scr_act();
    create_nina_dashboard(scr, instance_count);
    bsp_display_unlock();

    // Register swipe callback: update shared state when gesture changes page
    if (instance_count > 1) {
        nina_dashboard_set_page_change_cb(on_page_changed);
    }

    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);
    xTaskCreate(data_update_task, "data_task", 20480, NULL, 5, NULL); // Increased stack for 3 instances
}
