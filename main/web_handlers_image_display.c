#include "web_server_internal.h"
#include "nina_image_display.h"
#include "ui/nina_dashboard.h"
#include "tasks.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "display_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>

/**
 * @brief Apply Image Display config changes live (preview), comparing prev to cur.
 *
 * Extracted from image_display_config_post_handler so other code paths (e.g. the
 * control registry) can apply image-display changes identically. The branching
 * and side-effects mirror the original handler exactly:
 *   - enable/overlay state pushed to the dashboard under the display lock;
 *   - source/band/region (or custom-URL while Custom is active, or force_fetch on
 *     Custom) -> manual-fetch flag + wake goes_poll_task (re-download + overlay);
 *   - Moon-only change -> wake goes_poll_task (local re-render, no overlay);
 *   - crop/orientation only -> local re-render under the display lock.
 *
 * @param prev        Full config snapshot taken BEFORE the field writes.
 * @param cur         Saved config snapshot (post-write).
 * @param force_fetch Force a re-fetch for the Custom source even if no field changed.
 */
void image_display_apply_live(const app_config_t *prev, const app_config_t *cur, bool force_fetch)
{
    /* Live apply (preview): push display/task state immediately from the saved
     * snapshot so changes take effect without waiting for a reload. */
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_set_image_display_enabled(cur->image_display_enabled);
        nina_image_display_set_overlay_visible(cur->image_display_show_overlay);
        bsp_display_unlock();
    }

    if (cur->image_display_enabled) {
        bool source_band_region_changed =
            cur->image_display_source != prev->image_display_source ||
            cur->solar_band != prev->solar_band ||
            strcmp(cur->goes_region, prev->goes_region) != 0 ||
            (cur->image_display_source == 3 &&
             strcmp(cur->custom_image_url, prev->custom_image_url) != 0) ||
            (force_fetch && cur->image_display_source == 3);
        bool crop_changed = cur->image_display_crop != prev->image_display_crop;
        bool orient_changed = (cur->goes_orientation != prev->goes_orientation) ||
                              (cur->solar_orientation != prev->solar_orientation) ||
                              (cur->custom_orientation != prev->custom_orientation) ||
                              (cur->goes_vflip   != prev->goes_vflip) ||
                              (cur->goes_hflip   != prev->goes_hflip) ||
                              (cur->solar_vflip  != prev->solar_vflip) ||
                              (cur->solar_hflip  != prev->solar_hflip) ||
                              (cur->custom_vflip != prev->custom_vflip) ||
                              (cur->custom_hflip != prev->custom_hflip);

        if (source_band_region_changed) {
            /* Source/band/region needs a genuinely new image: flag the next
             * fetch as manual so goes_poll_task shows the wait overlay during
             * the re-download/decode, then wake the task. If crop also changed,
             * the re-download path re-applies the new crop, so no separate local
             * re-render is needed. */
            atomic_store(&image_display_manual_fetch, true);
            goes_ensure_task_running();
            if (goes_task_handle) {
                xTaskNotifyGive(goes_task_handle);
            }
        } else if (cur->image_display_source == 1 &&
                   (cur->moon_bg_style != prev->moon_bg_style ||
                    cur->moon_flip_u != prev->moon_flip_u ||
                    cur->moon_flip_v != prev->moon_flip_v ||
                    cur->moon_roll_offset != prev->moon_roll_offset ||
                    cur->moon_yaw_offset != prev->moon_yaw_offset ||
                    cur->moon_pitch_offset != prev->moon_pitch_offset ||
                    cur->moon_north_up != prev->moon_north_up ||
                    cur->moon_spin_mode != prev->moon_spin_mode ||
                    cur->moon_spin_return_s != prev->moon_spin_return_s)) {
            /* Moon background style changed only (source unchanged, so the
             * source_band_region_changed branch above did not fire): wake the
             * image-display task so its Moon branch re-renders moon_render() with
             * the new cfg->moon_bg_style and commits it via goes_set_image() /
             * nina_image_display_update(). The Moon branch (image_display_source
             * == 1 in goes_poll_task) is purely local — it renders, commits, and
             * `continue`s before ever reaching the manual-fetch / wait-overlay
             * block, so it never shows the "Loading image..." overlay and never
             * consumes image_display_manual_fetch. A bare task notify is therefore
             * sufficient: do NOT set image_display_manual_fetch here, since the
             * Moon branch would not clear it and it could leak a spurious overlay
             * onto a later GOES/Solar fetch. A background-only change leaves moon
             * illumination/orientation unchanged, so moon_anim_request stays clear
             * and only a single still re-render occurs. This branch is mutually
             * exclusive with the source-change branch above (that one runs only
             * when source/band/region changed), so the task is woken at most once
             * per request. */
            goes_ensure_task_running();
            if (goes_task_handle) {
                xTaskNotifyGive(goes_task_handle);
            }
        } else if (crop_changed || orient_changed) {
            /* Crop/full toggle or orientation change only: re-render the
             * already-decoded frame locally
             * instead of re-downloading. nina_image_display_update() locks
             * goes_data internally but REQUIRES the display lock held by the
             * caller (see tasks.c), so wrap both calls in bsp_display_lock. The
             * force-redraw gate is a safe no-op if no image is cached or the page
             * is not on the image path. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_image_display_force_redraw();
                nina_image_display_update(&goes_data);
                bsp_display_unlock();
            }
        }
    }
}

/**
 * @brief GET /api/image-display-config -- return Image Display config fields.
 */
esp_err_t image_display_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root, "image_display_enabled", cfg->image_display_enabled);
    cJSON_AddBoolToObject(root, "image_display_show_overlay", cfg->image_display_show_overlay);
    cJSON_AddBoolToObject(root, "image_display_crop", cfg->image_display_crop);
    cJSON_AddStringToObject(root, "goes_region", cfg->goes_region);
    cJSON_AddNumberToObject(root, "goes_update_interval_s", cfg->goes_update_interval_s);
    cJSON_AddNumberToObject(root, "image_display_source", cfg->image_display_source);
    cJSON_AddNumberToObject(root, "moon_bg_style", cfg->moon_bg_style);
    cJSON_AddNumberToObject(root, "moon_lat", cfg->moon_lat);
    cJSON_AddNumberToObject(root, "moon_lon", cfg->moon_lon);
    cJSON_AddNumberToObject(root, "solar_band", cfg->solar_band);
    cJSON_AddNumberToObject(root, "goes_orientation", cfg->goes_orientation);
    cJSON_AddNumberToObject(root, "goes_vflip", cfg->goes_vflip);
    cJSON_AddNumberToObject(root, "goes_hflip", cfg->goes_hflip);
    cJSON_AddNumberToObject(root, "solar_orientation", cfg->solar_orientation);
    cJSON_AddNumberToObject(root, "solar_vflip", cfg->solar_vflip);
    cJSON_AddNumberToObject(root, "solar_hflip", cfg->solar_hflip);
    cJSON_AddStringToObject(root, "custom_image_url", cfg->custom_image_url);
    cJSON_AddNumberToObject(root, "custom_orientation", cfg->custom_orientation);
    cJSON_AddNumberToObject(root, "custom_vflip", cfg->custom_vflip);
    cJSON_AddNumberToObject(root, "custom_hflip", cfg->custom_hflip);
    cJSON_AddNumberToObject(root, "custom_update_interval_s", cfg->custom_update_interval_s);
    cJSON_AddNumberToObject(root, "moon_drag_light_mode", cfg->moon_drag_light_mode);
    cJSON_AddNumberToObject(root, "moon_flip_u", cfg->moon_flip_u);
    cJSON_AddNumberToObject(root, "moon_flip_v", cfg->moon_flip_v);
    cJSON_AddNumberToObject(root, "moon_roll_offset", cfg->moon_roll_offset);
    cJSON_AddNumberToObject(root, "moon_yaw_offset", cfg->moon_yaw_offset);
    cJSON_AddNumberToObject(root, "moon_pitch_offset", cfg->moon_pitch_offset);
    cJSON_AddNumberToObject(root, "moon_north_up", cfg->moon_north_up);
    cJSON_AddNumberToObject(root, "moon_spin_mode", cfg->moon_spin_mode);
    cJSON_AddNumberToObject(root, "moon_spin_return_s", cfg->moon_spin_return_s);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief POST /api/image-display-config -- update Image Display config fields and save to NVS.
 */
esp_err_t image_display_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = receive_json_body(req, CONFIG_MAX_PAYLOAD);
    if (root == NULL) {
        return ESP_OK;  /* error response already sent */
    }

    /* Validate string lengths */
    if (!validate_string_len(root, "goes_region", sizeof(((app_config_t *)0)->goes_region))) {
        cJSON_Delete(root);
        return send_400(req, "goes_region too long");
    }
    if (!validate_string_len(root, "custom_image_url", sizeof(((app_config_t *)0)->custom_image_url))) {
        cJSON_Delete(root);
        return send_400(req, "custom_image_url too long");
    }
    /* A custom URL, when present, must be an http(s) URL. validate_url_format
     * also accepts mqtt(s) schemes, so additionally require http/https here.
     * An empty string is allowed (clears the URL / "not configured" state). */
    cJSON *custom_url_item = cJSON_GetObjectItem(root, "custom_image_url");
    if (cJSON_IsString(custom_url_item) && custom_url_item->valuestring[0] != '\0') {
        const char *u = custom_url_item->valuestring;
        bool http_scheme = (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0);
        if (!http_scheme || !validate_url_format(u)) {
            cJSON_Delete(root);
            return send_400(req, "custom_image_url must start with http:// or https://");
        }
    }

    /* Work on a mutex-protected snapshot copy; never field-write the live config.
     *
     * app_config_t is ~7.5 KB. Two copies (cfg + prev) on the HTTP task stack
     * overflow it (panic/reboot), so both live in PSRAM. The by-value snapshot
     * return uses an sret hidden pointer and writes straight into the
     * dereferenced destination, so no large stack temporary is created. */
    app_config_t *cur  = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    /* Capture the full pre-write snapshot so image_display_apply_live() can tell,
     * after saving, whether the change needs a new image (source/band/region ->
     * re-download + wait overlay), a Moon-only re-render, or only a crop/orient
     * toggle (-> local re-render of the cached frame, no download). */
    app_config_t *prev = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cur || !prev) {
        heap_caps_free(cur);
        heap_caps_free(prev);
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    *cur = app_config_get_snapshot();
    *prev = *cur;

    JSON_TO_BOOL(root, "image_display_enabled", cur->image_display_enabled);
    JSON_TO_BOOL(root, "image_display_show_overlay", cur->image_display_show_overlay);
    JSON_TO_BOOL(root, "image_display_crop", cur->image_display_crop);
    JSON_TO_STRING(root, "goes_region", cur->goes_region);

    cJSON *interval = cJSON_GetObjectItem(root, "goes_update_interval_s");
    if (cJSON_IsNumber(interval)) {
        int v = interval->valueint;
        if (v < 300) v = 300;
        if (v > 7200) v = 7200;
        cur->goes_update_interval_s = (uint16_t)v;
    }

    cJSON *src = cJSON_GetObjectItem(root, "image_display_source");
    if (cJSON_IsNumber(src)) { int v = src->valueint; cur->image_display_source = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *bg = cJSON_GetObjectItem(root, "moon_bg_style");
    if (cJSON_IsNumber(bg)) { int v = bg->valueint; cur->moon_bg_style = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *mlat = cJSON_GetObjectItem(root, "moon_lat");
    if (cJSON_IsNumber(mlat)) cur->moon_lat = (float)mlat->valuedouble;
    cJSON *mlon = cJSON_GetObjectItem(root, "moon_lon");
    if (cJSON_IsNumber(mlon)) cur->moon_lon = (float)mlon->valuedouble;
    cJSON *sb = cJSON_GetObjectItem(root, "solar_band");
    if (cJSON_IsNumber(sb)) { int v = sb->valueint; cur->solar_band = (v >= 0 && v <= 17) ? (uint8_t)v : 0; }
    cJSON *go = cJSON_GetObjectItem(root, "goes_orientation");
    if (cJSON_IsNumber(go)) { int v = go->valueint; cur->goes_orientation = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *gvf = cJSON_GetObjectItem(root, "goes_vflip");
    if (cJSON_IsNumber(gvf)) { cur->goes_vflip = (gvf->valueint != 0) ? 1 : 0; }
    cJSON *ghf = cJSON_GetObjectItem(root, "goes_hflip");
    if (cJSON_IsNumber(ghf)) { cur->goes_hflip = (ghf->valueint != 0) ? 1 : 0; }
    cJSON *so = cJSON_GetObjectItem(root, "solar_orientation");
    if (cJSON_IsNumber(so)) { int v = so->valueint; cur->solar_orientation = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *svf = cJSON_GetObjectItem(root, "solar_vflip");
    if (cJSON_IsNumber(svf)) { cur->solar_vflip = (svf->valueint != 0) ? 1 : 0; }
    cJSON *shf = cJSON_GetObjectItem(root, "solar_hflip");
    if (cJSON_IsNumber(shf)) { cur->solar_hflip = (shf->valueint != 0) ? 1 : 0; }
    /* Custom image URL: length + scheme already validated above; copy bounded
     * into the 256-byte field. */
    JSON_TO_STRING(root, "custom_image_url", cur->custom_image_url);
    cJSON *co = cJSON_GetObjectItem(root, "custom_orientation");
    if (cJSON_IsNumber(co)) { int v = co->valueint; cur->custom_orientation = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *cvf = cJSON_GetObjectItem(root, "custom_vflip");
    if (cJSON_IsNumber(cvf)) { cur->custom_vflip = (cvf->valueint != 0) ? 1 : 0; }
    cJSON *chf = cJSON_GetObjectItem(root, "custom_hflip");
    if (cJSON_IsNumber(chf)) { cur->custom_hflip = (chf->valueint != 0) ? 1 : 0; }
    cJSON *ci = cJSON_GetObjectItem(root, "custom_update_interval_s");
    if (cJSON_IsNumber(ci)) { int v = ci->valueint; if (v < 10) v = 10; if (v > 7200) v = 7200; cur->custom_update_interval_s = (uint16_t)v; }
    cJSON *dlm = cJSON_GetObjectItem(root, "moon_drag_light_mode");
    if (cJSON_IsNumber(dlm)) { int v = dlm->valueint; cur->moon_drag_light_mode = (v >= 0 && v <= 2) ? (uint8_t)v : 0; }
    cJSON *fu = cJSON_GetObjectItem(root, "moon_flip_u");
    if (cJSON_IsNumber(fu)) { cur->moon_flip_u = (fu->valueint != 0) ? 1 : 0; }
    cJSON *fv = cJSON_GetObjectItem(root, "moon_flip_v");
    if (cJSON_IsNumber(fv)) { cur->moon_flip_v = (fv->valueint != 0) ? 1 : 0; }
    cJSON *mro = cJSON_GetObjectItem(root, "moon_roll_offset");
    if (cJSON_IsNumber(mro)) { float v = (float)mro->valuedouble; if (v < -180.0f) v = -180.0f; if (v > 180.0f) v = 180.0f; cur->moon_roll_offset = v; }
    cJSON *myo = cJSON_GetObjectItem(root, "moon_yaw_offset");
    if (cJSON_IsNumber(myo)) { float v = (float)myo->valuedouble; if (v < -180.0f) v = -180.0f; if (v > 180.0f) v = 180.0f; cur->moon_yaw_offset = v; }
    cJSON *mpo = cJSON_GetObjectItem(root, "moon_pitch_offset");
    if (cJSON_IsNumber(mpo)) { float v = (float)mpo->valuedouble; if (v < -90.0f) v = -90.0f; if (v > 90.0f) v = 90.0f; cur->moon_pitch_offset = v; }
    cJSON *mnu = cJSON_GetObjectItem(root, "moon_north_up");
    if (cJSON_IsNumber(mnu)) { cur->moon_north_up = (mnu->valueint != 0) ? 1 : 0; }
    cJSON *msm = cJSON_GetObjectItem(root, "moon_spin_mode");
    if (cJSON_IsNumber(msm)) { cur->moon_spin_mode = (msm->valueint != 0) ? 1 : 0; }
    cJSON *msr = cJSON_GetObjectItem(root, "moon_spin_return_s");
    if (cJSON_IsNumber(msr)) { int v = msr->valueint; if (v < 3) v = 3; if (v > 60) v = 60; cur->moon_spin_return_s = (uint8_t)v; }

    /* Preview button: force an immediate re-fetch even when no field changed
     * (re-clicking Preview with the same URL must still refresh the device). */
    bool force_fetch = false;
    cJSON *ff = cJSON_GetObjectItem(root, "force_fetch");
    if (cJSON_IsBool(ff)) { force_fetch = cJSON_IsTrue(ff); }

    cJSON_Delete(root);

    /* Single atomic memcpy under mutex + NVS persist. */
    app_config_save(cur);

    /* Live apply (preview): push display/task state immediately from the saved
     * snapshot so changes take effect without waiting for a reload. Shared with
     * any other code path (e.g. the control registry) that mutates image-display
     * config, so the apply logic lives in one place. */
    image_display_apply_live(prev, cur, force_fetch);

    heap_caps_free(prev);
    heap_caps_free(cur);

    /* Surface the last-known fetch failure reason (if any) so the web UI can
     * toast it. error_msg reflects the most recent completed fetch; a refetch
     * triggered above runs asynchronously on the poll task. */
    char err_copy[sizeof(((goes_data_t *)0)->error_msg)] = {0};
    if (goes_data_lock(&goes_data, 200)) {
        strlcpy(err_copy, goes_data.error_msg, sizeof(err_copy));
        goes_data_unlock(&goes_data);
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(resp, "success", true);
    if (err_copy[0] != '\0') {
        cJSON_AddStringToObject(resp, "error_msg", err_copy);
    }
    const char *resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    if (resp_str != NULL) {
        httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
        free((void *)resp_str);
    } else {
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

/**
 * @brief POST /api/image-display/refresh -- force an immediate re-fetch of the
 *        currently configured image source (GOES/Moon/Solar/Custom), regardless
 *        of which source is active and without changing any config.
 *
 * Unlike the config POST handler's force_fetch (which only re-fetches the Custom
 * source), this flags the next fetch as manual so goes_poll_task shows the wait
 * overlay during the re-download/decode, then wakes the task for any source. The
 * Moon source's local re-render path also responds to the task notify. No-op when
 * the Image Display page is disabled.
 */
esp_err_t image_display_refresh_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    app_config_t cfg = app_config_get_snapshot();

    if (cfg.image_display_enabled) {
        atomic_store(&image_display_manual_fetch, true);
        goes_ensure_task_running();
        if (goes_task_handle) {
            xTaskNotifyGive(goes_task_handle);
        }
    }

    /* Surface the last-known fetch failure reason (if any) so the web UI can
     * toast it, mirroring the config POST handler. error_msg reflects the most
     * recent completed fetch; the refetch triggered above runs asynchronously. */
    char err_copy[sizeof(((goes_data_t *)0)->error_msg)] = {0};
    if (goes_data_lock(&goes_data, 200)) {
        strlcpy(err_copy, goes_data.error_msg, sizeof(err_copy));
        goes_data_unlock(&goes_data);
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(resp, "success", true);
    if (err_copy[0] != '\0') {
        cJSON_AddStringToObject(resp, "error_msg", err_copy);
    }
    const char *resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    if (resp_str != NULL) {
        httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
        free((void *)resp_str);
    } else {
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    }
    cJSON_Delete(resp);
    return ESP_OK;
}
