#include "web_server_internal.h"
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/* ---- Login rate limiting (global, not per-IP) ---- */
#define LOGIN_MAX_FAILURES 5
#define LOGIN_LOCKOUT_SEC  30

static int s_login_failures = 0;
static int64_t s_login_lockout_until_us = 0;
static portMUX_TYPE s_login_mux = portMUX_INITIALIZER_UNLOCKED;

extern const uint8_t login_html_start[] asm("_binary_login_html_start");
extern const uint8_t login_html_end[]   asm("_binary_login_html_end");

/* GET /login — serves the static login page (unauthenticated). */
esp_err_t login_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* No caching: avoids stale copy after logout/password change */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)login_html_start,
                    login_html_end - login_html_start);
    return ESP_OK;
}

/* POST /api/login — verify password, issue session cookie. Unauthenticated. */
esp_err_t login_post_handler(httpd_req_t *req)
{
    /* Rate limit: if globally locked out, reject without checking password.
     * Skip rate-limit when auth is disabled — there's no security value, and
     * a prank flood of bad passwords would otherwise lock the legitimate user
     * out of re-enabling auth. */
    const app_config_t *cfg_rl = app_config_get();
    bool auth_off = (cfg_rl && !cfg_rl->auth_enabled);
    int64_t now_us = esp_timer_get_time();
    int64_t lockout_remaining_us = 0;
    portENTER_CRITICAL(&s_login_mux);
    if (!auth_off && now_us < s_login_lockout_until_us) {
        lockout_remaining_us = s_login_lockout_until_us - now_us;
    }
    portEXIT_CRITICAL(&s_login_mux);
    if (lockout_remaining_us > 0) {
        int retry_s = (int)((lockout_remaining_us + 999999) / 1000000);
        if (retry_s < 1) retry_s = 1;
        char body[96];
        snprintf(body, sizeof(body),
                 "{\"error\":\"too many failed attempts, try again later\","
                 "\"retry_after_s\":%d}", retry_s);
        char retry_hdr[16];
        snprintf(retry_hdr, sizeof(retry_hdr), "%d", retry_s);
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Retry-After", retry_hdr);
        httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 256) {
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
            heap_caps_free(buf);
            httpd_resp_send_408(req);
            return ESP_OK;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!root) return send_400(req, "Invalid JSON");

    cJSON *pw_item = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(pw_item)) {
        cJSON_Delete(root);
        return send_400(req, "Missing 'password' string");
    }

    const char *pw = pw_item->valuestring;
    const app_config_t *cfg = app_config_get();

    /* Constant-time compare */
    size_t a = strlen(pw);
    size_t b = strlen(cfg->admin_password);
    unsigned char diff = (a != b) ? 1 : 0;
    size_t n = (a < b) ? a : b;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)pw[i] ^ (unsigned char)cfg->admin_password[i];
    }
    cJSON_Delete(root);

    if (diff != 0 || cfg->admin_password[0] == '\0') {
        /* Failed attempt: bump counter, engage lockout at threshold. */
        bool engaged = false;
        portENTER_CRITICAL(&s_login_mux);
        s_login_failures++;
        if (s_login_failures >= LOGIN_MAX_FAILURES) {
            s_login_lockout_until_us =
                esp_timer_get_time() + (int64_t)LOGIN_LOCKOUT_SEC * 1000000LL;
            s_login_failures = 0;
            engaged = true;
        }
        portEXIT_CRITICAL(&s_login_mux);
        if (engaged) {
            ESP_LOGW(TAG, "login lockout engaged for %d seconds", LOGIN_LOCKOUT_SEC);
        }
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid password\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Successful login: clear failure counter and any lockout. */
    portENTER_CRITICAL(&s_login_mux);
    s_login_failures = 0;
    s_login_lockout_until_us = 0;
    portEXIT_CRITICAL(&s_login_mux);

    const char *token = session_create();
    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             "session=%s; Path=/; HttpOnly; Max-Age=43200; SameSite=Lax",
             token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/logout — destroy session, clear cookie. Requires auth. */
esp_err_t logout_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char tok[80];
    if (session_extract_cookie(req, tok, sizeof(tok))) {
        session_destroy(tok);
    }
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "session=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
