#pragma once

#include "esp_http_server.h"

esp_err_t wifi_scan_get_handler(httpd_req_t *req);
esp_err_t wifi_setup_post_handler(httpd_req_t *req);
esp_err_t wifi_status_get_handler(httpd_req_t *req);
