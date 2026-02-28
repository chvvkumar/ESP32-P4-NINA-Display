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
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "ui/nina_dashboard.h"
#include "ui/nina_toast.h"
#include "ui/nina_event_log.h"
#include "ui/nina_alerts.h"
#include "ui/nina_safety.h"
#include "ui/nina_session_stats.h"
#include "app_config.h"
#include "web_server.h"
#include "mqtt_ha.h"
#include "tasks.h"
#include "esp_ota_ops.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "perf_monitor.h"
#include "nina_connection.h"

static void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }

static const char *TAG = "main";

/* Shared globals — used by event_handler() and tasks.c */
EventGroupHandle_t s_wifi_event_group;
int instance_count = 1;

/* WiFi reconnection backoff */
static int wifi_retry_count = 0;
static esp_timer_handle_t wifi_reconnect_timer = NULL;
static const int wifi_backoff_ms[] = {1000, 2000, 5000, 10000, 30000};
#define WIFI_BACKOFF_STEPS (sizeof(wifi_backoff_ms) / sizeof(wifi_backoff_ms[0]))

static void wifi_reconnect_cb(void *arg) {
    ESP_LOGI(TAG, "WiFi reconnect attempt %d", wifi_retry_count);
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* STA lost — re-enable the config AP so the user can reconfigure */
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_STA) {
            ESP_LOGI(TAG, "STA disconnected, re-enabling AP");
            esp_wifi_set_mode(WIFI_MODE_APSTA);
        }

        int idx = wifi_retry_count < (int)WIFI_BACKOFF_STEPS
                  ? wifi_retry_count : (int)WIFI_BACKOFF_STEPS - 1;
        int delay = wifi_backoff_ms[idx];
        wifi_retry_count++;
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting in %d ms (attempt %d)", delay, wifi_retry_count);
        esp_timer_start_once(wifi_reconnect_timer, (uint64_t)delay * 1000);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;

        /* STA connected — disable the config AP to keep the air clean */
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_APSTA) {
            ESP_LOGI(TAG, "STA connected, disabling AP");
            esp_wifi_set_mode(WIFI_MODE_STA);
        }

        /* Apply timezone before SNTP so localtime_r works as soon as time is set */
        const char *tz = app_config_get()->tz_string;
        if (tz[0] != '\0') {
            setenv("TZ", tz, 1);
            tzset();
            ESP_LOGI(TAG, "Timezone set to: %s", tz);
        }

        /* Guard against double-init on WiFi reconnect */
        if (!esp_sntp_enabled()) {
            ESP_LOGI(TAG, "Initializing SNTP");
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            const char *ntp = app_config_get()->ntp_server;
            esp_sntp_setservername(0, (ntp[0] != '\0') ? ntp : "pool.ntp.org");
            esp_sntp_init();
        }

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

    // Create WiFi reconnection backoff timer
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = wifi_reconnect_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &wifi_reconnect_timer));

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

    /*
     * STA credentials are NOT stored in app_config — they live in ESP-IDF's
     * own WiFi NVS namespace (auto-persisted by esp_wifi_set_config and
     * auto-restored on esp_wifi_start).  The web server POST handler calls
     * esp_wifi_set_config directly when the user saves new credentials.
     */

    wifi_config_t wifi_config_ap = {
        .ap = {
            .ssid = "NINA-DISPLAY",
            .ssid_len = strlen("NINA-DISPLAY"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
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

    /* Route all cJSON allocations to PSRAM to reduce internal heap pressure */
    cJSON_Hooks psram_hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = free };
    cJSON_InitHooks(&psram_hooks);

    /* Suppress verbose ESP-IDF HTTP/TLS transport errors — connection failures
     * are already reported cleanly by nina_client as "unreachable" messages. */
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);

    app_config_init();
    nina_connection_init();

    perf_monitor_init(30);
    perf_monitor_set_enabled(app_config_get()->debug_mode);

    instance_count = app_config_get_instance_count();
    ESP_LOGI(TAG, "Configured instances: %d", instance_count);

    wifi_init();
    start_web_server();

    /* Pre-allocate JPEG encoder DMA channel before display init claims DMA resources */
    screenshot_encoder_init();

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

    /* Initialize session stats (PSRAM allocation, no LVGL) */
    nina_session_stats_init();

    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        lv_obj_t *scr = lv_scr_act();
        create_nina_dashboard(scr, instance_count);

        /* Initialize notification overlays (must be after dashboard so they float on top) */
        nina_toast_init(scr);
        nina_event_log_overlay_create(scr);
        nina_alerts_init(scr);
        nina_safety_create(scr);

        {
            /* Apply persisted page override immediately on boot.
             * Override stores absolute page index: 0=summary, 1..N=NINA, N+1=settings, N+2=sysinfo */
            app_config_t *cfg = app_config_get();
            int total = nina_dashboard_get_total_page_count();
            if (cfg->active_page_override >= 0 && cfg->active_page_override < total) {
                nina_dashboard_show_page(cfg->active_page_override, 0);
            }
        }
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire display lock during init!");
    }

    nina_dashboard_set_page_change_cb(on_page_changed);

    nina_client_init();  // DNS cache mutex — must be called before poll tasks spawn

    /* Allocate task stacks in PSRAM to save internal heap; TCBs stay internal */
    {
        StackType_t *input_stack = heap_caps_malloc(4096 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *input_tcb  = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (input_stack && input_tcb) {
            xTaskCreateStaticPinnedToCore(input_task, "input_task", 4096, NULL, 5,
                                          input_stack, input_tcb, 0);
        } else {
            ESP_LOGE(TAG, "Failed to alloc input_task stack from PSRAM, falling back");
            if (input_stack) heap_caps_free(input_stack);
            if (input_tcb) heap_caps_free(input_tcb);
            xTaskCreatePinnedToCore(input_task, "input_task", 4096, NULL, 5, NULL, 0);
        }

        StackType_t *data_stack = heap_caps_malloc(12288 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *data_tcb  = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (data_stack && data_tcb) {
            data_task_handle = xTaskCreateStaticPinnedToCore(data_update_task, "data_task", 12288, NULL, 5,
                                                             data_stack, data_tcb, 1);
        } else {
            ESP_LOGE(TAG, "Failed to alloc data_task stack from PSRAM, falling back");
            if (data_stack) heap_caps_free(data_stack);
            if (data_tcb) heap_caps_free(data_tcb);
            xTaskCreatePinnedToCore(data_update_task, "data_task", 12288, NULL, 5, &data_task_handle, 1);
        }
    }

    /* Mark this firmware as valid so the bootloader won't roll back.
     * This must come after successful init — if we crash before here,
     * the bootloader will revert to the previous OTA partition. */
    esp_ota_mark_app_valid_cancel_rollback();
}
