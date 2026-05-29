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
    cJSON_AddStringToObject(root, "goes_region", cfg->goes_region);
    cJSON_AddNumberToObject(root, "goes_update_interval_s", cfg->goes_update_interval_s);
    cJSON_AddNumberToObject(root, "image_display_source", cfg->image_display_source);
    cJSON_AddNumberToObject(root, "moon_bg_style", cfg->moon_bg_style);
    cJSON_AddNumberToObject(root, "moon_lat", cfg->moon_lat);
    cJSON_AddNumberToObject(root, "moon_lon", cfg->moon_lon);

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
    char buf[CONFIG_MAX_PAYLOAD];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return send_400(req, "Empty request body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return send_400(req, "Invalid JSON");
    }

    /* Validate string lengths */
    if (!validate_string_len(root, "goes_region", sizeof(((app_config_t *)0)->goes_region))) {
        cJSON_Delete(root);
        return send_400(req, "goes_region too long");
    }

    app_config_t *cfg = app_config_get();

    JSON_TO_BOOL(root, "image_display_enabled", cfg->image_display_enabled);
    JSON_TO_BOOL(root, "image_display_show_overlay", cfg->image_display_show_overlay);
    JSON_TO_STRING(root, "goes_region", cfg->goes_region);

    cJSON *interval = cJSON_GetObjectItem(root, "goes_update_interval_s");
    if (cJSON_IsNumber(interval)) {
        int v = interval->valueint;
        if (v < 300) v = 300;
        if (v > 7200) v = 7200;
        cfg->goes_update_interval_s = (uint16_t)v;
    }

    cJSON *src = cJSON_GetObjectItem(root, "image_display_source");
    if (cJSON_IsNumber(src)) cfg->image_display_source = (src->valueint == 1) ? 1 : 0;
    cJSON *bg = cJSON_GetObjectItem(root, "moon_bg_style");
    if (cJSON_IsNumber(bg)) { int v = bg->valueint; cfg->moon_bg_style = (v >= 0 && v <= 2) ? (uint8_t)v : 0; }
    cJSON *mlat = cJSON_GetObjectItem(root, "moon_lat");
    if (cJSON_IsNumber(mlat)) cfg->moon_lat = (float)mlat->valuedouble;
    cJSON *mlon = cJSON_GetObjectItem(root, "moon_lon");
    if (cJSON_IsNumber(mlon)) cfg->moon_lon = (float)mlon->valuedouble;

    cJSON_Delete(root);

    app_config_save(cfg);

    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_set_image_display_enabled(cfg->image_display_enabled);
        nina_image_display_set_overlay_visible(cfg->image_display_show_overlay);
        bsp_display_unlock();
    }

    if (cfg->image_display_enabled) {
        goes_ensure_task_running();
        if (goes_task_handle) {
            xTaskNotifyGive(goes_task_handle);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
