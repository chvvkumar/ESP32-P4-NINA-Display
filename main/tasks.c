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
#include "perf_monitor.h"
#include "power_mgmt.h"
#include "demo_data.h"
#include "driver/jpeg_decode.h"

static const char *TAG = "tasks";

#define BOOT_BUTTON_GPIO    GPIO_NUM_35
#define DEBOUNCE_MS         200
#define HEARTBEAT_INTERVAL_MS 10000
/* Graph refresh interval read from config at runtime (graph_update_interval_s) */

/* Signals the data task that a page switch occurred */
volatile bool page_changed = false;
volatile bool ota_in_progress = false;

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
volatile bool ota_check_requested = false;
volatile bool screen_touch_wake = false;
volatile bool screen_asleep = false;
TaskHandle_t data_task_handle = NULL;
TaskHandle_t poll_task_handles[MAX_NINA_INSTANCES] = {NULL};
static int64_t last_graph_fetch_ms = 0;  /* Timestamp of last graph data fetch */
static bool hfr_graph_seeded = false;   /* True after initial API fetch for current HFR graph session */

/* Per-instance poll contexts (shared between UI coordinator and poll tasks) */
static instance_poll_ctx_t poll_contexts[MAX_NINA_INSTANCES];

/* Spotify task state */
TaskHandle_t spotify_task_handle = NULL;
volatile bool spotify_page_active = false;

/* AllSky polling state */
static allsky_data_t allsky_data;
static TaskHandle_t allsky_task_handle = NULL;
static demo_task_params_t demo_params;

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

            /* Get current page before sleeping */
            int current_page = nina_dashboard_get_active_page();

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
                app_config_get()->deep_sleep_wake_timer_s,
                current_page
            );
        } else if (!long_pressed) {
            /* Short press — cycle page */
            int total = nina_dashboard_get_total_page_count();
            int current = nina_dashboard_get_active_page();
            /* Skip settings, disabled allsky, and disabled spotify in button cycling */
            int new_page = current;
            for (int step = 1; step < total; step++) {
                int candidate = (current + step) % total;
                if (candidate == SETTINGS_PAGE_IDX(page_count)) continue;
                if (candidate == PAGE_IDX_ALLSKY && !nina_dashboard_is_allsky_page()
                    && !app_config_get()->allsky_enabled) continue;
                if (candidate == PAGE_IDX_SPOTIFY && !nina_dashboard_is_spotify_page()
                    && !app_config_get()->spotify_enabled) continue;
                new_page = candidate;
                break;
            }
            ESP_LOGI(TAG, "Button: switching to page %d", new_page);

            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_show_page(new_page, total);
                bsp_display_unlock();
            } else {
                ESP_LOGW(TAG, "Display lock timeout (button page switch)");
            }

            page_changed = true;
        }

        /* Record stack high water mark if perf monitoring is enabled */
        if (g_perf.enabled) {
            g_perf.input_task_stack_hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
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
        // Suspend during OTA or while Spotify page is active
        while ((ota_in_progress || spotify_page_active) && !ctx->shutdown) {
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
        if (!screen_asleep) {
            nina_websocket_check_reconnect(idx, url, ctx->client);
        }

        // Sleep: active = update_rate_s, background = heartbeat, screen_asleep = idle_poll
        uint32_t cycle_ms;
        if (screen_asleep) {
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
        /* Suspend during OTA or while Spotify page is active */
        while (ota_in_progress || spotify_page_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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
        /* Suspend during OTA updates */
        while (ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        app_config_t *cfg = app_config_get();
        if (!cfg->spotify_enabled || spotify_auth_get_state() != SPOTIFY_AUTH_AUTHORIZED) {
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
            vTaskDelay(pdMS_TO_TICKS(2000));
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
        if (g_perf.enabled) {
            g_perf.spotify_task_stack_hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        }
        vTaskDelay(pdMS_TO_TICKS(interval));
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

    /* ── Demo mode: skip all network tasks, spawn demo data generator ── */
    if (app_config_get()->demo_mode) {
        ESP_LOGI(TAG, "DEMO MODE — skipping WiFi wait, polling, MQTT, WebSocket");

        /* Initialize AllSky data struct (needed even in demo mode) */
        allsky_data_init(&allsky_data);

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
                        ota_github_save_installed_version(rel->tag);
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

        int current_active = nina_dashboard_get_active_page();  // Snapshot to avoid races
        bool on_allsky = nina_dashboard_is_allsky_page();
        bool on_sysinfo = nina_dashboard_is_sysinfo_page();
        bool on_settings = nina_dashboard_is_settings_page();
        bool on_summary = nina_dashboard_is_summary_page();

        /*
         * Page index convention (see PAGE_IDX_* / NINA_PAGE_OFFSET / EXTRA_PAGES):
         *   PAGE_IDX_ALLSKY  (0)                        = AllSky page
         *   PAGE_IDX_SPOTIFY (1)                        = Spotify page
         *   PAGE_IDX_SUMMARY (2)                        = summary page
         *   NINA_PAGE_OFFSET .. NINA_PAGE_OFFSET+pc-1   = NINA instance pages
         *   SETTINGS_PAGE_IDX(pc)                       = settings page
         *   SYSINFO_PAGE_IDX(pc)                        = sysinfo page
         *
         * active_nina_idx: the actual instance index (0..MAX_NINA_INSTANCES-1)
         *   for the active page, or -1 if on allsky/spotify/summary/settings/sysinfo.
         */
        bool on_spotify = nina_dashboard_is_spotify_page();
        int active_nina_idx = -1;   /* Actual instance index (for data access) */
        int active_page_idx = -1;  /* 0-based page index into pages[] (for UI calls) */
        if (!on_allsky && !on_spotify && !on_sysinfo && !on_settings && !on_summary
            && current_active >= NINA_PAGE_OFFSET) {
            active_page_idx = current_active - NINA_PAGE_OFFSET;
            active_nina_idx = nina_dashboard_page_to_instance(active_page_idx);
        }

        /* Update Spotify page activity flag for poll task + cleanup on transitions.
         * Free inactive context's TLS sessions and large buffers to prevent
         * internal DMA heap exhaustion (esp-aes OOM during TLS handshake). */
        {
            static bool prev_on_spotify = false;
            if (on_spotify && !prev_on_spotify) {
                /* Entering Spotify — free NINA resources */
                nina_websocket_stop_all();
                /* Dismiss thumbnail overlay if open (frees original + scaled buffers) */
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    if (nina_dashboard_thumbnail_visible())
                        nina_dashboard_hide_thumbnail();
                    bsp_display_unlock();
                }
                ESP_LOGI(TAG, "Entering Spotify: freed NINA WebSocket TLS sessions");
            } else if (!on_spotify && prev_on_spotify) {
                /* Leaving Spotify — free art buffer.  Don't destroy the
                 * Spotify HTTP client here: the spotify_poll_task may be
                 * mid-request on Core 0.  It will see spotify_page_active
                 * go false and clean up its own connection safely. */
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    nina_spotify_free_art();
                    bsp_display_unlock();
                }
                ESP_LOGI(TAG, "Leaving Spotify: freed album art buffer");
            }
            prev_on_spotify = on_spotify;
        }
        spotify_page_active = on_spotify;

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
                if (ota_github_check(include_pre, cur_ver, rel)) {
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
                            ota_github_save_installed_version(rel->tag);
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
                    locked[j] = nina_client_lock(&instances[j], 100);
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    summary_page_update(instances, instance_count);
                    bsp_display_unlock();
                }
                for (int j = 0; j < instance_count; j++)
                    if (locked[j]) nina_client_unlock(&instances[j]);
            }

            /* Immediate AllSky render with cached data */
            if (on_allsky) {
                if (allsky_data_lock(&allsky_data, 100)) {
                    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                        allsky_page_update(&allsky_data);
                        bsp_display_unlock();
                    }
                    allsky_data_unlock(&allsky_data);
                }
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

        /* If auto-rotate is active and we're on a NINA instance page but no instances
         * are connected, fall back to the summary page automatically. */
        if (app_config_get()->auto_rotate_enabled && !on_summary && !on_sysinfo && !on_settings && !on_allsky && !on_spotify) {
            bool any_connected = false;
            for (int i = 0; i < instance_count; i++) {
                if (nina_connection_is_connected(i)) { any_connected = true; break; }
            }
            if (!any_connected) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    nina_dashboard_show_page(PAGE_IDX_SUMMARY, instance_count);
                    bsp_display_unlock();
                }
                page_changed = true;
                ESP_LOGI(TAG, "No NINA connections — returning to summary page");
            }
        }

        /* Auto-rotate logic — rotates between pages selected by auto_rotate_pages bitmask.
         *
         * Page bitmask layout:
         *   bit 0  = Summary page (PAGE_IDX_SUMMARY)
         *   bit 1  = NINA instance 1 (NINA_PAGE_OFFSET + 0)
         *   bit 2  = NINA instance 2 (NINA_PAGE_OFFSET + 1)
         *   bit 3  = NINA instance 3 (NINA_PAGE_OFFSET + 2)
         *   bit 4  = System Info page (SYSINFO_PAGE_IDX)
         *   bit 5  = AllSky page (PAGE_IDX_ALLSKY)
         *   bit 6  = Spotify page (PAGE_IDX_SPOTIFY)
         */
        {
            app_config_t *r_cfg = app_config_get();
            if (r_cfg->auto_rotate_enabled && r_cfg->auto_rotate_interval_s > 0) {
                if (last_rotate_ms == 0) last_rotate_ms = now_ms;
                if (now_ms - last_rotate_ms >= (int64_t)r_cfg->auto_rotate_interval_s * 1000) {
                    uint8_t page_mask = r_cfg->auto_rotate_pages;
                    int total = nina_dashboard_get_total_page_count();

                    int ena_page_count = total - EXTRA_PAGES;  /* enabled NINA pages only */

                    /* Find next page in rotation by stepping through candidates */
                    int next_page = current_active;
                    for (int step = 1; step < total; step++) {
                        int candidate = (current_active + step) % total;

                        /* Check if this candidate is in the rotation bitmask.
                         * Bitmask bits 1-3 refer to instance indices (not page indices),
                         * so use the page-to-instance mapping for NINA pages. */
                        bool in_mask = false;
                        if (candidate == PAGE_IDX_ALLSKY) {
                            in_mask = (page_mask & 0x20) != 0;             /* AllSky (bit 5) */
                            if (!app_config_get()->allsky_enabled) in_mask = false;
                        }
                        else if (candidate == PAGE_IDX_SPOTIFY) {
                            in_mask = (page_mask & 0x40) != 0;             /* Spotify (bit 6) */
                            if (!app_config_get()->spotify_enabled) in_mask = false;
                        }
                        else if (candidate == PAGE_IDX_SUMMARY)
                            in_mask = (page_mask & 0x01) != 0;             /* Summary (bit 0) */
                        else if (candidate >= NINA_PAGE_OFFSET && candidate < NINA_PAGE_OFFSET + ena_page_count) {
                            int inst = nina_dashboard_page_to_instance(candidate - NINA_PAGE_OFFSET);
                            if (inst >= 0)
                                in_mask = (page_mask & (1 << (inst + 1))) != 0; /* NINA page */
                        }
                        else if (candidate == SETTINGS_PAGE_IDX(ena_page_count))
                            in_mask = false;                               /* Settings — never in rotation */
                        else if (candidate == SYSINFO_PAGE_IDX(ena_page_count))
                            in_mask = (page_mask & 0x10) != 0;             /* Sysinfo (bit 4) */

                        if (!in_mask) continue;

                        /* Skip disconnected NINA instances if configured */
                        if (candidate >= NINA_PAGE_OFFSET && candidate < NINA_PAGE_OFFSET + ena_page_count) {
                            int nina_idx = nina_dashboard_page_to_instance(candidate - NINA_PAGE_OFFSET);
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
                perf_monitor_record_wifi(&ap_info);
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

        /* Update AllSky page when visible */
        if (on_allsky) {
            if (allsky_data_lock(&allsky_data, 100)) {
                if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                    allsky_page_update(&allsky_data);
                    bsp_display_unlock();
                }
                allsky_data_unlock(&allsky_data);
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
                    lvgl_port_lock(0);
                    {
                        lv_obj_t *scr = lv_scr_act();
                        /* Black overlay covers entire screen */
                        lv_obj_t *overlay = lv_obj_create(scr);
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
                        lv_obj_t *scr = lv_scr_act();
                        /* Remove the last child (our overlay) */
                        uint32_t cnt = lv_obj_get_child_count(scr);
                        if (cnt > 0) {
                            lv_obj_t *last = lv_obj_get_child(scr, cnt - 1);
                            lv_obj_delete(last);
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
                    int current_page = nina_dashboard_get_active_page();
                    power_mgmt_enter_deep_sleep(
                        app_config_get()->deep_sleep_wake_timer_s,
                        current_page
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
                perf_monitor_report();
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
