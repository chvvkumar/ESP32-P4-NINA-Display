#include "web_server_internal.h"
#include "audio_alert.h"
#include "nina_allsky.h"
#include "nina_connection.h"
#include "tasks.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "display_defs.h"
#include "ui/nina_dashboard.h"
#include "ui/nina_dashboard_internal.h"
#include "ui/nina_nav_arbiter.h"
#include "ui/nina_image_page.h"
#include "ui/nina_octoprint.h"
#include "ui/nina_spotify.h"
#include "ui/themes.h"
#include "lvgl.h"
#include "driver/jpeg_encode.h"
#include "mqtt_ha.h"
#include "wifi_manager.h"
#include "weather_client.h"
#include "ui/nina_clock.h"
#include "ui/nina_idle_indicator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

// Trigger hardware side effects when config fields change
void config_trigger_side_effects(const app_config_t *old_cfg, const app_config_t *new_cfg) {
    if (new_cfg->brightness != old_cfg->brightness) {
        bsp_display_brightness_set(new_cfg->brightness);
    }
    if (new_cfg->theme_index != old_cfg->theme_index ||
        new_cfg->color_brightness != old_cfg->color_brightness ||
        new_cfg->widget_style != old_cfg->widget_style) {
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_apply_theme(new_cfg->theme_index);
            bsp_display_unlock();
        }
    }
    /* WiFi transmit power. Applied here rather than in the POST handler so all
     * three config write paths get it: /api/config (Save), /api/config/apply
     * (live preview, RAM only), and /api/config/revert (Discard, which calls
     * this with the NVS values as new_cfg). Revert genuinely puts the radio
     * back where it was, including all the way back to the chip default when
     * the saved value is 0 — wifi_apply_tx_power() restores the power it
     * probed at boot rather than treating 0 as "leave the cap in place". An
     * unsaved change therefore lasts only until Discard or the next reboot,
     * which is the lockout escape hatch the web UI's warning tells the user
     * about. */
    if (new_cfg->wifi_max_tx_dbm != old_cfg->wifi_max_tx_dbm) {
        wifi_apply_tx_power(new_cfg->wifi_max_tx_dbm);
    }
    if (new_cfg->screen_rotation != old_cfg->screen_rotation) {
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            display_rotation_apply(new_cfg->screen_rotation);
            bsp_display_unlock();
        }
    }
    /* Instance enable/disable or URL change — immediately wake poll tasks,
     * rebuild the affected NINA slot, and raise the arbiter topology flag so
     * the next resolve re-evaluates targets against the new slot set. */
    bool topology_changed = false;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        bool enable_changed = (new_cfg->instance_enabled[i] != old_cfg->instance_enabled[i]);
        bool url_changed = (strcmp(new_cfg->api_url[i], old_cfg->api_url[i]) != 0);
        if (enable_changed || url_changed) {
            if (!new_cfg->instance_enabled[i]) {
                /* User turned it off — offline now, no timeout window. */
                nina_connection_force_disconnect(i);
            }
            if (poll_task_handles[i]) {
                xTaskNotifyGive(poll_task_handles[i]);
            }
            /* Lazily create/destroy the slot to match the new config. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_rebuild_slot(i);
                bsp_display_unlock();
            }
            topology_changed = true;
        }
    }
    /* Demo mode is a connection/data override (issue 12): a live toggle changes
     * the connection picture, so force a topology re-resolve on the next tick. */
    if (new_cfg->demo_mode != old_cfg->demo_mode) {
        topology_changed = true;
    }
    /* Spotify enable change — the page is created lazily on first enable, so a
     * toggle from the main config save must reach the dashboard here or the
     * page only appears after a reboot. */
    if (new_cfg->spotify_enabled != old_cfg->spotify_enabled) {
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_set_spotify_enabled(new_cfg->spotify_enabled);
            bsp_display_unlock();
        }
        if (new_cfg->spotify_enabled) {
            spotify_ensure_task_running();
        }
        topology_changed = true;  /* optional page appeared/disappeared */
    }
    /* Spotify layout mode change — refresh overlay if visible */
    if (new_cfg->spotify_minimal_mode != old_cfg->spotify_minimal_mode ||
        new_cfg->spotify_show_progress_bar != old_cfg->spotify_show_progress_bar ||
        new_cfg->spotify_overlay_timeout_s != old_cfg->spotify_overlay_timeout_s ||
        new_cfg->spotify_scroll_text != old_cfg->spotify_scroll_text ||
        new_cfg->spotify_overlay_visible != old_cfg->spotify_overlay_visible) {
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_spotify_refresh_layout();
            bsp_display_unlock();
        }
    }
    if (new_cfg->allsky_enabled != old_cfg->allsky_enabled ||
        strcmp(new_cfg->allsky_field_config, old_cfg->allsky_field_config) != 0 ||
        strcmp(new_cfg->allsky_thresholds, old_cfg->allsky_thresholds) != 0 ||
        strcmp(new_cfg->allsky_hostname, old_cfg->allsky_hostname) != 0 ||
        new_cfg->allsky_update_interval_s != old_cfg->allsky_update_interval_s ||
        new_cfg->allsky_dew_offset != old_cfg->allsky_dew_offset) {
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            allsky_page_refresh_config();
            nina_dashboard_set_allsky_enabled(new_cfg->allsky_enabled);
            bsp_display_unlock();
        }
        if (new_cfg->allsky_enabled != old_cfg->allsky_enabled) {
            topology_changed = true;  /* optional page appeared/disappeared */
        }
    }
    /* Image source / snapshot URL: the poll task re-reads config every cycle,
     * but "every cycle" is up to octoprint_update_interval_s away (300 s at the
     * top of the range), so the switch would sit there invisible. Wake it and it
     * drops the held frame and fetches the new source in that one pass. The URL,
     * API key and interval keep the lazy path — they need no visible-now edit. */
    if (new_cfg->octoprint_enabled &&
        (new_cfg->octoprint_image_source != old_cfg->octoprint_image_source ||
         strcmp(new_cfg->octoprint_snapshot_url, old_cfg->octoprint_snapshot_url) != 0)) {
        /* A source SWITCH also has to clear what is on screen right now: the
         * held frame is the other source's picture, and the wake below still
         * needs a fetch and a decode (1-3 s) before the new one lands. Without
         * this the toggle looks dead for those seconds and then jumps. A
         * snapshot-URL edit alone keeps the frame -- same source, new address. */
        if (new_cfg->octoprint_image_source != old_cfg->octoprint_image_source) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                octoprint_page_image_source_changed();
                bsp_display_unlock();
            }
            octoprint_client_note_image_source_switch(&octoprint_data);
        }
        octoprint_wake_now();
    }
    /* OctoPrint page enable / layout change. The URL, API key and poll interval
     * need no live action — octoprint_poll_task re-reads config every cycle, so
     * the next poll picks them up. Only the enable toggle (page create/destroy)
     * and the layout selection (widget tree rebuild) have to be applied here. */
    if (new_cfg->octoprint_enabled != old_cfg->octoprint_enabled ||
        new_cfg->octoprint_layout != old_cfg->octoprint_layout) {
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            /* Only the layout selection needs the widget-tree rebuild; showing
             * or hiding the page is set_octoprint_enabled's job. */
            if (new_cfg->octoprint_layout != old_cfg->octoprint_layout) {
                octoprint_page_refresh_config();
            }
            nina_dashboard_set_octoprint_enabled(new_cfg->octoprint_enabled);
            bsp_display_unlock();
        }
        if (new_cfg->octoprint_enabled) {
            octoprint_ensure_task_running();
        }
        if (new_cfg->octoprint_enabled != old_cfg->octoprint_enabled) {
            topology_changed = true;  /* optional page appeared/disappeared */
        }
    }
    /* Readings-over-picture default. octoprint_page_set_overlay_visible() takes
     * the LVGL lock itself, so it is called OUTSIDE bsp_display_lock() -- and
     * outside the block above for the same reason. Only the change case needs
     * the call: a layout rebuild starts the new widget tree from the config
     * value anyway. Bento (Grid) has no overlay layer and ignores it. */
    if (new_cfg->octoprint_overlay_visible != old_cfg->octoprint_overlay_visible) {
        octoprint_page_set_overlay_visible(new_cfg->octoprint_overlay_visible);
    }
    /* Image pages (GOES/Moon/Solar/Custom/Radar): one live-apply for all five; an
     * enable toggle is a topology change (an optional page appeared/disappeared). */
    image_page_config_apply_live(old_cfg, new_cfg, false);
    if (new_cfg->goes_enabled != old_cfg->goes_enabled ||
        new_cfg->moon_enabled != old_cfg->moon_enabled ||
        new_cfg->solar_enabled != old_cfg->solar_enabled ||
        new_cfg->custom_enabled != old_cfg->custom_enabled ||
        new_cfg->radar_enabled != old_cfg->radar_enabled) {
        topology_changed = true;
    }
    /* Weather config change — invalidate stale data and force refresh */
    if (new_cfg->weather_provider != old_cfg->weather_provider ||
        strcmp(new_cfg->weather_api_key, old_cfg->weather_api_key) != 0 ||
        strcmp(new_cfg->weather_location_name, old_cfg->weather_location_name) != 0 ||
        new_cfg->weather_lat != old_cfg->weather_lat ||
        new_cfg->weather_lon != old_cfg->weather_lon ||
        new_cfg->weather_poll_interval_s != old_cfg->weather_poll_interval_s ||
        new_cfg->weather_units != old_cfg->weather_units) {
        weather_client_invalidate();    /* Clear stale data immediately */
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            clock_page_request_update(); /* Show placeholders immediately */
            bsp_display_unlock();
        }
        weather_client_force_refresh(); /* Wake task to fetch from new provider */
    }
    /* Idle indicator toggle — hide immediately when disabled;
     * when re-enabled, tasks.c loop will show it on the next cycle */
    if (!new_cfg->idle_indicator_enabled && old_cfg->idle_indicator_enabled) {
        nina_idle_indicator_set_active(false);
    }
    /* Apply timezone immediately */
    if (strcmp(new_cfg->tz_string, old_cfg->tz_string) != 0 &&
        new_cfg->tz_string[0] != '\0') {
        setenv("TZ", new_cfg->tz_string, 1);
        tzset();
    }
    /* Topology change (instance enable/URL, optional-page enables, demo toggle):
     * raise the flag once so the next nav_arbiter_resolve re-evaluates targets. */
    if (topology_changed) {
        nav_arbiter_notify_topology_changed();
    }
}

/**
 * Commit a live display tweak to the in-memory config, RAM-only either way.
 *
 * These five endpoints (/api/brightness, /api/color-brightness, /api/theme,
 * /api/widget-style, /api/screen-rotation) serve two very different callers: the
 * config UI's sliders, where the change IS a pending unsaved edit and the
 * "unsaved changes" bar is honest -- and HA automations or macro keypads driving
 * the same routes with X-Auth-Password, where the user never opened an editor
 * and a bar they cannot explain is a bug. A day/night theme automation firing a
 * few minutes after boot left dash2 permanently flagged dirty with nothing
 * actually unsaved.
 *
 * Split on the session cookie (see request_is_web_ui) -- an existing signal, not
 * a new one. Persistence is unchanged and identical on both paths: neither
 * writes NVS. Only the flag differs.
 */
static void apply_display_change(httpd_req_t *req, const app_config_t *cfg)
{
    if (request_is_web_ui(req)) {
        app_config_apply(cfg);        /* pending web edit -> bar is correct */
    } else {
        app_config_apply_external(cfg);   /* commanded from outside -> no bar */
    }
}

/* ---- Live display tweak spine ------------------------------------------
 * /api/brightness, /api/color-brightness, /api/theme, /api/widget-style and
 * /api/screen-rotation were five copies of one 45-line handler: recv a
 * single-key body, clamp the int, snapshot config into PSRAM, write one field,
 * apply RAM-only, poke the hardware, answer "OK". Only the key, the range and
 * the two per-setting steps below differ.
 *
 * set()   writes the field into the snapshot (called before apply).
 * after() drives the hardware and logs, with the snapshot still live (some
 *         settings need cfg->theme_index, which set() does not change). */
typedef void (*disp_set_fn)(app_config_t *cfg, int v);
typedef void (*disp_after_fn)(const app_config_t *cfg, int v);

static esp_err_t live_display_int_post(httpd_req_t *req, const char *key,
                                       int lo, int hi,
                                       disp_set_fn set, disp_after_fn after)
{
    char buf[128];
    cJSON *root = receive_small_json_body(req, buf, sizeof(buf));
    if (!root) {
        return ESP_FAIL;   /* response already sent */
    }

    /* A missing or non-numeric key is not an error: the endpoint has always
     * answered "OK" and changed nothing. */
    cJSON *val = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(val)) {
        int v = val->valueint;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        /* Live preview: apply to the in-memory config (no NVS write until Save). */
        app_config_t *cfg = config_snapshot_for_request(req);
        if (!cfg) {
            /* Apply nothing: driving the hardware without the matching config
             * update leaves device state and config silently diverged. */
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        set(cfg, v);
        apply_display_change(req, cfg);
        after(cfg, v);
        heap_caps_free(cfg);
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live brightness adjustment (no reboot needed)
static void disp_set_brightness(app_config_t *cfg, int v) { cfg->brightness = v; }
static void disp_after_brightness(const app_config_t *cfg, int v)
{
    (void)cfg;
    bsp_display_brightness_set(v);
    ESP_LOGI(TAG, "Brightness set to %d%%", v);
    mqtt_ha_publish_state();
}

esp_err_t brightness_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return live_display_int_post(req, "brightness", 0, 100,
                                 disp_set_brightness, disp_after_brightness);
}

// Handler for live color brightness adjustment (no reboot needed)
static void disp_set_color_brightness(app_config_t *cfg, int v) { cfg->color_brightness = v; }
static void disp_after_color_brightness(const app_config_t *cfg, int v)
{
    // Re-apply theme to update static text brightness
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_apply_theme(cfg->theme_index);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Display lock timeout (color brightness)");
    }
    ESP_LOGI(TAG, "Color brightness set to %d%%", v);
    mqtt_ha_publish_state();
}

esp_err_t color_brightness_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return live_display_int_post(req, "color_brightness", 0, 100,
                                 disp_set_color_brightness, disp_after_color_brightness);
}

// Handler for live theme switching (no reboot needed)
static void disp_set_theme(app_config_t *cfg, int v) { cfg->theme_index = v; }
static void disp_after_theme(const app_config_t *cfg, int v)
{
    (void)cfg;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_apply_theme(v);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Display lock timeout (theme switch)");
    }
    ESP_LOGI(TAG, "Theme set to %d", v);
}

esp_err_t theme_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return live_display_int_post(req, "theme_index", 0, themes_get_count() - 1,
                                 disp_set_theme, disp_after_theme);
}

// Handler for live widget style switching (no reboot needed)
static void disp_set_widget_style(app_config_t *cfg, int v) { cfg->widget_style = (uint8_t)v; }
static void disp_after_widget_style(const app_config_t *cfg, int v)
{
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_apply_theme(cfg->theme_index);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Display lock timeout (widget style switch)");
    }
    ESP_LOGI(TAG, "Widget style set to %d", v);
}

esp_err_t widget_style_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return live_display_int_post(req, "widget_style", 0, WIDGET_STYLE_COUNT - 1,
                                 disp_set_widget_style, disp_after_widget_style);
}

// Handler for live screen rotation adjustment (no reboot needed)
static void disp_set_screen_rotation(app_config_t *cfg, int v) { cfg->screen_rotation = (uint8_t)v; }
static void disp_after_screen_rotation(const app_config_t *cfg, int v)
{
    (void)cfg;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        display_rotation_apply(v);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Display lock timeout (screen rotation)");
    }
    ESP_LOGI(TAG, "Screen rotation set to %d", v);
}

esp_err_t screen_rotation_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return live_display_int_post(req, "screen_rotation", 0, 3,
                                 disp_set_screen_rotation, disp_after_screen_rotation);
}

/* Handler for live page switching. Off the spine above: it touches no config
 * field, it issues a navigation claim. */
esp_err_t page_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char buf[64];
    cJSON *root = receive_small_json_body(req, buf, sizeof(buf));
    if (!root) {
        return ESP_FAIL;   /* response already sent */
    }

    /* Task 4.1: /api/page is the pure immediate-navigation USER path; it no
     * longer touches the Home Page field (active_page_override); that moves to
     * the config save path (Task 5.2). A -1 (Auto/Summary radio) maps to a USER
     * claim to the Summary page so the existing web radio keeps working. */
    cJSON *val = cJSON_GetObjectItem(root, "page");
    if (cJSON_IsNumber(val)) {
        int page = val->valueint;
        int total = nina_dashboard_get_total_page_count();
        if (page < 0) page = PAGE_IDX_SUMMARY;          /* -1 (Auto) -> Summary USER claim */
        if (page >= 0 && page < total) {
            nav_arbiter_submit_user(page, esp_timer_get_time() / 1000);
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_show_page_animated(page, 0, 0);
                bsp_display_unlock();
            }
            ESP_LOGI(TAG, "Page switched to %d via web (USER claim)", page);
        }
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Persistent JPEG encoder engine — DMA link descriptors are allocated once from
// internal DMA memory at startup (before display driver claims DMA resources) and
// reused across all screenshot requests.
static jpeg_encoder_handle_t s_jpeg_encoder = NULL;

void screenshot_encoder_init(void)
{
    if (s_jpeg_encoder) return;
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    esp_err_t err = jpeg_new_encoder_engine(&engine_cfg, &s_jpeg_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
        s_jpeg_encoder = NULL;
    } else {
        ESP_LOGI(TAG, "JPEG encoder engine pre-allocated");
    }
}

// Handler for screenshot capture - serves a JPEG image via hardware encoder
esp_err_t screenshot_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    ESP_LOGI(TAG, "Screenshot requested");

    if (!s_jpeg_encoder) {
        ESP_LOGE(TAG, "JPEG encoder not available (init failed at startup)");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Take LVGL snapshot while holding the display lock
    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "Failed to acquire display lock for screenshot");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "No active screen");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    lv_draw_buf_t *snapshot = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();

    if (!snapshot || !snapshot->data) {
        ESP_LOGE(TAG, "Snapshot capture failed");
        if (snapshot) lv_draw_buf_destroy(snapshot);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint32_t width = snapshot->header.w;
    uint32_t height = snapshot->header.h;
    uint32_t stride = snapshot->header.stride;
    uint32_t row_size = width * 2;  // RGB565: 2 bytes per pixel
    uint32_t raw_size = row_size * height;

    ESP_LOGI(TAG, "Snapshot captured: %lux%lu, stride=%lu", width, height, stride);

    // Allocate DMA-aligned input buffer and copy pixels
    jpeg_encode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    size_t in_alloc_size = 0;
    uint8_t *enc_in = (uint8_t *)jpeg_alloc_encoder_mem(raw_size, &in_mem_cfg, &in_alloc_size);
    if (!enc_in) {
        ESP_LOGE(TAG, "Failed to alloc JPEG encoder input buffer (%lu bytes)", raw_size);
        lv_draw_buf_destroy(snapshot);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    for (uint32_t y = 0; y < height; y++) {
        memcpy(enc_in + y * row_size, snapshot->data + y * stride, row_size);
    }
    lv_draw_buf_destroy(snapshot);

    // Allocate DMA-aligned output buffer
    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t out_alloc_size = 0;
    uint8_t *enc_out = (uint8_t *)jpeg_alloc_encoder_mem(raw_size, &out_mem_cfg, &out_alloc_size);
    if (!enc_out) {
        ESP_LOGE(TAG, "Failed to alloc JPEG encoder output buffer");
        free(enc_in);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Encode RGB565 -> JPEG
    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = 90,
        .width = width,
        .height = height,
    };
    uint32_t jpg_size = 0;
    esp_err_t err = jpeg_encoder_process(s_jpeg_encoder, &enc_cfg,
        enc_in, raw_size, enc_out, out_alloc_size, &jpg_size);
    free(enc_in);

    if (err != ESP_OK || jpg_size == 0) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
        free(enc_out);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Screenshot encoded: %lu bytes JPEG (%.1f:1 ratio)",
        jpg_size, (float)raw_size / jpg_size);

    // Send as JPEG image
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.jpg\"");
    httpd_resp_send(req, (const char *)enc_out, jpg_size);

    free(enc_out);
    ESP_LOGI(TAG, "Screenshot sent successfully");
    return ESP_OK;
}

/* POST /api/voice-preview — speak a sample voice alert through the speaker.
 *   kind=event&category=0..11&instance=1..3[&equipment=0..N]
 *   kind=breach&type=rms|hfr|safety&instance=1..3[&value=F]
 *   kind=conn&state=connected|disconnected&instance=1..3   (NINA link edge)
 *   kind=jingle                       (startup jingle, no other params)
 *   kind=clip&name=X                  (single clip by filename stem, e.g. "chime")
 * Uses the preview/test entry points, which bypass the enable/mute/mask gates
 * so the web UI can audition sentences while voice alerts are switched off. */
esp_err_t voice_preview_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char q[128] = {0}, kind[12] = {0}, buf[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "kind", kind, sizeof(kind)) != ESP_OK) {
        return send_400(req, "missing kind");
    }
    if (strcmp(kind, "jingle") == 0) {   /* no instance/category params */
        audio_alert_preview_jingle();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }
    if (strcmp(kind, "clip") == 0) {     /* single clip, no instance param */
        char name[32] = {0};
        if (httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK ||
            name[0] == '\0') {
            return send_400(req, "missing name");
        }
        if (!audio_alert_test_clip(name)) {
            return send_400(req, "unknown clip");
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }
    if (httpd_query_key_value(q, "instance", buf, sizeof(buf)) != ESP_OK) {
        return send_400(req, "missing instance");
    }
    int instance = atoi(buf);
    if (instance < 1 || instance > 3) {
        return send_400(req, "instance must be 1-3");
    }

    if (strcmp(kind, "event") == 0) {
        if (httpd_query_key_value(q, "category", buf, sizeof(buf)) != ESP_OK) {
            return send_400(req, "missing category");
        }
        int category = atoi(buf);
        if (category < 0 || category > 11) {
            return send_400(req, "category must be 0-11");
        }
        int equipment = -1;
        if (httpd_query_key_value(q, "equipment", buf, sizeof(buf)) == ESP_OK) {
            equipment = atoi(buf);
        }
        audio_alert_preview_event(category, instance - 1, equipment);
    } else if (strcmp(kind, "breach") == 0) {
        char tbuf[12] = {0};
        if (httpd_query_key_value(q, "type", tbuf, sizeof(tbuf)) != ESP_OK) {
            return send_400(req, "missing type");
        }
        alert_type_t type;
        if      (strcmp(tbuf, "rms") == 0)    type = ALERT_RMS;
        else if (strcmp(tbuf, "hfr") == 0)    type = ALERT_HFR;
        else if (strcmp(tbuf, "safety") == 0) type = ALERT_SAFETY;
        else return send_400(req, "bad type");

        float value = 0.0f;
        if (httpd_query_key_value(q, "value", buf, sizeof(buf)) == ESP_OK) {
            char *end = NULL;
            value = strtof(buf, &end);
            if (end == buf) return send_400(req, "bad value");
        }
        audio_alert_test_speak(type, instance - 1, value);
    } else if (strcmp(kind, "conn") == 0) {
        char sbuf[16] = {0};
        if (httpd_query_key_value(q, "state", sbuf, sizeof(sbuf)) != ESP_OK) {
            return send_400(req, "missing state");
        }
        bool connected;
        if      (strcmp(sbuf, "connected") == 0)    connected = true;
        else if (strcmp(sbuf, "disconnected") == 0) connected = false;
        else return send_400(req, "bad state");

        audio_alert_preview_conn(instance - 1, connected);
    } else {
        return send_400(req, "bad kind");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
