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
#include "ui/nina_summary.h"
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

                {
                    int total = nina_dashboard_get_total_page_count();
                    int current = nina_dashboard_get_active_page();
                    int new_page = (current + 1) % total;
                    ESP_LOGI(TAG, "Button: switching to page %d", new_page);

                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_dashboard_show_page(new_page, total);
                        bsp_display_unlock();
                    } else {
                        ESP_LOGW(TAG, "Display lock timeout (button page switch)");
                    }

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
    nina_poll_state_t poll_states[MAX_NINA_INSTANCES] = {0};
    bool filters_synced[MAX_NINA_INSTANCES] = {false};
    int64_t last_heartbeat_ms[MAX_NINA_INSTANCES] = {0};
    int64_t last_rotate_ms = 0;

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        nina_poll_state_init(&poll_states[i]);
        nina_client_init_mutex(&instances[i]);
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
        bool on_sysinfo = nina_dashboard_is_sysinfo_page();
        bool on_summary = nina_dashboard_is_summary_page();

        /*
         * Page index convention:
         *   0                    = summary page
         *   1 .. instance_count  = NINA instance pages (instance i at page i+1)
         *   instance_count + 1   = sysinfo page
         *
         * active_nina_idx: the 0-based NINA instance index for the active page,
         *   or -1 if on summary/sysinfo.
         */
        int active_nina_idx = -1;
        if (!on_sysinfo && !on_summary && current_active >= 1)
            active_nina_idx = current_active - 1;

        // Re-read instance count from config so API URL changes take effect live
        instance_count = app_config_get_instance_count();

        // Handle page change: trigger immediate full poll for the new active page
        // (WebSocket connections for all instances remain persistent)
        if (page_changed) {
            page_changed = false;
            /* Background polling keeps poll_states warm for all instances,
             * so no need to reinitialize — pre-fetched data is preserved. */
            last_rotate_ms = esp_timer_get_time() / 1000;  // Reset auto-rotate timer on any page change
            ESP_LOGI(TAG, "Page switched to %d%s%s", current_active,
                     on_sysinfo ? " (sysinfo)" : "", on_summary ? " (summary)" : "");
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        /* Auto-rotate logic — rotates between pages selected by auto_rotate_pages bitmask.
         *
         * Page bitmask layout:
         *   bit 0  = Summary page (page index 0)
         *   bit 1  = NINA instance 1 (page index 1)
         *   bit 2  = NINA instance 2 (page index 2)
         *   bit 3  = NINA instance 3 (page index 3)
         *   bit 4  = System Info page (page index instance_count + 1)
         */
        {
            app_config_t *r_cfg = app_config_get();
            if (r_cfg->auto_rotate_enabled && r_cfg->auto_rotate_interval_s > 0) {
                if (last_rotate_ms == 0) last_rotate_ms = now_ms;
                if (now_ms - last_rotate_ms >= (int64_t)r_cfg->auto_rotate_interval_s * 1000) {
                    uint8_t page_mask = r_cfg->auto_rotate_pages;
                    int total = nina_dashboard_get_total_page_count();

                    /* Find next page in rotation by stepping through candidates */
                    int next_page = current_active;
                    for (int step = 1; step < total; step++) {
                        int candidate = (current_active + step) % total;

                        /* Check if this candidate is in the rotation bitmask */
                        bool in_mask = false;
                        if (candidate == 0)
                            in_mask = (page_mask & 0x01) != 0;             /* Summary */
                        else if (candidate >= 1 && candidate <= instance_count)
                            in_mask = (page_mask & (1 << candidate)) != 0; /* NINA page */
                        else if (candidate == instance_count + 1)
                            in_mask = (page_mask & 0x10) != 0;             /* Sysinfo */

                        if (!in_mask) continue;

                        /* Skip disconnected NINA instances if configured */
                        if (r_cfg->auto_rotate_skip_disconnected
                            && candidate >= 1 && candidate <= instance_count) {
                            int nina_idx = candidate - 1;
                            if (!instances[nina_idx].connected) continue;
                        }

                        next_page = candidate;
                        break;
                    }

                    if (next_page != current_active) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_dashboard_show_page_animated(next_page, instance_count, r_cfg->auto_rotate_effect);
                            bsp_display_unlock();
                        } else {
                            ESP_LOGW(TAG, "Display lock timeout (auto-rotate)");
                        }
                        page_changed = true;
                        ESP_LOGI(TAG, "Auto-rotate: switched to page %d", next_page);
                    }
                    last_rotate_ms = now_ms;
                }
            }
        }

        // Read WiFi RSSI once per cycle
        int rssi = -100;
        {
            wifi_ap_record_t ap_info = {0};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi;
            }
        }

        /* Pulse connection dot on the active NINA page (not summary/sysinfo) */
        if (active_nina_idx >= 0 && active_nina_idx < instance_count) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_update_status(active_nina_idx, rssi,
                                             instances[active_nina_idx].connected, true);
                bsp_display_unlock();
            }
        }

        for (int i = 0; i < instance_count; i++) {
            const char *url = app_config_get_instance_url(i);
            if (strlen(url) == 0) continue;

            /* Full poll when: on summary page (all instances) or on this instance's NINA page */
            if (on_summary || i == active_nina_idx) {
                nina_client_poll(url, &instances[i], &poll_states[i]);
                if (instances[i].connected)
                    instances[i].last_successful_poll_ms = now_ms;
                ESP_LOGI(TAG, "Instance %d (%s): connected=%d, status=%s, target=%s, ws=%d",
                    i + 1, on_summary ? "summary" : "active",
                    instances[i].connected, instances[i].status,
                    instances[i].target_name, instances[i].websocket_connected);
            } else {
                // Background instance: pre-fetch slow-changing data (every 10s)
                if (now_ms - last_heartbeat_ms[i] >= HEARTBEAT_INTERVAL_MS) {
                    nina_client_poll_background(url, &instances[i], &poll_states[i]);
                    if (instances[i].connected)
                        instances[i].last_successful_poll_ms = now_ms;
                    last_heartbeat_ms[i] = now_ms;
                    ESP_LOGD(TAG, "Instance %d (background): connected=%d",
                        i + 1, instances[i].connected);
                }
            }

            // Sync filters on first successful fetch (foreground or background)
            if (!filters_synced[i] && instances[i].filter_count > 0) {
                const char *names[MAX_FILTERS];
                for (int f = 0; f < instances[i].filter_count; f++) {
                    names[f] = instances[i].filters[f].name;
                }
                app_config_sync_filters(names, instances[i].filter_count, i);
                filters_synced[i] = true;
            }

            // Check WebSocket reconnection with exponential backoff
            nina_websocket_check_reconnect(i, url, &instances[i]);
        }

        /* Update summary page when visible — lock each instance while reading */
        if (on_summary) {
            for (int i = 0; i < instance_count; i++)
                nina_client_lock(&instances[i], 100);
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                summary_page_update(instances, instance_count);
                bsp_display_unlock();
            }
            for (int i = 0; i < instance_count; i++)
                nina_client_unlock(&instances[i]);
        }

        /* Update active NINA page UI */
        if (active_nina_idx >= 0 && active_nina_idx < instance_count) {
            nina_client_lock(&instances[active_nina_idx], 100);
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                update_nina_dashboard_page(active_nina_idx, &instances[active_nina_idx]);
                nina_dashboard_update_status(active_nina_idx, rssi,
                                             instances[active_nina_idx].connected, false);
                bsp_display_unlock();
            }
            nina_client_unlock(&instances[active_nina_idx]);

            // Handle thumbnail: initial request or auto-refresh on new image
            bool want_thumbnail = nina_dashboard_thumbnail_requested();
            bool auto_refresh = false;
            if (nina_client_lock(&instances[active_nina_idx], 100)) {
                auto_refresh = nina_dashboard_thumbnail_visible()
                               && instances[active_nina_idx].new_image_available;
                if (auto_refresh) instances[active_nina_idx].new_image_available = false;
                nina_client_unlock(&instances[active_nina_idx]);
            }

            if (want_thumbnail || auto_refresh) {
                if (want_thumbnail) nina_dashboard_clear_thumbnail_request();

                const char *thumb_url = app_config_get_instance_url(active_nina_idx);
                if (strlen(thumb_url) > 0 && instances[active_nina_idx].connected) {
                    if (!fetch_and_show_thumbnail(thumb_url) && want_thumbnail) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_dashboard_hide_thumbnail();
                            bsp_display_unlock();
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
