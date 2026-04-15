#include "web_server_internal.h"
#include "build_version.h"
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
#include "spotify_client.h"
#include "tasks.h"
#include "ota_github.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "display_defs.h"
#include "nvs.h"
#include "esp_timer.h"
#include "nina_connection.h"
#include "ui/nina_dashboard.h"
#include "power_mgmt.h"

// Handler for reboot
esp_err_t reboot_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_send(req, "Rebooting...", HTTPD_RESP_USE_STRLEN);
    // Delay slightly to let the response go out
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

// Handler for check-update (triggers on-demand OTA check on device)
esp_err_t check_update_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    ota_check_requested = true;
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for factory reset
esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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
    /* Give tasks time to reach their suspend points */
    vTaskDelay(pdMS_TO_TICKS(200));
    /* Free all TLS sessions to maximize bandwidth and DMA heap for OTA */
    spotify_client_destroy_connection();
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
    REQUIRE_AUTH(req);
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

// Handler for checking GitHub OTA updates (returns JSON result to web UI)
esp_err_t check_update_json_handler(httpd_req_t *req)
{
    bool include_pre = (app_config_get()->update_channel == 1);
    const char *cur_ver = ota_github_get_current_version();

    github_release_info_t *rel = heap_caps_calloc(1, sizeof(github_release_info_t), MALLOC_CAP_SPIRAM);
    if (!rel) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current_version", cur_ver);

    if (ota_github_check(include_pre, cur_ver, rel)) {
        cJSON_AddBoolToObject(root, "update_available", true);
        cJSON_AddStringToObject(root, "tag", rel->tag);
        cJSON_AddStringToObject(root, "summary", rel->summary);
        cJSON_AddBoolToObject(root, "is_prerelease", rel->is_prerelease);
    } else {
        cJSON_AddBoolToObject(root, "update_available", false);
    }

    heap_caps_free(rel);

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

// Handler for GitHub OTA download (triggered from web UI)
esp_err_t ota_github_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    /* Read JSON body with release tag to install */
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return send_400(req, "No body");
    }
    body[received] = '\0';

    /* Re-check GitHub for the release to get the OTA URL */
    bool include_pre = (app_config_get()->update_channel == 1);
    const char *cur_ver = ota_github_get_current_version();

    github_release_info_t *rel = heap_caps_calloc(1, sizeof(github_release_info_t), MALLOC_CAP_SPIRAM);
    if (!rel) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (!ota_github_check(include_pre, cur_ver, rel)) {
        heap_caps_free(rel);
        return send_400(req, "No update available");
    }

    /* Stop network and show OTA overlay on device */
    ota_stop_network();
    ota_show_overlay("OTA Update\nIn Progress");

    /* Send response before starting download (connection will close on reboot) */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"started\":true}");

    /* Download and flash */
    esp_err_t err = ota_github_download(rel->ota_url, ota_update_progress);
    if (err == ESP_OK) {
        ota_github_save_installed_version(rel->tag);
        ESP_LOGI(TAG, "GitHub OTA success (%s), rebooting...", rel->tag);
        ota_update_progress(100);
        heap_caps_free(rel);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "GitHub OTA failed: %s", esp_err_to_name(err));
        ota_remove_overlay();
        ota_restore_network();
        heap_caps_free(rel);
        /* Response already sent, can't send error — device will recover */
    }

    return ESP_OK;
}

// Handler for performance profiling data
esp_err_t perf_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
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
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "application/json");
    if (!g_perf.enabled) {
        httpd_resp_sendstr(req, "{\"error\":\"Debug mode not enabled\"}");
        return ESP_OK;
    }
    perf_monitor_reset_all();
    httpd_resp_sendstr(req, "{\"status\":\"reset\"}");
    return ESP_OK;
}

// Handler for lightweight device status (test automation)
esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));

    uint32_t boot_count = 0;
    nvs_handle_t nvs;
    if (nvs_open("system", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "boot_cnt", &boot_count);
        nvs_close(nvs);
    }
    cJSON_AddNumberToObject(root, "boot_count", boot_count);
    cJSON_AddNumberToObject(root, "active_page", nina_dashboard_get_active_page());
    cJSON_AddNumberToObject(root, "instance_count", instance_count);
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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

// Handler for per-instance NINA connection health (test automation)
esp_err_t nina_status_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    const app_config_t *cfg = app_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "instances");
    if (!root || !arr) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        cJSON *inst = cJSON_CreateObject();
        cJSON_AddNumberToObject(inst, "index", i);
        cJSON_AddBoolToObject(inst, "enabled", app_config_is_instance_enabled(i));
        cJSON_AddStringToObject(inst, "url", cfg->api_url[i]);

        nina_conn_state_t st = nina_connection_get_state(i);
        const char *state_str = "unknown";
        switch (st) {
            case NINA_CONN_CONNECTING:   state_str = "connecting";   break;
            case NINA_CONN_CONNECTED:    state_str = "connected";    break;
            case NINA_CONN_DISCONNECTED: state_str = "disconnected"; break;
            default: break;
        }
        cJSON_AddStringToObject(inst, "connection_state", state_str);
        cJSON_AddBoolToObject(inst, "websocket_connected", nina_connection_is_ws_connected(i));

        const nina_conn_info_t *info = nina_connection_get_info(i);
        cJSON_AddNumberToObject(inst, "consecutive_failures", info->consecutive_failures);
        cJSON_AddNumberToObject(inst, "consecutive_successes", info->consecutive_successes);
        cJSON_AddNumberToObject(inst, "last_successful_poll_ms", (double)info->last_connected_ms);

        cJSON_AddItemToArray(arr, inst);
    }

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

// Helper: map esp_reset_reason_t to human-readable string
static const char *reset_reason_to_str(uint32_t reason)
{
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "UNKNOWN";
    }
}

// Handler for crash info
esp_err_t crash_get_handler(httpd_req_t *req)
{
    power_mgmt_crash_info_t info = power_mgmt_get_crash_info();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "crash_count", info.crash_count);
    cJSON_AddStringToObject(root, "last_reset_reason",
                            reset_reason_to_str(info.last_crash_reason));
    cJSON_AddNumberToObject(root, "last_reset_reason_code", info.last_crash_reason);
    cJSON_AddNumberToObject(root, "boot_count", info.boot_count);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)esp_timer_get_time() / 1000000.0);

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

// Handler for changing the admin password. Requires the current password.
esp_err_t admin_password_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 512) {
        return send_400(req, "Invalid payload size");
    }
    char *buf = heap_caps_malloc(remaining + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, buf + received, remaining - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_408(req);
            return ESP_OK;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return send_400(req, "Invalid JSON");

    cJSON *cur_item = cJSON_GetObjectItem(root, "current");
    cJSON *new_item = cJSON_GetObjectItem(root, "new");
    if (!cJSON_IsString(cur_item) || !cJSON_IsString(new_item)) {
        cJSON_Delete(root);
        return send_400(req, "Missing 'current' or 'new' string");
    }
    const char *cur_pw = cur_item->valuestring;
    const char *new_pw = new_item->valuestring;

    /* Validate new password length (4-32 chars) */
    size_t new_len = strlen(new_pw);
    if (new_len < 4 || new_len > 32) {
        cJSON_Delete(root);
        return send_400(req, "New password must be 4-32 characters");
    }

    /* Constant-time compare of current against stored */
    const app_config_t *live = app_config_get();
    size_t a = strlen(cur_pw);
    size_t b = strlen(live->admin_password);
    unsigned char diff = (a != b) ? 1 : 0;
    size_t n = (a < b) ? a : b;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)cur_pw[i] ^ (unsigned char)live->admin_password[i];
    }
    if (diff != 0) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"current password incorrect\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Apply + persist */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    memcpy(cfg, live, sizeof(app_config_t));
    memset(cfg->admin_password, 0, sizeof(cfg->admin_password));
    strncpy(cfg->admin_password, new_pw, sizeof(cfg->admin_password) - 1);

    app_config_apply(cfg);
    app_config_save(cfg);
    free(cfg);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Admin password updated");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
