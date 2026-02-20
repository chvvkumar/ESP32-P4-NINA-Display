#include "web_server.h"
#include "app_config.h"
#include "mqtt_ha.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui/nina_dashboard.h"
#include "ui/themes.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/others/snapshot/lv_snapshot.h"
#include "driver/jpeg_encode.h"

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

/* Maximum accepted POST payload size for config endpoints */
#define CONFIG_MAX_PAYLOAD 4096

/**
 * @brief Send an HTTP 400 response with a JSON error message.
 */
static esp_err_t send_400(httpd_req_t *req, const char *message) {
    ESP_LOGW(TAG, "Config rejected: %s", message);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;  /* response was sent; ESP_OK tells httpd we handled it */
}

/**
 * @brief Validate a string field won't overflow its destination buffer.
 * @return true if valid, false if too long.
 */
static bool validate_string_len(cJSON *root, const char *key, size_t max_len) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item)) return true;  /* absent or non-string — OK, will be skipped */
    return strlen(item->valuestring) < max_len;
}

/**
 * @brief Validate that a URL field looks like a plausible URL (starts with a scheme).
 */
static bool validate_url_format(const char *url) {
    if (url[0] == '\0') return true;  /* empty is allowed (means "not configured") */
    return (strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "https://", 8) == 0 ||
            strncmp(url, "mqtt://", 7) == 0 ||
            strncmp(url, "mqtts://", 8) == 0);
}

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

    /* WiFi SSID: read from the WiFi stack (not stored in app_config).
     * Password is never exposed via the API. */
    wifi_config_t sta_cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", (const char *)sta_cfg.sta.ssid);
    } else {
        cJSON_AddStringToObject(root, "ssid", "");
    }

    cJSON_AddStringToObject(root, "url1", cfg->api_url[0]);
    cJSON_AddStringToObject(root, "url2", cfg->api_url[1]);
    cJSON_AddStringToObject(root, "url3", cfg->api_url[2]);
    cJSON_AddStringToObject(root, "ntp", cfg->ntp_server);
    cJSON_AddStringToObject(root, "timezone", cfg->tz_string);
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
    cJSON_AddBoolToObject(root, "auto_rotate_enabled", cfg->auto_rotate_enabled);
    cJSON_AddNumberToObject(root, "auto_rotate_interval_s", cfg->auto_rotate_interval_s);
    cJSON_AddNumberToObject(root, "auto_rotate_effect", cfg->auto_rotate_effect);
    cJSON_AddBoolToObject(root, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);
    cJSON_AddNumberToObject(root, "auto_rotate_pages", cfg->auto_rotate_pages);

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
    int remaining = req->content_len;

    /* 3.3: Enforce max payload size */
    if (remaining >= CONFIG_MAX_PAYLOAD) {
        return send_400(req, "Payload too large");
    }

    char *buf = malloc(CONFIG_MAX_PAYLOAD);
    if (!buf) {
        ESP_LOGE(TAG, "Config handler: malloc failed for payload buffer");
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, buf + received, remaining - received);
        if (ret <= 0) {
            free(buf);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Config handler: recv timeout (got %d/%d bytes)", received, remaining);
                httpd_resp_send_408(req);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Config handler: recv error %d (got %d/%d bytes)", ret, received, remaining);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return send_400(req, "Invalid JSON");
    }

    /* 3.3: Validate string field lengths against buffer sizes */
    if (!validate_string_len(root, "url1", 128) ||
        !validate_string_len(root, "url2", 128) ||
        !validate_string_len(root, "url3", 128) ||
        !validate_string_len(root, "ntp", 64) ||
        !validate_string_len(root, "timezone", 64) ||
        !validate_string_len(root, "mqtt_broker_url", 128) ||
        !validate_string_len(root, "mqtt_username", 64) ||
        !validate_string_len(root, "mqtt_password", 64) ||
        !validate_string_len(root, "mqtt_topic_prefix", 64) ||
        !validate_string_len(root, "filter_colors_1", 512) ||
        !validate_string_len(root, "filter_colors_2", 512) ||
        !validate_string_len(root, "filter_colors_3", 512) ||
        !validate_string_len(root, "rms_thresholds_1", 256) ||
        !validate_string_len(root, "rms_thresholds_2", 256) ||
        !validate_string_len(root, "rms_thresholds_3", 256) ||
        !validate_string_len(root, "hfr_thresholds_1", 256) ||
        !validate_string_len(root, "hfr_thresholds_2", 256) ||
        !validate_string_len(root, "hfr_thresholds_3", 256)) {
        cJSON_Delete(root);
        return send_400(req, "String field exceeds maximum length");
    }

    /* 3.3: Validate URL formats */
    cJSON *url_items[] = {
        cJSON_GetObjectItem(root, "url1"),
        cJSON_GetObjectItem(root, "url2"),
        cJSON_GetObjectItem(root, "url3"),
        cJSON_GetObjectItem(root, "mqtt_broker_url"),
    };
    for (int i = 0; i < 4; i++) {
        if (cJSON_IsString(url_items[i]) && !validate_url_format(url_items[i]->valuestring)) {
            cJSON_Delete(root);
            return send_400(req, "Invalid URL format");
        }
    }

    /*
     * WiFi credentials: pass directly to ESP-IDF WiFi NVS (not stored in
     * app_config).  The WiFi stack persists them automatically.
     */
    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(ssid_item) && ssid_item->valuestring[0] != '\0') {
        if (strlen(ssid_item->valuestring) >= 32) {
            cJSON_Delete(root);
            return send_400(req, "SSID too long (max 31 chars)");
        }
        if (cJSON_IsString(pass_item) && strlen(pass_item->valuestring) >= 64) {
            cJSON_Delete(root);
            return send_400(req, "WiFi password too long (max 63 chars)");
        }

        /* Read-modify-write: preserve current password if none provided */
        wifi_config_t sta_cfg = {0};
        esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        strncpy((char *)sta_cfg.sta.ssid, ssid_item->valuestring,
                sizeof(sta_cfg.sta.ssid) - 1);
        if (cJSON_IsString(pass_item) && pass_item->valuestring[0] != '\0') {
            memset(sta_cfg.sta.password, 0, sizeof(sta_cfg.sta.password));
            strncpy((char *)sta_cfg.sta.password, pass_item->valuestring,
                    sizeof(sta_cfg.sta.password) - 1);
        }
        /* else: password from esp_wifi_get_config is preserved */
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        ESP_LOGI(TAG, "WiFi STA credentials updated via esp_wifi_set_config");
    }

    app_config_t *cfg = malloc(sizeof(app_config_t));
    if (!cfg) {
        ESP_LOGE(TAG, "Config handler: malloc failed for app_config_t");
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    // Load current to preserve anything not sent
    memcpy(cfg, app_config_get(), sizeof(app_config_t));

    JSON_TO_STRING(root, "url1",           cfg->api_url[0]);
    JSON_TO_STRING(root, "url2",           cfg->api_url[1]);
    JSON_TO_STRING(root, "url3",           cfg->api_url[2]);
    JSON_TO_STRING(root, "ntp",            cfg->ntp_server);
    JSON_TO_STRING(root, "timezone",       cfg->tz_string);
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

    JSON_TO_BOOL(root, "auto_rotate_enabled", cfg->auto_rotate_enabled);
    JSON_TO_BOOL(root, "auto_rotate_skip_disconnected", cfg->auto_rotate_skip_disconnected);

    cJSON *apo_item = cJSON_GetObjectItem(root, "active_page_override");
    if (cJSON_IsNumber(apo_item)) {
        int v = apo_item->valueint;
        if (v < -1) v = -1;
        if (v > MAX_NINA_INSTANCES + 1) v = MAX_NINA_INSTANCES + 1;  /* max = sysinfo page */
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
        if (v > 3) v = 3;
        cfg->auto_rotate_effect = (uint8_t)v;
    }

    cJSON *arp_item = cJSON_GetObjectItem(root, "auto_rotate_pages");
    if (cJSON_IsNumber(arp_item)) {
        int v = arp_item->valueint;
        if (v < 0) v = 0;
        if (v > 0x1F) v = 0x1F;  /* 5-bit mask */
        cfg->auto_rotate_pages = (uint8_t)v;
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
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_apply_theme(app_config_get()->theme_index);
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "Display lock timeout (color brightness)");
        }
        
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
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_apply_theme(idx);
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "Display lock timeout (theme switch)");
        }
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
        int total = cnt + 2;  /* summary + NINA pages + sysinfo */
        app_config_t *cfg = app_config_get();

        if (page >= 0 && page < total) {
            cfg->active_page_override = (int8_t)page;
            app_config_save(cfg);
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_show_page(page, cnt);
                bsp_display_unlock();
            } else {
                ESP_LOGW(TAG, "Display lock timeout (page switch)");
            }
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

// Handler for screenshot capture - serves a JPEG image via hardware encoder
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
    uint32_t row_size = width * 2;  // RGB565: 2 bytes per pixel
    uint32_t raw_size = row_size * height;

    ESP_LOGI(TAG, "Snapshot captured: %lux%lu, stride=%lu", width, height, stride);

    // Allocate DMA-aligned input buffer for the JPEG encoder
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

    // Copy snapshot pixels into the DMA-aligned buffer (handle stride != row_size)
    for (uint32_t y = 0; y < height; y++) {
        memcpy(enc_in + y * row_size, snapshot->data + y * stride, row_size);
    }
    lv_draw_buf_destroy(snapshot);

    // Allocate DMA-aligned output buffer (raw_size is generous upper bound)
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

    // Create hardware JPEG encoder
    jpeg_encoder_handle_t encoder = NULL;
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    esp_err_t err = jpeg_new_encoder_engine(&engine_cfg, &encoder);
    if (err != ESP_OK || !encoder) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
        free(enc_in);
        free(enc_out);
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
    err = jpeg_encoder_process(encoder, &enc_cfg,
        enc_in, raw_size, enc_out, out_alloc_size, &jpg_size);
    jpeg_del_encoder_engine(encoder);
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