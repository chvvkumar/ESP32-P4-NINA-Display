#include "web_server.h"
#include "app_config.h"
#include "mqtt_ha.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui/nina_dashboard.h"
#include "ui/themes.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/others/snapshot/lv_snapshot.h"

static const char *TAG = "web_server";

extern const uint8_t config_html_start[] asm("_binary_config_ui_html_start");
extern const uint8_t config_html_end[]   asm("_binary_config_ui_html_end");

#define JSON_TO_STRING(root, key, dest) do { \
    cJSON *_item = cJSON_GetObjectItem(root, key); \
    if (cJSON_IsString(_item)) { \
        strncpy(dest, _item->valuestring, sizeof(dest) - 1); \
        dest[sizeof(dest) - 1] = '\0'; \
    } \
} while (0)

#define JSON_TO_INT(root, key, dest) do { \
    cJSON *_item = cJSON_GetObjectItem(root, key); \
    if (cJSON_IsNumber(_item)) { \
        dest = _item->valueint; \
    } \
} while (0)

#define JSON_TO_BOOL(root, key, dest) do { \
    cJSON *_item = cJSON_GetObjectItem(root, key); \
    if (cJSON_IsBool(_item)) { \
        dest = cJSON_IsTrue(_item); \
    } \
} while (0)

// Handler for root URL
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)config_html_start,
                    config_html_end - config_html_start);
    return ESP_OK;
}

// Handler for getting config
static esp_err_t config_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "pass", cfg->wifi_pass);
    cJSON_AddStringToObject(root, "url1", cfg->api_url[0]);
    cJSON_AddStringToObject(root, "url2", cfg->api_url[1]);
    cJSON_AddStringToObject(root, "url3", cfg->api_url[2]);
    cJSON_AddStringToObject(root, "ntp", cfg->ntp_server);
    cJSON_AddStringToObject(root, "filter_colors_1", cfg->filter_colors[0]);
    cJSON_AddStringToObject(root, "filter_colors_2", cfg->filter_colors[1]);
    cJSON_AddStringToObject(root, "filter_colors_3", cfg->filter_colors[2]);
    cJSON_AddStringToObject(root, "rms_thresholds_1", cfg->rms_thresholds[0]);
    cJSON_AddStringToObject(root, "rms_thresholds_2", cfg->rms_thresholds[1]);
    cJSON_AddStringToObject(root, "rms_thresholds_3", cfg->rms_thresholds[2]);
    cJSON_AddStringToObject(root, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
    cJSON_AddStringToObject(root, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
    cJSON_AddStringToObject(root, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
    cJSON_AddNumberToObject(root, "theme_index", cfg->theme_index);
    cJSON_AddNumberToObject(root, "brightness", cfg->brightness);
    cJSON_AddNumberToObject(root, "color_brightness", cfg->color_brightness);
    cJSON_AddBoolToObject(root, "mqtt_enabled", cfg->mqtt_enabled);
    cJSON_AddStringToObject(root, "mqtt_broker_url", cfg->mqtt_broker_url);
    cJSON_AddNumberToObject(root, "mqtt_port", cfg->mqtt_port);
    cJSON_AddStringToObject(root, "mqtt_username", cfg->mqtt_username);
    cJSON_AddStringToObject(root, "mqtt_password", cfg->mqtt_password);
    cJSON_AddStringToObject(root, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);
    cJSON_AddNumberToObject(root, "active_page_override", cfg->active_page_override);
    cJSON_AddNumberToObject(root, "auto_rotate_interval_s", cfg->auto_rotate_interval_s);
    cJSON_AddNumberToObject(root, "auto_rotate_effect", cfg->auto_rotate_effect);
    cJSON_AddBoolToObject(root, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    
    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Handler for saving config
static esp_err_t config_post_handler(httpd_req_t *req)
{
    #define POST_BUF_SIZE 5120
    int remaining = req->content_len;

    if (remaining >= POST_BUF_SIZE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = malloc(POST_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    app_config_t *cfg = malloc(sizeof(app_config_t));
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    // Load current to preserve anything not sent
    memcpy(cfg, app_config_get(), sizeof(app_config_t));

    JSON_TO_STRING(root, "ssid",           cfg->wifi_ssid);
    JSON_TO_STRING(root, "pass",           cfg->wifi_pass);
    JSON_TO_STRING(root, "url1",           cfg->api_url[0]);
    JSON_TO_STRING(root, "url2",           cfg->api_url[1]);
    JSON_TO_STRING(root, "url3",           cfg->api_url[2]);
    JSON_TO_STRING(root, "ntp",            cfg->ntp_server);
    JSON_TO_INT   (root, "theme_index",    cfg->theme_index);
    JSON_TO_STRING(root, "filter_colors_1", cfg->filter_colors[0]);
    JSON_TO_STRING(root, "filter_colors_2", cfg->filter_colors[1]);
    JSON_TO_STRING(root, "filter_colors_3", cfg->filter_colors[2]);
    JSON_TO_STRING(root, "rms_thresholds_1", cfg->rms_thresholds[0]);
    JSON_TO_STRING(root, "rms_thresholds_2", cfg->rms_thresholds[1]);
    JSON_TO_STRING(root, "rms_thresholds_3", cfg->rms_thresholds[2]);
    JSON_TO_STRING(root, "hfr_thresholds_1", cfg->hfr_thresholds[0]);
    JSON_TO_STRING(root, "hfr_thresholds_2", cfg->hfr_thresholds[1]);
    JSON_TO_STRING(root, "hfr_thresholds_3", cfg->hfr_thresholds[2]);
    JSON_TO_BOOL  (root, "mqtt_enabled",   cfg->mqtt_enabled);
    JSON_TO_STRING(root, "mqtt_broker_url", cfg->mqtt_broker_url);
    JSON_TO_STRING(root, "mqtt_username",  cfg->mqtt_username);
    JSON_TO_STRING(root, "mqtt_password",  cfg->mqtt_password);
    JSON_TO_STRING(root, "mqtt_topic_prefix", cfg->mqtt_topic_prefix);

    // Clamped int fields
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        int val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->brightness = val;
    }

    cJSON *color_brightness = cJSON_GetObjectItem(root, "color_brightness");
    if (cJSON_IsNumber(color_brightness)) {
        int val = color_brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->color_brightness = val;
    }

    cJSON *mqtt_port = cJSON_GetObjectItem(root, "mqtt_port");
    if (cJSON_IsNumber(mqtt_port)) {
        cfg->mqtt_port = (uint16_t)mqtt_port->valueint;
    }

    JSON_TO_BOOL(root, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);

    cJSON *apo_item = cJSON_GetObjectItem(root, "active_page_override");
    if (cJSON_IsNumber(apo_item)) {
        int v = apo_item->valueint;
        if (v < -1) v = -1;
        if (v >= MAX_NINA_INSTANCES) v = MAX_NINA_INSTANCES - 1;
        cfg->active_page_override = (int8_t)v;
    }

    cJSON *ari_item = cJSON_GetObjectItem(root, "auto_rotate_interval_s");
    if (cJSON_IsNumber(ari_item)) {
        int v = ari_item->valueint;
        if (v < 0) v = 0;
        if (v > 3600) v = 3600;
        cfg->auto_rotate_interval_s = (uint16_t)v;
    }

    cJSON *are_item = cJSON_GetObjectItem(root, "auto_rotate_effect");
    if (cJSON_IsNumber(are_item)) {
        int v = are_item->valueint;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        cfg->auto_rotate_effect = (uint8_t)v;
    }

    app_config_save(cfg);
    free(cfg);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config saved to NVS");

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live brightness adjustment (no reboot needed)
static esp_err_t brightness_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(val)) {
        int brightness = val->valueint;
        if (brightness < 0) brightness = 0;
        if (brightness > 100) brightness = 100;
        bsp_display_brightness_set(brightness);
        ESP_LOGI(TAG, "Brightness set to %d%%", brightness);
        mqtt_ha_publish_state();
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live color brightness adjustment (no reboot needed)
static esp_err_t color_brightness_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "color_brightness");
    if (cJSON_IsNumber(val)) {
        int cb = val->valueint;
        if (cb < 0) cb = 0;
        if (cb > 100) cb = 100;
        app_config_get()->color_brightness = cb;
        
        // Re-apply theme to update static text brightness
        bsp_display_lock(0);
        nina_dashboard_apply_theme(app_config_get()->theme_index);
        bsp_display_unlock();
        
        ESP_LOGI(TAG, "Color brightness set to %d%%", cb);
        mqtt_ha_publish_state();
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for live theme switching (no reboot needed)
static esp_err_t theme_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "theme_index");
    if (cJSON_IsNumber(val)) {
        int idx = val->valueint;
        if (idx < 0) idx = 0;
        if (idx >= themes_get_count()) idx = themes_get_count() - 1;
        app_config_get()->theme_index = idx;
        bsp_display_lock(0);
        nina_dashboard_apply_theme(idx);
        bsp_display_unlock();
        ESP_LOGI(TAG, "Theme set to %d", idx);
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for reboot
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "Rebooting...", HTTPD_RESP_USE_STRLEN);
    // Delay slightly to let the response go out
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

// Handler for factory reset
static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested via web interface");
    httpd_resp_send(req, "Factory reset initiated...", HTTPD_RESP_USE_STRLEN);

    // Delay slightly to let the response go out
    vTaskDelay(pdMS_TO_TICKS(100));

    // Perform factory reset
    app_config_factory_reset();

    // Reboot the device
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// Handler for live page switching (saves override and switches immediately)
static esp_err_t page_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *val = cJSON_GetObjectItem(root, "page");
    if (cJSON_IsNumber(val)) {
        int page = val->valueint;
        int cnt = app_config_get_instance_count();
        app_config_t *cfg = app_config_get();

        if (page >= 0 && page < cnt) {
            cfg->active_page_override = (int8_t)page;
            app_config_save(cfg);
            bsp_display_lock(0);
            nina_dashboard_show_page(page + 1, cnt);  /* +1: NINA pages start at index 1 (0 = summary) */
            bsp_display_unlock();
            ESP_LOGI(TAG, "Page switched to %d via web, override saved", page);
        } else if (page == -1) {
            cfg->active_page_override = -1;
            app_config_save(cfg);
            ESP_LOGI(TAG, "Page override cleared via web");
        }
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// BMP file header for 720x720 RGB565 image (bottom-up, no compression)
#define SCREENSHOT_W    BSP_LCD_H_RES
#define SCREENSHOT_H    BSP_LCD_V_RES
#define BMP_HEADER_SIZE 66  // 14 (file header) + 40 (DIB header) + 12 (RGB565 masks)

static void build_bmp_header(uint8_t *hdr, uint32_t width, uint32_t height)
{
    uint32_t row_size = width * 2;  // RGB565: 2 bytes per pixel
    uint32_t img_size = row_size * height;
    uint32_t file_size = BMP_HEADER_SIZE + img_size;

    memset(hdr, 0, BMP_HEADER_SIZE);

    // BMP file header (14 bytes)
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &file_size, 4);
    // reserved (4 bytes) = 0
    uint32_t offset = BMP_HEADER_SIZE;
    memcpy(&hdr[10], &offset, 4);

    // DIB header (BITMAPINFOHEADER = 40 bytes)
    uint32_t dib_size = 40;
    memcpy(&hdr[14], &dib_size, 4);
    int32_t w = (int32_t)width;
    int32_t h = -(int32_t)height;  // negative = top-down (no row reversal needed)
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    uint16_t planes = 1;
    memcpy(&hdr[26], &planes, 2);
    uint16_t bpp = 16;
    memcpy(&hdr[28], &bpp, 2);
    uint32_t compression = 3;  // BI_BITFIELDS
    memcpy(&hdr[30], &compression, 4);
    memcpy(&hdr[34], &img_size, 4);
    // pixels per meter (can be 0)
    // colors used, important (0)

    // RGB565 bitmasks (12 bytes after DIB header)
    uint32_t mask_r = 0xF800;
    uint32_t mask_g = 0x07E0;
    uint32_t mask_b = 0x001F;
    memcpy(&hdr[54], &mask_r, 4);
    memcpy(&hdr[58], &mask_g, 4);
    memcpy(&hdr[62], &mask_b, 4);
}

// Handler for screenshot capture - serves a BMP image viewable in any browser
static esp_err_t screenshot_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Screenshot requested");

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
    uint32_t row_size = width * 2;  // RGB565 packed row (no padding)

    ESP_LOGI(TAG, "Snapshot captured: %lux%lu, stride=%lu", width, height, stride);

    // Build BMP header
    uint8_t bmp_hdr[BMP_HEADER_SIZE];
    build_bmp_header(bmp_hdr, width, height);

    // Send as BMP image
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.bmp\"");

    // Send BMP header
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)bmp_hdr, BMP_HEADER_SIZE);
    if (err != ESP_OK) {
        lv_draw_buf_destroy(snapshot);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_FAIL;
    }

    // Send pixel rows - handle stride != row_size (LVGL may add padding per row)
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = snapshot->data + (y * stride);
        err = httpd_resp_send_chunk(req, (const char *)row, row_size);
        if (err != ESP_OK) {
            break;
        }
    }

    // End chunked response
    httpd_resp_send_chunk(req, NULL, 0);
    lv_draw_buf_destroy(snapshot);

    ESP_LOGI(TAG, "Screenshot sent successfully");
    return ESP_OK;
}

void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 13;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_get_config = {
            .uri       = "/api/config",
            .method    = HTTP_GET,
            .handler   = config_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_get_config);

        httpd_uri_t uri_post_config = {
            .uri       = "/api/config",
            .method    = HTTP_POST,
            .handler   = config_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_config);

        httpd_uri_t uri_post_brightness = {
            .uri       = "/api/brightness",
            .method    = HTTP_POST,
            .handler   = brightness_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_brightness);

        httpd_uri_t uri_post_color_brightness = {
            .uri       = "/api/color-brightness",
            .method    = HTTP_POST,
            .handler   = color_brightness_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_color_brightness);

        httpd_uri_t uri_post_theme = {
            .uri       = "/api/theme",
            .method    = HTTP_POST,
            .handler   = theme_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_theme);

        httpd_uri_t uri_post_reboot = {
            .uri       = "/api/reboot",
            .method    = HTTP_POST,
            .handler   = reboot_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_reboot);

        httpd_uri_t uri_post_factory_reset = {
            .uri       = "/api/factory-reset",
            .method    = HTTP_POST,
            .handler   = factory_reset_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_factory_reset);

        httpd_uri_t uri_get_screenshot = {
            .uri       = "/api/screenshot",
            .method    = HTTP_GET,
            .handler   = screenshot_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_get_screenshot);

        httpd_uri_t uri_post_page = {
            .uri       = "/api/page",
            .method    = HTTP_POST,
            .handler   = page_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post_page);

        ESP_LOGI(TAG, "Web server started");
    } else {
        ESP_LOGE(TAG, "Error starting server!");
    }
}