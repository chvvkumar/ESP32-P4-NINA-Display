#include "web_server.h"
#include "web_server_internal.h"
#include <string.h>

/**
 * @brief Send an HTTP 400 response with a JSON error message.
 */
esp_err_t send_400(httpd_req_t *req, const char *message) {
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
bool validate_string_len(cJSON *root, const char *key, size_t max_len) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item)) return true;  /* absent or non-string -- OK, will be skipped */
    return strlen(item->valuestring) < max_len;
}

/**
 * @brief Validate that a URL field looks like a plausible URL (starts with a scheme).
 */
bool validate_url_format(const char *url) {
    if (url[0] == '\0') return true;  /* empty is allowed (means "not configured") */
    return (strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "https://", 8) == 0 ||
            strncmp(url, "mqtt://", 7) == 0 ||
            strncmp(url, "mqtts://", 8) == 0);
}

void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 20;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server!");
        return;
    }

    static const httpd_uri_t routes[] = {
        { "/",                     HTTP_GET,  root_get_handler, NULL },
        { "/api/config",           HTTP_GET,  config_get_handler, NULL },
        { "/api/config",           HTTP_POST, config_post_handler, NULL },
        { "/api/brightness",       HTTP_POST, brightness_post_handler, NULL },
        { "/api/color-brightness", HTTP_POST, color_brightness_post_handler, NULL },
        { "/api/theme",            HTTP_POST, theme_post_handler, NULL },
        { "/api/reboot",           HTTP_POST, reboot_post_handler, NULL },
        { "/api/factory-reset",    HTTP_POST, factory_reset_post_handler, NULL },
        { "/api/screenshot",       HTTP_GET,  screenshot_get_handler, NULL },
        { "/api/page",             HTTP_POST, page_post_handler, NULL },
        { "/api/version",          HTTP_GET,  version_get_handler, NULL },
        { "/api/ota",              HTTP_POST, ota_post_handler, NULL },
        { "/api/perf",             HTTP_GET,  perf_get_handler, NULL },
        { "/api/perf/reset",       HTTP_POST, perf_reset_post_handler, NULL },
        { "/api/config/apply",     HTTP_POST, config_apply_handler, NULL },
        { "/api/config/revert",    HTTP_POST, config_revert_handler, NULL },
    };

    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "Web server started with %d routes",
             (int)(sizeof(routes)/sizeof(routes[0])));
}
