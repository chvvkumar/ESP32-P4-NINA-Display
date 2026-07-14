#include "web_server_internal.h"
#include "http_fetch.h"
#include "esp_heap_caps.h"
#include <string.h>

/* Maximum response buffer for the AllSky proxy (allocated from PSRAM) */
#define ALLSKY_PROXY_BUF_SIZE    16384

/**
 * @brief GET /api/allsky-config  -- return AllSky-related config fields.
 */
esp_err_t allsky_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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
    REQUIRE_AUTH(req);
    /* Snapshot the hostname into a local under the config mutex so the live
     * config is not read field-by-field during the (slow) outbound fetch.
     * app_config_t is ~7.6 KB — too large for the httpd stack, so snapshot into
     * a PSRAM heap copy, extract the hostname, and free before the fetch. */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);
    char allsky_hostname[sizeof(cfg->allsky_hostname)];
    strlcpy(allsky_hostname, cfg->allsky_hostname, sizeof(allsky_hostname));
    heap_caps_free(cfg);

    if (allsky_hostname[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"AllSky hostname not configured\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Build URL: http://<hostname>/all */
    char url[192];
    snprintf(url, sizeof(url), "http://%s/all", allsky_hostname);

    /* One-shot fetch (conn=NULL): this runs on an httpd worker task, not a
     * persistent poll loop, so there is no task-owned keep-alive slot to
     * reuse here. 3s timeout keeps the httpd worker from blocking on a slow
     * or unreachable AllSky host; cap matches the previous fixed buffer. */
    http_fetch_opts_t opts = {
        .timeout_ms = 3000,
        .max_response_bytes = ALLSKY_PROXY_BUF_SIZE,
    };
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_fetch_text(url, &opts, &body, &body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AllSky proxy: fetch failed for %s (%s)", url, esp_err_to_name(err));
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to reach AllSky API\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Forward the AllSky JSON response to the browser */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, body_len);

    heap_caps_free(body);
    return ESP_OK;
}
