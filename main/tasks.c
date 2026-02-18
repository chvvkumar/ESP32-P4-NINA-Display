/**
 * @file tasks.c
 * @brief FreeRTOS task implementations: data polling loop and button input task.
 */

#include "tasks.h"
#include "jpeg_utils.h"
#include "nina_client.h"
#include "nina_websocket.h"
#include "app_config.h"
#include "mqtt_ha.h"
#include "ui/nina_dashboard.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <time.h>

static const char *TAG = "tasks";

#define BOOT_BUTTON_GPIO    GPIO_NUM_35
#define DEBOUNCE_MS         200
#define HEARTBEAT_INTERVAL_MS 10000

/* Signals the data task that a page switch occurred */
volatile bool page_changed = false;

/**
 * @brief Swipe callback from the dashboard — signals the data task to re-tune polling.
 * Called from LVGL context (display lock already held by the gesture handler).
 * The dashboard has already updated its active_page before calling this.
 */
void on_page_changed(int new_page) {
    page_changed = true;
    ESP_LOGI(TAG, "Swipe: switched to page %d", new_page);
}

void input_task(void *arg) {
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
                    int current = nina_dashboard_get_active_page();
                    int new_page = (current + 1) % instance_count;
                    ESP_LOGI(TAG, "Button: switching to page %d", new_page);

                    bsp_display_lock(0);
                    nina_dashboard_show_page(new_page, instance_count);
                    bsp_display_unlock();

                    page_changed = true;
                }
            }
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void data_update_task(void *arg) {
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

    // Start MQTT if enabled
    mqtt_ha_start();

    ESP_LOGI(TAG, "Starting data polling with %d instances", instance_count);

    // Start WebSocket for all configured instances concurrently
    for (int i = 0; i < instance_count; i++) {
        const char *url = app_config_get_instance_url(i);
        if (strlen(url) > 0) {
            nina_websocket_start(i, url, &instances[i]);
        }
    }

    while (1) {
        int current_active = nina_dashboard_get_active_page();  // Snapshot to avoid races

        // Re-read instance count from config so API URL changes take effect live
        instance_count = app_config_get_instance_count();

        // Handle page change: trigger immediate full poll for the new active page
        // (WebSocket connections for all instances remain persistent)
        if (page_changed) {
            page_changed = false;
            nina_poll_state_init(&poll_states[current_active]);
            ESP_LOGI(TAG, "Page switched to %d", current_active);
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        // Read WiFi RSSI once per cycle
        int rssi = -100;
        {
            wifi_ap_record_t ap_info = {0};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi;
            }
        }

        // Signal that an API call is starting — pulse the connection dot
        bsp_display_lock(0);
        nina_dashboard_update_status(current_active, rssi,
                                     instances[current_active].connected, true);
        bsp_display_unlock();

        for (int i = 0; i < instance_count; i++) {
            const char *url = app_config_get_instance_url(i);
            if (strlen(url) == 0) continue;

            if (i == current_active) {
                nina_client_poll(url, &instances[i], &poll_states[i]);
                ESP_LOGI(TAG, "Instance %d (active): connected=%d, status=%s, target=%s, ws=%d",
                    i + 1, instances[i].connected, instances[i].status,
                    instances[i].target_name, instances[i].websocket_connected);

                // Sync filters on first successful fetch
                if (!filters_synced[i] && instances[i].filter_count > 0) {
                    const char *names[MAX_FILTERS];
                    for (int f = 0; f < instances[i].filter_count; f++) {
                        names[f] = instances[i].filters[f].name;
                    }
                    app_config_sync_filters(names, instances[i].filter_count, i);
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

        // Update active page UI; stop the pulse now that the call is done
        bsp_display_lock(0);
        update_nina_dashboard_page(current_active, &instances[current_active]);
        nina_dashboard_update_status(current_active, rssi,
                                     instances[current_active].connected, false);
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
                    bsp_display_lock(0);
                    nina_dashboard_hide_thumbnail();
                    bsp_display_unlock();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
