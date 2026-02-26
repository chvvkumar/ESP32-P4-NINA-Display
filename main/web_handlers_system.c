#include "web_server_internal.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "perf_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "nina_websocket.h"
#include "mqtt_ha.h"
#include "tasks.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "display_defs.h"

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

// ── OTA overlay helpers ──

static lv_obj_t *ota_overlay = NULL;
static lv_obj_t *ota_progress_label = NULL;
static lv_obj_t *ota_bar = NULL;
static lv_obj_t *ota_bar_glow = NULL;

/* Accent color for the progress bar */
#define OTA_ACCENT       0x00D4FF   /* Cyan */
#define OTA_ACCENT_DIM   0x005566   /* Dimmed cyan for track */
#define OTA_GLOW_OPA     LV_OPA_40

static void ota_show_overlay(const char *message) {
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        /* ── Fullscreen black overlay ── */
        ota_overlay = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(ota_overlay);
        lv_obj_set_size(ota_overlay, SCREEN_SIZE, SCREEN_SIZE);
        lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_COVER, 0);
        lv_obj_center(ota_overlay);

        /* ── Title — upper third ── */
        lv_obj_t *title = lv_label_create(ota_overlay);
        lv_label_set_text(title, message);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(title, LV_PCT(90));
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 200);

        /* ── Large percentage — center ── */
        ota_progress_label = lv_label_create(ota_overlay);
        lv_label_set_text(ota_progress_label, "0%");
        lv_obj_set_style_text_color(ota_progress_label, lv_color_hex(OTA_ACCENT), 0);
        lv_obj_set_style_text_font(ota_progress_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_align(ota_progress_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ota_progress_label, LV_ALIGN_CENTER, 0, 20);

        /* ── "Do not power off" hint ── */
        lv_obj_t *hint = lv_label_create(ota_overlay);
        lv_label_set_text(hint, "Do not power off");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 70);

        /* ── Glow layer behind progress bar (wider/taller, blurred look) ── */
        ota_bar_glow = lv_bar_create(ota_overlay);
        lv_obj_remove_style_all(ota_bar_glow);
        lv_obj_set_size(ota_bar_glow, 580, 24);
        lv_obj_align(ota_bar_glow, LV_ALIGN_BOTTOM_MID, 0, -90);
        lv_bar_set_range(ota_bar_glow, 0, 100);
        lv_bar_set_value(ota_bar_glow, 0, LV_ANIM_OFF);
        /* Track: invisible */
        lv_obj_set_style_bg_opa(ota_bar_glow, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(ota_bar_glow, 12, LV_PART_MAIN);
        /* Indicator: soft glow */
        lv_obj_set_style_bg_color(ota_bar_glow, lv_color_hex(OTA_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ota_bar_glow, OTA_GLOW_OPA, LV_PART_INDICATOR);
        lv_obj_set_style_radius(ota_bar_glow, 12, LV_PART_INDICATOR);

        /* ── Main progress bar ── */
        ota_bar = lv_bar_create(ota_overlay);
        lv_obj_remove_style_all(ota_bar);
        lv_obj_set_size(ota_bar, 560, 12);
        lv_obj_align(ota_bar, LV_ALIGN_BOTTOM_MID, 0, -96);
        lv_bar_set_range(ota_bar, 0, 100);
        lv_bar_set_value(ota_bar, 0, LV_ANIM_OFF);
        /* Track: dark rounded pill */
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(OTA_ACCENT_DIM), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ota_bar, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_radius(ota_bar, 6, LV_PART_MAIN);
        /* Indicator: bright accent with gradient */
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(OTA_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ota_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(ota_bar, lv_color_hex(0x0088FF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(ota_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
        lv_obj_set_style_radius(ota_bar, 6, LV_PART_INDICATOR);

        bsp_display_unlock();
    }
}

static void ota_update_progress(int percent) {
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        if (ota_progress_label) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", percent);
            lv_label_set_text(ota_progress_label, buf);
        }
        if (ota_bar) {
            lv_bar_set_value(ota_bar, percent, LV_ANIM_ON);
        }
        if (ota_bar_glow) {
            lv_bar_set_value(ota_bar_glow, percent, LV_ANIM_ON);
        }
        bsp_display_unlock();
    }
}

static void ota_remove_overlay(void) {
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        if (ota_overlay) {
            lv_obj_delete(ota_overlay);
            ota_overlay = NULL;
            ota_progress_label = NULL;
            ota_bar = NULL;
            ota_bar_glow = NULL;
        }
        bsp_display_unlock();
    }
}

/**
 * Stop all background network activity to give OTA maximum bandwidth.
 * Sets ota_in_progress to suspend the data task, then stops WebSockets and MQTT.
 */
static void ota_stop_network(void) {
    ota_in_progress = true;
    /* Give the data task time to reach its suspend point */
    vTaskDelay(pdMS_TO_TICKS(200));
    nina_websocket_stop_all();
    mqtt_ha_stop();
}

/**
 * Restore background network activity after a failed OTA.
 * WebSocket reconnects are handled by the data task's check_reconnect logic.
 * MQTT must be explicitly restarted since the data task only starts it once.
 */
static void ota_restore_network(void) {
    mqtt_ha_start();
    ota_in_progress = false;
}

// Handler for OTA firmware upload (receives raw binary via POST)
#define OTA_BUF_SIZE 4096

esp_err_t ota_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA update started, content length: %d", req->content_len);

    if (req->content_len <= 0) {
        return send_400(req, "No firmware data received");
    }

    if (req->content_len > 16 * 1024 * 1024) {
        return send_400(req, "Firmware too large (max 16 MB)");
    }

    /* ── Stop all network traffic and show OTA screen ── */
    ota_stop_network();
    ota_show_overlay("OTA Update\nIn Progress");

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: no update partition found");
        ota_remove_overlay();
        ota_restore_network();
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: writing to partition '%s' at offset 0x%lx",
             update_partition->label, update_partition->address);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_remove_overlay();
        ota_restore_network();
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OTA: malloc failed for receive buffer");
        esp_ota_abort(ota_handle);
        ota_remove_overlay();
        ota_restore_network();
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int total = remaining;
    int received_total = 0;
    int last_progress_pct = -1;
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

        /* Update progress on screen every 1% */
        int pct = (received_total * 100) / total;
        if (pct != last_progress_pct) {
            last_progress_pct = pct;
            ota_update_progress(pct);
            if (pct % 10 == 0) {
                ESP_LOGI(TAG, "OTA progress: %d%%", pct);
            }
        }
    }

    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        ota_remove_overlay();
        ota_restore_network();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"OTA receive/write failed\"}");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        ota_remove_overlay();
        ota_restore_network();
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
        ota_remove_overlay();
        ota_restore_network();
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful (%d bytes), rebooting...", received_total);
    ota_update_progress(100);
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
    httpd_resp_set_type(req, "application/json");
    if (!g_perf.enabled) {
        httpd_resp_sendstr(req, "{\"enabled\":false}");
        return ESP_OK;
    }
    perf_monitor_capture_memory();  // Get fresh memory snapshot
    char *json = perf_monitor_report_json();
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// Handler for resetting performance metrics
esp_err_t perf_reset_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (!g_perf.enabled) {
        httpd_resp_sendstr(req, "{\"error\":\"Debug mode not enabled\"}");
        return ESP_OK;
    }
    perf_monitor_reset_all();
    httpd_resp_sendstr(req, "{\"status\":\"reset\"}");
    return ESP_OK;
}
