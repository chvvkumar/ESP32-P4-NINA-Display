#include "web_server_internal.h"
#include "nina_image_display.h"
#include "ui/nina_dashboard.h"
#include "tasks.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "display_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

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
    cJSON_AddNumberToObject(root, "solar_orientation", cfg->solar_orientation);
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

    /* Work on a mutex-protected snapshot copy; never field-write the live config. */
    app_config_t cfg = app_config_get_snapshot();

    /* Capture the current source/band/region/crop so we can tell, after saving,
     * whether the change needs a new image (source/band/region -> re-download +
     * wait overlay) or only a crop/full toggle (-> local re-render of the cached
     * frame, no download). */
    uint8_t prev_source = cfg.image_display_source;
    uint8_t prev_band   = cfg.solar_band;
    bool    prev_crop   = cfg.image_display_crop;
    uint8_t prev_bg     = cfg.moon_bg_style;
    uint8_t prev_flip_u = cfg.moon_flip_u;
    uint8_t prev_flip_v = cfg.moon_flip_v;
    float   prev_roll   = cfg.moon_roll_offset;
    float   prev_yaw    = cfg.moon_yaw_offset;
    float   prev_pitch  = cfg.moon_pitch_offset;
    uint8_t prev_north_up = cfg.moon_north_up;
    uint8_t prev_spin_mode   = cfg.moon_spin_mode;
    uint8_t prev_spin_return = cfg.moon_spin_return_s;
    uint8_t prev_goes_orient  = cfg.goes_orientation;
    uint8_t prev_solar_orient = cfg.solar_orientation;
    char    prev_region[sizeof(cfg.goes_region)];
    strlcpy(prev_region, cfg.goes_region, sizeof(prev_region));

    JSON_TO_BOOL(root, "image_display_enabled", cfg.image_display_enabled);
    JSON_TO_BOOL(root, "image_display_show_overlay", cfg.image_display_show_overlay);
    JSON_TO_BOOL(root, "image_display_crop", cfg.image_display_crop);
    JSON_TO_STRING(root, "goes_region", cfg.goes_region);

    cJSON *interval = cJSON_GetObjectItem(root, "goes_update_interval_s");
    if (cJSON_IsNumber(interval)) {
        int v = interval->valueint;
        if (v < 300) v = 300;
        if (v > 7200) v = 7200;
        cfg.goes_update_interval_s = (uint16_t)v;
    }

    cJSON *src = cJSON_GetObjectItem(root, "image_display_source");
    if (cJSON_IsNumber(src)) { int v = src->valueint; cfg.image_display_source = (v >= 0 && v <= 2) ? (uint8_t)v : 0; }
    cJSON *bg = cJSON_GetObjectItem(root, "moon_bg_style");
    if (cJSON_IsNumber(bg)) { int v = bg->valueint; cfg.moon_bg_style = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *mlat = cJSON_GetObjectItem(root, "moon_lat");
    if (cJSON_IsNumber(mlat)) cfg.moon_lat = (float)mlat->valuedouble;
    cJSON *mlon = cJSON_GetObjectItem(root, "moon_lon");
    if (cJSON_IsNumber(mlon)) cfg.moon_lon = (float)mlon->valuedouble;
    cJSON *sb = cJSON_GetObjectItem(root, "solar_band");
    if (cJSON_IsNumber(sb)) { int v = sb->valueint; cfg.solar_band = (v >= 0 && v <= 17) ? (uint8_t)v : 0; }
    cJSON *go = cJSON_GetObjectItem(root, "goes_orientation");
    if (cJSON_IsNumber(go)) { int v = go->valueint; cfg.goes_orientation = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *so = cJSON_GetObjectItem(root, "solar_orientation");
    if (cJSON_IsNumber(so)) { int v = so->valueint; cfg.solar_orientation = (v >= 0 && v <= 3) ? (uint8_t)v : 0; }
    cJSON *dlm = cJSON_GetObjectItem(root, "moon_drag_light_mode");
    if (cJSON_IsNumber(dlm)) { int v = dlm->valueint; cfg.moon_drag_light_mode = (v >= 0 && v <= 2) ? (uint8_t)v : 0; }
    cJSON *fu = cJSON_GetObjectItem(root, "moon_flip_u");
    if (cJSON_IsNumber(fu)) { cfg.moon_flip_u = (fu->valueint != 0) ? 1 : 0; }
    cJSON *fv = cJSON_GetObjectItem(root, "moon_flip_v");
    if (cJSON_IsNumber(fv)) { cfg.moon_flip_v = (fv->valueint != 0) ? 1 : 0; }
    cJSON *mro = cJSON_GetObjectItem(root, "moon_roll_offset");
    if (cJSON_IsNumber(mro)) { float v = (float)mro->valuedouble; if (v < -180.0f) v = -180.0f; if (v > 180.0f) v = 180.0f; cfg.moon_roll_offset = v; }
    cJSON *myo = cJSON_GetObjectItem(root, "moon_yaw_offset");
    if (cJSON_IsNumber(myo)) { float v = (float)myo->valuedouble; if (v < -180.0f) v = -180.0f; if (v > 180.0f) v = 180.0f; cfg.moon_yaw_offset = v; }
    cJSON *mpo = cJSON_GetObjectItem(root, "moon_pitch_offset");
    if (cJSON_IsNumber(mpo)) { float v = (float)mpo->valuedouble; if (v < -90.0f) v = -90.0f; if (v > 90.0f) v = 90.0f; cfg.moon_pitch_offset = v; }
    cJSON *mnu = cJSON_GetObjectItem(root, "moon_north_up");
    if (cJSON_IsNumber(mnu)) { cfg.moon_north_up = (mnu->valueint != 0) ? 1 : 0; }
    cJSON *msm = cJSON_GetObjectItem(root, "moon_spin_mode");
    if (cJSON_IsNumber(msm)) { cfg.moon_spin_mode = (msm->valueint != 0) ? 1 : 0; }
    cJSON *msr = cJSON_GetObjectItem(root, "moon_spin_return_s");
    if (cJSON_IsNumber(msr)) { int v = msr->valueint; if (v < 3) v = 3; if (v > 60) v = 60; cfg.moon_spin_return_s = (uint8_t)v; }

    cJSON_Delete(root);

    /* Single atomic memcpy under mutex + NVS persist. */
    app_config_save(&cfg);

    /* Live apply (preview): push display/task state immediately from the saved
     * snapshot so changes take effect without waiting for a reload. */
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_set_image_display_enabled(cfg.image_display_enabled);
        nina_image_display_set_overlay_visible(cfg.image_display_show_overlay);
        bsp_display_unlock();
    }

    if (cfg.image_display_enabled) {
        bool source_band_region_changed =
            cfg.image_display_source != prev_source ||
            cfg.solar_band != prev_band ||
            strcmp(cfg.goes_region, prev_region) != 0;
        bool crop_changed = cfg.image_display_crop != prev_crop;
        bool orient_changed = (cfg.goes_orientation != prev_goes_orient) ||
                              (cfg.solar_orientation != prev_solar_orient);

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
        } else if (cfg.image_display_source == 1 &&
                   (cfg.moon_bg_style != prev_bg ||
                    cfg.moon_flip_u != prev_flip_u ||
                    cfg.moon_flip_v != prev_flip_v ||
                    cfg.moon_roll_offset != prev_roll ||
                    cfg.moon_yaw_offset != prev_yaw ||
                    cfg.moon_pitch_offset != prev_pitch ||
                    cfg.moon_north_up != prev_north_up ||
                    cfg.moon_spin_mode != prev_spin_mode ||
                    cfg.moon_spin_return_s != prev_spin_return)) {
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

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
