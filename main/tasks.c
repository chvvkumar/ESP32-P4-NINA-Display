/**
 * @file tasks.c
 * @brief FreeRTOS task implementations: data polling loop and button input task.
 */

#include "tasks.h"
#include "jpeg_utils.h"
#include "nina_client.h"
#include "nina_api_fetchers.h"
#include "nina_websocket.h"
#include "nina_connection.h"
#include "app_config.h"
#include "mqtt_ha.h"
#include "ui/nina_dashboard.h"
#include "ui/nina_summary.h"
#include "ui/nina_sysinfo.h"
#include "ui/nina_graph_overlay.h"
#include "ui/nina_info_overlay.h"
#include "ui/nina_safety.h"
#include "ui/nina_alerts.h"
#include "ui/nina_session_stats.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include <time.h>
#include "perf_monitor.h"

static const char *TAG = "tasks";

#define BOOT_BUTTON_GPIO    GPIO_NUM_35
#define DEBOUNCE_MS         200
#define HEARTBEAT_INTERVAL_MS 10000
/* Graph refresh interval read from config at runtime (graph_update_interval_s) */

/* Signals the data task that a page switch occurred */
volatile bool page_changed = false;
volatile bool ota_in_progress = false;
volatile bool screen_touch_wake = false;
volatile bool screen_asleep = false;
TaskHandle_t data_task_handle = NULL;
TaskHandle_t poll_task_handles[MAX_NINA_INSTANCES] = {NULL};
static int64_t last_graph_fetch_ms = 0;  /* Timestamp of last graph data fetch */
static bool hfr_graph_seeded = false;   /* True after initial API fetch for current HFR graph session */

/* Per-instance poll contexts (shared between UI coordinator and poll tasks) */
static instance_poll_ctx_t poll_contexts[MAX_NINA_INSTANCES];

/**
 * @brief Swipe callback from the dashboard — signals the data task to re-tune polling.
 * Called from LVGL context (display lock already held by the gesture handler).
 * The dashboard has already updated its active_page before calling this.
 */
void on_page_changed(int new_page) {
    page_changed = true;
    ESP_LOGI(TAG, "Swipe: switched to page %d", new_page);
    /* Wake all poll tasks so the newly-active one can start full polling immediately */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (poll_task_handles[i]) xTaskNotifyGive(poll_task_handles[i]);
    }
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

                /* Wake screen first if asleep — don't switch pages while dark */
                if (screen_asleep) {
                    screen_touch_wake = true;
                    ESP_LOGI(TAG, "Button: waking screen");
                } else {
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

// =============================================================================
// Per-Instance Poll Task — blocks independently on HTTP for its own instance
// =============================================================================

void instance_poll_task(void *arg) {
    instance_poll_ctx_t *ctx = (instance_poll_ctx_t *)arg;
    int idx = ctx->index;

    // Wait for WiFi
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    // Boot probe: check connectivity immediately
    {
        const char *url = app_config_get_instance_url(idx);
        if (strlen(url) > 0 && app_config_is_instance_enabled(idx)) {
            nina_connection_set_connecting(idx);
            if (nina_client_dns_check(url)) {
                nina_client_poll_heartbeat(url, ctx->client, idx);
                ESP_LOGI(TAG, "Poll[%d]: boot probe connected=%d", idx + 1, ctx->client->connected);
                // Start WebSocket after successful boot probe
                if (nina_connection_is_connected(idx)) {
                    nina_websocket_start(idx, url, ctx->client);
                }
            } else {
                ESP_LOGW(TAG, "Poll[%d]: boot DNS failed for %s", idx + 1, url);
                nina_connection_report_poll(idx, false);
            }
        }
    }

    while (!ctx->shutdown) {
        // Suspend during OTA
        while (ota_in_progress && !ctx->shutdown) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (ctx->shutdown) break;

        const char *url = app_config_get_instance_url(idx);

        // Skip disabled or unconfigured instances
        if (strlen(url) == 0 || !app_config_is_instance_enabled(idx)) {
            ctx->client->connected = false;
            nina_connection_report_poll(idx, false);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Check deferred camera-disconnect alerts
        nina_websocket_check_deferred_alerts(idx);

        // DNS pre-check
        if (!nina_client_dns_check(url)) {
            ctx->client->connected = false;
            nina_connection_report_poll(idx, false);
            ESP_LOGD(TAG, "Poll[%d]: DNS failed, skipping", idx + 1);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        // Poll based on active/background mode
        if (ctx->is_active) {
            nina_client_poll(url, ctx->client, ctx->poll_state, idx);
            if (nina_connection_is_connected(idx))
                ctx->client->last_successful_poll_ms = now_ms;
            ESP_LOGI(TAG, "Poll[%d] (active): connected=%d, status=%s, target=%s, ws=%d",
                idx + 1, ctx->client->connected, ctx->client->status,
                ctx->client->target_name, ctx->client->websocket_connected);
        } else {
            if (now_ms - ctx->last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
                nina_client_poll_background(url, ctx->client, ctx->poll_state, idx);
                if (nina_connection_is_connected(idx))
                    ctx->client->last_successful_poll_ms = now_ms;
                ctx->last_heartbeat_ms = now_ms;
                ESP_LOGD(TAG, "Poll[%d] (background): connected=%d", idx + 1, ctx->client->connected);
            }
        }

        // Sync filters on first successful fetch
        if (!ctx->filters_synced && ctx->client->filter_count > 0) {
            const char *names[MAX_FILTERS];
            for (int f = 0; f < ctx->client->filter_count; f++)
                names[f] = ctx->client->filters[f].name;
            app_config_sync_filters(names, ctx->client->filter_count, idx);
            ctx->filters_synced = true;
        }

        // WebSocket reconnection with exponential backoff
        nina_websocket_check_reconnect(idx, url, ctx->client);

        // Sleep: active = update_rate_s cycle, background = heartbeat interval
        uint16_t cycle_ms;
        if (ctx->is_active) {
            cycle_ms = (uint16_t)app_config_get()->update_rate_s * 1000;
            if (cycle_ms < 1000) cycle_ms = 1000;
        } else {
            cycle_ms = HEARTBEAT_INTERVAL_MS;
        }
        // Use task notification to allow early wake (page change, WS event)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cycle_ms));
    }

    // Cleanup on shutdown — clear handle BEFORE stopping WS/deleting task
    // to prevent TOCTOU race (WS handler could xTaskNotifyGive a stale handle).
    poll_task_handles[idx] = NULL;
    nina_websocket_stop(idx);
    ESP_LOGI(TAG, "Poll[%d]: task shutdown", idx + 1);
    vTaskDelete(NULL);
}

// =============================================================================
// UI Coordinator Task — fast loop, never blocks on HTTP data polling
// =============================================================================

void data_update_task(void *arg) {
    data_task_handle = xTaskGetCurrentTaskHandle();

    /* Allocate large per-instance structs in PSRAM instead of the task stack
     * to reduce internal heap usage (~7.6 KB saved, allowing smaller stack). */
    nina_client_t *instances = heap_caps_calloc(MAX_NINA_INSTANCES, sizeof(nina_client_t), MALLOC_CAP_SPIRAM);
    nina_poll_state_t *poll_states = heap_caps_calloc(MAX_NINA_INSTANCES, sizeof(nina_poll_state_t), MALLOC_CAP_SPIRAM);
    if (!instances || !poll_states) {
        ESP_LOGE(TAG, "Failed to allocate instance data from PSRAM");
        if (instances) heap_caps_free(instances);
        if (poll_states) heap_caps_free(poll_states);
        vTaskDelete(NULL);
        return;
    }

    int64_t last_rotate_ms = 0;

    /* Screen sleep state */
    int64_t all_disconnected_since_ms = 0;  /* 0 = at least one connected recently */

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        nina_poll_state_init(&poll_states[i]);
        nina_client_init_mutex(&instances[i]);
        /* Allocate per-instance HFR ring buffer in PSRAM (~4 KB per instance) */
        instances[i].hfr_ring.hfr   = heap_caps_calloc(HFR_RING_SIZE, sizeof(float), MALLOC_CAP_SPIRAM);
        instances[i].hfr_ring.stars = heap_caps_calloc(HFR_RING_SIZE, sizeof(int),   MALLOC_CAP_SPIRAM);
    }

    /* Allocate graph data in PSRAM to avoid ~10 KB stack pressure */
    graph_rms_data_t *rms_data = heap_caps_calloc(1, sizeof(graph_rms_data_t), MALLOC_CAP_SPIRAM);
    graph_hfr_data_t *hfr_data = heap_caps_calloc(1, sizeof(graph_hfr_data_t), MALLOC_CAP_SPIRAM);
    if (!rms_data || !hfr_data) {
        ESP_LOGE(TAG, "Failed to allocate graph data from PSRAM");
        if (rms_data) heap_caps_free(rms_data);
        if (hfr_data) heap_caps_free(hfr_data);
        vTaskDelete(NULL);
        return;
    }

    /* Initialize per-instance poll contexts */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        poll_contexts[i].index = i;
        poll_contexts[i].client = &instances[i];
        poll_contexts[i].poll_state = &poll_states[i];
        poll_contexts[i].task_handle = NULL;
        poll_contexts[i].is_active = false;
        poll_contexts[i].shutdown = false;
        poll_contexts[i].filters_synced = false;
        poll_contexts[i].last_heartbeat_ms = 0;
    }

    // Wait for WiFi
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi Connected, waiting for time sync...");

    // Check if time is already set; if not, SNTP will sync in the background
    {
        time_t now_t = 0;
        time(&now_t);
        if (now_t >= 1577836800) {  // Jan 1, 2020
            char strftime_buf[64];
            struct tm timeinfo;
            localtime_r(&now_t, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "System time already set: %s", strftime_buf);
        } else {
            ESP_LOGI(TAG, "System time not yet set, SNTP will sync in background");
        }
    }

    // Start MQTT if enabled
    mqtt_ha_start();

    instance_count = app_config_get_instance_count();
    ESP_LOGI(TAG, "Spawning %d per-instance poll tasks", instance_count);

    /* Spawn per-instance poll tasks (boot probe + WS start happen inside each task) */
    for (int i = 0; i < instance_count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "poll_%d", i);
        /* Use xTaskCreateStatic with PSRAM-allocated stack to save internal heap */
        StackType_t *stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        StaticTask_t *tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (stack && tcb) {
            poll_contexts[i].task_handle = xTaskCreateStatic(
                instance_poll_task, name, 8192, &poll_contexts[i], 4, stack, tcb);
            poll_task_handles[i] = poll_contexts[i].task_handle;
        } else {
            ESP_LOGE(TAG, "Failed to allocate poll task %d stack from PSRAM", i);
            if (stack) heap_caps_free(stack);
            if (tcb) heap_caps_free(tcb);
        }
    }

    while (1) {
        /* Suspend polling during OTA update */
        while (ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // ── Perf: Track effective cycle interval ──
        {
            static int64_t prev_cycle_start = 0;
            int64_t cycle_now = esp_timer_get_time();
            if (prev_cycle_start > 0) {
                perf_timer_record(&g_perf.effective_cycle_interval, cycle_now - prev_cycle_start);
            }
            prev_cycle_start = cycle_now;
        }
        perf_timer_start(&g_perf.poll_cycle_total);

        int current_active = nina_dashboard_get_active_page();  // Snapshot to avoid races
        bool on_sysinfo = nina_dashboard_is_sysinfo_page();
        bool on_settings = nina_dashboard_is_settings_page();
        bool on_summary = nina_dashboard_is_summary_page();

        /*
         * Page index convention (page_count = enabled instances only):
         *   0                    = summary page
         *   1 .. page_count      = NINA instance pages (mapped via page_instance_map)
         *   page_count + 1       = settings page
         *   page_count + 2       = sysinfo page
         *
         * active_nina_idx: the actual instance index (0..MAX_NINA_INSTANCES-1)
         *   for the active page, or -1 if on summary/settings/sysinfo.
         */
        int active_nina_idx = -1;   /* Actual instance index (for data access) */
        int active_page_idx = -1;  /* 0-based page index into pages[] (for UI calls) */
        if (!on_sysinfo && !on_settings && !on_summary && current_active >= 1) {
            active_page_idx = current_active - 1;
            active_nina_idx = nina_dashboard_page_to_instance(active_page_idx);
        }

        // Re-read instance count from config so API URL changes take effect live
        instance_count = app_config_get_instance_count();

        // Check for debug mode toggle
        {
            static bool last_debug_mode = false;
            bool current_debug = app_config_get()->debug_mode;
            if (current_debug != last_debug_mode) {
                perf_monitor_set_enabled(current_debug);
                last_debug_mode = current_debug;
            }
        }

        // Handle page change
        if (page_changed) {
            page_changed = false;
            last_rotate_ms = esp_timer_get_time() / 1000;  // Reset auto-rotate timer on any page change
            ESP_LOGI(TAG, "Page switched to %d%s%s%s", current_active,
                     on_sysinfo ? " (sysinfo)" : "", on_settings ? " (settings)" : "",
                     on_summary ? " (summary)" : "");

            /* Immediate summary render with cached data */
            if (on_summary) {
                bool locked[MAX_NINA_INSTANCES];
                for (int j = 0; j < instance_count; j++)
                    locked[j] = nina_client_lock(&instances[j], 100);
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    summary_page_update(instances, instance_count);
                    bsp_display_unlock();
                }
                for (int j = 0; j < instance_count; j++)
                    if (locked[j]) nina_client_unlock(&instances[j]);
            }
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        /* Update active/background flags for poll tasks.
         * On summary page all instances are active; on a NINA page only that instance is. */
        for (int i = 0; i < instance_count; i++) {
            bool should_active = (on_summary || i == active_nina_idx);
            bool was_active = poll_contexts[i].is_active;
            poll_contexts[i].is_active = should_active;
            /* Wake poll task immediately when transitioning to active */
            if (should_active && !was_active && poll_task_handles[i]) {
                xTaskNotifyGive(poll_task_handles[i]);
            }
        }

        /* Auto-rotate logic — rotates between pages selected by auto_rotate_pages bitmask.
         *
         * Page bitmask layout:
         *   bit 0  = Summary page (page index 0)
         *   bit 1  = NINA instance 1 (page index 1)
         *   bit 2  = NINA instance 2 (page index 2)
         *   bit 3  = NINA instance 3 (page index 3)
         *   bit 4  = System Info page (page index instance_count + 2)
         *   bit 5  = Settings page (page index instance_count + 1)
         */
        {
            app_config_t *r_cfg = app_config_get();
            if (r_cfg->auto_rotate_enabled && r_cfg->auto_rotate_interval_s > 0) {
                if (last_rotate_ms == 0) last_rotate_ms = now_ms;
                if (now_ms - last_rotate_ms >= (int64_t)r_cfg->auto_rotate_interval_s * 1000) {
                    uint8_t page_mask = r_cfg->auto_rotate_pages;
                    int total = nina_dashboard_get_total_page_count();

                    int ena_page_count = total - 3;  /* enabled NINA pages only */

                    /* Find next page in rotation by stepping through candidates */
                    int next_page = current_active;
                    for (int step = 1; step < total; step++) {
                        int candidate = (current_active + step) % total;

                        /* Check if this candidate is in the rotation bitmask.
                         * Bitmask bits 1-3 refer to instance indices (not page indices),
                         * so use the page-to-instance mapping for NINA pages. */
                        bool in_mask = false;
                        if (candidate == 0)
                            in_mask = (page_mask & 0x01) != 0;             /* Summary */
                        else if (candidate >= 1 && candidate <= ena_page_count) {
                            int inst = nina_dashboard_page_to_instance(candidate - 1);
                            if (inst >= 0)
                                in_mask = (page_mask & (1 << (inst + 1))) != 0; /* NINA page */
                        }
                        else if (candidate == ena_page_count + 1)
                            in_mask = (page_mask & 0x20) != 0;             /* Settings */
                        else if (candidate == ena_page_count + 2)
                            in_mask = (page_mask & 0x10) != 0;             /* Sysinfo */

                        if (!in_mask) continue;

                        /* Skip disconnected NINA instances if configured */
                        if (candidate >= 1 && candidate <= ena_page_count) {
                            int nina_idx = nina_dashboard_page_to_instance(candidate - 1);
                            if (nina_idx >= 0 && r_cfg->auto_rotate_skip_disconnected
                                && !nina_connection_is_connected(nina_idx)) continue;
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
        if (active_nina_idx >= 0 && active_page_idx >= 0) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_update_status(active_page_idx, rssi,
                                             nina_connection_is_connected(active_nina_idx), true);
                bsp_display_unlock();
            }
        }

        /* Update sysinfo page when visible — refreshes at the same rate as data polling */
        if (on_sysinfo) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                sysinfo_page_refresh();
                bsp_display_unlock();
            }
        }

        /* Update summary page when visible — lock each instance while reading */
        if (on_summary) {
            bool locked[MAX_NINA_INSTANCES];
            for (int j = 0; j < instance_count; j++)
                locked[j] = nina_client_lock(&instances[j], 100);

            perf_timer_start(&g_perf.ui_update_total);
            int64_t lock_start = g_perf.enabled ? esp_timer_get_time() : 0;
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                if (g_perf.enabled) perf_timer_record(&g_perf.ui_lock_wait, esp_timer_get_time() - lock_start);
                perf_timer_start(&g_perf.ui_summary_update);
                summary_page_update(instances, instance_count);
                perf_timer_stop(&g_perf.ui_summary_update);

                bsp_display_unlock();
            }
            perf_timer_stop(&g_perf.ui_update_total);

            for (int j = 0; j < instance_count; j++)
                if (locked[j]) nina_client_unlock(&instances[j]);
        }

        /* Update active NINA page UI */
        if (active_nina_idx >= 0 && active_page_idx >= 0) {
            nina_client_lock(&instances[active_nina_idx], 100);

            perf_timer_start(&g_perf.ui_update_total);
            int64_t lock_start2 = g_perf.enabled ? esp_timer_get_time() : 0;
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                if (g_perf.enabled) perf_timer_record(&g_perf.ui_lock_wait, esp_timer_get_time() - lock_start2);
                perf_timer_start(&g_perf.ui_dashboard_update);
                update_nina_dashboard_page(active_page_idx, &instances[active_nina_idx]);
                perf_timer_stop(&g_perf.ui_dashboard_update);

                // Measure WS-to-UI latency if a recent event was received
                if (g_perf.enabled && g_perf.last_ws_event_time_us > 0) {
                    int64_t latency = esp_timer_get_time() - g_perf.last_ws_event_time_us;
                    if (latency < 5000000) {  // Only if within 5 seconds (not stale)
                        perf_timer_record(&g_perf.latency_ws_to_ui, latency);
                    }
                    g_perf.last_ws_event_time_us = 0;  // Reset after measuring
                }

                nina_dashboard_update_status(active_page_idx, rssi,
                                             nina_connection_is_connected(active_nina_idx), false);
                bsp_display_unlock();
            }
            perf_timer_stop(&g_perf.ui_update_total);

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
                if (strlen(thumb_url) > 0 && nina_connection_is_connected(active_nina_idx)) {
                    if (!fetch_and_show_thumbnail(thumb_url) && want_thumbnail) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_dashboard_hide_thumbnail();
                            bsp_display_unlock();
                        }
                    }
                }
            }

            /* Reset HFR graph seed flag when graph is hidden */
            if (!nina_graph_visible()) {
                hfr_graph_seeded = false;
            }

            /* Auto-refresh graph at defined interval while visible */
            if (nina_graph_visible() && !nina_graph_requested()) {
                int64_t now_graph = esp_timer_get_time() / 1000;
                int graph_interval_ms = (int)app_config_get()->graph_update_interval_s * 1000;
                if (now_graph - last_graph_fetch_ms >= graph_interval_ms) {
                    nina_graph_set_refresh_pending();
                }
            }

            /* Handle graph overlay data fetch */
            if (nina_graph_requested()) {
                nina_graph_clear_request();
                const char *graph_url = app_config_get_instance_url(active_nina_idx);
                if (strlen(graph_url) > 0 && nina_connection_is_connected(active_nina_idx)) {
                    graph_type_t gtype = nina_graph_get_type();
                    int gpoints = nina_graph_get_requested_points();

                    if (gtype == GRAPH_TYPE_RMS) {
                        memset(rms_data, 0, sizeof(*rms_data));
                        fetch_guider_graph(graph_url, rms_data, gpoints);
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_graph_set_rms_data(rms_data);
                            bsp_display_unlock();
                        }
                    } else {
                        memset(hfr_data, 0, sizeof(*hfr_data));
                        /* HFR graph: initial open fetches from API to get historical data.
                         * Auto-refreshes use the local ring buffer (populated by WS events),
                         * eliminating the expensive /image-history?all=true fetch (50-400 KB). */
                        if (!hfr_graph_seeded) {
                            fetch_hfr_history(graph_url, hfr_data, gpoints);
                            hfr_graph_seeded = true;
                        } else if (nina_client_lock(&instances[active_nina_idx], 100)) {
                            build_hfr_from_ring(&instances[active_nina_idx], hfr_data, gpoints);
                            nina_client_unlock(&instances[active_nina_idx]);
                        }
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_graph_set_hfr_data(hfr_data);
                            bsp_display_unlock();
                        }
                    }
                }
                last_graph_fetch_ms = esp_timer_get_time() / 1000;
            }

            /* Auto-refresh autofocus overlay while visible (data comes from WebSocket) */
            if (nina_info_overlay_visible()
                && nina_info_overlay_get_type() == INFO_OVERLAY_AUTOFOCUS
                && !nina_info_overlay_requested()) {
                if (nina_client_lock(&instances[active_nina_idx], 100)) {
                    autofocus_data_t af_data = instances[active_nina_idx].autofocus;
                    nina_client_unlock(&instances[active_nina_idx]);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_info_overlay_set_autofocus_data(&af_data);
                        bsp_display_unlock();
                    }
                }
            }

            /* Auto-refresh session stats overlay (on-device data, updates each poll) */
            if (nina_info_overlay_visible()
                && nina_info_overlay_get_type() == INFO_OVERLAY_SESSION_STATS
                && !nina_info_overlay_requested()) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    nina_info_overlay_set_session_stats(active_nina_idx);
                    bsp_display_unlock();
                }
            }

            /* Handle info overlay data fetch */
            if (nina_info_overlay_requested()) {
                nina_info_overlay_clear_request();
                info_overlay_type_t itype = nina_info_overlay_get_type();

                /* Session stats uses on-device data — no API fetch needed */
                if (itype == INFO_OVERLAY_SESSION_STATS) {
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_info_overlay_set_session_stats(active_nina_idx);
                        bsp_display_unlock();
                    }
                } else {
                const char *info_url = app_config_get_instance_url(active_nina_idx);
                if (strlen(info_url) > 0 && nina_connection_is_connected(active_nina_idx)) {

                    if (itype == INFO_OVERLAY_CAMERA) {
                        camera_detail_data_t cam_data = {0};
                        fetch_camera_details(info_url, &cam_data);
                        fetch_weather_details(info_url, &cam_data);
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_camera_data(&cam_data);
                            bsp_display_unlock();
                        }
                    } else if (itype == INFO_OVERLAY_MOUNT) {
                        mount_detail_data_t mount_data = {0};
                        fetch_mount_details(info_url, &mount_data);
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_mount_data(&mount_data);
                            bsp_display_unlock();
                        }
                    } else if (itype == INFO_OVERLAY_IMAGESTATS) {
                        if (nina_client_lock(&instances[active_nina_idx], 100)) {
                            imagestats_detail_data_t stats = instances[active_nina_idx].last_image_stats;
                            nina_client_unlock(&instances[active_nina_idx]);
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_info_overlay_set_imagestats_data(&stats);
                                bsp_display_unlock();
                            }
                        }
                    } else if (itype == INFO_OVERLAY_SEQUENCE) {
                        sequence_detail_data_t seq_data = {0};
                        fetch_sequence_details(info_url, &seq_data);
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_sequence_data(&seq_data);
                            bsp_display_unlock();
                        }
                    } else if (itype == INFO_OVERLAY_FILTER) {
                        filter_detail_data_t filt_data = {0};
                        if (nina_client_lock(&instances[active_nina_idx], 100)) {
                            snprintf(filt_data.current_filter, sizeof(filt_data.current_filter), "%s", instances[active_nina_idx].current_filter);
                            filt_data.filter_count = instances[active_nina_idx].filter_count;
                            for (int f = 0; f < filt_data.filter_count && f < 10; f++) {
                                strncpy(filt_data.filters[f].name, instances[active_nina_idx].filters[f].name, sizeof(filt_data.filters[f].name) - 1);
                                filt_data.filters[f].id = instances[active_nina_idx].filters[f].id;
                                if (strcmp(filt_data.current_filter, filt_data.filters[f].name) == 0) {
                                    filt_data.current_position = filt_data.filters[f].id;
                                }
                            }
                            filt_data.connected = true;
                            nina_client_unlock(&instances[active_nina_idx]);
                        }
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_filter_data(&filt_data);
                            bsp_display_unlock();
                        }
                    } else if (itype == INFO_OVERLAY_AUTOFOCUS) {
                        if (nina_client_lock(&instances[active_nina_idx], 100)) {
                            autofocus_data_t af_data = instances[active_nina_idx].autofocus;
                            nina_client_unlock(&instances[active_nina_idx]);
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_info_overlay_set_autofocus_data(&af_data);
                                bsp_display_unlock();
                            }
                        }
                    }
                }
                } /* else (non-session-stats overlay) */
            }
        }

        // ── Event-driven UI refresh: check if any WS event needs immediate UI update ──
        if (active_nina_idx >= 0 && active_page_idx >= 0
            && instances[active_nina_idx].ui_refresh_needed) {
            instances[active_nina_idx].ui_refresh_needed = false;
            if (nina_client_lock(&instances[active_nina_idx], 50)) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    update_nina_dashboard_page(active_page_idx, &instances[active_nina_idx]);
                    nina_dashboard_update_status(active_page_idx, rssi,
                                                 nina_connection_is_connected(active_nina_idx), false);
                    bsp_display_unlock();
                }
                nina_client_unlock(&instances[active_nina_idx]);
            }
        }

        // ── Session stats recording + safety monitor + RMS/HFR alerts ──
        // Lock each instance briefly to read a consistent snapshot of scalar fields.
        for (int i = 0; i < instance_count; i++) {
            if (!nina_connection_is_connected(i)) continue;

            float rms_total, hfr, cam_temp, cooler_pwr;
            int stars;
            bool safety_conn, safety_safe;

            if (nina_client_lock(&instances[i], 50)) {
                rms_total  = instances[i].guider.rms_total;
                hfr        = instances[i].hfr;
                cam_temp   = instances[i].camera.temp;
                stars      = instances[i].stars;
                cooler_pwr = instances[i].camera.cooler_power;
                safety_conn = instances[i].safety_connected;
                safety_safe = instances[i].safety_is_safe;
                nina_client_unlock(&instances[i]);
            } else {
                continue;  // Skip this instance if lock contended
            }

            nina_session_stats_record(i, rms_total, hfr, cam_temp, stars, cooler_pwr);

            if (safety_conn) {
                nina_safety_update(true, safety_safe);
            }

            if (app_config_get()->alert_flash_enabled) {
                if (rms_total > 0.0f) {
                    threshold_config_t rms_cfg;
                    app_config_get_rms_threshold_config(i, &rms_cfg);
                    if (rms_cfg.ok_max > 0.0f && rms_total > rms_cfg.ok_max) {
                        nina_alert_trigger(ALERT_RMS, i, rms_total);
                    }
                }

                if (hfr > 0.0f) {
                    threshold_config_t hfr_cfg;
                    app_config_get_hfr_threshold_config(i, &hfr_cfg);
                    if (hfr_cfg.ok_max > 0.0f && hfr > hfr_cfg.ok_max) {
                        nina_alert_trigger(ALERT_HFR, i, hfr);
                    }
                }
            }
        }

        /* ── Screen sleep: turn off backlight when no NINA instances connected ── */
        {
            app_config_t *sl_cfg = app_config_get();
            if (sl_cfg->screen_sleep_enabled) {
                int connected = nina_connection_connected_count();

                /* Touch wake — always check first, even if connections are back */
                if (screen_asleep && screen_touch_wake) {
                    bsp_display_brightness_set(sl_cfg->brightness);
                    screen_asleep = false;
                    screen_touch_wake = false;
                    all_disconnected_since_ms = now_ms;  /* restart sleep timer */
                    ESP_LOGI(TAG, "Screen wake: touch detected");
                }

                if (connected > 0) {
                    all_disconnected_since_ms = 0;
                    if (screen_asleep) {
                        bsp_display_brightness_set(sl_cfg->brightness);
                        screen_asleep = false;
                        ESP_LOGI(TAG, "Screen wake: NINA connected");
                    }
                } else {
                    /* All disconnected */
                    if (all_disconnected_since_ms == 0) {
                        all_disconnected_since_ms = now_ms;
                    }
                    if (!screen_asleep &&
                        (now_ms - all_disconnected_since_ms >= (int64_t)sl_cfg->screen_sleep_timeout_s * 1000)) {
                        /* Turn off backlight completely via direct LEDC.
                         * BSP brightness_set(0) only dims to 47% due to offset mapping.
                         * With output_invert=1, duty=0 → GPIO HIGH → backlight off
                         * (backlight is active-low on this board). */
                        ledc_set_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH, 0);
                        ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH);
                        screen_asleep = true;
                        ESP_LOGI(TAG, "Screen sleep: no NINA connections for %ds",
                                 sl_cfg->screen_sleep_timeout_s);
                    }
                }
            } else if (screen_asleep) {
                /* Feature disabled while asleep — wake up */
                bsp_display_brightness_set(sl_cfg->brightness);
                screen_asleep = false;
                all_disconnected_since_ms = 0;
                ESP_LOGI(TAG, "Screen wake: sleep feature disabled");
            }
        }

        // ── Perf: End cycle, capture memory, periodic report ──
        perf_timer_stop(&g_perf.poll_cycle_total);
        perf_monitor_capture_memory();
        if (g_perf.enabled) {
            g_perf.data_task_stack_hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
            if (esp_timer_get_time() - g_perf.last_report_time_us >=
                (int64_t)g_perf.report_interval_s * 1000000) {
                perf_monitor_capture_cpu();
                perf_monitor_report();
            }
        }

        // UI coordinator loop delay — no HTTP blocking, so this always fires on time.
        // Use task notification to allow WS events to wake us for immediate UI refresh.
        {
            uint16_t cycle_ms = (uint16_t)app_config_get()->update_rate_s * 1000;
            if (cycle_ms < 1000) cycle_ms = 1000;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cycle_ms));
        }
    }
}
