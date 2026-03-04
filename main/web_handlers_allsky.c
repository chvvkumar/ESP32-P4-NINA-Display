#include "web_server_internal.h"
#include "nina_allsky.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include <string.h>

/* AllSky payloads can be larger than CONFIG_MAX_PAYLOAD (4096) because
 * allsky_field_config alone is up to 1536 bytes.  Use a dedicated limit. */
#define ALLSKY_CONFIG_MAX_PAYLOAD 8192

/* Maximum response buffer for the AllSky proxy (allocated from PSRAM) */
#define ALLSKY_PROXY_BUF_SIZE    16384

/**
 * @brief GET /api/allsky-config  -- return AllSky-related config fields.
 */
esp_err_t allsky_config_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "hostname",         cfg->allsky_hostname);
    cJSON_AddNumberToObject(root, "update_interval_s", cfg->allsky_update_interval_s);
    cJSON_AddNumberToObject(root, "dew_offset",       (double)cfg->allsky_dew_offset);
    cJSON_AddStringToObject(root, "field_config",     cfg->allsky_field_config);
    cJSON_AddStringToObject(root, "thresholds",       cfg->allsky_thresholds);
    cJSON_AddBoolToObject(root, "allsky_enabled",    cfg->allsky_enabled);

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
 * @brief POST /api/allsky-config  -- update AllSky-related config fields and persist to NVS.
 */
esp_err_t allsky_config_post_handler(httpd_req_t *req)
{
    int remaining = req->content_len;
    if (remaining >= ALLSKY_CONFIG_MAX_PAYLOAD) {
        return send_400(req, "Payload too large");
    }

    char *buf = heap_caps_malloc(ALLSKY_CONFIG_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "allsky_config_post: malloc failed for payload buffer");
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, buf + received, remaining - received);
        if (ret <= 0) {
            free(buf);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_OK;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return send_400(req, "Invalid JSON");
    }

    /* Validate string field lengths */
    if (!validate_string_len(root, "hostname", 128) ||
        !validate_string_len(root, "field_config", 1536) ||
        !validate_string_len(root, "thresholds", 1024)) {
        send_400(req, "String field exceeds maximum length");
        cJSON_Delete(root);
        return ESP_OK;
    }

    /* Apply fields directly to the live config — avoids copying the full
     * ~6.7 KB app_config_t onto the httpd task's stack. */
    app_config_t *cfg = app_config_get();

    JSON_TO_STRING(root, "hostname",     cfg->allsky_hostname);
    JSON_TO_INT   (root, "update_interval_s", cfg->allsky_update_interval_s);
    JSON_TO_STRING(root, "field_config", cfg->allsky_field_config);
    JSON_TO_STRING(root, "thresholds",   cfg->allsky_thresholds);

    cJSON *item = cJSON_GetObjectItem(root, "dew_offset");
    if (cJSON_IsNumber(item)) {
        cfg->allsky_dew_offset = (float)item->valuedouble;
    }

    cJSON *ena = cJSON_GetObjectItem(root, "allsky_enabled");
    if (cJSON_IsBool(ena)) {
        cfg->allsky_enabled = cJSON_IsTrue(ena);
    }

    cJSON_Delete(root);

    app_config_save(cfg);
    ESP_LOGI(TAG, "AllSky config saved to NVS");

    /* Refresh the AllSky page's threshold/field config from updated NVS data */
    if (lvgl_port_lock(100)) {
        allsky_page_refresh_config();
        lvgl_port_unlock();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/allsky-proxy  -- proxy the AllSky /all endpoint through the ESP32
 *        so that the browser config UI avoids CORS issues.
 */
esp_err_t allsky_proxy_get_handler(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    if (cfg->allsky_hostname[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"AllSky hostname not configured\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Build URL: http://<hostname>/all */
    char url[192];
    snprintf(url, sizeof(url), "http://%s/all", cfg->allsky_hostname);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to create HTTP client\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AllSky proxy: failed to connect to %s", url);
        esp_http_client_cleanup(client);
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to reach AllSky API\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200 || content_length <= 0) {
        ESP_LOGW(TAG, "AllSky proxy: bad response status=%d content_length=%d", status, content_length);
        esp_http_client_cleanup(client);
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to reach AllSky API\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Cap to our buffer size */
    if (content_length > ALLSKY_PROXY_BUF_SIZE - 1) {
        content_length = ALLSKY_PROXY_BUF_SIZE - 1;
    }

    char *buffer = heap_caps_malloc(ALLSKY_PROXY_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "AllSky proxy: PSRAM malloc failed");
        esp_http_client_cleanup(client);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, buffer + total_read,
                                            content_length - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    buffer[total_read] = '\0';

    esp_http_client_cleanup(client);

    /* Forward the AllSky JSON response to the browser */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buffer, total_read);

    free(buffer);
    return ESP_OK;
}
