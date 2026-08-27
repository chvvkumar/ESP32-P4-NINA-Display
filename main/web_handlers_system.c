#include "web_server_internal.h"
#include "build_version.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "perf_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdatomic.h>
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
#include "ui/nina_nav_arbiter.h"
#include "ui/page_registry.h"
#include "control_registry.h"   /* control_page_current_id — page id */
#include "power_mgmt.h"
#include "lwip/sockets.h"       /* getpeername — client IP in the reboot reason */
#include "lwip/inet.h"
#include "esp_wifi.h"
#include "telemetry.h"
#include "weather_client.h"
#include "ui/nina_event_log.h"
#include "voice_store.h"

// Handler for reboot
esp_err_t reboot_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_send(req, "Rebooting...", HTTPD_RESP_USE_STRLEN);

    /* Name the client in the log: an unexplained reset should point at whoever
     * asked for it, not just at the endpoint. */
    char reason[80];
    char ip[INET6_ADDRSTRLEN] = "";
    struct sockaddr_in6 peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &peer_len) == 0 &&
        inet_ntop(AF_INET6, &peer.sin6_addr, ip, sizeof(ip)) != NULL) {
        snprintf(reason, sizeof(reason), "web /api/reboot from %s", ip);
    } else {
        snprintf(reason, sizeof(reason), "web /api/reboot");
    }

    // Delay slightly to let the response go out
    vTaskDelay(pdMS_TO_TICKS(100));
    app_reboot(reason);
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
    app_reboot("web factory reset");
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

/* Active theme accessor (defined in nina_dashboard.c by the dashboard module) */
const theme_t *nina_dashboard_get_theme(void);

static void ota_show_overlay(const char *message) {
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        /* ── Theme-aware colors: under Red Night use red shades / black only ── */
        const theme_t *t = nina_dashboard_get_theme();
        bool red_night = (t && theme_is_red_night(t));

        uint32_t title_color   = red_night ? t->text_color     : 0xFFFFFF;
        uint32_t accent_color  = red_night ? t->text_color     : OTA_ACCENT;
        uint32_t glow_color    = red_night ? t->progress_color : OTA_ACCENT;
        uint32_t hint_color    = red_night ? t->label_color    : 0x555555;
        uint32_t track_color   = red_night ? t->bento_border   : OTA_ACCENT_DIM;
        uint32_t grad_color    = red_night ? t->bento_border   : 0x0088FF;

        /* ── Fullscreen black overlay ── */
        ota_overlay = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(ota_overlay);
        lv_obj_set_size(ota_overlay, screen_size(), screen_size());
        lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_COVER, 0);
        lv_obj_center(ota_overlay);

        /* ── Title — upper third ── */
        lv_obj_t *title = lv_label_create(ota_overlay);
        lv_label_set_text(title, message);
        lv_obj_set_style_text_color(title, lv_color_hex(title_color), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(title, LV_PCT(90));
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 200);

        /* ── Large percentage — center ── */
        ota_progress_label = lv_label_create(ota_overlay);
        lv_label_set_text(ota_progress_label, "0%");
        lv_obj_set_style_text_color(ota_progress_label, lv_color_hex(accent_color), 0);
        lv_obj_set_style_text_font(ota_progress_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_align(ota_progress_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ota_progress_label, LV_ALIGN_CENTER, 0, 20);

        /* ── "Do not power off" hint ── */
        lv_obj_t *hint = lv_label_create(ota_overlay);
        lv_label_set_text(hint, "Do not power off");
        lv_obj_set_style_text_color(hint, lv_color_hex(hint_color), 0);
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
        lv_obj_set_style_bg_color(ota_bar_glow, lv_color_hex(glow_color), LV_PART_INDICATOR);
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
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(track_color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ota_bar, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_radius(ota_bar, 6, LV_PART_MAIN);
        /* Indicator: bright accent with gradient */
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(accent_color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ota_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(ota_bar, lv_color_hex(grad_color), LV_PART_INDICATOR);
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
    /* Free all TLS sessions to maximize bandwidth and DMA heap for OTA.
     * prepare_shutdown blocks until any in-flight Spotify request completes,
     * then destroys the handle so the poll task cannot touch a freed client. */
    spotify_client_prepare_shutdown();
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

    /* Mutual exclusion: reject a second concurrent OTA without touching network state */
    if (atomic_exchange(&ota_in_progress, true)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Update already in progress\"}");
        return ESP_OK;
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

    /* Pre-flight: on the first boot after an OTA the running image may still
     * be pending verification, which makes esp_ota_begin refuse with
     * ESP_ERR_OTA_ROLLBACK_INVALID_STATE (HTTP 500 with no body, previously).
     * The device is demonstrably up — it is serving this request — so confirm
     * the image now; if that fails, say so instead of a bare 500. Runs on the
     * httpd worker (internal-RAM stack), as the flash write requires. */
    esp_err_t err = ota_github_ensure_can_update();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA refused: running image pending verify, confirm failed: %s",
                 esp_err_to_name(err));
        ota_remove_overlay();
        ota_restore_network();
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Current firmware is awaiting boot verification and could not be confirmed. Reboot the device, then retry the update.\"}");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_remove_overlay();
        ota_restore_network();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"esp_ota_begin failed\"}");
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
    bool timed_out = false;
    int timeout_count = 0;
    uint8_t fam_hdr[OTA_FAMILY_HDR_BYTES];
    int  fam_have = 0;
    bool fam_checked = false;

    while (remaining > 0) {
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Bound consecutive timeouts so a stalled upload cannot loop forever */
                if (++timeout_count >= 5) {
                    ESP_LOGE(TAG, "OTA: aborting after %d consecutive recv timeouts at %d/%d bytes",
                             timeout_count, received_total, total);
                    failed = true;
                    timed_out = true;
                    break;
                }
                continue;
            }
            ESP_LOGE(TAG, "OTA: recv error %d at %d/%d bytes", received, received_total, total);
            failed = true;
            break;
        }
        timeout_count = 0;

        const uint8_t *wp = (const uint8_t *)buf;
        int wl = received;

        if (!fam_checked) {
            int take = OTA_FAMILY_HDR_BYTES - fam_have;
            if (take > wl) take = wl;
            memcpy(fam_hdr + fam_have, wp, (size_t)take);
            fam_have += take;
            if (fam_have < OTA_FAMILY_HDR_BYTES) {
                /* httpd_req_recv returns whatever the socket has; a first chunk
                 * shorter than 112 bytes is legal, so hold the write. */
                remaining -= received;
                received_total += received;
                continue;
            }
            fam_checked = true;
            ota_family_verdict_t verdict = ota_family_check(fam_hdr, OTA_FAMILY_HDR_BYTES);
            if (verdict != OTA_FAMILY_ACCEPT) {
                /* Nothing has been written yet (OTA_WITH_SEQUENTIAL_WRITES
                 * erases nothing up front), so aborting here leaves the target
                 * slot exactly as it was. */
                free(buf);
                esp_ota_abort(ota_handle);
                ota_remove_overlay();
                ota_restore_network();
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_set_type(req, "application/json");
                if (verdict == OTA_FAMILY_NO_DESC) {
                    httpd_resp_sendstr(req,
                        "{\"error\":\"wrong firmware family: this file carries no firmware app "
                        "descriptor, so it is not an ESP32-P4 application image. Download the "
                        "binary that matches this device.\"}");
                } else {
                    httpd_resp_sendstr(req,
                        "{\"error\":\"wrong firmware family: this image is built for the other "
                        "panel shape and would leave the screen dark. Download the binary that "
                        "matches this device.\"}");
                }
                return ESP_FAIL;
            }
            err = esp_ota_write(ota_handle, fam_hdr, OTA_FAMILY_HDR_BYTES);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                failed = true;
                break;
            }
            wp += take;
            wl -= take;
        }

        if (wl > 0) {
            err = esp_ota_write(ota_handle, wp, (size_t)wl);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                failed = true;
                break;
            }
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
        if (timed_out) {
            httpd_resp_set_status(req, "408 Request Timeout");
            httpd_resp_sendstr(req, "{\"error\":\"OTA upload timed out\"}");
        } else {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"error\":\"OTA receive/write failed\"}");
        }
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

    ESP_LOGI(TAG, "OTA update successful (%d bytes)", received_total);
    ota_update_progress(100);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");

    vTaskDelay(pdMS_TO_TICKS(500));
    app_reboot("web OTA upload complete");
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

    return send_json_response(req, root);
}

/* ── Asynchronous GitHub update check ─────────────────────────────────
 * ota_github_check() is a ~7 s HTTPS round trip to api.github.com. The
 * esp_http_server dispatches every handler from one task, so running the
 * check inline stalls all other web requests behind it. Instead the handler
 * serves a cached answer when one is fresh, otherwise it wakes a persistent
 * worker task and tells the client to poll.
 *
 * The worker is created once, on first demand, and then parks forever on a
 * task notification. It is deliberately *not* a one-shot task: a task with a
 * heap-caps stack that deletes itself hands its own stack to the idle task's
 * cleanup path, which aborts if it cannot complete, so the self-delete form is
 * discouraged. Parking one 12 KB PSRAM stack costs nothing scarce.
 *
 * A successful answer is reused for an hour. A failed one is reused for only
 * a minute, so a transient network error does not pin the UI to an error for
 * the rest of the hour. `?refresh=1` bypasses the cache entirely (used by the
 * manual "Check for Updates" button) and drops the stored answer, so pollers
 * that follow a forced check see "checking" rather than the result it replaces.
 */
#define UPD_CACHE_TTL_OK_US    (3600LL * 1000000LL)
#define UPD_CACHE_TTL_FAIL_US  (60LL * 1000000LL)
/* A rate-limited answer is held for the full quota window: the home page triggers
 * a check on every load, so the 60 s fail TTL would keep the outage alive. */
#define UPD_CACHE_TTL_RATELIMIT_US (3600LL * 1000000LL)
/* An install may reuse the cached target only this soon after the check that
 * produced it: ota_url is a pre-signed asset URL with a short lifetime, so an
 * older entry would download a 403 instead of the image. */
#define UPD_INSTALL_REUSE_US   (300LL * 1000000LL)
#define UPD_WORKER_STACK       12288

static SemaphoreHandle_t s_upd_mutex = NULL;      /* guards the cache fields below */
static github_release_info_t *s_upd_rel = NULL;   /* PSRAM; last release info */
static ota_check_result_t s_upd_result = OTA_CHECK_ERROR;
static int64_t s_upd_stamp_us = 0;                /* esp_timer_get_time() at completion */
static bool s_upd_have_result = false;

/* Worker handle and request state. Everything below is guarded by
 * s_upd_mutex, including the handle itself: the worker is created lazily and
 * exactly once, by whichever request first needs it. */
static TaskHandle_t s_upd_task = NULL;
static bool s_upd_checking = false;   /* a cycle is running (state CHECKING) */
static int s_upd_req_channel = 0;
static char s_upd_req_version[48];

/* Persistent worker: parks on a notification, runs one check per wake,
 * publishes the result into the cache, then parks again. Never exits. */
static void update_check_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Copy the request parameters out; the HTTPS call below must not read
         * fields a concurrent handler may be rewriting. */
        int channel = 0;
        char version[sizeof(s_upd_req_version)];
        version[0] = '\0';
        if (xSemaphoreTake(s_upd_mutex, portMAX_DELAY) == pdTRUE) {
            channel = s_upd_req_channel;
            memcpy(version, s_upd_req_version, sizeof(version));
            version[sizeof(version) - 1] = '\0';
            xSemaphoreGive(s_upd_mutex);
        }

        ota_check_result_t chk = OTA_CHECK_ERROR;
        github_release_info_t *scratch =
            heap_caps_calloc(1, sizeof(github_release_info_t), MALLOC_CAP_SPIRAM);
        if (scratch) {
            chk = ota_github_check(channel, version, scratch);
        } else {
            ESP_LOGE(TAG, "Update check: no PSRAM for release info");
        }

        /* Publish under the mutex. The long HTTPS call above ran outside it, so
         * a concurrent reader never waits on the network.
         *
         * This is the single exit of a cycle, and portMAX_DELAY cannot time
         * out, so every path above -- including the allocation failure --
         * lands here and leaves a terminal state (DONE or FAILED, never
         * CHECKING). The flag cannot stick. */
        xSemaphoreTake(s_upd_mutex, portMAX_DELAY);
        if (scratch && s_upd_rel) {
            memcpy(s_upd_rel, scratch, sizeof(*s_upd_rel));
        }
        s_upd_result = chk;
        s_upd_stamp_us = esp_timer_get_time();
        s_upd_have_result = true;
        s_upd_checking = false;
        xSemaphoreGive(s_upd_mutex);

        if (scratch) {
            heap_caps_free(scratch);
        }
        ESP_LOGI(TAG, "Async update check finished (result=%d)", (int)chk);
    }
}

/* Serialise the cached result into `root`. Caller must hold s_upd_mutex. */
static void upd_cache_to_json(cJSON *root)
{
    if (s_upd_result == OTA_CHECK_UPDATE_AVAILABLE && s_upd_rel) {
        cJSON_AddBoolToObject(root, "update_available", true);
        cJSON_AddStringToObject(root, "tag", s_upd_rel->tag);
        cJSON_AddStringToObject(root, "summary", s_upd_rel->summary);
        cJSON_AddBoolToObject(root, "is_prerelease", s_upd_rel->is_prerelease);
        cJSON_AddBoolToObject(root, "requires_full_erase", s_upd_rel->requires_full_erase);
        cJSON_AddStringToObject(root, "full_erase_tag", s_upd_rel->full_erase_tag);
    } else if (s_upd_result == OTA_CHECK_RATE_LIMITED) {
        cJSON_AddBoolToObject(root, "update_available", false);
        cJSON_AddStringToObject(root, "error",
                                "GitHub update limit reached. Try again in about an hour.");
    } else if (s_upd_result == OTA_CHECK_ERROR) {
        cJSON_AddBoolToObject(root, "update_available", false);
        cJSON_AddStringToObject(root, "error", "Could not reach GitHub. Try again.");
    } else {
        cJSON_AddBoolToObject(root, "update_available", false);
    }
}

// Handler for checking GitHub OTA updates (returns JSON result to web UI)
esp_err_t check_update_json_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    /* Every handler runs on the single esp_http_server task, so this lazy init
     * cannot race with itself. */
    if (!s_upd_mutex) {
        s_upd_mutex = xSemaphoreCreateMutex();
        if (!s_upd_mutex) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }
    if (!s_upd_rel) {
        s_upd_rel = heap_caps_calloc(1, sizeof(github_release_info_t), MALLOC_CAP_SPIRAM);
        if (!s_upd_rel) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    /* ?refresh=1 forces a new check instead of serving the cached answer. */
    bool force = false;
    {
        char query[48];
        char val[8];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "refresh", val, sizeof(val)) == ESP_OK) {
            force = (val[0] == '1');
        }
    }

    const char *cur_ver = ota_github_get_current_version();
    /* Read the config before taking s_upd_mutex, so the two locks never nest. */
    int channel = app_config_get()->update_channel;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "current_version", cur_ver);

    if (xSemaphoreTake(s_upd_mutex, portMAX_DELAY) != pdTRUE) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* ── Fresh cached answer: reply immediately ── */
    int64_t ttl = UPD_CACHE_TTL_OK_US;
    if (s_upd_result == OTA_CHECK_ERROR) {
        ttl = UPD_CACHE_TTL_FAIL_US;
    } else if (s_upd_result == OTA_CHECK_RATE_LIMITED) {
        ttl = UPD_CACHE_TTL_RATELIMIT_US;
    }
    if (s_upd_have_result && !force &&
        (esp_timer_get_time() - s_upd_stamp_us) < ttl) {
        cJSON_AddBoolToObject(root, "cached", true);
        upd_cache_to_json(root);
        xSemaphoreGive(s_upd_mutex);
        return send_json_response(req, root);
    }

    /* ── A check is needed. Bring the worker up on first demand only. ── */
    if (!s_upd_task) {
        /* PSRAM stack, Core 0 with the other network tasks, below httpd. */
        if (xTaskCreatePinnedToCoreWithCaps(update_check_worker, "upd_chk",
                                            UPD_WORKER_STACK, NULL, 4, &s_upd_task,
                                            0, MALLOC_CAP_SPIRAM) != pdPASS) {
            /* Leave the state untouched so a later request retries the
             * creation. Never fall back to running the check inline: that puts
             * mbedTLS on the httpd worker stack and overflows it. */
            s_upd_task = NULL;
            ESP_LOGE(TAG, "Failed to start update check worker");
            if (s_upd_have_result) {
                cJSON_AddBoolToObject(root, "cached", true);
                upd_cache_to_json(root);
            } else {
                cJSON_AddBoolToObject(root, "update_available", false);
                cJSON_AddStringToObject(root, "error",
                                        "Could not start update check. Try again.");
            }
            xSemaphoreGive(s_upd_mutex);
            return send_json_response(req, root);
        }
    }

    /* Only the request that moves the state to CHECKING notifies the worker.
     * ulTaskNotifyTake(pdTRUE, ...) clears the count when the cycle starts, so
     * a notify sent mid-cycle would be latched and drive a redundant second
     * HTTPS round trip. Requests arriving while CHECKING simply coalesce onto
     * the running cycle and poll for its result. */
    if (!s_upd_checking) {
        s_upd_req_channel = channel;
        strncpy(s_upd_req_version, cur_ver, sizeof(s_upd_req_version) - 1);
        s_upd_req_version[sizeof(s_upd_req_version) - 1] = '\0';
        /* Drop the previous answer. Without this a forced check keeps serving
         * the result it was asked to replace: the client's follow-up polls do
         * not carry ?refresh=1, so they would read the still-fresh cache and
         * report the stale answer before the new one lands. */
        s_upd_have_result = false;
        s_upd_checking = true;
        xTaskNotifyGive(s_upd_task);
    }

    cJSON_AddStringToObject(root, "status", "checking");
    xSemaphoreGive(s_upd_mutex);
    return send_json_response(req, root);
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

    /* Optional {"tag":"..."} pins the install to a specific release; the web UI
     * sends an empty object and takes whatever the last check offered. */
    char want_tag[32];
    want_tag[0] = '\0';
    cJSON *req_json = cJSON_Parse(body);
    if (req_json) {
        cJSON *tag_item = cJSON_GetObjectItem(req_json, "tag");
        if (cJSON_IsString(tag_item) && tag_item->valuestring) {
            strncpy(want_tag, tag_item->valuestring, sizeof(want_tag) - 1);
            want_tag[sizeof(want_tag) - 1] = '\0';
        }
        cJSON_Delete(req_json);
    }

    /* Read the config before taking s_upd_mutex, so the two locks never nest. */
    int update_channel = app_config_get()->update_channel;
    const char *cur_ver = ota_github_get_current_version();

    github_release_info_t *rel = heap_caps_calloc(1, sizeof(github_release_info_t), MALLOC_CAP_SPIRAM);
    if (!rel) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Reuse the async worker's target when it is the one the UI just offered,
     * instead of re-walking the releases list inline on the httpd task. Bounded
     * by UPD_INSTALL_REUSE_US because the cached ota_url is pre-signed. The
     * channel must match too: the user can switch channels between the check
     * and the install, and the cached target belongs to the old channel. */
    bool from_cache = false;
    if (s_upd_mutex && s_upd_rel &&
        xSemaphoreTake(s_upd_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_upd_have_result && s_upd_result == OTA_CHECK_UPDATE_AVAILABLE &&
            s_upd_req_channel == update_channel &&
            (esp_timer_get_time() - s_upd_stamp_us) < UPD_INSTALL_REUSE_US &&
            (want_tag[0] == '\0' || strcmp(want_tag, s_upd_rel->tag) == 0)) {
            memcpy(rel, s_upd_rel, sizeof(*rel));
            from_cache = true;
        }
        xSemaphoreGive(s_upd_mutex);
    }

    if (from_cache) {
        ESP_LOGI(TAG, "GitHub OTA install using cached check result (%s)", rel->tag);
    } else if (ota_github_check(update_channel, cur_ver, rel) != OTA_CHECK_UPDATE_AVAILABLE) {
        heap_caps_free(rel);
        return send_400(req, "No update available");
    }

    /* Releases that changed the partition table or bootloader cannot be flashed
     * over the air. The web UI hides the button, but this endpoint is also used
     * directly by automation, so gate here for both the cached and fresh paths. */
    if (rel->requires_full_erase) {
        heap_caps_free(rel);
        return send_400(req, "This update requires a manual USB flash. See the release notes.");
    }

    /* Pre-flight: confirm a still-pending running image before promising an
     * install — while pending, esp_ota_begin refuses every OTA. Doing it here
     * surfaces a clear error; after this point the "started" response is
     * already committed and a download failure can only be logged. */
    if (ota_github_ensure_can_update() != ESP_OK) {
        heap_caps_free(rel);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Current firmware is awaiting boot verification and could not be confirmed. Reboot the device, then retry the update.\"}");
        return ESP_OK;
    }

    /* Mutual exclusion: reject a second concurrent OTA without touching network state */
    if (atomic_exchange(&ota_in_progress, true)) {
        heap_caps_free(rel);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Update already in progress\"}");
        return ESP_OK;
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
        ota_github_save_pending_version(rel->tag);
        ESP_LOGI(TAG, "GitHub OTA success (%s)", rel->tag);
        ota_update_progress(100);
        heap_caps_free(rel);
        vTaskDelay(pdMS_TO_TICKS(1000));
        app_reboot("web GitHub OTA complete");
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

/* Map a nav_source_t rung to the web contract's "level" string. */
static const char *nav_level_str(nav_source_t src)
{
    switch (src) {
        case NAV_SRC_HOLD:      return "menu";
        case NAV_SRC_HOME_LOCK: return "lock";
        case NAV_SRC_USER:      return "override";
        case NAV_SRC_SLIDESHOW: return "slideshow";
        case NAV_SRC_SESSION:   return "session";
        case NAV_SRC_IDLE:      return "idle";
        case NAV_SRC_BOOT:      /* fallthrough */
        case NAV_SRC_DEFAULT:
        default:                return "home";
    }
}

/* Slug for a page_ref id. Unknown/legacy ids (including the -1 Home Page
 * sentinel) fall back to Summary's slug, matching the arbiter's home_page()
 * Summary fallback. */
static const char *page_id_slug_or_summary(int id)
{
    const page_ref_entry_t *e = NULL;
    if (id >= 0 && id < (int)PAGE_REF_ID_MAX) {
        e = page_ref_by_id((page_ref_t)id);
    }
    if (!e || !e->slug) {
        e = page_ref_by_id(PAGE_REF_SUMMARY);
    }
    return (e && e->slug) ? e->slug : "summary";
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
    cJSON_AddNumberToObject(root, "heap_internal_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_total", (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "psram_total", (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    // ESP32-P4 internal SoC temperature. telemetry_read_temp_c() is the sole
    // installer/owner of the sensor now; it returns the last known reading,
    // 0 until the first successful read.
    cJSON_AddNumberToObject(root, "temperature_c", telemetry_read_temp_c());

    // WiFi station signal + SSID (null/empty when not associated).
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
        cJSON_AddStringToObject(root, "wifi_ssid", (const char *)ap_info.ssid);
    } else {
        cJSON_AddNullToObject(root, "wifi_rssi");
        cJSON_AddStringToObject(root, "wifi_ssid", "");
    }

    // ── Navigation state (Home web page) ──
    const app_config_t *cfg = app_config_get();
    nav_arbiter_web_status_t nav;
    nav_arbiter_get_web_status(&nav);

    cJSON *nav_obj = cJSON_AddObjectToObject(root, "nav");
    if (nav_obj) {
        cJSON_AddStringToObject(nav_obj, "mode",
                                cfg->auto_rotate_enabled ? "slideshow" : "pinned");
        cJSON_AddStringToObject(nav_obj, "level", nav_level_str(nav.level));
        cJSON_AddNumberToObject(nav_obj, "override_remaining_s", nav.grace_remaining_s);
        cJSON_AddNumberToObject(nav_obj, "grace_s", cfg->nav_grace_s);
        cJSON_AddBoolToObject(nav_obj, "home_page_lock", cfg->home_page_lock);
        cJSON_AddNumberToObject(nav_obj, "current_page", nina_dashboard_get_active_page());
        cJSON_AddStringToObject(nav_obj, "current_page_slug",
                                page_id_slug_or_summary(control_page_current_id()));
        cJSON_AddStringToObject(nav_obj, "home_page_slug",
                                page_id_slug_or_summary(cfg->active_page_override));
        cJSON_AddBoolToObject(nav_obj, "idle_enabled", cfg->idle_page_override_enabled);
        cJSON_AddStringToObject(nav_obj, "idle_page_slug",
                                cfg->idle_page_override_enabled
                                    ? page_id_slug_or_summary(cfg->idle_page_override_target)
                                    : "");
        cJSON_AddBoolToObject(nav_obj, "auto_rotate", cfg->auto_rotate_enabled);
        // Slideshow stop values ARE page_ref ids; skip empty/invalid entries
        // the same way slideshow_build_candidates() does.
        cJSON *rot = cJSON_AddArrayToObject(nav_obj, "rotation");
        if (rot) {
            for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
                uint8_t bit = cfg->auto_rotate_order2[i];
                if (bit == 0xFF || !ARP_STOP_IS_VALID(bit)) continue;
                const page_ref_entry_t *e = page_ref_by_id((page_ref_t)bit);
                if (e && e->slug) {
                    cJSON_AddItemToArray(rot, cJSON_CreateString(e->slug));
                }
            }
        }
    }

    // ── Integration availability (Home web page badges) ──
    cJSON *integ = cJSON_AddObjectToObject(root, "integrations");
    if (integ) {
        // Live broker connection state (false when MQTT is disabled).
        cJSON_AddBoolToObject(integ, "mqtt", mqtt_ha_is_connected());
        cJSON_AddBoolToObject(integ, "allsky", cfg->allsky_enabled);
        // Weather has no enable flag; "configured" mirrors the poll task's own
        // gate (location set, plus an API key for the providers that need one).
        bool weather_needs_key = (cfg->weather_provider == 0 || cfg->weather_provider == 2);
        bool weather_configured = (cfg->weather_location_name[0] != '\0')
                               && (!weather_needs_key || cfg->weather_api_key[0] != '\0');
        cJSON_AddBoolToObject(integ, "weather", weather_configured);
        cJSON_AddBoolToObject(integ, "spotify", cfg->spotify_enabled);
    }

    // ── Cached update-check answer only; NEVER a network call here. The cache
    // is filled by the async worker above (check_update_json_handler); when no
    // check has run yet this reports false. ──
    bool upd_avail = false;
    if (s_upd_mutex && xSemaphoreTake(s_upd_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        upd_avail = s_upd_have_result && (s_upd_result == OTA_CHECK_UPDATE_AVAILABLE);
        xSemaphoreGive(s_upd_mutex);
    }
    cJSON_AddBoolToObject(root, "update_available", upd_avail);

    // ── CPU load (ungated getter; works with debug_mode off) ──
    float cpu0 = 0.0f, cpu1 = 0.0f, cpu_total = 0.0f;
    perf_monitor_get_core_loads(&cpu0, &cpu1, &cpu_total);
    cJSON_AddNumberToObject(root, "cpu_load", cpu_total);
    cJSON_AddNumberToObject(root, "cpu0", cpu0);
    cJSON_AddNumberToObject(root, "cpu1", cpu1);

    // ── Voice clip storage (SPIFFS). Fields are always present; all zeros
    // with "ready":false semantics while the store is unavailable/formatting. ──
    size_t vs_used = 0, vs_total = 0, vs_custom = 0;
    if (voice_store_ready()) {
        voice_store_stats(&vs_used, &vs_total, &vs_custom);
    }
    cJSON_AddNumberToObject(root, "spiffs_used", (double)vs_used);
    cJSON_AddNumberToObject(root, "spiffs_total", (double)vs_total);
    cJSON_AddNumberToObject(root, "voice_custom_bytes", (double)vs_custom);
    cJSON_AddNumberToObject(root, "voice_custom_budget", (double)VOICE_STORE_BUDGET);

    return send_json_response(req, root);
}

// Handler for the telemetry preview: returns the EXACT bytes the daily report
// would POST (telemetry_build_payload is the single source of truth), so the
// user can inspect what leaves the device before opting in.
esp_err_t telemetry_preview_get_handler(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int len = telemetry_build_payload(buf, 1024, true);
    if (len <= 0) {
        heap_caps_free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    heap_caps_free(buf);
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

    return send_json_response(req, root);
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
    REQUIRE_AUTH(req);
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

    return send_json_response(req, root);
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

    /* Apply + persist. Snapshot under the config mutex rather than memcpy'ing
     * the live pointer, so a concurrent save cannot be copied half-written. */
    app_config_t *cfg = config_snapshot_for_request(req);
    if (!cfg) {
        cJSON_Delete(root);
        return ESP_OK;   /* 500 already sent */
    }
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

// Handler for current weather data (public, no auth)
esp_err_t weather_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (!weather_client_has_valid_data()) {
        cJSON_AddBoolToObject(root, "available", false);
    } else {
        weather_data_t wx;
        weather_client_get_data(&wx);

        if (!wx.valid) {
            cJSON_AddBoolToObject(root, "available", false);
        } else {
            cJSON_AddBoolToObject(root, "available", true);
            cJSON_AddNumberToObject(root, "temp_current", wx.temp_current);
            cJSON_AddNumberToObject(root, "temp_high", wx.temp_high);
            cJSON_AddNumberToObject(root, "temp_low", wx.temp_low);
            cJSON_AddNumberToObject(root, "humidity", wx.humidity);
            cJSON_AddNumberToObject(root, "dew_point", wx.dew_point);
            cJSON_AddNumberToObject(root, "wind_speed", wx.wind_speed);
            cJSON_AddStringToObject(root, "wind_dir", wx.wind_dir);
            cJSON_AddNumberToObject(root, "uv_index", wx.uv_index);
            cJSON_AddStringToObject(root, "condition", wx.condition);
            cJSON_AddNumberToObject(root, "last_update_ts", (double)wx.last_update_ts);

            const app_config_t *cfg = app_config_get();
            cJSON_AddStringToObject(root, "units",
                                    cfg->weather_units == 1 ? "metric" : "imperial");
            cJSON_AddStringToObject(root, "location_name", cfg->weather_location_name);

            cJSON *hourly = cJSON_AddArrayToObject(root, "hourly");
            if (hourly) {
                for (int i = 0; i < 10; i++) {
                    cJSON *h = cJSON_CreateObject();
                    if (!h) continue;
                    cJSON_AddNumberToObject(h, "hour", wx.hourly_hours[i]);
                    cJSON_AddNumberToObject(h, "temp", wx.hourly_temps[i]);
                    cJSON_AddItemToArray(hourly, h);
                }
            }
        }
    }

    return send_json_response(req, root);
}

// Handler for the on-device UI event log ring (public, no auth)
#define EVENTS_MAX_SNAPSHOT 100

esp_err_t events_get_handler(httpd_req_t *req)
{
    nina_event_log_entry_t *snap =
        heap_caps_malloc(sizeof(nina_event_log_entry_t) * EVENTS_MAX_SNAPSHOT,
                         MALLOC_CAP_SPIRAM);
    if (!snap) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int n = nina_event_log_copy_entries(snap, EVENTS_MAX_SNAPSHOT);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        heap_caps_free(snap);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(root, "count", n);
    cJSON *arr = cJSON_AddArrayToObject(root, "events");
    if (arr) {
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            if (!e) continue;
            const char *sev_str = "info";
            switch (snap[i].sev) {
                case EVENT_SEV_SUCCESS: sev_str = "success"; break;
                case EVENT_SEV_WARNING: sev_str = "warning"; break;
                case EVENT_SEV_ERROR:   sev_str = "error";   break;
                case EVENT_SEV_INFO:    /* fallthrough */
                default:                sev_str = "info";    break;
            }
            cJSON_AddStringToObject(e, "severity", sev_str);
            cJSON_AddNumberToObject(e, "instance", snap[i].instance);
            cJSON_AddStringToObject(e, "message", snap[i].message);
            cJSON_AddNumberToObject(e, "timestamp_ms", (double)snap[i].timestamp_ms);
            cJSON_AddItemToArray(arr, e);
        }
    }

    heap_caps_free(snap);

    return send_json_response(req, root);
}

// Handler for clearing the on-device UI event log (auth required)
esp_err_t events_clear_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    nina_event_log_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
