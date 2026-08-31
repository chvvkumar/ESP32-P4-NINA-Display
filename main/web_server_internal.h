#pragma once

/**
 * @file web_server_internal.h
 * @brief Shared types, macros, and forward declarations for web server handler modules.
 */

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app_config.h"

/* Logging tag -- shared across all handler files */
static const char *TAG __attribute__((unused)) = "web_server";

/* Maximum accepted POST payload size for config endpoints. Buffers are
 * PSRAM-backed (see receive_json_body), so these are generous by design.
 *
 * RESTORE was 16384 until the backup gained the two tiles blobs. A restore body
 * wraps a whole backup file: the pre-tiles config section is ~8 KB, and each
 * tiles blob is up to 6144 bytes whose embedded quotes roughly double under JSON
 * escaping. 49152 covers the worst case with headroom; a body at or above the
 * cap is rejected with "Payload too large" rather than silently truncated. */
#define CONFIG_MAX_PAYLOAD 8192
#define CONFIG_MAX_RESTORE_PAYLOAD 49152

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

/* Receive and parse a JSON request body using a PSRAM-backed buffer.
 * Loops httpd_req_recv() until content_len bytes are read (handles
 * multi-segment bodies); rejects bodies >= max_size. On any error the
 * appropriate HTTP response is already sent and NULL is returned.
 * On success returns a cJSON root the caller must cJSON_Delete().
 * Defined in web_handlers_config.c. */
cJSON *receive_json_body(httpd_req_t *req, int max_size);

/* Receive and parse a SMALL JSON request body into a caller-supplied buffer
 * (typically 64-256 bytes on the handler stack). Companion to the PSRAM-backed
 * receive_json_body() above, for the single-scalar control endpoints.
 *
 * Rejects a body that does not fit (content_len >= buf_size) with 500 rather
 * than truncating; 408 on recv timeout; 500 on parse failure. On any failure
 * the HTTP response has already been sent and NULL is returned. On success
 * returns a root the caller must cJSON_Delete().
 *
 * The >= guard is what keeps the buf[ret] NUL write in bounds -- do not relax
 * it to >. Single-recv by design: these bodies are one TCP segment. */
static inline cJSON *receive_small_json_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int remaining = req->content_len;
    if (remaining < 0 || (size_t)remaining >= buf_size) {
        httpd_resp_send_500(req);
        return NULL;
    }
    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return NULL;   /* socket-level failure: nothing sensible to send */
    }
    buf[ret] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
    }
    return root;
}

/* Serialize @p root, send it as application/json, then free the printed string
 * and delete @p root.
 *
 * CONSUMES @p root on EVERY path including failure -- callers must not delete
 * it afterwards and must not touch it after the call. A NULL @p root (failed
 * cJSON_Create*) and a print failure both mean out of memory, so both send 500;
 * do not "succeed" with a literal body in that case, the client would cache a
 * lie. */
static inline esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

/* Allocate a PSRAM app_config_t and fill it with the current config snapshot.
 * app_config_t is ~8.6 KB: never stack-allocate one in an httpd handler, and
 * never hold two live at once unless the handler genuinely diffs old vs new.
 * On allocation failure a 500 is sent and NULL is returned -- never carry on
 * without the snapshot, or device state and config diverge silently.
 * Caller frees with heap_caps_free(). */
static inline app_config_t *config_snapshot_for_request(httpd_req_t *req)
{
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        httpd_resp_send_500(req);
        return NULL;
    }
    app_config_get_snapshot_into(cfg);
    return cfg;
}

/* ---- Session cookie auth (defined in web_server.c) ---- */
bool check_session(httpd_req_t *req);
esp_err_t send_auth_required(httpd_req_t *req);

/* True when the request carries a valid browser SESSION COOKIE (config UI), as
 * opposed to a stateless X-Auth-Password client (HA automation, macro keypad).
 * Cosmetic use only -- decides whether a live-apply raises the "unsaved changes"
 * bar. Never a security decision; REQUIRE_AUTH has already run. Returns true
 * when auth is disabled (the two are indistinguishable then). */
bool request_is_web_ui(httpd_req_t *req);

/* Session API used by login/logout handlers */
const char *session_create(void);
bool session_valid(const char *token);
void session_destroy(const char *token);
/* Extract session token from the request's Cookie header. Returns true if present.
 * out buffer must hold at least 65 bytes (64 hex + NUL). */
bool session_extract_cookie(httpd_req_t *req, char *out, size_t out_len);

/* ---- Shared login lockout (defined in web_handlers_auth.c) ----
 * Let alternate auth paths (X-Auth-Password header) share the cookie-login
 * rate limiter so the header is not an unthrottled brute-force bypass. */
bool auth_is_locked_out(void);  /* true if inside the lockout window (false when auth disabled) */
void auth_note_failure(void);   /* record a failed auth; engages lockout at threshold */
void auth_note_success(void);   /* clear failure counter and any lockout */

/**
 * @brief Guard handler entry with session auth. Returns 401/302 if missing/invalid.
 * Must be the first statement in the handler body.
 */
#define REQUIRE_AUTH(req) do { if (!check_session(req)) return send_auth_required(req); } while (0)

/* ---- Handler forward declarations (registered by start_web_server) ---- */
esp_err_t root_get_handler(httpd_req_t *req);
esp_err_t home_get_handler(httpd_req_t *req);
esp_err_t ui_fragment_get_handler(httpd_req_t *req);
esp_err_t favicon_get_handler(httpd_req_t *req);
esp_err_t config_get_handler(httpd_req_t *req);
esp_err_t config_post_handler(httpd_req_t *req);
esp_err_t brightness_post_handler(httpd_req_t *req);
esp_err_t color_brightness_post_handler(httpd_req_t *req);
esp_err_t theme_post_handler(httpd_req_t *req);
esp_err_t widget_style_post_handler(httpd_req_t *req);
esp_err_t page_post_handler(httpd_req_t *req);
esp_err_t screen_rotation_post_handler(httpd_req_t *req);
esp_err_t screenshot_get_handler(httpd_req_t *req);
esp_err_t voice_preview_post_handler(httpd_req_t *req);
esp_err_t reboot_post_handler(httpd_req_t *req);
esp_err_t factory_reset_post_handler(httpd_req_t *req);
esp_err_t ota_post_handler(httpd_req_t *req);
esp_err_t ota_check_post_handler(httpd_req_t *req);
esp_err_t version_get_handler(httpd_req_t *req);
esp_err_t panel_get_handler(httpd_req_t *req);
esp_err_t panel_post_handler(httpd_req_t *req);
esp_err_t perf_get_handler(httpd_req_t *req);
esp_err_t perf_reset_post_handler(httpd_req_t *req);
esp_err_t config_apply_handler(httpd_req_t *req);
esp_err_t config_revert_handler(httpd_req_t *req);
esp_err_t check_update_post_handler(httpd_req_t *req);
esp_err_t check_update_json_handler(httpd_req_t *req);
esp_err_t ota_github_post_handler(httpd_req_t *req);
esp_err_t allsky_config_get_handler(httpd_req_t *req);
esp_err_t allsky_proxy_get_handler(httpd_req_t *req);
esp_err_t json_config_get_handler(httpd_req_t *req);
esp_err_t json_config_post_handler(httpd_req_t *req);
esp_err_t json_proxy_get_handler(httpd_req_t *req);
esp_err_t ha_config_get_handler(httpd_req_t *req);
esp_err_t ha_config_post_handler(httpd_req_t *req);
esp_err_t ha_probe_get_handler(httpd_req_t *req);
esp_err_t ha_test_get_handler(httpd_req_t *req);
esp_err_t octoprint_appkey_request_post_handler(httpd_req_t *req);
esp_err_t octoprint_appkey_status_get_handler(httpd_req_t *req);
esp_err_t config_pull_post_handler(httpd_req_t *req);

/* ---- Page live-apply spines (defined alongside their page-config handlers) ----
 * Apply the CURRENT live config to the JSON Display / Home Assistant page: drop
 * the client's parsed-tiles cache, rebuild the page widget tree, show/hide the
 * page per @p enabled, re-resolve navigation, and ensure the poll task runs.
 * Persistence is the caller's job -- these only read app_config_get() and the
 * tiles getters. Used by the page-config POST handlers and by config restore. */
void json_page_apply_live(bool enabled);
void ha_page_apply_live(bool enabled);
esp_err_t spotify_config_get_handler(httpd_req_t *req);
esp_err_t spotify_config_post_handler(httpd_req_t *req);
esp_err_t spotify_callback_get_handler(httpd_req_t *req);
esp_err_t spotify_token_exchange_post_handler(httpd_req_t *req);
esp_err_t spotify_logout_post_handler(httpd_req_t *req);
esp_err_t spotify_status_get_handler(httpd_req_t *req);
esp_err_t spotify_control_post_handler(httpd_req_t *req);
esp_err_t image_display_config_get_handler(httpd_req_t *req);
esp_err_t image_display_config_post_handler(httpd_req_t *req);
esp_err_t image_display_refresh_post_handler(httpd_req_t *req);
esp_err_t pages_get_handler(httpd_req_t *req);
esp_err_t navigate_post_handler(httpd_req_t *req);
esp_err_t nav_pin_post_handler(httpd_req_t *req);
esp_err_t control_list_get_handler(httpd_req_t *req);
esp_err_t control_get_get_handler(httpd_req_t *req);
esp_err_t control_toggle_post_handler(httpd_req_t *req);
esp_err_t control_cycle_post_handler(httpd_req_t *req);
esp_err_t control_set_post_handler(httpd_req_t *req);
esp_err_t control_adjust_post_handler(httpd_req_t *req);
esp_err_t backup_get_handler(httpd_req_t *req);
esp_err_t restore_post_handler(httpd_req_t *req);
esp_err_t status_get_handler(httpd_req_t *req);
esp_err_t telemetry_preview_get_handler(httpd_req_t *req);
esp_err_t nina_status_get_handler(httpd_req_t *req);
esp_err_t crash_get_handler(httpd_req_t *req);
esp_err_t weather_get_handler(httpd_req_t *req);
esp_err_t events_get_handler(httpd_req_t *req);
esp_err_t events_clear_post_handler(httpd_req_t *req);
esp_err_t admin_password_post_handler(httpd_req_t *req);
esp_err_t login_page_get_handler(httpd_req_t *req);
esp_err_t login_post_handler(httpd_req_t *req);
esp_err_t logout_post_handler(httpd_req_t *req);
esp_err_t wifi_scan_get_handler(httpd_req_t *req);
esp_err_t wifi_setup_post_handler(httpd_req_t *req);
esp_err_t wifi_status_get_handler(httpd_req_t *req);
esp_err_t auth_status_get_handler(httpd_req_t *req);
esp_err_t logs_get_handler(httpd_req_t *req);
esp_err_t logs_clear_post_handler(httpd_req_t *req);
esp_err_t crashlog_get_handler(httpd_req_t *req);
esp_err_t crashlog_clear_post_handler(httpd_req_t *req);
esp_err_t coredump_info_get_handler(httpd_req_t *req);
esp_err_t coredump_get_handler(httpd_req_t *req);
esp_err_t coredump_clear_post_handler(httpd_req_t *req);
esp_err_t voice_clips_get_handler(httpd_req_t *req);
esp_err_t voice_clip_post_handler(httpd_req_t *req);
esp_err_t voice_clip_reset_post_handler(httpd_req_t *req);
void config_trigger_side_effects(const app_config_t *old_cfg, const app_config_t *new_cfg);
