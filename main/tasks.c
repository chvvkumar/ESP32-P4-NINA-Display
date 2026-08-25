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
#include "allsky_client.h"
#include "json_client.h"
#include "ha_client.h"
#include "octoprint_client.h"
#include "adsb_client.h"
#include "spotify_auth.h"
#include "spotify_client.h"
#include "app_config.h"
#include "mqtt_ha.h"
#include "ui/nina_dashboard.h"
#include "ui/nina_dashboard_internal.h"
#include "ui/nina_layout_alt.h"
#include "ui/nina_summary.h"
#include "ui/nina_sysinfo.h"
#include "ui/nina_allsky.h"
#include "ui/nina_json.h"
#include "ui/nina_ha.h"
#include "ui/nina_octoprint.h"
#include "ui/nina_adsb.h"
#include "ui/nina_spotify.h"
#include "ui/nina_graph_overlay.h"
#include "ui/nina_info_overlay.h"
#include "ui/nina_net_debug.h"
#include "ui/nina_safety.h"
#include "ui/nina_alerts.h"
#include "ui/nina_session_stats.h"
#include "ui/nina_ota_prompt.h"
#include "ui/nina_nav_arbiter.h"
#include "ui/nina_image_page.h"
#include "ui/nina_wait_overlay.h"
#include "ui/nina_toast.h"
#include "ota_github.h"
#include "esp_ota_ops.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <time.h>
#include <math.h>
#include <stdatomic.h>     /* atomic_exchange — read-and-clear WS event flags */
#include "perf_monitor.h"
#include "power_mgmt.h"
#include "crash_log.h"
#include "demo_data.h"
#include "weather_client.h"
#include "freertos/queue.h"
#include "ui/nina_thumbnail.h"
#include "poll_task.h"
#include "net_trace.h"
#include "wifi_manager.h"   /* wifi_apply_tx_power — re-applied on screen-sleep wake */

static const char *TAG = "tasks";

/* ── Async fetch queues (Core 0 worker ↔ Core 1 UI coordinator) ── */
static QueueHandle_t s_fetch_queue = NULL;        /* fetch_request_t */
static QueueHandle_t s_fetch_result_queue = NULL;  /* fetch_result_t */
#define FETCH_QUEUE_LEN      4
#define FETCH_RESULT_QUEUE_LEN 4

#define BOOT_BUTTON_GPIO    GPIO_NUM_35
#define HEARTBEAT_INTERVAL_MS 10000
/* Graph refresh interval read from config at runtime (graph_update_interval_s) */

/* Signals the data task that a page switch occurred */
_Atomic bool page_changed = false;
_Atomic bool ota_in_progress = false;

/* ── PSRAM static task spawn ─────────────────────────────────────────────────
 * Every background task in this file has the same spawn shape: PSRAM stack +
 * internal-RAM TCB + xTaskCreateStaticPinnedToCore, freeing both on a partial
 * allocation failure. Internal heap is the scarce resource (SDIO WiFi RX DMA
 * competes for it), so the stack must come from SPIRAM; the TCB must not,
 * FreeRTOS touches it from contexts where PSRAM access is not guaranteed.
 *
 * @p depth is passed to FreeRTOS unchanged AND sizes the buffer as
 * depth * sizeof(StackType_t), which is exactly right and not a 4x waste:
 * ESP-IDF's RISC-V port defines portSTACK_TYPE as uint8_t
 * (components/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos/portmacro.h:99),
 * so StackType_t is one byte and the FreeRTOS "stack depth" argument is in
 * BYTES here, not words as in vanilla FreeRTOS. The kernel sizes the same way
 * (FreeRTOS-Kernel/tasks.c:1044, ulStackDepth * sizeof(StackType_t)), so the
 * buffer and the task's usable stack are identical. Do NOT divide the
 * allocation by sizeof(StackType_t) "to save PSRAM" — that would hand every
 * task a stack a quarter of the declared size and let it run off the end.
 *
 * Returns the new handle, or NULL if allocation failed (nothing was spawned).
 */
TaskHandle_t psram_task_spawn(TaskFunction_t fn, const char *name,
                              uint32_t depth, void *arg,
                              UBaseType_t prio, BaseType_t core)
{
    StackType_t  *stack = heap_caps_malloc(depth * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb   = heap_caps_calloc(1, sizeof(StaticTask_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stack || !tcb) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM stack for task '%s'", name);
        if (stack) heap_caps_free(stack);
        if (tcb) heap_caps_free(tcb);
        return NULL;
    }
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(fn, name, depth, arg, prio,
                                                   stack, tcb, core);
    ESP_LOGI(TAG, "Task '%s' spawned on core %d", name, (int)core);
    return h;
}

/* Spawn @p fn once and publish its handle into *@p handle.
 *
 * @p mux guards the check-and-publish against a concurrent caller: these are
 * reachable both from the boot task and from an httpd worker on a runtime
 * enable. The task-create itself cannot run inside the critical section (it
 * allocates and takes kernel locks), so the check and the publish are two
 * separate critical sections — the same narrow check-then-act window the
 * hand-written guards had. Closing it fully needs an in-progress sentinel;
 * not done here because it would change behaviour, not just shape.
 *
 * Returns the running handle (existing or new), or NULL only when allocation
 * failed and nothing is running.
 */
TaskHandle_t psram_task_ensure(TaskHandle_t *handle, portMUX_TYPE *mux,
                               TaskFunction_t fn, const char *name,
                               uint32_t depth, void *arg,
                               UBaseType_t prio, BaseType_t core)
{
    portENTER_CRITICAL(mux);
    TaskHandle_t existing = *handle;
    portEXIT_CRITICAL(mux);
    if (existing) return existing;

    TaskHandle_t h = psram_task_spawn(fn, name, depth, arg, prio, core);
    if (h) {
        portENTER_CRITICAL(mux);
        *handle = h;
        portEXIT_CRITICAL(mux);
    }
    return h;
}

/* ── Shared poll-interval getter ─────────────────────────────────────────────
 * The allsky/json/ha spine specs all wanted the same thing: a live config
 * field in seconds, scaled to ms and floored. The pointer is into the static
 * s_config, so it stays valid for the life of the task and the read is live.
 * Passed as the poll_loop_run() arg; the poll_once callbacks ignore it. */
typedef struct {
    const uint16_t *seconds;  /**< Live config field, e.g. &cfg->json_update_interval_s. */
    uint32_t floor_ms;        /**< Lower bound applied after scaling. */
} poll_interval_src_t;

static uint32_t config_interval_ms(void *arg)
{
    const poll_interval_src_t *src = (const poll_interval_src_t *)arg;
    uint32_t ms = (uint32_t)*src->seconds * 1000;
    return (ms < src->floor_ms) ? src->floor_ms : ms;
}

/**
 * Strip JPEG COM (0xFFFE) markers in-place.
 * The ESP32-P4 hardware JPEG decoder cannot handle COM markers in some images
 * (e.g., Spotify CDN album art). COM markers are optional comment data and
 * can be safely removed without affecting the image.
 *
 * Parses markers sequentially up to SOS (0xFFDA), skips any COM segments,
 * then copies the rest (entropy data + EOI) verbatim.
 *
 * @return New data size after stripping (≤ original size).
 */
static size_t strip_jpeg_com_markers(uint8_t *data, size_t size)
{
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return size;

    size_t rp = 2, wp = 2;  /* SOI already in place */

    while (rp + 1 < size) {
        if (data[rp] != 0xFF) break;   /* Not a marker — enter entropy data */

        uint8_t marker = data[rp + 1];

        /* SOS — everything after is entropy data, copy rest verbatim */
        if (marker == 0xDA) break;

        /* Standalone markers (no length field): SOI, EOI, RST0-7, TEM, padding */
        if (marker == 0x00 || marker == 0x01 || marker == 0xD8 || marker == 0xD9 ||
            (marker >= 0xD0 && marker <= 0xD7) || marker == 0xFF) {
            if (wp != rp) data[wp] = data[rp];
            wp++; rp++;
            if (marker == 0xFF) continue;  /* FF padding — next byte is the real marker */
            if (wp != rp) data[wp] = data[rp];
            wp++; rp++;
            continue;
        }

        /* Marker with length field */
        if (rp + 3 >= size) break;
        uint16_t seg_len = ((uint16_t)data[rp + 2] << 8) | data[rp + 3];
        size_t total = 2 + seg_len;  /* marker (2 bytes) + segment data (seg_len includes length field) */
        if (rp + total > size) break;

        if (marker == 0xFE) {
            /* COM marker — skip entirely */
            ESP_LOGD("jpeg_strip", "Stripped COM marker (%u bytes)", seg_len);
            rp += total;
            continue;
        }

        /* Keep this marker segment */
        if (wp != rp) memmove(data + wp, data + rp, total);
        wp += total;
        rp += total;
    }

    /* Copy remaining data (SOS header + entropy-coded data + EOI) */
    size_t remaining = size - rp;
    if (remaining > 0) {
        if (wp != rp) memmove(data + wp, data + rp, remaining);
        wp += remaining;
    }

    if (wp < size) {
        ESP_LOGI("jpeg_strip", "Stripped %zu bytes of COM markers from JPEG", size - wp);
    }
    return wp;
}
_Atomic bool ota_check_requested = false;
_Atomic bool screen_touch_wake = false;
_Atomic bool screen_asleep = false;
TaskHandle_t data_task_handle = NULL;
TaskHandle_t poll_task_handles[MAX_NINA_INSTANCES] = {NULL};
static int64_t last_graph_fetch_ms = 0;  /* Timestamp of last graph data fetch */
static bool hfr_graph_seeded = false;   /* True after initial API fetch for current HFR graph session */

/* Per-instance poll contexts (shared between UI coordinator and poll tasks) */
static instance_poll_ctx_t poll_contexts[MAX_NINA_INSTANCES];

/* Feature task state and page-active flags.
 * Each flag is set by data_update_task and read by the corresponding poll task.
 * When false, the poll task suspends and frees resources. */
TaskHandle_t spotify_task_handle = NULL;
_Atomic bool spotify_page_active = false;
_Atomic bool allsky_page_active  = false;
_Atomic bool json_page_active    = false;
_Atomic bool ha_page_active      = false;
_Atomic bool octoprint_page_active = false;
_Atomic bool clock_page_active   = false;
_Atomic bool nina_pages_active   = false;

/* AllSky polling state */
static allsky_data_t allsky_data;
static TaskHandle_t allsky_task_handle = NULL;

/* JSON Display polling state */
static json_data_t json_data;
static TaskHandle_t json_task_handle = NULL;

/* Home Assistant polling state */
static ha_data_t ha_data;
static TaskHandle_t ha_task_handle = NULL;

/* OctoPrint (3D Printer page) polling state. octoprint_data is non-static and
 * externed in tasks.h so the page renderer can read it under its own mutex,
 * mirroring the other shared poll-task data structs. */
octoprint_data_t octoprint_data;
static TaskHandle_t octoprint_task_handle = NULL;

/* True while the demo generator owns the instance structs. Poll tasks park on
 * it (touch nothing); set/cleared only by data_update_task around
 * demo_data_start()/demo_data_stop(). */
static _Atomic bool demo_active = false;

/**
 * @brief Page-change callback from the dashboard — signals the data task to re-tune polling.
 * Called from LVGL context (display lock already held by the gesture handler).
 * The dashboard has already updated its active_page before calling this.
 * Navigation decisions live in the arbiter now; this only wakes the poll tasks.
 */
void on_page_changed(int new_page) {
    page_changed = true;
    ESP_LOGI(TAG, "Page changed to %d", new_page);
    /* Wake all poll tasks so the newly-active one can start full polling immediately */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (poll_task_handles[i]) xTaskNotifyGive(poll_task_handles[i]);
    }
    /* Wake data_update_task so page-transition cleanup (TLS teardown) runs
     * immediately instead of waiting for the next poll cycle (2-3 s).
     * Prevents internal DMA heap exhaustion when Spotify TLS races with
     * still-open NINA WebSocket sessions. */
    if (data_task_handle) xTaskNotifyGive(data_task_handle);
}

static void IRAM_ATTR boot_button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)arg, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void input_task(void *arg) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&io_conf);

    /* Install GPIO ISR service and register falling-edge handler */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, boot_button_isr_handler, (void *)xTaskGetCurrentTaskHandle());

    while (1) {
        /* Record stack HWM unconditionally — measurement inside the button
         * handler never executes if no button is pressed. */
        if (g_perf.enabled) {
            g_perf.input_task_stack_hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        }

        /* Block until button press ISR fires */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Debounce — wait 50ms then verify still pressed */
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            continue;  /* Noise, ignore */
        }

        /* Check if screen is asleep — wake it */
        if (screen_asleep) {
            screen_touch_wake = true;
            ESP_LOGI(TAG, "Button: waking screen");
            /* Wait for button release */
            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }

        /* Track press duration for long-press detection */
        TickType_t press_start = xTaskGetTickCount();
        bool long_pressed = false;

        while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if ((xTaskGetTickCount() - press_start) >= pdMS_TO_TICKS(3000)) {
                long_pressed = true;
                break;
            }
        }

        if (long_pressed && ota_in_progress) {
            /* An OTA is flashing — deep sleep now would stop WiFi mid-write and
             * silently discard the update. Skip it and tell the user. */
            ESP_LOGW(TAG, "Long press ignored — firmware update in progress");
            nina_toast_show(TOAST_WARNING, "Update in progress");
        } else if (long_pressed && app_config_get()->deep_sleep_enabled) {
            /* Long press — enter deep sleep */
            ESP_LOGI(TAG, "Long press detected — entering deep sleep");

            /* Stop LVGL processing */
            lvgl_port_lock(0);
            lvgl_port_stop();
            lvgl_port_unlock();

            /* Turn off backlight — use LEDC directly for true 0% */
            ledc_set_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH);

            /* Small delay for visual feedback */
            vTaskDelay(pdMS_TO_TICKS(500));

            /* Enter deep sleep — does not return */
            power_mgmt_enter_deep_sleep(
                app_config_get()->deep_sleep_wake_timer_s
            );
        } else if (!long_pressed) {
            /* Short press — cycle page.
             * Modal guard: match the swipe path (gesture_event_cb in
             * nina_dashboard.c) — when a detail overlay is open, do not change
             * the page underneath it. */
            if (nina_dashboard_thumbnail_visible()
                || nina_graph_visible()
                || nina_info_overlay_visible()
                || nina_net_debug_visible()) {
                continue;
            }
            int total = nina_dashboard_get_total_page_count();
            int current = nina_dashboard_get_active_page();
            /* Skip settings, disabled allsky, disabled spotify, and disabled
             * image display in button cycling */
            int new_page = current;
            for (int step = 1; step < total; step++) {
                int candidate = (current + step) % total;
                if (candidate == SETTINGS_PAGE_IDX(page_count)) continue;
                if (candidate == PAGE_IDX_ALLSKY && !nina_dashboard_is_allsky_page()
                    && !app_config_get()->allsky_enabled) continue;
                if (candidate == PAGE_IDX_SPOTIFY && !nina_dashboard_is_spotify_page()
                    && !app_config_get()->spotify_enabled) continue;
                if (candidate == PAGE_IDX_JSON && !nina_dashboard_is_json_page()
                    && !app_config_get()->json_enabled) continue;
                if (candidate == PAGE_IDX_HA && !nina_dashboard_is_ha_page()
                    && !app_config_get()->ha_enabled) continue;
                if (candidate == PAGE_IDX_OCTOPRINT && !nina_dashboard_is_octoprint_page()
                    && !app_config_get()->octoprint_enabled) continue;
                if (candidate == PAGE_IDX_ADSB && !nina_dashboard_is_adsb_page()
                    && !app_config_get()->flights_enabled) continue;
                if (PAGE_IDX_IS_IMAGE(candidate) && !nina_dashboard_page_is_available(candidate)) continue;
                new_page = candidate;
                break;
            }
            ESP_LOGI(TAG, "Button: switching to page %d", new_page);

            /* Task 4.1: route USER nav through the arbiter. Commit immediately
             * for instant button feedback AND record a USER claim so the grace
             * window (nav_grace_s) protects this page until the next resolve(). */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_show_page_animated(new_page, 0, 0);
                bsp_display_unlock();
            } else {
                ESP_LOGW(TAG, "Display lock timeout (button page switch)");
            }
            nav_arbiter_submit_user(new_page, esp_timer_get_time() / 1000);

            page_changed = true;
        }
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

    // Boot probe: check connectivity immediately (skipped while demo owns the structs)
    {
        const char *url = app_config_get_instance_url(idx);
        if (!demo_active && strlen(url) > 0 && app_config_is_instance_enabled(idx)) {
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

        if (demo_active) {                      /* demo owns the instance structs */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;                           /* touch nothing: no polls, no conn reports */
        }

        const char *url = app_config_get_instance_url(idx);

        // Skip disabled or unconfigured instances — release network resources
        if (strlen(url) == 0 || !app_config_is_instance_enabled(idx)) {
            if (ctx->client->connected || ctx->client->websocket_connected) {
                nina_websocket_stop(idx);
                nina_poll_state_init(ctx->poll_state);
                ctx->filters_synced = false;
                ESP_LOGI(TAG, "Poll[%d]: instance disabled, resources released", idx + 1);
            }
            ctx->client->connected = false;
            /* Explicit shutdown, not a poll failure — skip the patience window. */
            nina_connection_force_disconnect(idx);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }

        // Check deferred camera-disconnect alerts
        nina_websocket_check_deferred_alerts(idx);

        int64_t now_ms = esp_timer_get_time() / 1000;

        /* Is a request actually due this wake? Mirrors the tier branch below.
         * The heartbeat-gated tiers wake far more often than they poll, and a
         * powered-off host would otherwise re-resolve DNS on every wake. */
        bool poll_due;
        if (screen_asleep) {
            poll_due = true;
        } else if (!nina_pages_active) {
            poll_due = (now_ms - ctx->last_heartbeat_ms >=
                        (int64_t)app_config_get()->idle_poll_interval_s * 1000);
        } else if (ctx->is_active) {
            poll_due = true;
        } else {
            poll_due = (now_ms - ctx->last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS);
        }

        // DNS pre-check — only ahead of a request we are about to issue
        if (poll_due && !nina_client_dns_check(url)) {
            ctx->client->connected = false;
            nina_connection_report_poll(idx, false);
            ESP_LOGD(TAG, "Poll[%d]: DNS failed, skipping", idx + 1);
            ctx->last_heartbeat_ms = now_ms;  // offline host: retry at the tier interval, not every 5 s
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Poll based on active/background/idle mode
        if (screen_asleep) {
            /* Screen sleeping — lightweight heartbeat only to detect reconnection */
            bool was_connected = nina_connection_is_connected(idx);
            nina_client_poll_heartbeat(url, ctx->client, idx);
            if (nina_connection_is_connected(idx)) {
                ctx->client->last_successful_poll_ms = now_ms;
                /* Wake the UI coordinator immediately so it can turn the screen on */
                if (!was_connected && data_task_handle)
                    xTaskNotifyGive(data_task_handle);
            }
            ctx->last_heartbeat_ms = now_ms;
            ESP_LOGD(TAG, "Poll[%d] (idle): connected=%d", idx + 1, ctx->client->connected);
        } else if (!nina_pages_active) {
            /* Non-NINA page active — heartbeat-only for liveness detection.
             * WebSockets are already torn down by data_update_task.
             * Full/background polling is skipped to free resources. */
            /* The ADS-B page draws each rig's mount pointing, so while it is
             * visible the idle tier ticks at the 10 s background rate instead
             * of idle_poll_interval_s (30 s default) to keep that fresh. */
            int64_t idle_ms = adsb_page_active ? (int64_t)HEARTBEAT_INTERVAL_MS
                                               : (int64_t)app_config_get()->idle_poll_interval_s * 1000;
            if (now_ms - ctx->last_heartbeat_ms >= idle_ms) {
                nina_client_poll_heartbeat(url, ctx->client, idx);
                if (nina_connection_is_connected(idx))
                    ctx->client->last_successful_poll_ms = now_ms;
                ctx->last_heartbeat_ms = now_ms;
                ESP_LOGD(TAG, "Poll[%d] (page-idle): connected=%d", idx + 1, ctx->client->connected);
            }
        } else if (ctx->is_active) {
            nina_client_poll(url, ctx->client, ctx->poll_state, idx);
            if (nina_connection_is_connected(idx))
                ctx->client->last_successful_poll_ms = now_ms;
            if (app_config_get()->debug_mode) {
                ESP_LOGI(TAG, "Poll[%d] (active): connected=%d, status=%s, target=%s, ws=%d",
                    idx + 1, ctx->client->connected, ctx->client->status,
                    ctx->client->target_name, ctx->client->websocket_connected);
            } else if (ctx->client->connected) {
                /* Extract hostname from URL for clean log output */
                const char *host = strstr(url, "://");
                host = host ? host + 3 : url;
                const char *host_end = strchr(host, ':');
                if (!host_end) host_end = strchr(host, '/');
                int host_len = host_end ? (int)(host_end - host) : (int)strlen(host);
                ESP_LOGI(TAG, "Poll[%d]: %.*s — data received", idx + 1, host_len, host);
            }
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

        // WebSocket: skip reconnect while screen sleeping (saves network resources);
        // reconnect will happen naturally when screen wakes and poll resumes.
        if (!screen_asleep && nina_pages_active) {
            // If WebSocket was never started (boot probe missed) but instance is
            // now connected, start it. check_reconnect only handles post-disconnect.
            if (!nina_websocket_is_running(idx) && nina_connection_is_connected(idx)) {
                nina_websocket_start(idx, url, ctx->client);
            }
            nina_websocket_check_reconnect(idx, url, ctx->client);
        }

        // Sleep: active = update_rate_s, background = heartbeat, screen_asleep = idle_poll
        uint32_t cycle_ms;
        if (screen_asleep || !nina_pages_active) {
            cycle_ms = (uint32_t)app_config_get()->idle_poll_interval_s * 1000;
            if (cycle_ms < 5000) cycle_ms = 5000;
        } else if (ctx->is_active) {
            cycle_ms = (uint32_t)app_config_get()->update_rate_s * 1000;
            if (cycle_ms < 1000) cycle_ms = 1000;
        } else {
            cycle_ms = HEARTBEAT_INTERVAL_MS;
        }
        // Use task notification to allow early wake (page change, WS event)
        net_sched_note(pcTaskGetName(NULL), (uint32_t)(esp_timer_get_time() / 1000) + cycle_ms);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cycle_ms));
    }

    // Cleanup on shutdown — clear handle BEFORE stopping WS/deleting task
    // to prevent TOCTOU race (WS handler could xTaskNotifyGive a stale handle).
    poll_task_handles[idx] = NULL;
    nina_websocket_stop(idx);
    ESP_LOGI(TAG, "Poll[%d]: task shutdown", idx + 1);
    vTaskDelete(NULL);
}

/* OTA progress callback — updates the LVGL progress bar from the download task */
static void ota_progress_cb(int percent) {
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_ota_prompt_set_progress(percent);
        bsp_display_unlock();
    }
}

// =============================================================================
// AllSky Poll Task — independent poller pinned to Core 0
// =============================================================================

static bool allsky_poll_once(void *arg) {
    (void)arg;

    /* Read fields directly from config pointer — avoids copying the full
     * ~6.7 KB app_config_t onto this task's small stack. */
    const app_config_t *cfg = app_config_get();

    /* Only poll when hostname is configured */
    if (cfg->allsky_hostname[0] != '\0') {
        allsky_client_poll(cfg->allsky_hostname, cfg->allsky_field_config, &allsky_data);
    }
    return true; /* no failure signal — matches original unconditional-retry-at-interval behavior */
}

void allsky_poll_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "AllSky poll task started");

    /* Interval clamped 1-300s at config-validate time; floored here too.
     * Lives on this task's stack, which never unwinds (poll_loop_run does not
     * return), so the pointer handed to the spine stays valid. */
    poll_interval_src_t interval = {
        .seconds  = &app_config_get()->allsky_update_interval_s,
        .floor_ms = 1000,
    };

    poll_loop_spec_t spec = {
        .name = "allsky",
        .wifi_group = s_wifi_event_group,
        .wifi_bits = WIFI_CONNECTED_BIT,
        .page_active = &allsky_page_active,
        .poll_once = allsky_poll_once,
        .interval_ms = config_interval_ms,
        .backoff_initial_ms = 0,
        .backoff_max_ms = 0,
    };

    poll_loop_run(&spec, &interval);
}

// =============================================================================
// JSON Display Poll Task — independent poller pinned to Core 0
// =============================================================================

static bool json_poll_once(void *arg) {
    (void)arg;

    /* Read fields directly from config pointer — avoids copying the full
     * ~7.6 KB app_config_t onto this task's small stack. */
    const app_config_t *cfg = app_config_get();

    /* Only poll when a URL is configured. */
    if (cfg->json_url[0] != '\0') {
        json_client_poll(cfg->json_url, cfg->json_auth_header,
                         app_config_get_json_tiles(), &json_data);
    }
    return true; /* no failure signal — retry at interval, matches allsky */
}

void json_poll_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "JSON Display poll task started");

    /* Interval clamped 5-300s at config-validate time; floored here too. */
    poll_interval_src_t interval = {
        .seconds  = &app_config_get()->json_update_interval_s,
        .floor_ms = 5000,
    };

    poll_loop_spec_t spec = {
        .name = "json",
        .wifi_group = s_wifi_event_group,
        .wifi_bits = WIFI_CONNECTED_BIT,
        .page_active = &json_page_active,
        .poll_once = json_poll_once,
        .interval_ms = config_interval_ms,
        .backoff_initial_ms = 0,
        .backoff_max_ms = 0,
    };

    poll_loop_run(&spec, &interval);
}

/* Sole spawn path for json_poll_task — called both at boot and on a runtime
 * enable from the web UI, so the page starts polling without a reboot.
 * 10240 words (not 6144) gives TLS headroom: an https JSON source runs an
 * mbedTLS handshake on this task's stack. */
void json_ensure_task_running(void)
{
    if (!app_config_get()->json_enabled) return;

    static portMUX_TYPE json_spawn_mux = portMUX_INITIALIZER_UNLOCKED;
    psram_task_ensure(&json_task_handle, &json_spawn_mux,
                      json_poll_task, "json", 10240, NULL, 3, 0);
}

// =============================================================================
// Home Assistant Poll Task — independent poller pinned to Core 0
// =============================================================================

static bool ha_poll_once(void *arg) {
    (void)arg;

    /* Read fields directly from config pointer — avoids copying the full
     * ~7.6 KB app_config_t onto this task's small stack. */
    const app_config_t *cfg = app_config_get();

    /* Only poll when a base URL is configured. */
    if (cfg->ha_base_url[0] != '\0') {
        ha_client_poll(cfg->ha_base_url, cfg->ha_token,
                       app_config_get_ha_tiles(), &ha_data);
    }
    return true; /* no failure signal — retry at interval, matches json */
}

void ha_poll_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Home Assistant poll task started");

    /* Interval clamped 5-300s at config-validate time; floored here too. */
    poll_interval_src_t interval = {
        .seconds  = &app_config_get()->ha_update_interval_s,
        .floor_ms = 5000,
    };

    poll_loop_spec_t spec = {
        .name = "ha",
        .wifi_group = s_wifi_event_group,
        .wifi_bits = WIFI_CONNECTED_BIT,
        .page_active = &ha_page_active,
        .poll_once = ha_poll_once,
        .interval_ms = config_interval_ms,
        .backoff_initial_ms = 0,
        .backoff_max_ms = 0,
    };

    poll_loop_run(&spec, &interval);
}

/* Sole spawn path for ha_poll_task — boot and runtime enable both land here.
 * 10240 words (not 6144) gives TLS headroom: an https HA base runs an mbedTLS
 * handshake on this task's stack. */
void ha_ensure_task_running(void)
{
    if (!app_config_get()->ha_enabled) return;

    static portMUX_TYPE ha_spawn_mux = portMUX_INITIALIZER_UNLOCKED;
    psram_task_ensure(&ha_task_handle, &ha_spawn_mux,
                      ha_poll_task, "ha", 10240, NULL, 3, 0);
}

// =============================================================================
// OctoPrint Poll Task — independent poller pinned to Core 0
// =============================================================================

static bool octoprint_poll_once(void *arg) {
    (void)arg;

    /* Read fields directly from the config pointer — avoids copying the full
     * app_config_t onto this task's small stack. */
    const app_config_t *cfg = app_config_get();

    /* Only poll when a base URL is configured. */
    if (cfg->octoprint_url[0] != '\0') {
        octoprint_client_poll(cfg->octoprint_url, cfg->octoprint_api_key,
                              cfg->octoprint_image_source,
                              cfg->octoprint_snapshot_url, &octoprint_data);
    }
    return true; /* no failure signal — retry at interval, matches json/ha */
}

void octoprint_poll_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "OctoPrint poll task started");

    /* Interval clamped 2-300s at config-validate time; floored here too. */
    poll_interval_src_t interval = {
        .seconds  = &app_config_get()->octoprint_update_interval_s,
        .floor_ms = 2000,
    };

    poll_loop_spec_t spec = {
        .name = "octoprint",
        .wifi_group = s_wifi_event_group,
        .wifi_bits = WIFI_CONNECTED_BIT,
        .page_active = &octoprint_page_active,
        .poll_once = octoprint_poll_once,
        .interval_ms = config_interval_ms,
        .backoff_initial_ms = 0,
        .backoff_max_ms = 0,
    };

    poll_loop_run(&spec, &interval);
}

/* Sole spawn path for octoprint_poll_task — boot and runtime enable both land
 * here. 12288 bytes (vs json/ha's 10240): an https OctoPrint host runs an
 * mbedTLS handshake on this task's stack, and the image path nests URL/path
 * buffers plus an stb_image decode on top of it. */
void octoprint_ensure_task_running(void)
{
    if (!app_config_get()->octoprint_enabled) return;

    static portMUX_TYPE octoprint_spawn_mux = portMUX_INITIALIZER_UNLOCKED;
    psram_task_ensure(&octoprint_task_handle, &octoprint_spawn_mux,
                      octoprint_poll_task, "octoprint", 12288, NULL, 3, 0);
}

/* Cuts the wait for a config change that only the poller can act on (image
 * source, snapshot URL) from up to one poll interval to ~0. Same shape as the
 * page-activation wake below and image_page_wake's image-poller wake:
 * poll_loop_run sleeps in ulTaskNotifyTake, so this returns it immediately and
 * the next poll re-reads config. Harmless when the page is inactive -- the
 * gate loop consumes the notify and tasks.c re-wakes on activation. */
void octoprint_wake_now(void)
{
    if (octoprint_task_handle) xTaskNotifyGive(octoprint_task_handle);
}

// =============================================================================
// Spotify Poll Task — fetches currently-playing, album art on track change
// =============================================================================

void spotify_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Spotify poll task started");

    /* Wait for WiFi */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    char prev_track_id[SPOTIFY_MAX_TRACK_ID_LEN] = {0};
    int consecutive_errors = 0;
    int art_retries = 0;           /* retries for current track's album art */
    #define ART_MAX_RETRIES 3      /* give up on art after this many failures */

    while (1) {
        /* Record stack HWM unconditionally — the measurement at the end of
         * the poll cycle is unreachable when the task is suspended by the
         * page-gate or when Spotify is not configured. */
        if (g_perf.enabled) {
            g_perf.spotify_task_stack_hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        }

        /* Suspend during OTA updates */
        while (ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        app_config_t *cfg = app_config_get();
        if (!cfg->spotify_enabled || spotify_auth_get_state() != SPOTIFY_AUTH_AUTHORIZED) {
            /* Push the current setup/connection status to the UI so it updates
             * live (e.g. when the user links the account or a token error
             * occurs). Cheap — just sets label text. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_spotify_refresh_status();
                bsp_display_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* Wait until the Spotify page is active.  data_update_task sets
         * spotify_page_active AFTER it has torn down NINA WebSocket TLS
         * sessions, so this gate ensures we don't open new TLS connections
         * while internal DMA heap is still held by NINA resources. */
        if (!spotify_page_active) {
            /* Tear down our own TLS session while idle so it doesn't hold
             * internal DMA memory that NINA or SDIO may need. */
            spotify_client_destroy_connection();
            /* Clear prev_track_id so album art is re-fetched when the
             * page becomes active again (the art buffer was freed). */
            prev_track_id[0] = '\0';
            art_retries = 0;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }

        /* Drain action queue — process playback control requests */
        if (spotify_action_queue) {
            spotify_action_t action;
            while (xQueueReceive(spotify_action_queue, &action, 0) == pdTRUE) {
                switch (action) {
                    case SPOTIFY_ACTION_PLAY:  spotify_client_play();     break;
                    case SPOTIFY_ACTION_PAUSE: spotify_client_pause();    break;
                    case SPOTIFY_ACTION_NEXT:  spotify_client_next();     break;
                    case SPOTIFY_ACTION_PREV:  spotify_client_previous(); break;
                }
            }
        }

        perf_timer_start(&g_perf.spotify_poll_cycle);

        spotify_playback_t pb;
        perf_timer_start(&g_perf.spotify_api_fetch);
        esp_err_t err = spotify_client_get_currently_playing(&pb);
        perf_timer_stop(&g_perf.spotify_api_fetch);
        perf_counter_increment(&g_perf.spotify_poll_count);

        if (err == ESP_OK) {
            consecutive_errors = 0;

            /* Update text UI immediately so the user sees new track info
             * without waiting for the album art TLS handshake + download. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                perf_timer_start(&g_perf.spotify_ui_update);
                nina_spotify_update(&pb);
                perf_timer_stop(&g_perf.spotify_ui_update);
                bsp_display_unlock();
            }

            /* Check if track changed — fetch new album art */
            if (strcmp(pb.track_id, prev_track_id) != 0) {
                bool art_ok = false;
                /* Don't reset art_retries here — we re-enter this block every
                 * poll cycle until prev_track_id is set (after success or max
                 * retries).  Resetting here made retries infinite. */

                if (pb.album_art_url[0] != '\0' && art_retries < ART_MAX_RETRIES) {
                    /* Free the persistent currently-playing TLS session before
                     * opening a second one to the CDN — internal DMA heap can't
                     * hold two concurrent AES contexts + SDIO WiFi buffers. */
                    spotify_client_destroy_connection();

                    uint8_t *jpg_buf = NULL;
                    size_t jpg_size = 0;
                    perf_timer_start(&g_perf.spotify_art_fetch);
                    bool art_fetch_ok = (spotify_client_fetch_album_art(pb.album_art_url, &jpg_buf, &jpg_size) == ESP_OK
                        && jpg_buf && jpg_size > 0);
                    perf_timer_stop(&g_perf.spotify_art_fetch);
                    perf_counter_increment(&g_perf.spotify_art_fetch_count);
                    if (art_fetch_ok) {
                        /* Strip COM markers that the HW JPEG decoder can't handle */
                        jpg_size = strip_jpeg_com_markers(jpg_buf, jpg_size);

                        /* HW-first decode through the shared spine (its own stb
                         * fallback covers progressive/CMYK), then one PPA pass to
                         * 720x720 so LVGL blits 1:1 instead of scaling per redraw.
                         * nina_spotify_set_album_art() TAKES OWNERSHIP, frees the
                         * previous buffer and red-remaps this one in place, so each
                         * handoff has to be its own allocation -- no shared static. */
                        uint8_t *art = NULL;
                        uint32_t art_w = 0, art_h = 0;
                        size_t art_size = 0;
                        perf_timer_start(&g_perf.spotify_art_decode);
                        bool art_dec = jpeg_decode_rgb565(jpg_buf, jpg_size,
                                                          &art, &art_w, &art_h, &art_size);
                        if (art_dec && art && (art_w != 720 || art_h != 720)) {
                            size_t scaled_size = 0;
                            uint8_t *scaled = ppa_scale_rgb565(art, art_w, art_h, 0,
                                                               720, 720, &scaled_size);
                            if (scaled) {
                                free(art);
                                art = scaled;
                                art_w = 720;
                                art_h = 720;
                                art_size = scaled_size;
                            }
                            /* PPA refused: hand over the unscaled frame and let LVGL
                             * software-scale it, exactly as before. */
                        }
                        perf_timer_stop(&g_perf.spotify_art_decode);

                        if (art_dec && art) {
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_spotify_set_album_art(art, art_w, art_h,
                                                           (uint32_t)art_size);
                                bsp_display_unlock();
                                art_ok = true;
                                /* Ownership transferred to UI -- don't free art */
                            } else {
                                /* Lock timed out -- free the buffer and leave
                                 * art_ok false so the art is retried next poll. */
                                free(art);
                            }
                        } else {
                            ESP_LOGW(TAG, "Album art JPEG decode failed");
                        }
                        free(jpg_buf);
                    } else {
                        art_retries++;
                    }
                }

                /* Record track ID if art succeeded, no art URL, or retries exhausted.
                 * Prevents infinite retry loop when CDN is unreachable. */
                if (art_ok || pb.album_art_url[0] == '\0' || art_retries >= ART_MAX_RETRIES) {
                    if (art_retries >= ART_MAX_RETRIES) {
                        ESP_LOGW(TAG, "Album art fetch failed after %d retries, skipping", ART_MAX_RETRIES);
                    }
                    snprintf(prev_track_id, sizeof(prev_track_id), "%s", pb.track_id);
                    art_retries = 0;
                }
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            /* Nothing playing — update UI to show idle state */
            consecutive_errors = 0;
            prev_track_id[0] = '\0';
            art_retries = 0;
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_spotify_set_idle();
                bsp_display_unlock();
            }
        } else {
            /* Connection error — back off to avoid TLS handshake storm */
            consecutive_errors++;
            perf_counter_increment(&g_perf.spotify_error_count);
        }

        uint32_t interval = cfg->spotify_poll_interval_ms;
        if (!spotify_page_active) {
            interval = 10000; /* Background: poll every 10s */
            /* When page goes inactive, clear prev_track_id so album art
             * is re-fetched when returning (buffer was freed by page transition). */
            prev_track_id[0] = '\0';
            art_retries = 0;
        }
        /* Exponential backoff on errors: 2x, 4x, ... up to 30s */
        if (consecutive_errors > 0) {
            uint32_t backoff = interval * (1u << (consecutive_errors < 4 ? consecutive_errors : 4));
            if (backoff > 30000) backoff = 30000;
            interval = backoff;
        }
        perf_timer_stop(&g_perf.spotify_poll_cycle);
        net_sched_note(pcTaskGetName(NULL), (uint32_t)(esp_timer_get_time() / 1000) + interval);
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

// =============================================================================
// Spotify task lifecycle
// =============================================================================

void spotify_ensure_task_running(void)
{
    static portMUX_TYPE spotify_spawn_mux = portMUX_INITIALIZER_UNLOCKED;
    if (psram_task_ensure(&spotify_task_handle, &spotify_spawn_mux,
                          spotify_poll_task, "spotify_poll", 10240, NULL, 4, 0)) {
        return;
    }

    /* PSRAM exhausted: fall back to an internal-heap task rather than leaving
     * the page dead. Unique to Spotify — the other pollers simply stay
     * unspawned. Dynamic create allocates internally, so it must not run inside
     * the critical section; publish the handle under the mux once it returns. */
    ESP_LOGE(TAG, "Failed to alloc spotify_poll stack from PSRAM, falling back");
    TaskHandle_t dyn = NULL;
    xTaskCreatePinnedToCore(spotify_poll_task, "spotify_poll", 10240, NULL, 4,
                            &dyn, 0);
    portENTER_CRITICAL(&spotify_spawn_mux);
    spotify_task_handle = dyn;
    portEXIT_CRITICAL(&spotify_spawn_mux);
    ESP_LOGI(TAG, "Spotify poll task created dynamically");
}

// =============================================================================
// Async Fetch Worker — runs HTTP fetches on Core 0 to keep Core 1 free for UI
// =============================================================================

void fetch_worker_task(void *arg) {
    ESP_LOGI(TAG, "Fetch worker task started on core %d", xPortGetCoreID());

    /* Allocate graph data buffers in PSRAM (reused across requests) */
    graph_rms_data_t *rms_buf = heap_caps_calloc(1, sizeof(graph_rms_data_t), MALLOC_CAP_SPIRAM);
    graph_hfr_data_t *hfr_buf = heap_caps_calloc(1, sizeof(graph_hfr_data_t), MALLOC_CAP_SPIRAM);

    while (1) {
        fetch_request_t req;
        if (xQueueReceive(s_fetch_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        fetch_result_t result = {
            .type = req.type,
            .instance_idx = req.instance_idx,
            .success = false,
        };

        switch (req.type) {
        case FETCH_THUMBNAIL: {
            size_t jpeg_size = 0;
            perf_timer_start(&g_perf.jpeg_fetch);
            uint8_t *jpeg_buf = nina_client_fetch_prepared_image(req.url, 720, 720, 70, &jpeg_size);
            perf_timer_stop(&g_perf.jpeg_fetch);
            if (!jpeg_buf || jpeg_size == 0) break;

            /* HW-first decode: tight w x h RGB565 in 128 B aligned PSRAM, MCU
             * padding removed, single-component JPEG expanded from GRAY8, stb
             * fallback for progressive/CMYK. Carries its own low-DMA-heap
             * guard, so the one that used to sit here is gone. */
            uint8_t *rgb_buf = NULL;
            uint32_t rgb_w = 0, rgb_h = 0;
            size_t rgb_size = 0;
            perf_timer_start(&g_perf.jpeg_decode);
            bool decoded = jpeg_decode_rgb565(jpeg_buf, jpeg_size,
                                              &rgb_buf, &rgb_w, &rgb_h, &rgb_size);
            perf_timer_stop(&g_perf.jpeg_decode);
            free(jpeg_buf);
            if (!decoded || !rgb_buf) break;

            result.success = true;
            result.thumbnail.rgb565_data = rgb_buf;
            result.thumbnail.w = rgb_w;
            result.thumbnail.h = rgb_h;
            result.thumbnail.data_size = (uint32_t)rgb_size;
            break;
        }

        case FETCH_GRAPH_RMS:
            if (rms_buf) {
                memset(rms_buf, 0, sizeof(*rms_buf));
                fetch_guider_graph(req.url, rms_buf, req.max_points);
                result.success = true;
                result.data = rms_buf;  /* Pointer to worker-owned buffer, consumed before next request */
            }
            break;

        case FETCH_GRAPH_HFR:
            if (hfr_buf) {
                memset(hfr_buf, 0, sizeof(*hfr_buf));
                fetch_hfr_history(req.url, hfr_buf, req.max_points);
                result.success = true;
                result.data = hfr_buf;
            }
            break;

        case FETCH_GRAPH_HFR_RING:
            if (hfr_buf && req.client) {
                memset(hfr_buf, 0, sizeof(*hfr_buf));
                if (nina_client_lock(req.client, 50)) {
                    build_hfr_from_ring(req.client, hfr_buf, req.max_points);
                    nina_client_unlock(req.client);
                    result.success = true;
                    result.data = hfr_buf;
                }
            }
            break;

        case FETCH_INFO_CAMERA: {
            camera_detail_data_t *cam = heap_caps_calloc(1, sizeof(camera_detail_data_t), MALLOC_CAP_SPIRAM);
            if (cam) {
                fetch_camera_details(req.url, cam);
                fetch_weather_details(req.url, cam);
                result.success = true;
                result.data = cam;
            }
            break;
        }

        case FETCH_INFO_MOUNT: {
            mount_detail_data_t *mnt = heap_caps_calloc(1, sizeof(mount_detail_data_t), MALLOC_CAP_SPIRAM);
            if (mnt) {
                fetch_mount_details(req.url, mnt);
                result.success = true;
                result.data = mnt;
            }
            break;
        }

        case FETCH_INFO_SEQUENCE: {
            sequence_detail_data_t *seq = heap_caps_calloc(1, sizeof(sequence_detail_data_t), MALLOC_CAP_SPIRAM);
            if (seq) {
                fetch_sequence_details(req.url, seq);
                result.success = true;
                result.data = seq;
            }
            break;
        }

        case FETCH_INFO_FILTER: {
            /* Filter data comes from nina_client_t, not HTTP — handled in UI coordinator */
            break;
        }

        case FETCH_INFO_IMAGESTATS: {
            /* Image stats come from WebSocket events — handled in UI coordinator */
            break;
        }

        case FETCH_INFO_AUTOFOCUS: {
            /* Autofocus data comes from WebSocket events — handled in UI coordinator */
            break;
        }
        }

        /* Post result (non-blocking — drop if queue full, next cycle will retry) */
        if (result.success) {
            if (xQueueSend(s_fetch_result_queue, &result, 0) != pdTRUE) {
                /* Queue full — free any allocated result data */
                if (result.type == FETCH_THUMBNAIL && result.thumbnail.rgb565_data) {
                    free(result.thumbnail.rgb565_data);
                } else if (result.type == FETCH_INFO_CAMERA || result.type == FETCH_INFO_MOUNT
                           || result.type == FETCH_INFO_SEQUENCE) {
                    heap_caps_free(result.data);
                }
                ESP_LOGW(TAG, "Fetch result queue full, dropping result type %d", result.type);
            }
        } else {
            /* Post failure result so UI coordinator knows the fetch failed */
            xQueueSend(s_fetch_result_queue, &result, 0);
        }

        /* Wake UI coordinator to process the result */
        if (data_task_handle) xTaskNotifyGive(data_task_handle);
    }
}

// =============================================================================
// Network-stack bring-up — MQTT, per-instance pollers, feature pollers, fetch
// worker. Runs once: on a normal boot at the old inline location, or on the
// first demo-OFF transition after a demo boot (which skipped all of this).
// Everything inside is already ensure-style or handle-guarded; the single
// guard makes the whole block idempotent regardless.
// =============================================================================

static void ensure_network_stack(void) {
    static bool s_net_stack_started = false;
    if (s_net_stack_started) {
        return;
    }
    s_net_stack_started = true;

    // Start MQTT if enabled
    mqtt_ha_start();

    instance_count = app_config_get_instance_count();
    ESP_LOGI(TAG, "Spawning %d per-instance poll tasks", instance_count);

    /* Spawn per-instance poll tasks (boot probe + WS start happen inside each task) */
    for (int i = 0; i < instance_count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "poll_%d", i);
        /* Pin poll tasks to Core 0 (networking), leaving Core 1 for UI/LVGL. */
        poll_contexts[i].task_handle = psram_task_spawn(
            instance_poll_task, name, 8192, &poll_contexts[i], 4, 0);
        poll_task_handles[i] = poll_contexts[i].task_handle;
    }

    /* Spawn AllSky poll task (pinned to Core 0, networking).
     * On a demo boot allsky_data_init already ran in the demo branch; a second
     * init would leak the mutex, so guard on it. */
    if (allsky_data.mutex == NULL) {
        allsky_data_init(&allsky_data);
    }
    if (app_config_get()->allsky_enabled) {
        allsky_task_handle = psram_task_spawn(allsky_poll_task, "allsky", 6144, NULL, 3, 0);
    }

    /* JSON Display poll task (pinned to Core 0, networking).
     * json_data_init runs UNCONDITIONALLY so the mutex exists before any later
     * web-handler-triggered enable + page entry; json_ensure_task_running()
     * spawns the task itself only when the page is enabled — same call the web
     * handler makes on a runtime enable. */
    json_client_init(&json_data);
    json_ensure_task_running();

    /* Home Assistant poll task (pinned to Core 0, networking).
     * ha_client_init runs UNCONDITIONALLY so the mutex exists before any later
     * web-handler-triggered enable + page entry; the enable check lives inside
     * ha_ensure_task_running(). */
    ha_client_init(&ha_data);
    ha_ensure_task_running();

    /* OctoPrint poll task (pinned to Core 0, networking).
     * octoprint_client_init runs UNCONDITIONALLY so the mutex exists before any
     * later web-handler-triggered enable + page entry; the enable check lives
     * inside octoprint_ensure_task_running(). */
    octoprint_client_init(&octoprint_data);
    octoprint_ensure_task_running();

    /* ADS-B poll task (pinned to Core 0, networking). adsb_ensure_task_running()
     * does its own init and its own flights_enabled check, so this one call
     * covers both boot and the runtime enable from the web handler. */
    adsb_ensure_task_running();

    /* Image pages (GOES / Moon / Solar / Custom). Mutexes for all four, a
     * PSRAM poller for each source enabled in config; disabled sources spawn
     * lazily on the first enable/entry (image_page_ensure_task_running). */
    image_page_init(true);

    /* Spawn async fetch worker (pinned to Core 0, networking) */
    if (s_fetch_queue && s_fetch_result_queue) {
        psram_task_spawn(fetch_worker_task, "fetch_wk", 12288, NULL, 4, 0);  /* jpeg_decode_rgb565 wants ~10 KB headroom */
    }
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
    int64_t last_crash_purge_ms = 0;  /* daily crash-log retention purge tick */

    /* Screen sleep state */
    int64_t all_disconnected_since_ms = 0;  /* 0 = at least one connected recently */
    int64_t spotify_idle_since_ms = 0;      /* 0 = Spotify playing or not on Spotify page */

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        nina_poll_state_init(&poll_states[i]);
        nina_client_init_mutex(&instances[i]);
        /* Allocate per-instance HFR ring buffer in PSRAM (~4 KB per instance) */
        instances[i].hfr_ring.hfr   = heap_caps_calloc(HFR_RING_SIZE, sizeof(float), MALLOC_CAP_SPIRAM);
        instances[i].hfr_ring.stars = heap_caps_calloc(HFR_RING_SIZE, sizeof(int),   MALLOC_CAP_SPIRAM);
    }

    /* Allocate graph data in PSRAM for local use (overlays that don't go through fetch worker) */
    graph_rms_data_t *rms_data = heap_caps_calloc(1, sizeof(graph_rms_data_t), MALLOC_CAP_SPIRAM);
    graph_hfr_data_t *hfr_data = heap_caps_calloc(1, sizeof(graph_hfr_data_t), MALLOC_CAP_SPIRAM);
    if (!rms_data || !hfr_data) {
        ESP_LOGE(TAG, "Failed to allocate graph data from PSRAM");
        if (rms_data) heap_caps_free(rms_data);
        if (hfr_data) heap_caps_free(hfr_data);
        vTaskDelete(NULL);
        return;
    }

    /* Create async fetch queues (request/result between UI coordinator and fetch worker) */
    s_fetch_queue = xQueueCreate(FETCH_QUEUE_LEN, sizeof(fetch_request_t));
    s_fetch_result_queue = xQueueCreate(FETCH_RESULT_QUEUE_LEN, sizeof(fetch_result_t));
    if (!s_fetch_queue || !s_fetch_result_queue) {
        ESP_LOGE(TAG, "Failed to create fetch queues");
    }

    /* Track pending async fetch requests to avoid duplicate submissions */
    bool fetch_thumbnail_pending = false;
    bool fetch_graph_pending = false;
    bool fetch_info_pending = false;

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

    /* ── Demo mode: skip all network tasks, spawn demo data generator ── */
    if (app_config_get()->demo_mode) {
        ESP_LOGI(TAG, "DEMO MODE — skipping WiFi wait, polling, MQTT, WebSocket");

        /* Initialize AllSky data struct (needed even in demo mode) */
        allsky_data_init(&allsky_data);
        /* Image pages: create the four instance mutexes so a later web-handler
         * enable + page entry never NULL-derefs; no pollers in demo mode. */
        image_page_init(false);

        instance_count = app_config_get_instance_count();
        instance_count = 3;  /* demo mode always shows all 3 instance profiles */

        /* Start demo data generator (spawns the persistent demo task) */
        demo_active = true;
        demo_data_start(instances, allsky_task_handle ? NULL : &allsky_data, 3);

        goto main_loop;
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

    /* ── Boot-time firmware update check ── */
    /* Done before MQTT to avoid concurrent TLS + MQTT traffic exhausting the
     * esp_hosted SDIO receive buffer pool (sdio_rx_get_buffer assert). */
    if (app_config_get()->auto_update_check) {
        ESP_LOGI(TAG, "Checking for firmware updates...");
        github_release_info_t *rel = heap_caps_calloc(1, sizeof(github_release_info_t), MALLOC_CAP_SPIRAM);
        if (rel) {
            int update_channel = app_config_get()->update_channel;
            const char *cur_ver = ota_github_get_current_version();
            ota_check_result_t chk = ota_github_check(update_channel, cur_ver, rel);
            if (chk == OTA_CHECK_UPDATE_AVAILABLE) {
                ESP_LOGI(TAG, "New firmware available: %s", rel->tag);
                if (rel->requires_full_erase) {
                    /* This release cannot be installed over WiFi — show a
                     * blocking warning and wait only for dismissal. */
                    ESP_LOGW(TAG, "Firmware %s requires manual USB erase+flash", rel->tag);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show_manual_flash(rel->tag, rel->full_erase_tag);
                        bsp_display_unlock();
                    }
                    while (nina_ota_prompt_visible()) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                    heap_caps_free(rel);
                    goto boot_update_check_done;
                }
                /* Show the update prompt overlay */
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    nina_ota_prompt_show(rel->tag, cur_ver, rel->summary);
                    bsp_display_unlock();
                }
                /* Wait for user to accept or skip (flags clear on read, so store result) */
                bool accepted = false;
                while (1) {
                    if (nina_ota_prompt_update_accepted()) { accepted = true;  break; }
                    if (nina_ota_prompt_skipped())          { accepted = false; break; }
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                if (accepted && atomic_exchange(&ota_in_progress, true)) {
                    /* Another OTA is already running — do not start a second
                     * write stream against the same partition. */
                    ESP_LOGW(TAG, "OTA already in progress — ignoring boot update request");
                    nina_toast_show(TOAST_WARNING, "Update already in progress");
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_hide();
                        bsp_display_unlock();
                    }
                } else if (accepted) {
                    /* Accepted — ota_in_progress was set true by the exchange above. */
                    ESP_LOGI(TAG, "User accepted OTA update to %s", rel->tag);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show_progress();
                        bsp_display_unlock();
                    }
                    esp_err_t ota_err = ota_github_download(rel->ota_url, ota_progress_cb);
                    if (ota_err == ESP_OK) {
                        ota_github_save_pending_version(rel->tag);
                        ESP_LOGI(TAG, "OTA download success");
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_ota_prompt_set_progress(100);
                            bsp_display_unlock();
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        app_reboot("boot OTA complete");
                    } else {
                        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(ota_err));
                        ota_in_progress = false;
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_ota_prompt_show_error("Download failed. Please try again later.");
                            bsp_display_unlock();
                        }
                        /* Wait for user to dismiss the error */
                        while (nina_ota_prompt_visible()) {
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                    }
                } else {
                    /* Skipped */
                    ESP_LOGI(TAG, "User skipped firmware update");
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_hide();
                        bsp_display_unlock();
                    }
                }
            } else if (chk == OTA_CHECK_RATE_LIMITED) {
                ESP_LOGW(TAG, "Boot firmware update check hit the GitHub rate limit; will retry later");
            } else if (chk == OTA_CHECK_ERROR) {
                ESP_LOGW(TAG, "Boot firmware update check failed (network/GitHub); will retry next check");
            } else {
                ESP_LOGI(TAG, "No firmware update available");
            }
            heap_caps_free(rel);
        }
    }

boot_update_check_done:
    /* MQTT + all poll tasks + fetch worker (factored so the first demo-OFF
     * transition after a demo boot can bring the same stack up late). */
    ensure_network_stack();

main_loop:
    while (1) {
        /* Suspend polling during OTA */
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

        /* ── Drain async fetch results from Core 0 worker ── */
        {
            fetch_result_t fres;
            while (s_fetch_result_queue && xQueueReceive(s_fetch_result_queue, &fres, 0) == pdTRUE) {
                switch (fres.type) {
                case FETCH_THUMBNAIL:
                    fetch_thumbnail_pending = false;
                    if (fres.success && fres.thumbnail.rgb565_data) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            /* Image-forward (layout 1) shows the last capture as its
                             * page background. The page must still be the visible
                             * one: hide_page_at() has already released the capture,
                             * so attaching a frame to a hidden page would leak it
                             * until that page is entered and left again. */
                            bool want_cap = (fres.instance_idx >= 0
                                && fres.instance_idx < MAX_NINA_INSTANCES
                                && nina_slot_available[fres.instance_idx]
                                && pages[fres.instance_idx].layout == 1
                                && nina_dashboard_get_active_page()
                                       == NINA_PAGE_OFFSET + fres.instance_idx);
                            /* Both consumers TAKE OWNERSHIP, so the frame is copied
                             * only when both want it. The overlay is normally hidden
                             * (the capture is asked for by the layout, not by a tap)
                             * and that case now hands the original straight to the
                             * layout: no 1 MB alloc + memcpy + free per sub. */
                            bool want_thumb = nina_dashboard_thumbnail_wants_data();
                            uint8_t *buf = fres.thumbnail.rgb565_data;

                            if (want_cap && want_thumb) {
                                uint8_t *cap = heap_caps_malloc(fres.thumbnail.data_size,
                                                                MALLOC_CAP_SPIRAM);
                                if (cap) {
                                    memcpy(cap, buf, fres.thumbnail.data_size);
                                    nina_layout_image_set_capture(fres.instance_idx, cap,
                                        fres.thumbnail.w, fres.thumbnail.h,
                                        fres.thumbnail.data_size);
                                } else {
                                    /* Nothing allocated, nothing to free — just let
                                     * the next event ask again. */
                                    nina_layout_image_note_capture_request(fres.instance_idx,
                                                                          false);
                                }
                                nina_dashboard_set_thumbnail(buf, fres.thumbnail.w,
                                    fres.thumbnail.h, fres.thumbnail.data_size);
                            } else if (want_cap) {
                                nina_layout_image_set_capture(fres.instance_idx, buf,
                                    fres.thumbnail.w, fres.thumbnail.h,
                                    fres.thumbnail.data_size);
                            } else if (want_thumb) {
                                nina_dashboard_set_thumbnail(buf, fres.thumbnail.w,
                                    fres.thumbnail.h, fres.thumbnail.data_size);
                            } else {
                                free(buf);
                            }
                            bsp_display_unlock();
                        } else {
                            free(fres.thumbnail.rgb565_data);
                        }
                    } else {
                        /* No frame came back. Drop the Image-forward latch so the
                         * next event gets one more try, and hide the overlay if a
                         * user-triggered thumbnail was what failed. */
                        bool hide_overlay = nina_dashboard_thumbnail_requested();
                        if (hide_overlay) nina_dashboard_clear_thumbnail_request();
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_layout_image_note_capture_request(fres.instance_idx, false);
                            if (hide_overlay) nina_dashboard_hide_thumbnail();
                            bsp_display_unlock();
                        }
                    }
                    break;

                case FETCH_GRAPH_RMS:
                    fetch_graph_pending = false;
                    if (fres.success && fres.data) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_graph_set_rms_data((graph_rms_data_t *)fres.data);
                            bsp_display_unlock();
                        }
                    }
                    last_graph_fetch_ms = esp_timer_get_time() / 1000;
                    break;

                case FETCH_GRAPH_HFR:
                case FETCH_GRAPH_HFR_RING:
                    fetch_graph_pending = false;
                    if (fres.success && fres.data) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_graph_set_hfr_data((graph_hfr_data_t *)fres.data);
                            bsp_display_unlock();
                        }
                    }
                    if (fres.type == FETCH_GRAPH_HFR) hfr_graph_seeded = true;
                    last_graph_fetch_ms = esp_timer_get_time() / 1000;
                    break;

                case FETCH_INFO_CAMERA:
                    fetch_info_pending = false;
                    if (fres.success && fres.data) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_camera_data((camera_detail_data_t *)fres.data);
                            bsp_display_unlock();
                        }
                        heap_caps_free(fres.data);
                    }
                    break;

                case FETCH_INFO_MOUNT:
                    fetch_info_pending = false;
                    if (fres.success && fres.data) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_mount_data((mount_detail_data_t *)fres.data);
                            bsp_display_unlock();
                        }
                        heap_caps_free(fres.data);
                    }
                    break;

                case FETCH_INFO_SEQUENCE:
                    fetch_info_pending = false;
                    if (fres.success && fres.data) {
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_sequence_data((sequence_detail_data_t *)fres.data);
                            bsp_display_unlock();
                        }
                        heap_caps_free(fres.data);
                    }
                    break;

                default:
                    break;
                }
            }
        }

        /* ── Apply any pending MQTT commands (brightness/text/theme/reboot) ──
         * The MQTT event callback only parses+enqueues; the blocking apply
         * (display lock, backlight, config save) happens here in UI context. */
        mqtt_ha_process_pending();

        int current_active = nina_dashboard_get_active_page();  // Snapshot to avoid races
        bool on_allsky = nina_dashboard_is_allsky_page();
        bool on_json = nina_dashboard_is_json_page();
        bool on_ha = nina_dashboard_is_ha_page();
        bool on_octoprint = nina_dashboard_is_octoprint_page();
        bool on_adsb = nina_dashboard_is_adsb_page();
        bool on_sysinfo = nina_dashboard_is_sysinfo_page();
        bool on_settings = nina_dashboard_is_settings_page();
        bool on_summary = nina_dashboard_is_summary_page();
        bool on_clock = nina_dashboard_is_clock_page();
        bool on_image = PAGE_IDX_IS_IMAGE(current_active);

        /*
         * Page index convention (see PAGE_IDX_* / NINA_PAGE_OFFSET / EXTRA_PAGES):
         *   PAGE_IDX_ALLSKY        (0)                  = AllSky page
         *   PAGE_IDX_SPOTIFY       (1)                  = Spotify page
         *   PAGE_IDX_CLOCK         (2)                  = Clock page (always present)
         *   PAGE_IDX_IMG_GOES      (3)                  = GOES Satellite image page
         *   PAGE_IDX_IMG_MOON      (4)                  = Moon image page
         *   PAGE_IDX_IMG_SOLAR     (5)                  = Solar image page
         *   PAGE_IDX_IMG_CUSTOM    (6)                  = Custom Image page
         *   PAGE_IDX_IMG_RADAR     (7)                  = Weather Radar image page
         *   PAGE_IDX_IMG_CLOUDS    (8)                  = Clouds satellite image page
         *   PAGE_IDX_JSON          (9)                  = JSON Display page
         *   PAGE_IDX_HA            (10)                 = Home Assistant page
         *   PAGE_IDX_OCTOPRINT     (11)                 = OctoPrint 3D Printer page
         *   PAGE_IDX_ADSB          (12)                 = ADS-B aircraft page
         *   PAGE_IDX_SUMMARY       (13)                 = summary page
         *   NINA_PAGE_OFFSET .. NINA_PAGE_OFFSET+pc-1   = NINA instance pages
         *   SETTINGS_PAGE_IDX(pc)                       = settings page
         *   SYSINFO_PAGE_IDX(pc)                        = sysinfo page
         *
         * active_nina_idx: the actual instance index (0..MAX_NINA_INSTANCES-1)
         *   for the active page, or -1 if on allsky/json/ha/octoprint/spotify/clock/summary/settings/sysinfo.
         */
        bool on_spotify = nina_dashboard_is_spotify_page();
        int active_nina_idx = -1;   /* Actual instance index (for data access) */
        int active_page_idx = -1;  /* ABSOLUTE page index (for UI calls) */
        if (!on_allsky && !on_spotify && !on_sysinfo && !on_settings && !on_summary
            && current_active >= NINA_PAGE_OFFSET) {
            active_page_idx = current_active;  /* absolute index */
            active_nina_idx = nina_dashboard_page_to_instance(current_active);
            /* Mapping is pure-offset; gate on slot availability explicitly. */
            if (active_nina_idx >= 0 && !nina_slot_available[active_nina_idx])
                active_nina_idx = -1;
        }

        /* ── Page-gate flags and resource lifecycle ──
         * Each feature's poll task checks its page-active flag and suspends when
         * inactive.  WebSocket TLS is torn down whenever leaving NINA/Summary pages
         * to free internal DMA heap for SDIO WiFi transport buffers. */
        {
            bool now_nina_active = (on_summary || active_nina_idx >= 0);

            /* NINA WebSocket lifecycle — tear down when leaving NINA pages,
             * poll tasks will reconnect naturally when nina_pages_active goes true */
            static bool prev_nina_active = true;  /* Assume NINA active on boot */
            if (!now_nina_active && prev_nina_active) {
                nina_websocket_stop_all();
                /* Dismiss thumbnail overlay if open (frees original + scaled buffers) */
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    if (nina_dashboard_thumbnail_visible())
                        nina_dashboard_hide_thumbnail();
                    bsp_display_unlock();
                }
                ESP_LOGI(TAG, "Left NINA pages: freed WebSocket TLS sessions");
            } else if (now_nina_active && !prev_nina_active) {
                ESP_LOGI(TAG, "Entered NINA pages: poll tasks will reconnect");
            }
            prev_nina_active = now_nina_active;
            nina_pages_active = now_nina_active;

            /* Spotify lifecycle — wake on entry, free art on leave */
            static bool prev_on_spotify = false;
            if (on_spotify && !prev_on_spotify && spotify_task_handle) {
                xTaskNotifyGive(spotify_task_handle);
            } else if (!on_spotify && prev_on_spotify) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    nina_spotify_free_art();
                    bsp_display_unlock();
                }
                ESP_LOGI(TAG, "Left Spotify: freed album art buffer");
            }
            prev_on_spotify = on_spotify;
            spotify_page_active = on_spotify;

            /* AllSky and Clock flags — wake tasks immediately on page entry.
             * On leave, tell the client so it can destroy its keep-alive conn
             * slot (gate flag first, so the poll task stops before teardown —
             * same ordering as OctoPrint below). */
            static bool prev_on_allsky = false;
            if (on_allsky && !prev_on_allsky && allsky_task_handle) {
                xTaskNotifyGive(allsky_task_handle);
            }
            allsky_page_active = on_allsky;
            if (on_allsky != prev_on_allsky) {
                allsky_client_set_page_active(on_allsky);
            }
            prev_on_allsky = on_allsky;

            /* JSON Display flag — wake task immediately on page entry; conn
             * teardown on leave, as above */
            static bool prev_on_json = false;
            if (on_json && !prev_on_json && json_task_handle) {
                xTaskNotifyGive(json_task_handle);
            }
            json_page_active = on_json;
            if (on_json != prev_on_json) {
                json_client_set_page_active(on_json);
            }
            prev_on_json = on_json;

            /* Home Assistant flag — wake task immediately on page entry; conn
             * teardown on leave, as above */
            static bool prev_on_ha = false;
            if (on_ha && !prev_on_ha && ha_task_handle) {
                xTaskNotifyGive(ha_task_handle);
            }
            ha_page_active = on_ha;
            if (on_ha != prev_on_ha) {
                ha_client_set_page_active(on_ha);
            }
            prev_on_ha = on_ha;

            /* OctoPrint flag — wake on entry; on leave, gate the poll task
             * BEFORE releasing the decoded image. A poll already past its
             * page-active check still publishes, so this is not a lifetime
             * guarantee (the client frees its buffer under its own mutex); it
             * only keeps the common case from re-decoding a frame we are about
             * to drop. Two buffers are held: the client's, released by
             * set_page_active(false), and the page's own copy. */
            static bool prev_on_octoprint = false;
            if (on_octoprint && !prev_on_octoprint && octoprint_task_handle) {
                xTaskNotifyGive(octoprint_task_handle);
            }
            octoprint_page_active = on_octoprint;
            if (on_octoprint != prev_on_octoprint) {
                octoprint_client_set_page_active(on_octoprint, &octoprint_data);
                if (!on_octoprint) {
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        octoprint_page_free_image();
                        bsp_display_unlock();
                    }
                    ESP_LOGI(TAG, "Left OctoPrint page: freed image buffers");
                }
            }
            prev_on_octoprint = on_octoprint;

            static bool prev_on_clock = false;
            if (on_clock && !prev_on_clock) {
                weather_client_force_refresh();  /* Wakes weather task via xTaskNotifyGive */
            }
            prev_on_clock = on_clock;
            clock_page_active = on_clock;
        }

        /* ── Demo mode live transitions ──
         * Reconcile the config flag against the generator each cycle: no
         * reboot needed in either direction. ON gates the pollers first so
         * nothing writes over demo data; OFF waits (bounded, <=3 s) for the
         * demo task's stop cleanup, then hands the structs back to real
         * polling with fresh poll state. */
        {
            bool want = app_config_get()->demo_mode;
            if (want && !demo_data_is_running()) {
                demo_active = true;                       /* gate pollers first */
                for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
                    if (poll_task_handles[i]) xTaskNotifyGive(poll_task_handles[i]);
                    nina_websocket_stop(i);               /* real WS must not write over demo data */
                }
                instance_count = 3;
                demo_data_start(instances, allsky_task_handle ? NULL : &allsky_data, 3);
                nav_arbiter_notify_topology_changed();
            } else if (!want && demo_active) {
                /* OFF path keyed on demo_active, not demo_data_is_running():
                 * a failed demo spawn latches demo_active with no generator,
                 * and the gate must still drop or pollers park forever. */
                if (demo_data_is_running()) {
                    demo_data_stop();
                    for (int i = 0; i < 30 && demo_data_is_running(); i++) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {   /* fresh poll state for the real world */
                        nina_poll_state_init(&poll_states[i]);
                        poll_contexts[i].filters_synced = false;
                        poll_contexts[i].last_heartbeat_ms = 0;
                    }
                    ensure_network_stack();               /* no-op on a normal boot; first spawn after a demo boot */
                }
                demo_active = false;
                instance_count = app_config_get_instance_count();
                for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
                    if (poll_task_handles[i]) xTaskNotifyGive(poll_task_handles[i]);
                }
                nav_arbiter_notify_topology_changed();
            }
            /* Re-read instance count from config so API URL changes take
             * effect live; in demo mode it stays pinned at 3. */
            if (!want) {
                instance_count = app_config_get_instance_count();
            }
        }

        // Check for debug mode toggle
        {
            static bool last_debug_mode = false;
            static bool first_check = true;
            bool current_debug = app_config_get()->debug_mode;
            if (first_check || current_debug != last_debug_mode) {
                perf_monitor_set_enabled(current_debug);
                net_trace_set_verbose(current_debug);
                /* Suppress verbose per-poll INFO logs when not debugging */
                esp_log_level_t lvl = current_debug ? ESP_LOG_INFO : ESP_LOG_WARN;
                esp_log_level_set("nina_client", lvl);
                esp_log_level_set("nina_fetch", lvl);
                esp_log_level_set("nina_seq", lvl);
                last_debug_mode = current_debug;
                first_check = false;
            }
        }

        // Check for WiFi power save toggle
        {
            static bool last_wifi_ps = true;
            static bool first_ps_check = true;
            bool current_ps = app_config_get()->wifi_power_save;
            if (first_ps_check || current_ps != last_wifi_ps) {
                esp_wifi_set_ps(current_ps ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
                ESP_LOGI(TAG, "WiFi power save %s", current_ps ? "enabled" : "disabled");
                last_wifi_ps = current_ps;
                first_ps_check = false;
            }
        }

        /* ── On-demand firmware update check (triggered from settings page) ── */
        if (ota_check_requested) {
            ota_check_requested = false;
            ESP_LOGI(TAG, "On-demand firmware update check...");
            github_release_info_t *rel = heap_caps_calloc(1, sizeof(github_release_info_t), MALLOC_CAP_SPIRAM);
            if (rel) {
                int update_channel = app_config_get()->update_channel;
                const char *cur_ver = ota_github_get_current_version();
                ota_check_result_t chk = ota_github_check(update_channel, cur_ver, rel);
                if (chk == OTA_CHECK_UPDATE_AVAILABLE && rel->requires_full_erase) {
                    /* This release cannot be installed over WiFi — show a
                     * blocking warning and wait only for dismissal. */
                    ESP_LOGW(TAG, "Firmware %s requires manual USB erase+flash", rel->tag);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show_manual_flash(rel->tag, rel->full_erase_tag);
                        bsp_display_unlock();
                    }
                    while (nina_ota_prompt_visible()) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                } else if (chk == OTA_CHECK_UPDATE_AVAILABLE) {
                    ESP_LOGI(TAG, "New firmware available: %s", rel->tag);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show(rel->tag, cur_ver, rel->summary);
                        bsp_display_unlock();
                    }
                    /* Wait for user to accept or skip (flags clear on read, so store result) */
                    bool accepted = false;
                    while (1) {
                        if (nina_ota_prompt_update_accepted()) { accepted = true;  break; }
                        if (nina_ota_prompt_skipped())          { accepted = false; break; }
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                    if (accepted && atomic_exchange(&ota_in_progress, true)) {
                        /* Another OTA is already running — do not start a second
                         * write stream against the same partition. */
                        ESP_LOGW(TAG, "OTA already in progress — ignoring update request");
                        nina_toast_show(TOAST_WARNING, "Update already in progress");
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_ota_prompt_hide();
                            bsp_display_unlock();
                        }
                    } else if (accepted) {
                        /* Accepted — ota_in_progress was set true by the exchange above. */
                        ESP_LOGI(TAG, "User accepted OTA update to %s", rel->tag);
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_ota_prompt_show_progress();
                            bsp_display_unlock();
                        }
                        esp_err_t ota_err = ota_github_download(rel->ota_url, ota_progress_cb);
                        if (ota_err == ESP_OK) {
                            ota_github_save_pending_version(rel->tag);
                            ESP_LOGI(TAG, "OTA download success");
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_ota_prompt_set_progress(100);
                                bsp_display_unlock();
                            }
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            app_reboot("on-demand OTA complete");
                        } else {
                            ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(ota_err));
                            ota_in_progress = false;
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_ota_prompt_show_error("Download failed. Please try again later.");
                                bsp_display_unlock();
                            }
                            while (nina_ota_prompt_visible()) {
                                vTaskDelay(pdMS_TO_TICKS(200));
                            }
                        }
                    } else {
                        ESP_LOGI(TAG, "User skipped firmware update");
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_ota_prompt_hide();
                            bsp_display_unlock();
                        }
                    }
                } else if (chk == OTA_CHECK_ERROR || chk == OTA_CHECK_RATE_LIMITED) {
                    bool limited = (chk == OTA_CHECK_RATE_LIMITED);
                    ESP_LOGW(TAG, "Firmware update check %s",
                             limited ? "hit the GitHub rate limit" : "failed (network/GitHub)");
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show("", cur_ver, NULL);
                        nina_ota_prompt_show_status(
                            limited ? "Update limit reached" : "Update check failed",
                            limited ? "GitHub update limit reached. Try again in about an hour."
                                    : "Update check failed - try again");
                        bsp_display_unlock();
                    }
                    while (nina_ota_prompt_visible()) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                } else {
                    ESP_LOGI(TAG, "No firmware update available");
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show("", cur_ver, NULL);
                        nina_ota_prompt_show_status("Up to Date", "You are running the latest firmware.");
                        bsp_display_unlock();
                    }
                    while (nina_ota_prompt_visible()) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
                heap_caps_free(rel);
            }
        }

        // Handle page change
        if (page_changed) {
            page_changed = false;
            last_rotate_ms = esp_timer_get_time() / 1000;  // Reset auto-rotate timer on any page change
            ESP_LOGI(TAG, "Page switched to %d%s%s%s%s", current_active,
                     on_allsky ? " (allsky)" : "",
                     on_sysinfo ? " (sysinfo)" : "", on_settings ? " (settings)" : "",
                     on_summary ? " (summary)" : "");

            /* Immediate summary render with cached data */
            if (on_summary) {
                bool locked[MAX_NINA_INSTANCES];
                for (int j = 0; j < instance_count; j++)
                    locked[j] = nina_client_lock(&instances[j], 15);
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    summary_page_update(instances, instance_count, locked);
                    bsp_display_unlock();
                }
                for (int j = 0; j < instance_count; j++)
                    if (locked[j]) nina_client_unlock(&instances[j]);
            }

            /* Immediate AllSky render with cached data */
            if (on_allsky) {
                if (allsky_data_lock(&allsky_data, 15)) {
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        allsky_page_update(&allsky_data);
                        bsp_display_unlock();
                    }
                    allsky_data_unlock(&allsky_data);
                }
            }

            /* Immediate JSON Display render with cached data */
            if (on_json) {
                if (json_client_lock(&json_data, 15)) {
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        json_page_update(&json_data);
                        bsp_display_unlock();
                    }
                    json_client_unlock(&json_data);
                }
            }

            /* Immediate Home Assistant render with cached data */
            if (on_ha) {
                if (ha_client_lock(&ha_data, 15)) {
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        ha_page_update(&ha_data);
                        bsp_display_unlock();
                    }
                    ha_client_unlock(&ha_data);
                }
            }

            /* Immediate ADS-B render with cached data */
            if (on_adsb) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    nina_adsb_update();
                    bsp_display_unlock();
                }
            }

            /* Immediate OctoPrint render with cached data.
             * octoprint_page_update takes the display lock itself (client lock
             * outside, display lock inside) so the image rescale runs outside
             * the display lock. */
            if (on_octoprint) {
                if (octoprint_client_lock(&octoprint_data, 15)) {
                    octoprint_page_update(&octoprint_data);
                    octoprint_client_unlock(&octoprint_data);
                }
            }
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        /* Once-daily crash-log retention purge (piggybacked on this loop — no
         * dedicated task). First pass runs ~24 h after boot; the boot-time purge
         * inside crash_log_init() covers the startup case. */
        if (last_crash_purge_ms == 0) {
            last_crash_purge_ms = now_ms;
        } else if (now_ms - last_crash_purge_ms >= (int64_t)86400 * 1000) {
            crash_log_purge_old(app_config_get()->crash_log_retention_days);
            last_crash_purge_ms = now_ms;
        }

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

        /* Slideshow-interval edge feeder. The navigation arbiter owns the actual
         * page advance; tasks.c only fires the tick when the configured interval
         * elapses. resolve() (called once near the end of this cycle) consumes it.
         * A content-ready dwell restart (picture finished loading) resets the
         * interval so the loaded page gets its full dwell. */
        {
            app_config_t *r_cfg = app_config_get();
            if (r_cfg->auto_rotate_enabled && r_cfg->auto_rotate_interval_s > 0) {
                if (nav_arbiter_take_dwell_restart()) last_rotate_ms = now_ms;
                if (last_rotate_ms == 0) last_rotate_ms = now_ms;
                if (now_ms - last_rotate_ms >= (int64_t)r_cfg->auto_rotate_interval_s * 1000) {
                    nav_arbiter_notify_slideshow_tick();
                    last_rotate_ms = now_ms;
                }
            } else {
                (void)nav_arbiter_take_dwell_restart();   /* drain: never leak a stale flag */
                last_rotate_ms = 0;
            }
        }

        // Read WiFi RSSI once per cycle
        int rssi = -100;
        {
            wifi_ap_record_t ap_info = {0};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi;
                perf_monitor_record_wifi(&ap_info);
            }
        }

        /* ── Consolidated UI update: one LVGL lock section per active page type ──
         * Pre-compute data outside the LVGL lock, then do all UI updates in a single
         * lock/unlock to minimize contention with the LVGL render task. */

        if (on_sysinfo) {
            /* Sysinfo page — no external mutexes needed, single lock section */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                sysinfo_page_refresh();
                bsp_display_unlock();
            }
        } else if (on_allsky) {
            /* AllSky page — pre-lock allsky data, then single LVGL lock */
            if (allsky_data_lock(&allsky_data, 15)) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    allsky_page_update(&allsky_data);
                    bsp_display_unlock();
                }
                allsky_data_unlock(&allsky_data);
            }
        } else if (on_json) {
            /* JSON Display page — trylock-and-skip the json data (like AllSky),
             * then a single LVGL lock. Skipping a cycle rather than blocking the
             * UI preserves the lock-ordering discipline. */
            if (json_client_lock(&json_data, 15)) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    json_page_update(&json_data);
                    bsp_display_unlock();
                }
                json_client_unlock(&json_data);
            }
        } else if (on_ha) {
            /* Home Assistant page — trylock-and-skip the ha data (like JSON),
             * then a single LVGL lock. Skipping a cycle rather than blocking the
             * UI preserves the lock-ordering discipline. */
            if (ha_client_lock(&ha_data, 15)) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    ha_page_update(&ha_data);
                    bsp_display_unlock();
                }
                ha_client_unlock(&ha_data);
            }
        } else if (on_octoprint) {
            /* OctoPrint page — trylock-and-skip the octoprint data (like JSON).
             * The display lock is taken INSIDE octoprint_page_update (client
             * lock outside, display lock inside), so the bilinear image rescale
             * runs before it and never stalls the flush task. Skipping a cycle
             * rather than blocking the UI preserves lock-ordering discipline. */
            if (octoprint_client_lock(&octoprint_data, 15)) {
                octoprint_page_update(&octoprint_data);
                /* Any terminal image verdict (OK / no job / no thumbnail /
                 * webcam failed) means the page has resolved what it shows. */
                bool octo_loaded = (octoprint_data.image_status != OCTO_IMG_PENDING);
                octoprint_client_unlock(&octoprint_data);
                if (octo_loaded) nav_arbiter_notify_content_ready(PAGE_IDX_OCTOPRINT);
            }
        } else if (on_adsb) {
            /* ADS-B page — one LVGL lock section. nina_adsb_update() takes the
             * adsb_client mutex (pointer-swap publication, never held across a
             * parse) and the per-instance NINA client lock, both as LEAF locks
             * with a bounded timed acquire that skips on timeout. That is the
             * one documented exception to "client lock outside, display lock
             * inside": nothing here can block the display lock indefinitely, and
             * the same call already runs from the LVGL touch handler on a drag,
             * where the display lock is held by LVGL itself and cannot be
             * released first. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_adsb_update();
                bsp_display_unlock();
            }
        } else if (on_image) {
            /* Image page — repaint if the poller committed a newer frame (the
             * poller also pushes directly; this is the retry after a skipped
             * crossfade). render_frame takes frame_mux internally; the display
             * lock is held here (LVGL outer, frame_mux inner). */
            image_page_t *ip = image_page_by_page_idx(current_active);
            if (ip && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                image_page_render_frame(ip);
                bsp_display_unlock();
            }
        } else if (on_summary) {
            /* Summary page — pre-lock all instances with short timeout, then single LVGL lock */
            bool locked[MAX_NINA_INSTANCES];
            for (int j = 0; j < instance_count; j++)
                locked[j] = nina_client_lock(&instances[j], 15);

            perf_timer_start(&g_perf.ui_update_total);
            int64_t lock_start = g_perf.enabled ? esp_timer_get_time() : 0;
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                if (g_perf.enabled) perf_timer_record(&g_perf.ui_lock_wait, esp_timer_get_time() - lock_start);
                perf_timer_start(&g_perf.ui_summary_update);
                summary_page_update(instances, instance_count, locked);
                perf_timer_stop(&g_perf.ui_summary_update);
                bsp_display_unlock();
            }
            perf_timer_stop(&g_perf.ui_update_total);

            for (int j = 0; j < instance_count; j++)
                if (locked[j]) nina_client_unlock(&instances[j]);

            /* Yield to LVGL render task after summary update */
            taskYIELD();
        } else if (active_nina_idx >= 0 && active_page_idx >= 0) {
            /* NINA instance page — pre-lock instance data, then single LVGL lock
             * for dashboard update + status dot (combined, no separate lock) */
            if (nina_client_lock(&instances[active_nina_idx], 15)) {

                perf_timer_start(&g_perf.ui_update_total);
                int64_t lock_start2 = g_perf.enabled ? esp_timer_get_time() : 0;
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    if (g_perf.enabled) perf_timer_record(&g_perf.ui_lock_wait, esp_timer_get_time() - lock_start2);
                    perf_timer_start(&g_perf.ui_dashboard_update);
                    update_nina_dashboard_page(active_nina_idx, &instances[active_nina_idx]);
                    perf_timer_stop(&g_perf.ui_dashboard_update);

                    // Measure WS-to-UI latency if a recent event was received
                    if (g_perf.enabled && g_perf.last_ws_event_time_us > 0) {
                        int64_t latency = esp_timer_get_time() - g_perf.last_ws_event_time_us;
                        if (latency < 5000000) {  // Only if within 5 seconds (not stale)
                            perf_timer_record(&g_perf.latency_ws_to_ui, latency);
                        }
                        g_perf.last_ws_event_time_us = 0;  // Reset after measuring
                    }

                    /* Status dot update combined in same lock section (was separate lock before) */
                    nina_dashboard_update_status(active_nina_idx, rssi,
                                                 nina_connection_is_connected(active_nina_idx), true);
                    bsp_display_unlock();
                }
                perf_timer_stop(&g_perf.ui_update_total);

                nina_client_unlock(&instances[active_nina_idx]);
            }

            /* Yield to LVGL render task between UI update and fetch handling */
            taskYIELD();

            /* ── Async thumbnail fetch (offloaded to Core 0 fetch worker) ── */
            bool want_thumbnail = nina_dashboard_thumbnail_requested();
            /* Image-forward needs the same decoded frame for its page background:
             * once on entry (it has none yet) and on every new image after that. */
            bool on_image_layout = (nina_slot_available[active_nina_idx]
                                    && pages[active_nina_idx].layout == 1);
            bool want_capture = on_image_layout
                                && nina_layout_image_needs_capture(active_nina_idx);
            bool auto_refresh = false;
            if (nina_client_lock(&instances[active_nina_idx], 15)) {
                auto_refresh = (nina_dashboard_thumbnail_visible() || on_image_layout)
                               && instances[active_nina_idx].new_image_available;
                if (auto_refresh) instances[active_nina_idx].new_image_available = false;
                nina_client_unlock(&instances[active_nina_idx]);
            }

            if ((want_thumbnail || auto_refresh || want_capture) && !fetch_thumbnail_pending) {
                if (want_thumbnail) nina_dashboard_clear_thumbnail_request();

                const char *thumb_url = app_config_get_instance_url(active_nina_idx);
                if (strlen(thumb_url) > 0 && nina_connection_is_connected(active_nina_idx) && s_fetch_queue) {
                    fetch_request_t req = { .type = FETCH_THUMBNAIL, .instance_idx = active_nina_idx };
                    strlcpy(req.url, thumb_url, sizeof(req.url));
                    if (xQueueSend(s_fetch_queue, &req, 0) == pdTRUE) {
                        fetch_thumbnail_pending = true;
                        /* Latch only now the request is really in flight. An empty
                         * URL, a disconnected rig or a full queue must leave the
                         * next cycle free to ask again. */
                        if (want_capture && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_layout_image_note_capture_request(active_nina_idx, true);
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

            /* ── Async graph overlay data fetch (offloaded to Core 0) ── */
            if (nina_graph_requested() && !fetch_graph_pending) {
                nina_graph_clear_request();
                const char *graph_url = app_config_get_instance_url(active_nina_idx);
                if (strlen(graph_url) > 0 && nina_connection_is_connected(active_nina_idx) && s_fetch_queue) {
                    graph_type_t gtype = nina_graph_get_type();
                    int gpoints = nina_graph_get_requested_points();

                    fetch_request_t req = {
                        .instance_idx = active_nina_idx,
                        .max_points = gpoints,
                    };
                    strlcpy(req.url, graph_url, sizeof(req.url));

                    if (gtype == GRAPH_TYPE_RMS) {
                        req.type = FETCH_GRAPH_RMS;
                    } else if (!hfr_graph_seeded) {
                        req.type = FETCH_GRAPH_HFR;
                    } else {
                        req.type = FETCH_GRAPH_HFR_RING;
                        req.client = &instances[active_nina_idx];
                    }

                    if (xQueueSend(s_fetch_queue, &req, 0) == pdTRUE) {
                        fetch_graph_pending = true;
                    }
                }
            }

            /* Auto-refresh autofocus overlay while visible (data comes from WebSocket — no HTTP) */
            if (nina_info_overlay_visible()
                && nina_info_overlay_get_type() == INFO_OVERLAY_AUTOFOCUS
                && !nina_info_overlay_requested()) {
                if (nina_client_lock(&instances[active_nina_idx], 15)) {
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

            /* ── Async info overlay data fetch (HTTP types offloaded to Core 0) ── */
            if (nina_info_overlay_requested() && !fetch_info_pending) {
                nina_info_overlay_clear_request();
                info_overlay_type_t itype = nina_info_overlay_get_type();

                /* Session stats uses on-device data — no API fetch needed */
                if (itype == INFO_OVERLAY_SESSION_STATS) {
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_info_overlay_set_session_stats(active_nina_idx);
                        bsp_display_unlock();
                    }
                } else if (itype == INFO_OVERLAY_IMAGESTATS) {
                    /* Image stats come from WebSocket events — read locally, no HTTP */
                    if (nina_client_lock(&instances[active_nina_idx], 15)) {
                        imagestats_detail_data_t stats = instances[active_nina_idx].last_image_stats;
                        nina_client_unlock(&instances[active_nina_idx]);
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_imagestats_data(&stats);
                            bsp_display_unlock();
                        }
                    }
                } else if (itype == INFO_OVERLAY_FILTER) {
                    /* Filter data comes from nina_client_t — read locally, no HTTP */
                    filter_detail_data_t filt_data = {0};
                    if (nina_client_lock(&instances[active_nina_idx], 15)) {
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
                    /* Autofocus data comes from WebSocket events — read locally */
                    if (nina_client_lock(&instances[active_nina_idx], 15)) {
                        autofocus_data_t af_data = instances[active_nina_idx].autofocus;
                        nina_client_unlock(&instances[active_nina_idx]);
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_info_overlay_set_autofocus_data(&af_data);
                            bsp_display_unlock();
                        }
                    }
                } else {
                    /* HTTP-requiring overlays: camera, mount, sequence — offload to Core 0 */
                    const char *info_url = app_config_get_instance_url(active_nina_idx);
                    if (strlen(info_url) > 0 && nina_connection_is_connected(active_nina_idx) && s_fetch_queue) {
                        fetch_request_t req = { .instance_idx = active_nina_idx };
                        strlcpy(req.url, info_url, sizeof(req.url));

                        if (itype == INFO_OVERLAY_CAMERA) req.type = FETCH_INFO_CAMERA;
                        else if (itype == INFO_OVERLAY_MOUNT) req.type = FETCH_INFO_MOUNT;
                        else if (itype == INFO_OVERLAY_SEQUENCE) req.type = FETCH_INFO_SEQUENCE;

                        if (xQueueSend(s_fetch_queue, &req, 0) == pdTRUE) {
                            fetch_info_pending = true;
                        }
                    }
                }
            }
        }

        // ── Event-driven UI refresh: check if any WS event needs immediate UI update ──
        if (active_nina_idx >= 0 && active_page_idx >= 0
            && atomic_exchange(&instances[active_nina_idx].ui_refresh_needed, false)) {
            if (nina_client_lock(&instances[active_nina_idx], 15)) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    update_nina_dashboard_page(active_nina_idx, &instances[active_nina_idx]);
                    nina_dashboard_update_status(active_nina_idx, rssi,
                                                 nina_connection_is_connected(active_nina_idx), false);
                    bsp_display_unlock();
                }
                nina_client_unlock(&instances[active_nina_idx]);
            }
        }

        // ── Session stats recording + safety monitor + RMS/HFR alerts ──
        // Lock each instance briefly (trylock) to read a consistent snapshot of scalar fields.
        // These are non-critical — if lock contended, skip this cycle.
        for (int i = 0; i < instance_count; i++) {
            if (!nina_connection_is_connected(i)) continue;

            float rms_total, hfr, cam_temp, cooler_pwr;
            int stars;
            bool safety_conn, safety_safe;

            if (nina_client_lock(&instances[i], 0)) {
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
                /* Also feed the breach engine here, not just on the WebSocket
                 * SAFETY-CHANGED edge: a sustained unsafe state has no further
                 * events, and the periodic re-announce needs a periodic sample.
                 * Idempotent -- repeat samples of the same state do nothing. */
                nina_alert_eval_safety(i, safety_safe);
            }

            /* Alert eval runs unconditionally: nina_alert_eval() owns the breach
             * edge, the hysteresis recovery and the voice re-announce timer, and
             * applies the alert_flash_enabled / alert_voice_enabled gates itself.
             * Gating here instead would leave breach state stale whenever a
             * setting is toggled mid-session. */
            threshold_config_t rms_cfg;
            app_config_get_rms_threshold_config(i, &rms_cfg);
            nina_alert_eval(ALERT_RMS, i, rms_total, rms_cfg.ok_max);

            threshold_config_t hfr_cfg;
            app_config_get_hfr_threshold_config(i, &hfr_cfg);
            nina_alert_eval(ALERT_HFR, i, hfr, hfr_cfg.ok_max);
        }

        /* ── Navigation arbiter: resolve the page-commit ladder once per cycle ──
         * Runs AFTER per-page UI updates and the slideshow-tick feeder, OUTSIDE
         * any LVGL lock (the arbiter takes the lock itself around the commit).
         * This is the single owner of the navigation decision. It also owns the
         * idle indicator: when the resolved level enters or leaves NAV_SRC_IDLE
         * it calls nina_idle_indicator_set_active() itself (nina_nav_arbiter.c),
         * so nothing here touches that overlay. A user wake (xTaskNotifyGive
         * from on_page_changed) produces a resolve within one cycle. */
        nav_arbiter_resolve(esp_timer_get_time() / 1000);

        /* ── Screen sleep: turn off backlight when idle ── */
        {
            app_config_t *sl_cfg = app_config_get();
            if (sl_cfg->screen_sleep_enabled) {

                /* Touch wake — always check first, even if connections are back */
                if (screen_asleep && screen_touch_wake) {
                    /* Restore WiFi power save mode */
                    esp_wifi_set_ps(sl_cfg->wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
                    wifi_apply_tx_power(sl_cfg->wifi_max_tx_dbm);
                    /* Resume LVGL processing */
                    lvgl_port_lock(0);
                    lvgl_port_resume();
                    lv_obj_invalidate(lv_scr_act());
                    lvgl_port_unlock();
                    bsp_display_brightness_set(sl_cfg->brightness);
                    screen_asleep = false;
                    screen_touch_wake = false;
                    all_disconnected_since_ms = now_ms;  /* restart sleep timer */
                    spotify_idle_since_ms = now_ms;      /* restart Spotify sleep timer */
                    /* Wake poll tasks so they resume full polling + WS reconnect */
                    for (int i = 0; i < instance_count; i++)
                        if (poll_task_handles[i]) xTaskNotifyGive(poll_task_handles[i]);
                    ESP_LOGI(TAG, "Screen wake: touch detected");
                }

                /* Determine if we should sleep or wake based on context:
                 * - On Spotify page: sleep when not playing, wake when playing
                 * - On other pages: sleep when all NINA disconnected, wake on reconnect */
                bool should_sleep = false;
                bool should_wake = false;
                const char *wake_reason = NULL;
                const char *sleep_reason = NULL;

                if (on_spotify && sl_cfg->spotify_enabled) {
                    /* Spotify page: sleep/wake based on playback state */
                    spotify_playback_t spb;
                    spotify_client_get_cached_playback(&spb);

                    if (spb.is_playing) {
                        spotify_idle_since_ms = 0;
                        if (screen_asleep) {
                            should_wake = true;
                            wake_reason = "Spotify playing";
                        }
                    } else {
                        /* Not playing — start/continue idle timer */
                        if (spotify_idle_since_ms == 0) {
                            spotify_idle_since_ms = now_ms;
                        }
                        if (!screen_asleep &&
                            (now_ms - spotify_idle_since_ms >= (int64_t)sl_cfg->screen_sleep_timeout_s * 1000)) {
                            should_sleep = true;
                            sleep_reason = "Spotify idle";
                        }
                    }
                } else {
                    /* Not on Spotify — reset Spotify timer, use NINA logic */
                    spotify_idle_since_ms = 0;
                    int connected = nina_connection_connected_count();

                    if (connected > 0) {
                        all_disconnected_since_ms = 0;
                        if (screen_asleep) {
                            should_wake = true;
                            wake_reason = "NINA connected";
                        }
                    } else {
                        /* All disconnected */
                        if (all_disconnected_since_ms == 0) {
                            all_disconnected_since_ms = now_ms;
                        }
                        if (!screen_asleep &&
                            (now_ms - all_disconnected_since_ms >= (int64_t)sl_cfg->screen_sleep_timeout_s * 1000)) {
                            should_sleep = true;
                            sleep_reason = "no NINA connections";
                        }
                    }
                }

                if (should_wake) {
                    /* Restore WiFi power save mode */
                    esp_wifi_set_ps(sl_cfg->wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
                    wifi_apply_tx_power(sl_cfg->wifi_max_tx_dbm);
                    /* Resume LVGL processing */
                    lvgl_port_lock(0);
                    lvgl_port_resume();
                    lv_obj_invalidate(lv_scr_act());
                    lvgl_port_unlock();
                    bsp_display_brightness_set(sl_cfg->brightness);
                    screen_asleep = false;
                    /* Wake poll tasks so they resume full polling + WS reconnect */
                    for (int i = 0; i < instance_count; i++)
                        if (poll_task_handles[i]) xTaskNotifyGive(poll_task_handles[i]);
                    ESP_LOGI(TAG, "Screen wake: %s", wake_reason);
                }

                if (should_sleep) {
                    /* Show "Sleeping..." message briefly before turning off */
                    lv_obj_t *sleep_overlay = NULL;
                    lvgl_port_lock(0);
                    {
                        lv_obj_t *scr = lv_scr_act();
                        /* Black overlay covers entire screen */
                        lv_obj_t *overlay = lv_obj_create(scr);
                        sleep_overlay = overlay;
                        lv_obj_remove_style_all(overlay);
                        lv_obj_set_size(overlay, 720, 720);
                        lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
                        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
                        lv_obj_center(overlay);
                        /* "Sleeping..." label — same size as "No Connections",
                         * same color as "Waiting for NINA connections" */
                        lv_obj_t *lbl = lv_label_create(overlay);
                        lv_label_set_text(lbl, "Sleeping...");
                        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32, 0);
                        const theme_t *th = nina_dashboard_get_current_theme();
                        if (th) {
                            int gb = sl_cfg->color_brightness;
                            lv_obj_set_style_text_color(lbl,
                                lv_color_hex(app_config_apply_brightness(th->label_color, gb)), 0);
                        } else {
                            lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
                        }
                        lv_obj_center(lbl);
                    }
                    lvgl_port_unlock();

                    /* Hold the message on screen for 2.5 seconds */
                    vTaskDelay(pdMS_TO_TICKS(2500));

                    /* Clean up the overlay before sleeping */
                    lvgl_port_lock(0);
                    {
                        if (sleep_overlay) {
                            lv_obj_delete(sleep_overlay);
                            sleep_overlay = NULL;
                        }
                    }
                    lvgl_port_unlock();

                    /* Turn off backlight completely via direct LEDC.
                     * BSP brightness_set(0) only dims to 47% due to offset mapping.
                     * With output_invert=1, duty=0 → GPIO HIGH → backlight off
                     * (backlight is active-low on this board). */
                    ledc_set_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH, 0);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH);
                    screen_asleep = true;
                    /* Disconnect WebSockets to save network resources while sleeping */
                    for (int i = 0; i < instance_count; i++)
                        nina_websocket_stop(i);
                    /* Stop LVGL processing during screen sleep */
                    lvgl_port_lock(0);
                    lvgl_port_stop();
                    lvgl_port_unlock();
                    /* Deeper WiFi power save during screen sleep */
                    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
                    ESP_LOGI(TAG, "Screen sleep: %s for %ds",
                             sleep_reason, sl_cfg->screen_sleep_timeout_s);
                }
            } else if (screen_asleep) {
                /* Feature disabled while asleep — wake up */
                /* Restore WiFi power save mode */
                esp_wifi_set_ps(sl_cfg->wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
                wifi_apply_tx_power(sl_cfg->wifi_max_tx_dbm);
                /* Resume LVGL processing */
                lvgl_port_lock(0);
                lvgl_port_resume();
                lv_obj_invalidate(lv_scr_act());
                lvgl_port_unlock();
                bsp_display_brightness_set(sl_cfg->brightness);
                screen_asleep = false;
                all_disconnected_since_ms = 0;
                spotify_idle_since_ms = 0;
                for (int i = 0; i < instance_count; i++)
                    if (poll_task_handles[i]) xTaskNotifyGive(poll_task_handles[i]);
                ESP_LOGI(TAG, "Screen wake: sleep feature disabled");
            }
        }

        /* ── Auto deep sleep when idle (if enabled) ── */
        if (app_config_get()->deep_sleep_on_idle && app_config_get()->deep_sleep_enabled) {
            int64_t idle_since = all_disconnected_since_ms > 0 ? all_disconnected_since_ms
                               : spotify_idle_since_ms > 0     ? spotify_idle_since_ms : 0;
            if (screen_asleep && idle_since > 0) {
                uint32_t idle_duration_ms = (uint32_t)(esp_timer_get_time() / 1000) - (uint32_t)idle_since;
                uint32_t idle_threshold_ms = (uint32_t)app_config_get()->screen_sleep_timeout_s * 2 * 1000; /* 2x screen sleep timeout */
                if (idle_duration_ms > idle_threshold_ms) {
                    ESP_LOGI(TAG, "Auto deep sleep after extended idle (%lu ms)", (unsigned long)idle_duration_ms);
                    power_mgmt_enter_deep_sleep(
                        app_config_get()->deep_sleep_wake_timer_s
                    );
                    /* Does not return */
                }
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
                perf_monitor_report();  // report() also runs the DMA-heap watchdog
            }
        } else {
            // Perf profiling disabled, but the low-DMA-heap watchdog is a safety
            // diagnostic that must fire regardless of debug_mode/perf state. Run
            // it on the same ~report cadence. Allocates nothing from any heap.
            static int64_t s_last_wd_us = 0;
            uint32_t wd_interval_s = g_perf.report_interval_s ? g_perf.report_interval_s : 30;
            int64_t now_wd = esp_timer_get_time();
            if (s_last_wd_us == 0 ||
                (now_wd - s_last_wd_us) >= (int64_t)wd_interval_s * 1000000) {
                s_last_wd_us = now_wd;
                perf_monitor_dma_heap_watchdog();
            }
        }

        // UI coordinator loop delay — no HTTP blocking, so this always fires on time.
        // Use task notification to allow WS events or screen wake to interrupt sleep.
        {
            uint32_t cycle_ms;
            if (screen_asleep) {
                /* No UI to render — match the idle poll interval */
                cycle_ms = (uint32_t)app_config_get()->idle_poll_interval_s * 1000;
                if (cycle_ms < 5000) cycle_ms = 5000;
            } else {
                cycle_ms = (uint32_t)app_config_get()->update_rate_s * 1000;
                if (cycle_ms < 1000) cycle_ms = 1000;
            }
            net_sched_note(pcTaskGetName(NULL), (uint32_t)(esp_timer_get_time() / 1000) + cycle_ms);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cycle_ms));
        }
    }
}
