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
#include "goes_client.h"
#include "moon_render.h"
#include "moon_sphere.h"   /* tgx sphere renderer for the Moon page */
#include "moon_interaction.h"  /* drag-to-rotate touch state */
#include "moon_ephemeris.h"
#include <math.h>           /* acos() for seamless anim start cycle */
#include "spotify_auth.h"
#include "spotify_client.h"
#include "app_config.h"
#include "mqtt_ha.h"
#include "ui/nina_dashboard.h"
#include "ui/nina_dashboard_internal.h"
#include "ui/nina_summary.h"
#include "ui/nina_sysinfo.h"
#include "ui/nina_allsky.h"
#include "ui/nina_spotify.h"
#include "ui/nina_graph_overlay.h"
#include "ui/nina_info_overlay.h"
#include "ui/nina_safety.h"
#include "ui/nina_alerts.h"
#include "ui/nina_session_stats.h"
#include "ui/nina_ota_prompt.h"
#include "ui/nina_nav_arbiter.h"
#include "ui/nina_image_display.h"
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
#include "esp_cache.h"        /* esp_cache_msync — flush PPA-source guard rows once */
#include "esp_system.h"
#include <time.h>
#include <math.h>          /* expf — framerate-independent settle ease */
#include "perf_monitor.h"
#include "power_mgmt.h"
#include "crash_log.h"
#include "demo_data.h"
#include "weather_client.h"
#include "driver/jpeg_decode.h"
#include "freertos/queue.h"
#include "ui/nina_thumbnail.h"

static const char *TAG = "tasks";

/* ── Async fetch queues (Core 0 worker ↔ Core 1 UI coordinator) ── */
static QueueHandle_t s_fetch_queue = NULL;        /* fetch_request_t */
static QueueHandle_t s_fetch_result_queue = NULL;  /* fetch_result_t */
#define FETCH_QUEUE_LEN      4
#define FETCH_RESULT_QUEUE_LEN 4

#define BOOT_BUTTON_GPIO    GPIO_NUM_35
#define DEBOUNCE_MS         200
#define HEARTBEAT_INTERVAL_MS 10000
/* Graph refresh interval read from config at runtime (graph_update_interval_s) */

/* Signals the data task that a page switch occurred */
_Atomic bool page_changed = false;
_Atomic bool ota_in_progress = false;

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
_Atomic bool clock_page_active   = false;
_Atomic bool nina_pages_active   = false;

/* AllSky polling state */
static allsky_data_t allsky_data;
static TaskHandle_t allsky_task_handle = NULL;

/* GOES / Image Display polling state.
 * Non-static: tasks.h externs these and web handlers use goes_task_handle /
 * goes_ensure_task_running to spawn/wake the task on runtime enable. */
TaskHandle_t goes_task_handle = NULL;
_Atomic bool image_display_page_active = false;
goes_data_t goes_data;
static demo_task_params_t demo_params;

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

        if (long_pressed && app_config_get()->deep_sleep_enabled) {
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
                || nina_info_overlay_visible()) {
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
                if (candidate == PAGE_IDX_IMAGE_DISPLAY && !nina_dashboard_is_image_display_page()
                    && !app_config_get()->image_display_enabled) continue;
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

        // Skip disabled or unconfigured instances — release network resources
        if (strlen(url) == 0 || !app_config_is_instance_enabled(idx)) {
            if (ctx->client->connected || ctx->client->websocket_connected) {
                nina_websocket_stop(idx);
                nina_poll_state_init(ctx->poll_state);
                ctx->filters_synced = false;
                ESP_LOGI(TAG, "Poll[%d]: instance disabled, resources released", idx + 1);
            }
            ctx->client->connected = false;
            nina_connection_report_poll(idx, false);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
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
            if (now_ms - ctx->last_heartbeat_ms >= (int64_t)app_config_get()->idle_poll_interval_s * 1000) {
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

void allsky_poll_task(void *arg) {
    ESP_LOGI(TAG, "AllSky poll task started");

    // Wait for WiFi before attempting any HTTP requests
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    while (1) {
        /* Suspend during OTA or when AllSky page is not visible */
        while (ota_in_progress || !allsky_page_active) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        }

        /* Read fields directly from config pointer — avoids copying the full
         * ~6.7 KB app_config_t onto this task's small stack. */
        const app_config_t *cfg = app_config_get();

        /* Only poll when hostname is configured */
        if (cfg->allsky_hostname[0] != '\0') {
            allsky_client_poll(cfg->allsky_hostname, cfg->allsky_field_config, &allsky_data);
        }

        /* Sleep for the configured interval (clamped 1-300s) */
        uint32_t interval_ms = (uint32_t)cfg->allsky_update_interval_s * 1000;
        if (interval_ms < 1000) interval_ms = 1000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(interval_ms));
    }
}

// =============================================================================
// GOES Image Display Poll Task — independent poller pinned to Core 0
// =============================================================================

/* Last-computed moon state, written only by goes_poll_task (single writer) and
 * read by moon_caption() from the UI task. The fields are slow-changing and the
 * read is benign (pointer + float), so a lock is intentionally omitted. */
static moon_state_t s_moon_state;

/* One-shot tap-animation request. Set by the moon-page tap handler (UI/Core 1),
 * consumed once by goes_poll_task via atomic_exchange. Declared extern in tasks.h. */
_Atomic bool moon_anim_request = false;

/* Set by the Image Display config POST handler on a user source/band change,
 * consumed once by goes_poll_task to gate the wait overlay. Declared extern in
 * tasks.h. */
_Atomic bool image_display_manual_fetch = false;

/* ── Moon drag-to-rotate scratch + PPA output buffers ────────────────────────
 * Persistent PSRAM buffers reused across every drag frame so the realtime loop
 * does ZERO per-frame heap alloc. Lazily allocated on the first drag of a moon-
 * page visit and freed on page leave via moon_drag_buffers_free(). Owned solely
 * by goes_poll_task.
 *
 * Touch-frame pipeline (per frame):
 *   1. moon_sphere_render_into() the sphere at MOON_DRAG_SZ_TOUCH (240) into the
 *      small color/z scratch (s_drag_color / s_drag_zbuf), CPU-written;
 *   2. ppa_scale_rgb565_into_noclear() hardware-upscales 240->720 (an EXACT 3.0x
 *      ratio: 720/240=3, representable on PPA's 1/16 scale grid so the whole 720
 *      output is filled with no edge-streak remainder, and no per-frame memset)
 *      into the ping-pong output buffer s_ppa_out[ping];
 *   3. nina_image_display_show_borrowed(s_ppa_out[ping], 720, 720) points the
 *      LVGL descriptor straight at it (no copy) at scale 1.0; then flip ping.
 * The PPA driver handles cache coherency for the BLOCKING transfer in BOTH
 * directions, so the output is coherent for LVGL read when the call returns — no
 * manual esp_cache_msync. The output is DOUBLE-BUFFERED (s_ppa_out[0]/[1]); PPA
 * writes the buffer NOT currently on screen so it never tears with the LVGL flush.
 *
 * 240->720 is the SINGLE touch render size for the whole interaction (active drag
 * AND the post-release settle ease). 240 was chosen because 720/240 = 3.0x is an
 * exact integer ratio on PPA (11520 % 240 == 0); 300 (the prior settle size) maps
 * to 2.40x which PPA truncates to 2.375x, leaving a ~8px unfilled streak.
 *
 * GUARD ROWS (required): SRM bilinear UPSCALE over-reads the source bottom edge.
 * At 3.0x the last output row (719) maps to source y≈239.67, so the sampler reads
 * source row 240 — one past the 240 valid rows. The driver does NOT clamp or
 * bounds-check the INPUT sampling, so without padding that read lands in adjacent
 * heap and renders as a colored garbage band along the bottom of the disc. This is
 * INDEPENDENT of the exact-ratio fix: 3.0x kills the fill STREAK (Gotcha 4), the
 * guard rows kill this OVER-READ. Allocating MOON_DRAG_GUARD_ROWS extra zeroed rows
 * below the valid region makes that boundary sample read our own black memory. The
 * rows are zeroed + flushed to PSRAM (C2M) ONCE at alloc; the per-frame render
 * writes only rows 0..239, so they stay zeroed (no per-frame memset/msync). PPA
 * in.pic_h/block_h stay 240 (the buffer is just taller). The right edge needs no
 * guard (col 240 wraps to col 0 of the next row, in-bounds; the last row's wrap
 * lands in the guard). This matches existing project practice — the GOES/decode PPA
 * paths in this file already memset their PPA source ("Zero buffer so PPA edge
 * interpolation reads black, not heap garbage"). See reference_lvgl_ppa_scale_limitation.md
 * (Gotcha 3/4): a clean PPA upscale needs BOTH the exact n/16 ratio AND guard rows.
 *
 *   - s_drag_color: (MOON_DRAG_SZ_TOUCH + GUARD_ROWS) * MOON_DRAG_SZ_TOUCH * 2  (CPU-written + guard)
 *   - s_drag_zbuf:  MOON_DRAG_SZ_TOUCH^2 * 2  depth buffer (not DMA-read by PPA; no guard needed)
 *   - s_ppa_out[2]: 720*720*2 each            PPA upscale output, ping-pong
 * If PPA scale fails or a buffer didn't allocate, the loop falls back to the
 * software-scale path (moon_sphere_render + nina_image_display_show_scaled) so it
 * degrades gracefully instead of crashing. */
#define MOON_DRAG_SZ_TOUCH  240          /* render size for the whole touch interaction (240->720 = exact 3.0x) */
#define MOON_DRAG_GUARD_ROWS 8           /* zeroed rows below s_drag_color: catch PPA upscale bottom over-read */
#define MOON_DRAG_SECTORS   96           /* tessellation: longitude bands (kills quad faceting) */
#define MOON_DRAG_STACKS    48           /* tessellation: latitude bands */
/* Active-drag easing is per-frame exponential (responsive to the finger). The
 * release settle uses a TIME-based ease (see below) so it reads smooth regardless
 * of the achieved framerate (~14-19fps) instead of finishing in 2-3 jumpy frames. */
#define MOON_DRAG_EASE_A    0.35f        /* per-frame easing alpha while dragging */
#define MOON_DRAG_SETTLE_MS 450          /* target duration of the release ease-back */
#define MOON_DRAG_FRAME_US  22000        /* target frame period (cap; render usually dominates) */
#define MOON_DRAG_REST_GRACE_MS 250      /* idle hold after settle before committing the crisp resting render; a new touch within this window re-enters the drag instead (avoids the eager reset / blocking-render stall on rapid re-swipe) */
static uint16_t *s_drag_color  = NULL;
static uint16_t *s_drag_zbuf   = NULL;
static uint16_t *s_ppa_out[2]  = { NULL, NULL };   /* 720x720 PPA-output ping-pong */
static int       s_ppa_ping    = 0;                /* index PPA writes next (flips each frame) */

/* Free the moon-drag scratch and PPA output buffers and NULL them. Called on
 * Image Display page leave. Safe to call when the buffers were never allocated
 * (NULL frees are no-ops). The page's own software-scale copy buffers are freed
 * separately in nina_image_display_cleanup(). */
static void moon_drag_buffers_free(void)
{
    if (s_drag_color) { heap_caps_free(s_drag_color); s_drag_color = NULL; }
    if (s_drag_zbuf)  { heap_caps_free(s_drag_zbuf);  s_drag_zbuf  = NULL; }
    if (s_ppa_out[0]) { heap_caps_free(s_ppa_out[0]); s_ppa_out[0] = NULL; }
    if (s_ppa_out[1]) { heap_caps_free(s_ppa_out[1]); s_ppa_out[1] = NULL; }
    s_ppa_ping = 0;
}

/* Provide the caption text for the Image Display page when the Moon source is
 * active. Declared (extern) in nina_image_display.c. */
void moon_caption(char *name_out, size_t name_sz, char *pct_out, size_t pct_sz)
{
    /* Lock-free read is acceptable here (slow-changing, single writer). */
    snprintf(name_out, name_sz, "%s", s_moon_state.phase_name ? s_moon_state.phase_name : "Moon");
    snprintf(pct_out, pct_sz, "%d%%", (int)(s_moon_state.illum * 100.0f + 0.5f));
}

/* Cached rise/set results — recomputed at most once per 30 s or on location
 * change.  Avoids running the 457-sample scan on every drag frame. */
static time_t s_rs_calc_at  = 0;
static time_t s_rs_rise     = 0;
static time_t s_rs_set      = 0;
static bool   s_rs_rise_v   = false;
static bool   s_rs_set_v    = false;
static double s_rs_lat      = 1e9;
static double s_rs_lon      = 1e9;

/* Return the signed difference in LOCAL calendar days between evt and now.
 * Normalises both instants to local noon before subtracting so DST transitions
 * within the interval do not produce off-by-one errors. */
static int local_day_delta(time_t evt, time_t ref)
{
    struct tm ta, tb;
    localtime_r(&evt, &ta);
    localtime_r(&ref, &tb);
    ta.tm_hour = 12; ta.tm_min = 0; ta.tm_sec = 0; ta.tm_isdst = -1;
    tb.tm_hour = 12; tb.tm_min = 0; tb.tm_sec = 0; tb.tm_isdst = -1;
    time_t na = mktime(&ta);
    time_t nb = mktime(&tb);
    return (int)lround((double)(na - nb) / 86400.0);
}

/* Format a single moon event (rise or set) into `out[sz]`.
 * Output examples (all fit within 20 bytes including NUL):
 *   "Rise 14:32"       24h, same day
 *   "Rise 2:34am +1"   12h, next day
 *   "Set 09:02 -1"     24h, previous day
 *   "Rise --:--"       invalid */
static void fmt_moon_event(char *out, size_t sz, const char *label,
                           time_t evt, bool valid, time_t now_t, bool use_24h)
{
    if (!valid) {
        snprintf(out, sz, "%s --:--", label);
        return;
    }

    struct tm lt;
    localtime_r(&evt, &lt);

    /* Time string: 5 chars for 24h "HH:MM", up to 7 for 12h "12:34pm". */
    char tbuf[10];
    if (use_24h) {
        strftime(tbuf, sizeof(tbuf), "%H:%M", &lt);
    } else {
        int hr = lt.tm_hour % 12;
        if (hr == 0) hr = 12;
        snprintf(tbuf, sizeof(tbuf), "%d:%02d%s",
                 hr, lt.tm_min, lt.tm_hour < 12 ? "am" : "pm");
    }

    /* Day-offset suffix: "" / " +N" / " -N". Sized for the full int range so
     * -Werror=format-truncation is satisfied (delta is realistically +/-1..2). */
    char sfx[16] = "";
    int  delta   = local_day_delta(evt, now_t);
    if (delta > 0) {
        snprintf(sfx, sizeof(sfx), " +%d", delta);
    } else if (delta < 0) {
        snprintf(sfx, sizeof(sfx), " %d", delta);   /* "-N" via %d with negative */
    }

    /* Longest possible result: "Rise 12:34pm +1\0" = 16 chars — fits in 20. */
    snprintf(out, sz, "%s %s%s", label, tbuf, sfx);
}

/* Fills the four moon-overlay corner strings.  All buffers must be non-NULL.
 *   age:  "Age 11.2d"
 *   next: "Full in 3d" or "New in 18d" (whichever lunar event is sooner);
 *         "Full <1d" / "New <1d" when less than one day away.
 *   rise: "Rise 14:32"  or  "Rise --:--" (24h when cfg->weather_time_format==1,
 *         12h e.g. "Rise 2:34am" otherwise).  A day-offset suffix " +N" / " -N"
 *         is appended when the event falls on a different local calendar day.
 *   set:  same format as rise.
 * Reads cached s_moon_state (lock-free, single writer) for age/next; calls
 * moon_rise_set() at most once per 30 s (throttled) for the rise/set times. */
void moon_overlay_info(char *age,  size_t age_sz,
                       char *next, size_t next_sz,
                       char *rise, size_t rise_sz,
                       char *set,  size_t set_sz)
{
    const double SYNODIC = 29.530588853;

    /* --- Age --- */
    double cycle = (double)s_moon_state.cycle;
    double age_days = cycle * SYNODIC;
    snprintf(age, age_sz, "Age %.1fd", age_days);

    /* --- Next phase (Full or New, whichever is sooner) --- */
    double days_to_full = fmod(0.5 - cycle + 1.0, 1.0) * SYNODIC;
    double days_to_new  = fmod(1.0 - cycle,        1.0) * SYNODIC;
    /* fmod(1.0 - 0.0, 1.0) == 0.0, which means "already new"; push to a full
     * cycle so we report "New in 29.5d" rather than "New in 0d". */
    if (days_to_new < 0.01) days_to_new = SYNODIC;

    if (days_to_full <= days_to_new) {
        if (days_to_full < 1.0)
            snprintf(next, next_sz, "Full <1d");
        else
            snprintf(next, next_sz, "Full in %.0fd", days_to_full);
    } else {
        if (days_to_new < 1.0)
            snprintf(next, next_sz, "New <1d");
        else
            snprintf(next, next_sz, "New in %.0fd", days_to_new);
    }

    /* --- Rise / Set (throttled) --- */
    const app_config_t *cfg = app_config_get();
    double lat = cfg->moon_lat, lon = cfg->moon_lon;
    if (lat == 0.0 && lon == 0.0) { lat = cfg->weather_lat; lon = cfg->weather_lon; }

    time_t now;
    time(&now);

    /* Recompute only when the cache is stale (>= 30 s old) or location changed. */
    if (s_rs_calc_at == 0 || (now - s_rs_calc_at) >= 30 ||
        lat != s_rs_lat  || lon != s_rs_lon) {
        moon_rise_set(now, lat, lon, &s_rs_rise, &s_rs_rise_v, &s_rs_set, &s_rs_set_v);
        s_rs_calc_at = now;
        s_rs_lat     = lat;
        s_rs_lon     = lon;
    }

    bool use_24h = (cfg->weather_time_format == 1);

    fmt_moon_event(rise, rise_sz, "Rise", s_rs_rise, s_rs_rise_v, now, use_24h);
    fmt_moon_event(set,  set_sz,  "Set",  s_rs_set,  s_rs_set_v,  now, use_24h);
}

void goes_poll_task(void *arg)
{
    ESP_LOGI(TAG, "GOES poll task started");

    while (1) {
        /* Suspend during OTA or when the Image Display page is not visible */
        while (ota_in_progress || !image_display_page_active) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        }

        /* Read fields directly from config pointer — avoids copying the full
         * app_config_t onto this task's small stack. */
        const app_config_t *cfg = app_config_get();

        /* Moon source: render locally on-device, no network. The result is
         * pushed into the same goes_data RGB565 sink the crossfade page reads. */
        if (cfg->image_display_source == 1) {
            time_t now; time(&now);
            /* The moon phase/orientation needs a correct wall clock. Before SNTP
             * sets the time (near-epoch at boot), rendering would show the wrong
             * phase, e.g. a half-lit disc, until the next recompute. Skip the
             * render until the clock is valid and retry quickly so the correct
             * moon appears as soon as time syncs; then settle to ~60s. */
            bool time_valid = (now > (time_t)1577836800);  /* >= 2020-01-01 UTC */
            /* tgx path prepares its own texture lazily in moon_sphere_render();
             * gate only on a valid clock (the flat PNG decode is not used). */
            bool moon_ready = moon_sphere_init();
            if (time_valid && moon_ready) {
                double lat = cfg->moon_lat, lon = cfg->moon_lon;
                if (lat == 0.0 && lon == 0.0) { lat = cfg->weather_lat; lon = cfg->weather_lon; }
                moon_state_t live;
                moon_compute(now, lat, lon, &live);

                /* Interactive drag-to-rotate: while a finger is down OR the disc is
                 * still easing back to the live sky orientation after release, render
                 * small frames in realtime and PPA hardware-upscale them to fill the
                 * panel. The touch handlers in nina_image_display.c notify this task on
                 * press/release, so the outer ulTaskNotifyTake wakes us; this inner
                 * loop owns the realtime frames and SELF-SPINS (it does not wait on
                 * notifications) until moon_drag_settled() — i.e. the finger is up AND
                 * the eased orientation is home — then falls through to the crisp
                 * full-res resting render (crossfaded in for a smooth handoff).
                 *
                 * Architecture (per-frame, zero heap alloc):
                 *   1. ease current orientation toward the finger target (per-frame
                 *      exp while dragging; TIME-based ease-out during the settle so it
                 *      reads smooth at any framerate instead of jumping home);
                 *   2. render the sphere at the single touch size (240px, 96x48
                 *      tessellation) into PERSISTENT scratch (no alloc);
                 *   3. PPA hardware-upscale 240->720 (exact 3.0x) into a ping-pong
                 *      output buffer, then show_borrowed it at scale 1.0 (no copy),
                 *      taking the display lock only around the LVGL swap.
                 * The render + PPA run OUTSIDE the display lock so the UI task is
                 * blocked for the minimum time. To dissolve the lighting/phase pop on
                 * release, SETTLE frames render in TRUE_PHASE lighting (the resting
                 * mode) so as the orientation eases home they converge on the resting
                 * appearance; the final crisp native-720 frame is then crossfaded in
                 * (set below). If PPA or the buffers are unavailable, the loop falls
                 * back to a software-scaled render so it degrades instead of crashing. */
                /* Whether the drag loop body actually ran. Sampling moon_drag_settled()
                 * up front is unreliable: a press+release can complete before this task
                 * wakes, leaving settled() already true so the loop never executes. We
                 * set this inside the loop instead, so the post-settle resting commit
                 * fires whenever a drag frame was actually put on screen. Declared
                 * OUTSIDE the for(;;) below so it intentionally persists across outer
                 * iterations: once any drag frame has run it stays true, so the grace
                 * window fires after every drag including re-grabs. */
                bool ran_drag = false;
                /* Set when the inner render loop breaks because a free-spin hold is
                 * active (finger up, disc held at its spun orientation). Drives the
                 * dedicated hold-wait block below instead of the normal resting commit.
                 * Re-evaluated each outer iteration. */
                bool freespin_hold = false;
                if (!moon_drag_settled()) {
                    /* Lazy one-time allocation of the drag scratch + PPA output
                     * (PSRAM, 128-byte aligned for the cache line). The color buffer
                     * is 240+GUARD_ROWS tall (the guard rows absorb the SRM bottom-edge
                     * upscale over-read; see the buffer-block comment above); the zbuf
                     * is plain 240px (not DMA-read by PPA). The two 720x720 PPA-output
                     * buffers ping-pong. All freed on page leave via
                     * moon_drag_buffers_free(). This runs ONCE on entry (before the
                     * for(;;) below); the inner "if any buffer NULL" guard makes it a
                     * no-op when the buffers already exist, so a re-grab that loops back
                     * into the drag while-loop never re-allocates. */
                    if (!s_drag_color || !s_drag_zbuf || !s_ppa_out[0] || !s_ppa_out[1]) {
                        size_t row_bytes  = (size_t)MOON_DRAG_SZ_TOUCH * 2;
                        size_t color_sz   = ((size_t)MOON_DRAG_SZ_TOUCH + MOON_DRAG_GUARD_ROWS) * row_bytes;
                        color_sz = (color_sz + 127) & ~(size_t)127;   /* 248*480=119040, already 128-aligned */
                        size_t zbuf_sz    = (size_t)MOON_DRAG_SZ_TOUCH * row_bytes;
                        zbuf_sz  = (zbuf_sz + 127) & ~(size_t)127;
                        size_t out_sz     = (size_t)SCREEN_SIZE * SCREEN_SIZE * 2;   /* 720*720*2 = 128-aligned */
                        moon_drag_buffers_free();   /* drop any partial alloc first */
                        s_drag_color = (uint16_t *)heap_caps_aligned_alloc(128, color_sz, MALLOC_CAP_SPIRAM);
                        s_drag_zbuf  = (uint16_t *)heap_caps_aligned_alloc(128, zbuf_sz, MALLOC_CAP_SPIRAM);
                        s_ppa_out[0] = (uint16_t *)heap_caps_aligned_alloc(128, out_sz, MALLOC_CAP_SPIRAM);
                        s_ppa_out[1] = (uint16_t *)heap_caps_aligned_alloc(128, out_sz, MALLOC_CAP_SPIRAM);
                        if (!s_drag_color || !s_drag_zbuf || !s_ppa_out[0] || !s_ppa_out[1]) {
                            ESP_LOGE(TAG, "moon drag buffer alloc failed; falling back to software scale");
                            moon_drag_buffers_free();   /* drop the partial set; loop uses the SW fallback */
                        } else {
                            /* Zero the guard rows below the 240 valid rows and flush
                             * them to PSRAM ONCE (C2M) so the SRM bottom-edge over-read
                             * samples our own black memory, not adjacent heap. The
                             * per-frame render writes only rows 0..239, so the guard
                             * rows stay zeroed and this never repeats. Offset/length
                             * are both 128-aligned (115200 / 3840). */
                            uint8_t *guard = (uint8_t *)s_drag_color +
                                             (size_t)MOON_DRAG_SZ_TOUCH * row_bytes;
                            size_t   guard_bytes = (size_t)MOON_DRAG_GUARD_ROWS * row_bytes;
                            memset(guard, 0, guard_bytes);
                            esp_cache_msync(guard, guard_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        }
                    }
                }

                /* Outer loop wraps the drag/settle while-loop and a post-settle GRACE
                 * window. The grace hold (MOON_DRAG_REST_GRACE_MS) makes the reset less
                 * eager: after the disc eases home we wait briefly, and if the user
                 * starts a NEW touch within that window we `continue` to re-enter the
                 * drag while-loop and track the finger immediately, SKIPPING the
                 * expensive crisp resting render + crossfade entirely. Only when the
                 * full grace window elapses with no re-touch do we break out to commit
                 * the resting frame (genuine rest -> smooth eased settle + crossfade is
                 * preserved). A new touch makes moon_drag_settled() false again, so
                 * re-entering the while-loop resumes tracking with no special-casing. */
                for (;;) {
                /* Seed the per-frame dt clock for the time-based settle ease. Keep
                 * this immediately before the loop (after the scratch alloc above) so
                 * the first settle frame's dt is a true frame interval and never folds
                 * in one-time alloc latency. Re-seeded each outer iteration so a
                 * re-touch's first settle frame gets a true dt. */
                int64_t prev_frame_us = esp_timer_get_time();
                freespin_hold = false;   /* re-evaluated for this outer iteration */
                while (!moon_drag_settled()) {
                    if (!image_display_page_active || cfg->image_display_source != 1) break;
                    ran_drag = true;   /* a drag frame is about to be shown */
                    int64_t frame_t0 = esp_timer_get_time();

                    /* 1. Ease current orientation toward the finger target. While the
                     * finger is down, use the responsive per-frame alpha. After release
                     * (settle), derive a framerate-independent ease-out alpha from the
                     * frame dt so the snap-back always takes ~MOON_DRAG_SETTLE_MS and
                     * reads smooth even at ~14fps (a fixed per-frame alpha would finish
                     * in 2-3 frames and look like a jump). */
                    bool active = moon_drag_active();
                    float alpha;
                    if (active) {
                        alpha = MOON_DRAG_EASE_A;
                    } else {
                        /* tau so ~95% of the distance is covered in SETTLE_MS:
                         * alpha = 1 - exp(-dt/tau), tau = SETTLE_MS/3. */
                        float dt_ms = (float)(frame_t0 - prev_frame_us) / 1000.0f;
                        float tau   = (float)MOON_DRAG_SETTLE_MS / 3.0f;
                        alpha = 1.0f - expf(-dt_ms / tau);
                        if (alpha < 0.02f) alpha = 0.02f;   /* never fully stall */
                        if (alpha > 1.0f)  alpha = 1.0f;
                    }
                    prev_frame_us = frame_t0;
                    moon_drag_advance(alpha);
                    float yaw, pitch; moon_drag_get(&yaw, &pitch);

                    /* Free-spin hold: after a rotate-release in free-spin mode the
                     * disc holds its spun orientation, so moon_drag_settled() never
                     * becomes true and this loop would busy-render an identical frame
                     * for the whole hold window. Wait until the eased CURRENT has
                     * actually converged onto the spun TARGET (moon_drag_advance, run
                     * above each frame, is still easing it there) before breaking;
                     * otherwise we would lock in a half-eased orientation as the crisp
                     * held frame and the disc would visibly snap after lift. Once
                     * converged, break to the hold-wait below (which sleeps instead of
                     * re-rendering). */
                    if (!active && moon_drag_freespin_converged()) {
                        freespin_hold = true;
                        break;
                    }

                    /* 2. Lighting for this frame. Active frames use the explore/config
                     * lighting for free exploration. On settle, true-phase/explore force
                     * TRUE_PHASE so the terminator shadow dissolves back in as the disc
                     * returns home (matching the resting render). SURFACE_LOCKED keeps its
                     * mode through the ease-back: its sun = R_drag(yaw,pitch)*R_sky*sun_body,
                     * which at yaw=pitch=0 equals true-phase exactly, so the terminator
                     * stays pinned to the surface and converges with no pop (forcing
                     * TRUE_PHASE here would flash full-bright/dark on the first large-yaw
                     * settle frame). The render SIZE is the single touch size (240) for the
                     * whole interaction — active and settle — because 240->720 is an exact
                     * 3.0x PPA ratio. */
                    moon_light_mode_t cfg_light = (moon_light_mode_t)cfg->moon_drag_light_mode;
                    moon_light_mode_t light;
                    if (active) {
                        light = cfg_light;                 /* finger down: use configured mode */
                    } else if (cfg_light == MOON_LIGHT_SURFACE_LOCKED) {
                        light = MOON_LIGHT_SURFACE_LOCKED; /* settle: keep terminator pinned; converges to true phase at yaw=pitch=0 */
                    } else {
                        light = MOON_LIGHT_TRUE_PHASE;     /* settle for true-phase/explore: dissolve terminator back in */
                    }

                    bool shown = false;
                    if (s_drag_color && s_drag_zbuf && s_ppa_out[0] && s_ppa_out[1]) {
                        /* Render 240px into persistent scratch — NO per-frame alloc. */
                        uint16_t *fimg = moon_sphere_render_into(
                            MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH, &live,
                            MOON_DRAG_SECTORS, MOON_DRAG_STACKS, cfg->moon_bg_style,
                            yaw, pitch, light, s_drag_color, s_drag_zbuf);
                        if (fimg) {
                            /* PPA hardware-upscale 240->720 (exact 3.0x) into the
                             * ping-pong output buffer NOT currently on screen. No
                             * memset (every output pixel is written) and the blocking
                             * PPA handles cache coherency both ways, so the buffer is
                             * coherent for LVGL read on return. */
                            int ping = s_ppa_ping;
                            uint8_t *out = ppa_scale_rgb565_into_noclear(
                                (const uint8_t *)fimg, MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH,
                                MOON_DRAG_SZ_TOUCH /* stride in pixels */,
                                SCREEN_SIZE, SCREEN_SIZE,
                                (uint8_t *)s_ppa_out[ping],
                                (size_t)SCREEN_SIZE * SCREEN_SIZE * 2, NULL);
                            if (out) {
                                /* Point the LVGL descriptor straight at the 720 buffer
                                 * (no copy) at scale 1.0. Lock held only around the
                                 * swap. The ping-pong guarantees we never overwrite the
                                 * buffer LVGL is flushing. */
                                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                    nina_image_display_show_borrowed(s_ppa_out[ping], SCREEN_SIZE, SCREEN_SIZE);
                                    bsp_display_unlock();
                                }
                                s_ppa_ping ^= 1;   /* flip: PPA writes the other buffer next frame */
                                shown = true;
                            }
                            /* PPA failed: fall through to the software-scale fallback
                             * below using the SAME already-rendered 240px scratch. */
                            if (!shown && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_image_display_show_scaled(fimg, MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH);
                                bsp_display_unlock();
                                shown = true;
                            }
                        }
                    }

                    if (!shown) {
                        /* Buffers unavailable (alloc failed): render to a fresh PSRAM
                         * buffer and let nina_image_display_update() software-scale it.
                         * Degrades gracefully. NOTE: unlike the PPA happy path, this
                         * exceptional fallback does ~2 allocs/frame (the fresh
                         * moon_sphere_render_ex color/z here, plus update()'s owned-copy
                         * alloc); the "zero per-frame heap alloc" guarantee applies only
                         * to the PPA path, not this degraded fallback. */
                        uint16_t *fimg = moon_sphere_render_ex(MOON_DRAG_SZ_TOUCH, MOON_DRAG_SZ_TOUCH, &live,
                                                               MOON_DRAG_SECTORS, MOON_DRAG_STACKS,
                                                               cfg->moon_bg_style, yaw, pitch, light);
                        if (fimg) {
                            if (goes_data_lock(&goes_data, 1000)) {
                                if (goes_data.image_buf) heap_caps_free(goes_data.image_buf);
                                goes_data.image_buf = (uint8_t *)fimg;
                                goes_data.image_w = MOON_DRAG_SZ_TOUCH;
                                goes_data.image_h = MOON_DRAG_SZ_TOUCH;
                                goes_data.vflip = false;
                                goes_data.label[0] = '\0';
                                goes_data.connected = true;
                                goes_data.last_poll_ms = esp_timer_get_time() / 1000;
                                goes_data_unlock(&goes_data);
                                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                    nina_image_display_update(&goes_data);
                                    bsp_display_unlock();
                                }
                            } else {
                                heap_caps_free(fimg);
                            }
                        }
                    }

                    /* Delay only the remainder to hit the target frame period. If the
                     * frame overran (the usual case; the PPA upscale is cheap but the
                     * 240px tgx render dominates), just yield so we don't starve
                     * idle/UI but also don't add latency. */
                    int64_t spent = esp_timer_get_time() - frame_t0;
                    int64_t remain_us = (int64_t)MOON_DRAG_FRAME_US - spent;
                    if (remain_us > 1000) {
                        vTaskDelay(pdMS_TO_TICKS((uint32_t)(remain_us / 1000)));
                    } else {
                        vTaskDelay(1);
                    }
                }

                /* FREE-SPIN HOLD. In free-spin mode (moon_spin_mode == 1) a
                 * rotate-release leaves the disc at its spun orientation instead of
                 * snapping home. Render that held orientation ONCE as a crisp native-720
                 * frame (the inner loop only ever showed the 240px PPA-upscaled version),
                 * then sleep-poll — NOT busy-render — until either a new finger-down
                 * re-enters the drag loop (cancels the pending return) or the configured
                 * moon_spin_return_s elapses, at which point we trigger the eased return
                 * home and `continue` so the inner settle loop runs the snap-back +
                 * resting commit exactly as the rubber-band path does. */
                if (freespin_hold) {
                    /* One crisp held-orientation frame at native 720. moon_sphere_render_ex
                     * owns its own PSRAM buffer; update() takes ownership and crossfades. */
                    float hy, hp; moon_drag_get(&hy, &hp);
                    uint16_t *hold_img = moon_sphere_render_ex(SCREEN_SIZE, SCREEN_SIZE, &live,
                                                               96, 48, cfg->moon_bg_style,
                                                               hy, hp, (moon_light_mode_t)cfg->moon_drag_light_mode);
                    if (hold_img && image_display_page_active && cfg->image_display_source == 1 &&
                        !moon_drag_active()) {
                        if (goes_data_lock(&goes_data, 1000)) {
                            if (goes_data.image_buf) heap_caps_free(goes_data.image_buf);
                            goes_data.image_buf = (uint8_t *)hold_img;
                            goes_data.image_w = SCREEN_SIZE;
                            goes_data.image_h = SCREEN_SIZE;
                            goes_data.vflip = false;
                            goes_data.label[0] = '\0';
                            goes_data.connected = true;
                            goes_data.last_poll_ms = esp_timer_get_time() / 1000;
                            goes_data_unlock(&goes_data);
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_image_display_force_redraw();
                                nina_image_display_update(&goes_data);
                                bsp_display_unlock();
                            }
                        } else {
                            heap_caps_free(hold_img);
                        }
                    } else if (hold_img) {
                        heap_caps_free(hold_img);
                    }

                    /* Sleep-poll the hold window. The configured seconds are read each
                     * iteration so a live web-UI change takes effect within one poll step. */
                    for (;;) {
                        if (!image_display_page_active || cfg->image_display_source != 1) break;
                        if (moon_drag_active()) break;   /* re-touch: outer continue re-enters the drag loop */
                        /* Mode switched to rubber band mid-hold, or the hold was cleared:
                         * resolve by snapping home (acceptable per spec). */
                        if (!moon_drag_freespin_pending()) {
                            moon_drag_trigger_return();
                            break;
                        }
                        if (moon_drag_freespin_elapsed(cfg->moon_spin_return_s)) {
                            moon_drag_trigger_return();   /* target -> 0: ease home next iteration */
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                    continue;   /* re-enter outer loop: re-touch resumes drag, else snap-back eases home */
                }

                /* GRACE / idle hold before committing the resting render. Poll in
                 * small steps; if a new touch begins, restart the outer loop so the
                 * drag while-loop re-tracks the finger immediately (no rest render,
                 * no crossfade). Bail the whole interaction if the page/source changed.
                 * Only a fully-elapsed grace window (no re-touch) falls through to the
                 * resting commit below. */
                bool regrabbed = false;
                if (ran_drag) {
                    int waited_ms = 0;
                    while (waited_ms < MOON_DRAG_REST_GRACE_MS) {
                        if (!image_display_page_active || cfg->image_display_source != 1) break;
                        if (moon_drag_active()) { regrabbed = true; break; }
                        vTaskDelay(pdMS_TO_TICKS(20));
                        waited_ms += 20;
                    }
                }
                if (regrabbed) continue;   /* re-enter the drag while-loop */
                break;                     /* genuine rest: commit the resting frame */
                } /* end outer for(;;) */

                /* Drag settled: the last 240px settle frame (TRUE_PHASE, PPA-upscaled)
                 * is still on screen. The full-res resting render below commits an
                 * OWNED native-720 frame and crossfades it in over that settle frame
                 * for a smooth sharpen-up. The render scratch / PPA buffers are freed
                 * on page leave, not here, so the next drag reuses them. */

                /* One-shot tap animation: ~4s eased sweep through a full synodic
                 * cycle plus a full bright-limb spin, rendered at reduced size for
                 * smoothness. Consume the request atomically so a single tap fires
                 * once. Both phase and orientation are periodic over the sweep, so
                 * t=1 lands back on the live values with no visible jump. */
                if (atomic_exchange(&moon_anim_request, false)) {
                    /* tgx tap-animation is a later phase; consume the tap (above)
                     * and otherwise no-op, so the resting tgx frame renders below. */
                }

                /* Normal full-res render of the live current phase. Runs whether or
                 * not an animation played; the caption reads the resting state.
                 * Rendered at NATIVE 720 so it displays 1:1 (no software scale) and is
                 * the sharpest possible resting frame; the ~297ms cost is a one-shot at
                 * rest (not per-frame), so it is fine. update() copies it into an owned
                 * buffer and crossfades it in at scale 1.0. */
                s_moon_state = live;
                const int MOON_SZ = SCREEN_SIZE;

                /* ABORT-IF-TOUCH-RESUMED backstop. The grace window above makes this
                 * rare, but a touch can land right at the grace boundary, AFTER we
                 * broke out of the outer loop. A single moon_sphere_render(720) blocks
                 * ~297ms and cannot be interrupted mid-call, so the guard MUST be
                 * BEFORE it: if a finger is down now, skip the resting render + crossfade
                 * commit entirely and `continue` the task loop. moon_drag_active() makes
                 * moon_drag_settled() false, so re-entering the moon block immediately
                 * re-enters the drag loop and tracks the finger with no blocking stall
                 * and no dropped frame. */
                if (moon_drag_active()) continue;

                /* Render with tgx and log timing. When debug_mode is on, also
                 * sweep candidate sizes so the size/fps tradeoff is visible on
                 * serial for evaluation. */
                if (cfg->debug_mode && !moon_drag_active()) {
                    const int sizes[3] = {240, 300, 400};
                    for (int si = 0; si < 3; si++) {
                        int64_t te0 = esp_timer_get_time();
                        uint16_t *tmp = moon_sphere_render(sizes[si], sizes[si], &live, 96, 48, cfg->moon_bg_style);
                        int64_t te = esp_timer_get_time() - te0;
                        ESP_LOGI(TAG, "tgx moon %dx%d render %lld ms", sizes[si], sizes[si], te/1000);
                        if (tmp) heap_caps_free(tmp);
                    }
                }
                int64_t t0 = esp_timer_get_time();
                uint16_t *img = moon_sphere_render(MOON_SZ, MOON_SZ, &live, 96, 48, cfg->moon_bg_style);
                ESP_LOGI(TAG, "tgx moon %dx%d render %lld ms", MOON_SZ, MOON_SZ, (esp_timer_get_time()-t0)/1000);
                if (img) {
                    if (goes_data_lock(&goes_data, 1000)) {
                        if (goes_data.image_buf) heap_caps_free(goes_data.image_buf);
                        goes_data.image_buf = (uint8_t *)img;
                        goes_data.image_w = MOON_SZ;
                        goes_data.image_h = MOON_SZ;
                        goes_data.vflip = false;
                        goes_data.label[0] = '\0';
                        goes_data.connected = true;
                        goes_data.last_poll_ms = esp_timer_get_time() / 1000;
                        goes_data_unlock(&goes_data);
                        /* Push this OWNED native-720 resting frame to the panel
                         * immediately rather than waiting on the periodic UI cadence
                         * (data_update_task, up to update_rate_s away) to notice the new
                         * goes_data timestamp via its new-image gate. This makes
                         * config-driven re-renders (flip / background / orientation
                         * toggles from the web UI, which wake this task via
                         * xTaskNotifyGive) appear as soon as the render completes,
                         * matching the snappiness of the other live settings. It also
                         * still REPLACES the post-drag 240px settle frame, whose
                         * equal-millisecond timestamp the cadence gate could otherwise
                         * tie on and skip. force_redraw bypasses that gate; the swap is
                         * instant (matching every other moon frame) so there is no
                         * midpoint brightness dip. */
                        if (image_display_page_active &&
                            cfg->image_display_source == 1) {
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_image_display_force_redraw();
                                nina_image_display_update(&goes_data);
                                bsp_display_unlock();
                            }
                        }
                    } else {
                        heap_caps_free(img);
                    }
                }
            }
            /* Recompute ~every 60s once time is valid so orientation tracks the
             * sky; poll every ~3s while waiting for the clock to sync. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(time_valid ? 60000 : 3000));
            continue;
        }

        /* GOES needs network — wait for WiFi before attempting any HTTP requests. */
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        /* Show the full-screen wait overlay while the Image Display page is
         * visible and either (a) the user changed the source/band (manual flag)
         * or (b) nothing is on screen yet — which is the case on page (re)entry
         * because the buffers are freed on leave, so has_image() is false on the
         * first refresh after entry. Periodic background refreshes keep the
         * current image on screen (has_image() true) and skip the overlay so it
         * never flickers over a good frame. A takeover on an unrelated page
         * would be jarring, so it stays gated on image_display_page_active. The
         * overlay is hidden again when the new image is committed in
         * nina_image_display_update(), or below on a fetch error. */
        bool manual_fetch = atomic_exchange(&image_display_manual_fetch, false);
        bool show_wait = false;
        if (image_display_page_active) {
            const char *band_name = (cfg->image_display_source == 2)
                                        ? solar_band_label(cfg->solar_band) : NULL;
            /* Only treat the overlay as shown if the lock was acquired and the
             * show actually ran — otherwise the error-hide below would be a
             * spurious no-op against an overlay that never appeared. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                if (manual_fetch || !nina_image_display_has_image()) {
                    nina_wait_overlay_show("Loading image...", band_name);
                    nina_wait_overlay_set_progress(-1);   /* indeterminate */
                    show_wait = true;
                }
                bsp_display_unlock();
            }
        }

        esp_err_t fetch_err = ESP_OK;
        if (cfg->image_display_source == 3) {                       /* Custom image URL */
            if (cfg->custom_image_url[0] == '\0') {
                /* No URL configured: skip the fetch and surface the reason so the
                 * page shows why nothing loads instead of a stuck overlay. */
                if (goes_data_lock(&goes_data, 200)) {
                    strlcpy(goes_data.error_msg, "No URL configured", sizeof(goes_data.error_msg));
                    goes_data_unlock(&goes_data);
                }
                fetch_err = ESP_ERR_INVALID_ARG;
            } else {
                /* Custom uses the same software JPEG decode path as GOES/Solar,
                 * which needs a vertical flip to display upright. */
                fetch_err = goes_client_poll_url(cfg->custom_image_url, &goes_data, true, "Custom");
            }
        } else if (cfg->image_display_source == 2) {                /* Solar (SDO/AIA) */
            const char *url = solar_band_url(cfg->solar_band);
            /* All solar bands need a vertical flip to display upright (see solar_band_vflip). */
            if (url && url[0]) {
                fetch_err = goes_client_poll_url(url, &goes_data, solar_band_vflip(cfg->solar_band), solar_band_label(cfg->solar_band));
            } else {
                fetch_err = ESP_ERR_INVALID_ARG;
            }
        } else if (cfg->goes_region[0] != '\0') {                   /* GOES */
            fetch_err = goes_client_poll(cfg->goes_region, &goes_data);
        } else {
            /* No region configured: nothing is fetched, so the new image never
             * arrives. Mark as failed so the error-hide below clears any
             * manual-fetch overlay instead of leaving it stuck. */
            fetch_err = ESP_FAIL;
        }

        /* On a failed manual fetch the new image never arrives, so
         * nina_image_display_update() will not hide the overlay — clear it here
         * so it never gets stuck. */
        if (show_wait && fetch_err != ESP_OK) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_wait_overlay_hide();
                bsp_display_unlock();
            }
            nina_toast_show(TOAST_WARNING, "Failed to load image");
        }

        /* Sleep for the configured interval. The satellite sources (GOES/Solar)
         * clamp to 5min-2h to respect the image cadence; the custom source uses
         * its own interval (10s-2h) since the user controls the endpoint. */
        uint32_t interval_ms;
        if (cfg->image_display_source == 3) {
            interval_ms = (uint32_t)cfg->custom_update_interval_s * 1000;
            if (interval_ms < 10000) interval_ms = 10000;
            if (interval_ms > 7200000) interval_ms = 7200000;
        } else {
            interval_ms = (uint32_t)cfg->goes_update_interval_s * 1000;
            if (interval_ms < 300000) interval_ms = 300000;
            if (interval_ms > 7200000) interval_ms = 7200000;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(interval_ms));
    }
}

// =============================================================================
// GOES task lifecycle
// =============================================================================

void goes_ensure_task_running(void)
{
    /* Guard check-and-assign against a double-spawn race: this may be called
     * concurrently from the boot task and an httpd task (runtime enable). */
    static portMUX_TYPE goes_spawn_mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&goes_spawn_mux);
    bool already = (goes_task_handle != NULL);
    portEXIT_CRITICAL(&goes_spawn_mux);
    if (already) return;

    /* 12288 words: TLS handshake + software JPEG decode headroom (matches/
     * exceeds the spotify TLS task). */
    StackType_t  *stack = heap_caps_malloc(12288 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb   = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stack && tcb) {
        portENTER_CRITICAL(&goes_spawn_mux);
        goes_task_handle = xTaskCreateStaticPinnedToCore(
            goes_poll_task, "goes", 12288, NULL, 3, stack, tcb, 0);
        portEXIT_CRITICAL(&goes_spawn_mux);
        ESP_LOGI(TAG, "GOES poll task spawned");
    } else {
        ESP_LOGE(TAG, "Failed to allocate GOES poll task stack");
        if (stack) heap_caps_free(stack);
        if (tcb) heap_caps_free(tcb);
    }
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

                        /* Hardware JPEG decode to RGB565.
                         * Buffer always rounded to 16px — the ESP32-P4 HW decoder
                         * outputs with 16-pixel MCU alignment regardless of subsampling.
                         * PPA uses actual pic_info dimensions to crop MCU padding. */
                        perf_timer_start(&g_perf.spotify_art_decode);
                        jpeg_decode_picture_info_t pic_info = {0};
                        esp_err_t info_err = jpeg_decoder_get_info(jpg_buf, jpg_size, &pic_info);
                        if (info_err == ESP_OK && pic_info.width > 0) {
                            uint32_t out_w = ((pic_info.width + 15) / 16) * 16;
                            uint32_t out_h = ((pic_info.height + 15) / 16) * 16;

                            jpeg_decode_memory_alloc_cfg_t mem_cfg = {
                                .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
                            };
                            size_t allocated = 0;
                            uint8_t *rgb_buf = (uint8_t *)jpeg_alloc_decoder_mem(
                                out_w * out_h * 2, &mem_cfg, &allocated);

                            if (rgb_buf) {
                                memset(rgb_buf, 0, allocated); /* Zero buffer so PPA edge interpolation reads black, not heap garbage */
                                jpeg_decoder_handle_t decoder = NULL;
                                jpeg_decode_engine_cfg_t engine_cfg = {
                                    .intr_priority = 0, .timeout_ms = 5000
                                };
                                esp_err_t dec_err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
                                if (dec_err == ESP_OK && decoder) {
                                    jpeg_decode_cfg_t dec_cfg = {
                                        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                                        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                                    };
                                    uint32_t out_size = 0;
                                    dec_err = jpeg_decoder_process(decoder, &dec_cfg,
                                        jpg_buf, jpg_size, rgb_buf, allocated, &out_size);
                                    jpeg_del_decoder_engine(decoder);

                                    if (dec_err == ESP_OK && out_size > 0) {
                                        /* Pre-scale to 720x720 using PPA hardware to
                                         * eliminate per-frame LVGL software scaling */
                                        uint8_t *final_buf = rgb_buf;
                                        uint32_t final_w = out_w, final_h = out_h;
                                        size_t final_size = out_size;

                                        if (pic_info.width != 720 || pic_info.height != 720) {
                                            size_t scaled_size = 0;
                                            uint8_t *scaled = ppa_scale_rgb565(
                                                rgb_buf, pic_info.width, pic_info.height,
                                                out_w, 720, 720, &scaled_size);
                                            if (scaled) {
                                                free(rgb_buf);
                                                final_buf = scaled;
                                                final_w = 720;
                                                final_h = 720;
                                                final_size = scaled_size;
                                            }
                                            /* If PPA fails, fall through with original —
                                             * LVGL will SW-scale as before */
                                        }

                                        perf_timer_stop(&g_perf.spotify_art_decode);

                                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                            nina_spotify_set_album_art(final_buf, final_w, final_h, final_size);
                                            bsp_display_unlock();
                                        }
                                        art_ok = true;
                                        /* Ownership transferred to UI — don't free final_buf */
                                    } else {
                                        perf_timer_stop(&g_perf.spotify_art_decode);
                                        ESP_LOGW(TAG, "HW JPEG decode failed, trying SW fallback");
                                        free(rgb_buf);
                                        rgb_buf = NULL;
                                        /* SW fallback (stb_image) — handles CMYK and other unsupported formats */
                                        goto sw_fallback;
                                    }
                                } else {
                                    perf_timer_stop(&g_perf.spotify_art_decode);
                                    ESP_LOGW(TAG, "HW decoder engine creation failed, trying SW");
                                    free(rgb_buf);
                                    goto sw_fallback;
                                }
                            } else {
                                perf_timer_stop(&g_perf.spotify_art_decode);
                                ESP_LOGW(TAG, "HW decoder mem alloc failed, trying SW");
                                goto sw_fallback;
                            }
                        } else {
                            perf_timer_stop(&g_perf.spotify_art_decode);
                        sw_fallback: ;
                            uint8_t *sw_buf = NULL;
                            uint32_t sw_w = 0, sw_h = 0;
                            size_t sw_size = 0;
                            perf_timer_start(&g_perf.spotify_art_decode);
                            bool sw_ok = jpeg_sw_decode_rgb565(jpg_buf, jpg_size,
                                &sw_buf, &sw_w, &sw_h, &sw_size);
                            perf_timer_stop(&g_perf.spotify_art_decode);
                            if (sw_ok && sw_buf) {
                                uint8_t *final_buf = sw_buf;
                                uint32_t final_w = sw_w, final_h = sw_h;
                                size_t final_size = sw_size;
                                if (sw_w != 720 || sw_h != 720) {
                                    size_t scaled_size = 0;
                                    uint8_t *scaled = ppa_scale_rgb565(
                                        sw_buf, sw_w, sw_h, 0, 720, 720, &scaled_size);
                                    if (scaled) {
                                        free(sw_buf);
                                        final_buf = scaled;
                                        final_w = 720;
                                        final_h = 720;
                                        final_size = scaled_size;
                                    }
                                }
                                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                    nina_spotify_set_album_art(final_buf, final_w, final_h, final_size);
                                    bsp_display_unlock();
                                }
                                art_ok = true;
                            } else {
                                ESP_LOGW(TAG, "SW JPEG decode also failed for album art");
                            }
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
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

// =============================================================================
// Spotify task lifecycle
// =============================================================================

void spotify_ensure_task_running(void)
{
    if (spotify_task_handle != NULL) return;

    StackType_t  *sp_stack = heap_caps_malloc(10240 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    StaticTask_t *sp_tcb   = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (sp_stack && sp_tcb) {
        spotify_task_handle = xTaskCreateStaticPinnedToCore(
            spotify_poll_task, "spotify_poll", 10240, NULL, 4,
            sp_stack, sp_tcb, 0);
    } else {
        ESP_LOGE(TAG, "Failed to alloc spotify_poll stack from PSRAM, falling back");
        if (sp_stack) heap_caps_free(sp_stack);
        if (sp_tcb) heap_caps_free(sp_tcb);
        xTaskCreatePinnedToCore(spotify_poll_task, "spotify_poll", 10240, NULL, 4,
                                &spotify_task_handle, 0);
    }
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

            jpeg_decode_picture_info_t pic_info = {0};
            esp_err_t err = jpeg_decoder_get_info(jpeg_buf, jpeg_size, &pic_info);
            if (err != ESP_OK || pic_info.width == 0 || pic_info.height == 0) {
                free(jpeg_buf);
                break;
            }

            bool is_gray = (pic_info.sample_method == JPEG_DOWN_SAMPLING_GRAY);
            uint32_t out_w = ((pic_info.width + 15) / 16) * 16;
            uint32_t out_h = ((pic_info.height + 15) / 16) * 16;
            uint32_t decode_buf_size = out_w * out_h * (is_gray ? 1 : 2);

            jpeg_decode_memory_alloc_cfg_t mem_cfg = {
                .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
            };
            size_t allocated_size = 0;
            uint8_t *decode_buf = (uint8_t *)jpeg_alloc_decoder_mem(decode_buf_size, &mem_cfg, &allocated_size);
            if (!decode_buf) { free(jpeg_buf); break; }
            memset(decode_buf, 0, allocated_size); /* Zero buffer so PPA edge interpolation reads black, not heap garbage */

            size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (free_dma < 20 * 1024) {
                ESP_LOGW(TAG, "Fetch worker: low DMA heap (%d bytes), skipping HW decode", (int)free_dma);
                free(decode_buf);
                free(jpeg_buf);
                break;
            }

            jpeg_decoder_handle_t decoder = NULL;
            jpeg_decode_engine_cfg_t engine_cfg = { .intr_priority = 0, .timeout_ms = 5000 };
            err = jpeg_new_decoder_engine(&engine_cfg, &decoder);
            if (err != ESP_OK || !decoder) { free(decode_buf); free(jpeg_buf); break; }

            jpeg_decode_cfg_t decode_cfg = {
                .output_format = is_gray ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565,
                .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            };
            uint32_t out_size = 0;
            perf_timer_start(&g_perf.jpeg_decode);
            err = jpeg_decoder_process(decoder, &decode_cfg, jpeg_buf, jpeg_size,
                                       decode_buf, allocated_size, &out_size);
            perf_timer_stop(&g_perf.jpeg_decode);
            jpeg_del_decoder_engine(decoder);

            if (err != ESP_OK || out_size == 0) {
                free(decode_buf);
                free(jpeg_buf);
                break;
            }

            uint8_t *rgb_buf = decode_buf;
            uint32_t rgb_size = out_size;

            if (is_gray) {
                uint32_t pixel_count = out_w * out_h;
                rgb_size = pixel_count * 2;
                rgb_buf = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
                if (!rgb_buf) { free(decode_buf); free(jpeg_buf); break; }
                uint16_t *dst = (uint16_t *)rgb_buf;
                for (uint32_t i = 0; i < pixel_count; i++) {
                    uint8_t g = decode_buf[i];
                    dst[i] = ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3);
                }
                free(decode_buf);
            }

            result.success = true;
            result.thumbnail.rgb565_data = rgb_buf;
            result.thumbnail.w = out_w;
            result.thumbnail.h = out_h;
            result.thumbnail.data_size = rgb_size;
            free(jpeg_buf);
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
        /* Initialize GOES data struct so its mutex exists even in demo mode
         * (a later web-handler enable + page entry would otherwise NULL-deref). */
        goes_data_init(&goes_data);

        instance_count = app_config_get_instance_count();
        instance_count = 3;  /* demo mode always shows all 3 instance profiles */

        /* Prepare demo task parameters */
        demo_params.instances = instances;
        demo_params.allsky = &allsky_data;
        demo_params.instance_count = instance_count;

        /* Spawn demo data generator on Core 0 */
        StackType_t *demo_stack = heap_caps_malloc(6144 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *demo_tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (demo_stack && demo_tcb) {
            xTaskCreateStaticPinnedToCore(demo_data_task, "demo", 6144,
                                          &demo_params, 4, demo_stack, demo_tcb, 0);
            ESP_LOGI(TAG, "Demo data task spawned");
        } else {
            ESP_LOGE(TAG, "Failed to allocate demo task stack");
            if (demo_stack) heap_caps_free(demo_stack);
            if (demo_tcb) heap_caps_free(demo_tcb);
        }

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
            bool include_pre = (app_config_get()->update_channel == 1);
            const char *cur_ver = ota_github_get_current_version();
            if (ota_github_check(include_pre, cur_ver, rel)) {
                ESP_LOGI(TAG, "New firmware available: %s", rel->tag);
                if (rel->requires_full_erase) {
                    /* This release cannot be installed over WiFi — show a
                     * blocking warning and wait only for dismissal. */
                    ESP_LOGW(TAG, "Firmware %s requires manual USB erase+flash", rel->tag);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show_manual_flash(rel->tag);
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
                if (accepted) {
                    /* Accepted — proceed with OTA download */
                    ESP_LOGI(TAG, "User accepted OTA update to %s", rel->tag);
                    ota_in_progress = true;
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show_progress();
                        bsp_display_unlock();
                    }
                    esp_err_t ota_err = ota_github_download(rel->ota_url, ota_progress_cb);
                    if (ota_err == ESP_OK) {
                        ota_github_save_pending_version(rel->tag);
                        ESP_LOGI(TAG, "OTA download success, rebooting...");
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_ota_prompt_set_progress(100);
                            bsp_display_unlock();
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
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
            } else {
                ESP_LOGI(TAG, "No firmware update available");
            }
            heap_caps_free(rel);
        }
    }

boot_update_check_done:
    // Start MQTT if enabled
    mqtt_ha_start();

    instance_count = app_config_get_instance_count();
    ESP_LOGI(TAG, "Spawning %d per-instance poll tasks", instance_count);

    /* Spawn per-instance poll tasks (boot probe + WS start happen inside each task) */
    for (int i = 0; i < instance_count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "poll_%d", i);
        /* Use xTaskCreateStaticPinnedToCore with PSRAM-allocated stack to save internal heap.
         * Pin poll tasks to Core 0 (networking), leaving Core 1 for UI/LVGL. */
        StackType_t *stack = heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (stack && tcb) {
            poll_contexts[i].task_handle = xTaskCreateStaticPinnedToCore(
                instance_poll_task, name, 8192, &poll_contexts[i], 4, stack, tcb, 0);
            poll_task_handles[i] = poll_contexts[i].task_handle;
        } else {
            ESP_LOGE(TAG, "Failed to allocate poll task %d stack from PSRAM", i);
            if (stack) heap_caps_free(stack);
            if (tcb) heap_caps_free(tcb);
        }
    }

    /* Spawn AllSky poll task (pinned to Core 0, networking) */
    allsky_data_init(&allsky_data);
    if (app_config_get()->allsky_enabled) {
        StackType_t *as_stack = heap_caps_malloc(6144 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *as_tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (as_stack && as_tcb) {
            allsky_task_handle = xTaskCreateStaticPinnedToCore(
                allsky_poll_task, "allsky", 6144, NULL, 3, as_stack, as_tcb, 0);
            ESP_LOGI(TAG, "AllSky poll task spawned");
        } else {
            ESP_LOGE(TAG, "Failed to allocate AllSky poll task stack");
            if (as_stack) heap_caps_free(as_stack);
            if (as_tcb) heap_caps_free(as_tcb);
        }
    }

    /* GOES / Image Display poll task.
     * goes_data_init must run UNCONDITIONALLY so the mutex exists before any
     * later web-handler-triggered enable + page entry. */
    goes_data_init(&goes_data);
    if (app_config_get()->image_display_enabled) {
        goes_ensure_task_running();
    }

    /* Spawn async fetch worker (pinned to Core 0, networking) */
    if (s_fetch_queue && s_fetch_result_queue) {
        StackType_t *fw_stack = heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        StaticTask_t *fw_tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (fw_stack && fw_tcb) {
            xTaskCreateStaticPinnedToCore(
                fetch_worker_task, "fetch_wk", 8192, NULL, 4, fw_stack, fw_tcb, 0);
            ESP_LOGI(TAG, "Fetch worker task spawned on Core 0");
        } else {
            ESP_LOGE(TAG, "Failed to allocate fetch worker stack");
            if (fw_stack) heap_caps_free(fw_stack);
            if (fw_tcb) heap_caps_free(fw_tcb);
        }
    }

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
                            nina_dashboard_set_thumbnail(fres.thumbnail.rgb565_data,
                                fres.thumbnail.w, fres.thumbnail.h, fres.thumbnail.data_size);
                            bsp_display_unlock();
                            /* Ownership transferred to UI */
                        } else {
                            free(fres.thumbnail.rgb565_data);
                        }
                    } else if (!fres.success && nina_dashboard_thumbnail_requested()) {
                        /* Fetch failed — hide loading overlay */
                        nina_dashboard_clear_thumbnail_request();
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_dashboard_hide_thumbnail();
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
        bool on_sysinfo = nina_dashboard_is_sysinfo_page();
        bool on_settings = nina_dashboard_is_settings_page();
        bool on_summary = nina_dashboard_is_summary_page();
        bool on_clock = nina_dashboard_is_clock_page();
        bool on_image_display = nina_dashboard_is_image_display_page();

        /*
         * Page index convention (see PAGE_IDX_* / NINA_PAGE_OFFSET / EXTRA_PAGES):
         *   PAGE_IDX_ALLSKY        (0)                  = AllSky page
         *   PAGE_IDX_SPOTIFY       (1)                  = Spotify page
         *   PAGE_IDX_CLOCK         (2)                  = Clock page (always present)
         *   PAGE_IDX_IMAGE_DISPLAY (3)                  = Image Display page
         *   PAGE_IDX_SUMMARY       (4)                  = summary page
         *   NINA_PAGE_OFFSET .. NINA_PAGE_OFFSET+pc-1   = NINA instance pages
         *   SETTINGS_PAGE_IDX(pc)                       = settings page
         *   SYSINFO_PAGE_IDX(pc)                        = sysinfo page
         *
         * active_nina_idx: the actual instance index (0..MAX_NINA_INSTANCES-1)
         *   for the active page, or -1 if on allsky/spotify/clock/summary/settings/sysinfo.
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

            /* AllSky and Clock flags — wake tasks immediately on page entry */
            static bool prev_on_allsky = false;
            if (on_allsky && !prev_on_allsky && allsky_task_handle) {
                xTaskNotifyGive(allsky_task_handle);
            }
            prev_on_allsky = on_allsky;
            allsky_page_active = on_allsky;

            static bool prev_on_clock = false;
            if (on_clock && !prev_on_clock) {
                weather_client_force_refresh();  /* Wakes weather task via xTaskNotifyGive */
            }
            prev_on_clock = on_clock;
            clock_page_active = on_clock;

            /* Image Display lifecycle — wake on entry, free buffers on leave */
            static bool prev_on_image_display = false;
            if (on_image_display && !prev_on_image_display && goes_task_handle) {
                xTaskNotifyGive(goes_task_handle);
            }
            image_display_page_active = on_image_display;   /* gate poll task BEFORE cleanup */
            if (!on_image_display && prev_on_image_display) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    nina_image_display_cleanup();
                    /* If a manual fetch was still in flight when the user left,
                     * clear its loading overlay so it can't linger off-page. */
                    nina_wait_overlay_hide();
                    bsp_display_unlock();
                }
                goes_client_cleanup(&goes_data);
                moon_render_deinit();   /* release the cached moon texture */
                /* Free the moon drag-to-rotate render scratch (color/z). The page's
                 * own software-scale copy buffers are freed inside
                 * nina_image_display_cleanup() above, so each side frees only what it
                 * owns — no leak, no double-free. */
                moon_drag_buffers_free();
                /* Reset drag orientation so a visit that ended mid-settle does not
                 * carry a stale s_cur_* into the next visit (which would snap the
                 * disc home on the first frame). */
                moon_drag_reset();
                ESP_LOGI(TAG, "Left Image Display: freed buffers");
            }
            prev_on_image_display = on_image_display;
        }

        // Re-read instance count from config so API URL changes take effect live
        // In demo mode, keep instance_count at 3 (set during task init)
        if (!app_config_get()->demo_mode) {
            instance_count = app_config_get_instance_count();
        }

        // Check for debug mode toggle
        {
            static bool last_debug_mode = false;
            static bool first_check = true;
            bool current_debug = app_config_get()->debug_mode;
            if (first_check || current_debug != last_debug_mode) {
                perf_monitor_set_enabled(current_debug);
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
                bool include_pre = (app_config_get()->update_channel == 1);
                const char *cur_ver = ota_github_get_current_version();
                bool update_found = ota_github_check(include_pre, cur_ver, rel);
                if (update_found && rel->requires_full_erase) {
                    /* This release cannot be installed over WiFi — show a
                     * blocking warning and wait only for dismissal. */
                    ESP_LOGW(TAG, "Firmware %s requires manual USB erase+flash", rel->tag);
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        nina_ota_prompt_show_manual_flash(rel->tag);
                        bsp_display_unlock();
                    }
                    while (nina_ota_prompt_visible()) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                } else if (update_found) {
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
                    if (accepted) {
                        ESP_LOGI(TAG, "User accepted OTA update to %s", rel->tag);
                        ota_in_progress = true;
                        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                            nina_ota_prompt_show_progress();
                            bsp_display_unlock();
                        }
                        esp_err_t ota_err = ota_github_download(rel->ota_url, ota_progress_cb);
                        if (ota_err == ESP_OK) {
                            ota_github_save_pending_version(rel->tag);
                            ESP_LOGI(TAG, "OTA download success, rebooting...");
                            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                                nina_ota_prompt_set_progress(100);
                                bsp_display_unlock();
                            }
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            esp_restart();
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
                    summary_page_update(instances, instance_count);
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
         * elapses. resolve() (called once near the end of this cycle) consumes it. */
        {
            app_config_t *r_cfg = app_config_get();
            if (r_cfg->auto_rotate_enabled && r_cfg->auto_rotate_interval_s > 0) {
                if (last_rotate_ms == 0) last_rotate_ms = now_ms;
                if (now_ms - last_rotate_ms >= (int64_t)r_cfg->auto_rotate_interval_s * 1000) {
                    nav_arbiter_notify_slideshow_tick();
                    last_rotate_ms = now_ms;
                }
            } else {
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
        } else if (on_image_display) {
            /* Image Display page — repaint when a new GOES image has arrived.
             * nina_image_display_update locks goes_data internally; it must be
             * called with the display lock held. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_image_display_update(&goes_data);
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
                summary_page_update(instances, instance_count);
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
            bool auto_refresh = false;
            if (nina_client_lock(&instances[active_nina_idx], 15)) {
                auto_refresh = nina_dashboard_thumbnail_visible()
                               && instances[active_nina_idx].new_image_available;
                if (auto_refresh) instances[active_nina_idx].new_image_available = false;
                nina_client_unlock(&instances[active_nina_idx]);
            }

            if ((want_thumbnail || auto_refresh) && !fetch_thumbnail_pending) {
                if (want_thumbnail) nina_dashboard_clear_thumbnail_request();

                const char *thumb_url = app_config_get_instance_url(active_nina_idx);
                if (strlen(thumb_url) > 0 && nina_connection_is_connected(active_nina_idx) && s_fetch_queue) {
                    fetch_request_t req = { .type = FETCH_THUMBNAIL, .instance_idx = active_nina_idx };
                    strlcpy(req.url, thumb_url, sizeof(req.url));
                    if (xQueueSend(s_fetch_queue, &req, 0) == pdTRUE) {
                        fetch_thumbnail_pending = true;
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
            && instances[active_nina_idx].ui_refresh_needed) {
            instances[active_nina_idx].ui_refresh_needed = false;
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

        /* ── Navigation arbiter: resolve the page-commit ladder once per cycle ──
         * Runs AFTER per-page UI updates and the slideshow-tick feeder, OUTSIDE
         * any LVGL lock (the arbiter takes the lock itself around the commit).
         * This is the single owner of the navigation decision; it also drives the
         * idle indicator via nav_arbiter_idle_active() inside its commit. A user
         * wake (xTaskNotifyGive from on_page_changed) produces a resolve within
         * one cycle. */
        nav_arbiter_resolve(esp_timer_get_time() / 1000);

        /* ── Screen sleep: turn off backlight when idle ── */
        {
            app_config_t *sl_cfg = app_config_get();
            if (sl_cfg->screen_sleep_enabled) {

                /* Touch wake — always check first, even if connections are back */
                if (screen_asleep && screen_touch_wake) {
                    /* Restore WiFi power save mode */
                    esp_wifi_set_ps(sl_cfg->wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
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
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cycle_ms));
        }
    }
}
