#include "web_server_internal.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "perf_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// Handler for reboot
esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "Rebooting...", HTTPD_RESP_USE_STRLEN);
    // Delay slightly to let the response go out
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

// Handler for factory reset
esp_err_t factory_reset_post_handler(httpd_req_t *req)
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

// Handler for OTA firmware upload (receives raw binary via POST)
#define OTA_BUF_SIZE 4096

esp_err_t ota_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA update started, content length: %d", req->content_len);

    if (req->content_len <= 0) {
        return send_400(req, "No firmware data received");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: no update partition found");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: writing to partition '%s' at offset 0x%lx",
             update_partition->label, update_partition->address);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "OTA: malloc failed for receive buffer");
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int total = remaining;
    int received_total = 0;
    int last_log_kb = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry on timeout */
                continue;
            }
            ESP_LOGE(TAG, "OTA: recv error %d at %d/%d bytes", received, received_total, total);
            failed = true;
            break;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }

        remaining -= received;
        received_total += received;

        /* Log progress every 100 KB */
        int current_kb = received_total / (100 * 1024);
        if (current_kb > last_log_kb) {
            last_log_kb = current_kb;
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes (%d%%)",
                     received_total, total, (received_total * 100) / total);
        }
    }

    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"OTA receive/write failed\"}");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"Firmware image validation failed\"}");
        } else {
            httpd_resp_send_500(req);
        }
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful (%d bytes), rebooting...", received_total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// Handler for firmware version info
esp_err_t version_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "date", app->date);
    cJSON_AddStringToObject(root, "time", app->time);
    cJSON_AddStringToObject(root, "idf", app->idf_ver);
    cJSON_AddStringToObject(root, "partition", running ? running->label : "unknown");
    cJSON_AddStringToObject(root, "git_tag", BUILD_GIT_TAG);
    cJSON_AddStringToObject(root, "git_sha", BUILD_GIT_SHA);
    cJSON_AddStringToObject(root, "git_branch", BUILD_GIT_BRANCH);

    const char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
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

// Handler for performance profiling data
esp_err_t perf_get_handler(httpd_req_t *req)
{
#if PERF_MONITOR_ENABLED
    perf_monitor_capture_memory();  // Get fresh memory snapshot
    char *json = perf_monitor_report_json();
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
#else
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Profiling disabled\"}");
    return ESP_OK;
#endif
}

// Handler for resetting performance metrics
esp_err_t perf_reset_post_handler(httpd_req_t *req)
{
#if PERF_MONITOR_ENABLED
    perf_monitor_reset_all();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"reset\"}");
    return ESP_OK;
#else
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Profiling disabled\"}");
    return ESP_OK;
#endif
}
