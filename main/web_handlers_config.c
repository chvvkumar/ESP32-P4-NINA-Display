#include "web_server_internal.h"
#include "mqtt_ha.h"
#include "esp_wifi.h"
#include <string.h>
#include "esp_heap_caps.h"

extern const uint8_t config_html_start[] asm("_binary_config_ui_html_start");
extern const uint8_t config_html_end[]   asm("_binary_config_ui_html_end");

// Handler for root URL
esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)config_html_start,
                    config_html_end - config_html_start);
    return ESP_OK;
}

// Handler for getting config
esp_err_t config_get_handler(httpd_req_t *req)
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
    cJSON_AddNumberToObject(root, "update_rate_s", cfg->update_rate_s);
    cJSON_AddNumberToObject(root, "graph_update_interval_s", cfg->graph_update_interval_s);
    cJSON_AddNumberToObject(root, "connection_timeout_s", cfg->connection_timeout_s);
    cJSON_AddNumberToObject(root, "toast_duration_s", cfg->toast_duration_s);
    cJSON_AddBoolToObject(root, "debug_mode", cfg->debug_mode);
    cJSON_AddBoolToObject(root, "instance_enabled_1", cfg->instance_enabled[0]);
    cJSON_AddBoolToObject(root, "instance_enabled_2", cfg->instance_enabled[1]);
    cJSON_AddBoolToObject(root, "instance_enabled_3", cfg->instance_enabled[2]);
    cJSON_AddBoolToObject(root, "screen_sleep_enabled", cfg->screen_sleep_enabled);
    cJSON_AddNumberToObject(root, "screen_sleep_timeout_s", cfg->screen_sleep_timeout_s);
    cJSON_AddBoolToObject(root, "alert_flash_enabled", cfg->alert_flash_enabled);

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
esp_err_t config_post_handler(httpd_req_t *req)
{
    int remaining = req->content_len;

    /* 3.3: Enforce max payload size */
    if (remaining >= CONFIG_MAX_PAYLOAD) {
        return send_400(req, "Payload too large");
    }

    char *buf = heap_caps_malloc(CONFIG_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
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
        if (v > 0x3F) v = 0x3F;  /* 6-bit mask (includes settings page) */
        cfg->auto_rotate_pages = (uint8_t)v;
    }

    cJSON *ur_item = cJSON_GetObjectItem(root, "update_rate_s");
    if (cJSON_IsNumber(ur_item)) {
        int v = ur_item->valueint;
        if (v < 1) v = 1;
        if (v > 10) v = 10;
        cfg->update_rate_s = (uint8_t)v;
    }

    cJSON *gui_item = cJSON_GetObjectItem(root, "graph_update_interval_s");
    if (cJSON_IsNumber(gui_item)) {
        int v = gui_item->valueint;
        if (v < 2) v = 2;
        if (v > 30) v = 30;
        cfg->graph_update_interval_s = (uint8_t)v;
    }

    cJSON *ct_item = cJSON_GetObjectItem(root, "connection_timeout_s");
    if (cJSON_IsNumber(ct_item)) {
        int v = ct_item->valueint;
        if (v < 2) v = 2;
        if (v > 30) v = 30;
        cfg->connection_timeout_s = (uint8_t)v;
    }

    cJSON *td_item = cJSON_GetObjectItem(root, "toast_duration_s");
    if (cJSON_IsNumber(td_item)) {
        int v = td_item->valueint;
        if (v < 3) v = 3;
        if (v > 30) v = 30;
        cfg->toast_duration_s = (uint8_t)v;
    }

    JSON_TO_BOOL(root, "debug_mode", cfg->debug_mode);
    JSON_TO_BOOL(root, "instance_enabled_1", cfg->instance_enabled[0]);
    JSON_TO_BOOL(root, "instance_enabled_2", cfg->instance_enabled[1]);
    JSON_TO_BOOL(root, "instance_enabled_3", cfg->instance_enabled[2]);

    JSON_TO_BOOL(root, "screen_sleep_enabled", cfg->screen_sleep_enabled);
    JSON_TO_BOOL(root, "alert_flash_enabled", cfg->alert_flash_enabled);
    cJSON *sst_item = cJSON_GetObjectItem(root, "screen_sleep_timeout_s");
    if (cJSON_IsNumber(sst_item)) {
        int v = sst_item->valueint;
        if (v < 10) v = 10;
        if (v > 3600) v = 3600;
        cfg->screen_sleep_timeout_s = (uint16_t)v;
    }

    app_config_save(cfg);
    free(cfg);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config saved to NVS");

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
