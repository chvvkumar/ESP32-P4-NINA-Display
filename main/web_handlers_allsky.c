#include "web_server_internal.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include <string.h>

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
