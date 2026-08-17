#include "web_server_internal.h"
#include "ui/nina_image_page.h"
#include "ui/nina_dashboard.h"            /* nina_dashboard_get_active_page */
#include "ui/nina_dashboard_internal.h"   /* PAGE_IDX_IS_IMAGE */
#include "ui/nina_nav_arbiter.h"          /* nav_arbiter_notify_topology_changed */
#include "esp_heap_caps.h"
#include <string.h>

/**
 * @brief Is this a legal NWS radar area token?
 *
 * The token is pasted straight into the image URL
 * (https://radar.weather.gov/ridge/standard/<TOKEN>_0.gif), so it is a trust
 * boundary even though the web UI only ever sends a value from its dropdown:
 * anything outside [A-Za-z0-9] could redirect the fetch to another path.
 * An empty token is legal and means "pick the nearest site automatically".
 */
static bool radar_token_is_valid(const char *tok)
{
    for (const char *p = tok; *p != '\0'; p++) {
        bool alnum = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                     (*p >= '0' && *p <= '9');
        if (!alnum) {
            return false;
        }
    }
    return true;
}

/**
 * @brief GET /api/image-display-config -- return the config fields of all five
 *        image pages (GOES, Moon, Solar, Custom URL, Radar).
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

    cJSON_AddBoolToObject(root, "goes_enabled",         cfg->goes_enabled);
    cJSON_AddBoolToObject(root, "moon_enabled",         cfg->moon_enabled);
    cJSON_AddBoolToObject(root, "solar_enabled",        cfg->solar_enabled);
    cJSON_AddBoolToObject(root, "custom_enabled",       cfg->custom_enabled);
    cJSON_AddBoolToObject(root, "radar_enabled",        cfg->radar_enabled);
    cJSON_AddBoolToObject(root, "goes_show_overlay",    cfg->goes_show_overlay);
    cJSON_AddBoolToObject(root, "moon_show_overlay",    cfg->moon_show_overlay);
    cJSON_AddBoolToObject(root, "solar_show_overlay",   cfg->solar_show_overlay);
    cJSON_AddBoolToObject(root, "custom_show_overlay",  cfg->custom_show_overlay);
    cJSON_AddBoolToObject(root, "radar_show_overlay",   cfg->radar_show_overlay);
    cJSON_AddBoolToObject(root, "goes_crop",            cfg->goes_crop);
    cJSON_AddBoolToObject(root, "solar_crop",           cfg->solar_crop);
    cJSON_AddBoolToObject(root, "custom_crop",          cfg->custom_crop);
    cJSON_AddBoolToObject(root, "radar_crop",           cfg->radar_crop);
    /* true = dark basemap, false = the NWS picture as published. */
    cJSON_AddBoolToObject(root, "radar_dark_mode",      cfg->radar_dark_mode);
    cJSON_AddNumberToObject(root, "solar_update_interval_s", cfg->solar_update_interval_s);
    cJSON_AddNumberToObject(root, "moon_update_interval_s",  cfg->moon_update_interval_s);
    cJSON_AddNumberToObject(root, "radar_update_interval_s", cfg->radar_update_interval_s);
    cJSON_AddNumberToObject(root, "radar_frames", cfg->radar_frames);
    /* Empty string = "automatic: nearest site, else the national view". */
    cJSON_AddStringToObject(root, "radar_token", cfg->radar_token);

    cJSON_AddStringToObject(root, "goes_region", cfg->goes_region);
    cJSON_AddNumberToObject(root, "goes_update_interval_s", cfg->goes_update_interval_s);
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

    return send_json_response(req, root);
}

/**
 * @brief POST /api/image-display-config -- update image page config fields.
 *        Any subset of the keys may be posted; each is applied only when
 *        present. Optional "preview" (true) applies live WITHOUT persisting;
 *        without it the values are saved to NVS as before. Optional "source"
 *        (0..3) selects which page's last fetch error to echo back; optional
 *        "force_fetch" re-downloads Custom even when nothing changed (it drives
 *        the live-apply step only, so it works in preview mode too).
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
    if (!validate_string_len(root, "radar_token", sizeof(((app_config_t *)0)->radar_token))) {
        cJSON_Delete(root);
        return send_400(req, "radar_token too long");
    }
    cJSON *radar_tok_item = cJSON_GetObjectItem(root, "radar_token");
    if (cJSON_IsString(radar_tok_item) && !radar_token_is_valid(radar_tok_item->valuestring)) {
        cJSON_Delete(root);
        return send_400(req, "radar area must be letters and digits only");
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
     * app_config_t is ~8.6 KB. Two copies (cur + prev) on the HTTP task stack
     * overflow it (panic/reboot), so both live in PSRAM. */
    app_config_t *cur  = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    /* Capture the full pre-write snapshot so the live-apply step below can
     * tell, after saving, whether a page needs a new image (re-download + wait
     * overlay), a local re-render, or nothing at all. */
    app_config_t *prev = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cur || !prev) {
        heap_caps_free(cur);
        heap_caps_free(prev);
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cur);
    *prev = *cur;

    JSON_TO_BOOL(root, "goes_enabled",        cur->goes_enabled);
    JSON_TO_BOOL(root, "moon_enabled",        cur->moon_enabled);
    JSON_TO_BOOL(root, "solar_enabled",       cur->solar_enabled);
    JSON_TO_BOOL(root, "custom_enabled",      cur->custom_enabled);
    JSON_TO_BOOL(root, "radar_enabled",       cur->radar_enabled);
    JSON_TO_BOOL(root, "goes_show_overlay",   cur->goes_show_overlay);
    JSON_TO_BOOL(root, "moon_show_overlay",   cur->moon_show_overlay);
    JSON_TO_BOOL(root, "solar_show_overlay",  cur->solar_show_overlay);
    JSON_TO_BOOL(root, "custom_show_overlay", cur->custom_show_overlay);
    JSON_TO_BOOL(root, "radar_show_overlay",  cur->radar_show_overlay);
    JSON_TO_BOOL(root, "goes_crop",           cur->goes_crop);
    JSON_TO_BOOL(root, "solar_crop",          cur->solar_crop);
    JSON_TO_BOOL(root, "custom_crop",         cur->custom_crop);
    JSON_TO_BOOL(root, "radar_crop",          cur->radar_crop);
    JSON_TO_BOOL(root, "radar_dark_mode",     cur->radar_dark_mode);
    /* Charset and length checked above; empty string clears it back to auto. */
    JSON_TO_STRING(root, "radar_token", cur->radar_token);
    cJSON *rinterval = cJSON_GetObjectItem(root, "radar_update_interval_s");
    if (cJSON_IsNumber(rinterval)) {
        int v = rinterval->valueint;
        if (v < 120) v = 120;
        if (v > 7200) v = 7200;
        cur->radar_update_interval_s = (uint16_t)v;
    }
    /* How many radar pictures the page keeps and animates. Same 1..10 bound as
     * validate_config() and the SETTINGS_TABLE row; each frame is a retained
     * decoded image, so the upper bound is a memory bound, not a taste one. */
    cJSON *rframes = cJSON_GetObjectItem(root, "radar_frames");
    if (cJSON_IsNumber(rframes)) {
        int v = rframes->valueint;
        if (v < 1) v = 1;
        if (v > 10) v = 10;
        cur->radar_frames = (uint8_t)v;
    }
    cJSON *sinterval = cJSON_GetObjectItem(root, "solar_update_interval_s");
    if (cJSON_IsNumber(sinterval)) {
        int v = sinterval->valueint;
        if (v < 300) v = 300;
        if (v > 7200) v = 7200;
        cur->solar_update_interval_s = (uint16_t)v;
    }
    cJSON *minterval = cJSON_GetObjectItem(root, "moon_update_interval_s");
    if (cJSON_IsNumber(minterval)) {
        int v = minterval->valueint;
        if (v < 10) v = 10;
        if (v > 3600) v = 3600;
        cur->moon_update_interval_s = (uint16_t)v;
    }
    /* Which page's last fetch error to echo back (each web tab posts its own). */
    int echo_src = -1;
    cJSON *src_item = cJSON_GetObjectItem(root, "source");
    if (cJSON_IsNumber(src_item) && src_item->valueint >= 0 && src_item->valueint < IMG_SRC_COUNT) {
        echo_src = src_item->valueint;
    }

    JSON_TO_STRING(root, "goes_region", cur->goes_region);

    cJSON *interval = cJSON_GetObjectItem(root, "goes_update_interval_s");
    if (cJSON_IsNumber(interval)) {
        int v = interval->valueint;
        if (v < 300) v = 300;
        if (v > 7200) v = 7200;
        cur->goes_update_interval_s = (uint16_t)v;
    }

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

    /* Preview: apply live, persist nothing (mirrors json_config_post_handler).
     * The web UI's image tabs always post preview:true -- persisting here wrote
     * the whole config snapshot to NVS, so one image toggle also committed every
     * other tab's unsaved live-applied preview. Those keys are SETTINGS_TABLE
     * rows, so the main /api/config Save persists them instead. Absent/false =>
     * save as before (kept for the Control API and any non-UI client). */
    bool preview = cJSON_IsTrue(cJSON_GetObjectItem(root, "preview"));

    cJSON_Delete(root);

    /* Single atomic memcpy under mutex, + NVS persist unless preview. */
    if (preview) {
        app_config_apply_preview(cur);
    } else {
        app_config_save(cur);
    }

    /* Live apply (preview): create/destroy pages, wake pollers and re-render
     * from the saved snapshot so changes take effect without waiting for a
     * reload. Shared with config_trigger_side_effects and the control registry,
     * so the apply logic lives in one place (nina_image_page.c). */
    image_page_config_apply_live(prev, cur, force_fetch);

    /* An optional page appeared/disappeared: same signal config_trigger_side_effects
     * raises for the other optional pages, so the arbiter re-resolves. */
    if (cur->goes_enabled   != prev->goes_enabled   ||
        cur->moon_enabled   != prev->moon_enabled   ||
        cur->solar_enabled  != prev->solar_enabled  ||
        cur->custom_enabled != prev->custom_enabled ||
        cur->radar_enabled  != prev->radar_enabled) {
        nav_arbiter_notify_topology_changed();
    }

    heap_caps_free(prev);
    heap_caps_free(cur);

    /* Surface the last-known fetch failure reason of the posting tab's page (if
     * any) so the web UI can toast it. error_msg reflects the most recent
     * completed fetch; a refetch triggered above runs asynchronously. */
    char err_copy[sizeof(((image_frame_t *)0)->error_msg)] = {0};
    if (echo_src >= 0) {
        image_page_get_error(image_page_get((image_src_t)echo_src), err_copy, sizeof(err_copy));
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp != NULL) {
        cJSON_AddBoolToObject(resp, "success", true);
        if (err_copy[0] != '\0') {
            cJSON_AddStringToObject(resp, "error_msg", err_copy);
        }
    }
    /* A NULL object or a failed print both mean out of memory -> 500. */
    return send_json_response(req, resp);
}

/**
 * @brief POST /api/image-display/refresh [{"source":0..3}] -- force an immediate
 *        re-fetch (or Moon re-render) of ONE image page with the loading overlay,
 *        without changing any config. With no body or no "source" the currently
 *        visible image page is refreshed (no-op success if none is on screen).
 *        No-op when the chosen page is disabled.
 */
esp_err_t image_display_refresh_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    int src = -1;
    if (req->content_len > 0) {
        cJSON *root = receive_json_body(req, 256);
        if (root == NULL) {
            return ESP_OK;   /* malformed body: error response already sent */
        }
        cJSON *src_item = cJSON_GetObjectItem(root, "source");
        if (cJSON_IsNumber(src_item)) {
            src = src_item->valueint;
            if (src < 0 || src >= IMG_SRC_COUNT) {
                cJSON_Delete(root);
                return send_400(req, "source must be 0 (GOES), 1 (Moon), 2 (Solar), 3 (Custom) or 4 (Radar)");
            }
        }
        cJSON_Delete(root);
    }

    image_page_t *p = NULL;
    if (src >= 0) {
        p = image_page_get((image_src_t)src);
    } else {
        int cur = nina_dashboard_get_active_page();
        if (PAGE_IDX_IS_IMAGE(cur)) p = image_page_by_page_idx(cur);
    }
    char err_copy[sizeof(((image_frame_t *)0)->error_msg)] = {0};
    if (p && image_page_config_enabled(app_config_get(), p->src)) {
        image_page_request_manual_fetch(p);
    }
    if (p) image_page_get_error(p, err_copy, sizeof(err_copy));

    cJSON *resp = cJSON_CreateObject();
    if (resp != NULL) {
        cJSON_AddBoolToObject(resp, "success", true);
        if (err_copy[0] != '\0') {
            cJSON_AddStringToObject(resp, "error_msg", err_copy);
        }
    }
    return send_json_response(req, resp);
}
