#pragma once

/**
 * @file web_server_internal.h
 * @brief Shared types, macros, and forward declarations for web server handler modules.
 */

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "app_config.h"

/* Logging tag -- shared across all handler files */
static const char *TAG = "web_server";

/* Maximum accepted POST payload size for config endpoints */
#define CONFIG_MAX_PAYLOAD 4096

/* ---- JSON extraction macros ---- */
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

/* ---- Shared helpers ---- */
esp_err_t send_400(httpd_req_t *req, const char *message);
bool validate_string_len(cJSON *root, const char *key, size_t max_len);
bool validate_url_format(const char *url);

/* ---- Handler forward declarations (registered by start_web_server) ---- */
esp_err_t root_get_handler(httpd_req_t *req);
esp_err_t config_get_handler(httpd_req_t *req);
esp_err_t config_post_handler(httpd_req_t *req);
esp_err_t brightness_post_handler(httpd_req_t *req);
esp_err_t color_brightness_post_handler(httpd_req_t *req);
esp_err_t theme_post_handler(httpd_req_t *req);
esp_err_t widget_style_post_handler(httpd_req_t *req);
esp_err_t page_post_handler(httpd_req_t *req);
esp_err_t screenshot_get_handler(httpd_req_t *req);
esp_err_t reboot_post_handler(httpd_req_t *req);
esp_err_t factory_reset_post_handler(httpd_req_t *req);
esp_err_t ota_post_handler(httpd_req_t *req);
esp_err_t version_get_handler(httpd_req_t *req);
esp_err_t perf_get_handler(httpd_req_t *req);
esp_err_t perf_reset_post_handler(httpd_req_t *req);
esp_err_t config_apply_handler(httpd_req_t *req);
esp_err_t config_revert_handler(httpd_req_t *req);
esp_err_t check_update_post_handler(httpd_req_t *req);
void config_trigger_side_effects(const app_config_t *old_cfg, const app_config_t *new_cfg);
