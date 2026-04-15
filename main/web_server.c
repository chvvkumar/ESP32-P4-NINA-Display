#include "web_server.h"
#include "web_server_internal.h"
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* ---- Session store ---- */
#define MAX_SESSIONS 8
#define SESSION_TOKEN_BYTES 32                         /* raw bytes */
#define SESSION_TOKEN_HEX_LEN (SESSION_TOKEN_BYTES * 2) /* 64 hex chars */
#define SESSION_TTL_SEC (12 * 3600)

typedef struct {
    char token[SESSION_TOKEN_HEX_LEN + 1];
    int64_t expires_us;
} session_t;

static session_t s_sessions[MAX_SESSIONS];
static portMUX_TYPE s_sessions_mux = portMUX_INITIALIZER_UNLOCKED;

static void hex_encode(const uint8_t *in, size_t in_len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[ in[i]       & 0xF];
    }
    out[in_len * 2] = '\0';
}

const char *session_create(void) {
    uint8_t raw[SESSION_TOKEN_BYTES];
    esp_fill_random(raw, sizeof(raw));
    char hex[SESSION_TOKEN_HEX_LEN + 1];
    hex_encode(raw, sizeof(raw), hex);

    int64_t now_us = esp_timer_get_time();
    int64_t expiry = now_us + (int64_t)SESSION_TTL_SEC * 1000000LL;

    const char *result = NULL;
    taskENTER_CRITICAL(&s_sessions_mux);
    /* Prefer empty slot; otherwise oldest */
    int target = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0') { target = i; break; }
        if (s_sessions[i].expires_us < oldest) {
            oldest = s_sessions[i].expires_us;
            target = i;
        }
    }
    memcpy(s_sessions[target].token, hex, sizeof(hex));
    s_sessions[target].expires_us = expiry;
    result = s_sessions[target].token;
    taskEXIT_CRITICAL(&s_sessions_mux);
    return result;
}

bool session_valid(const char *token) {
    if (!token || token[0] == '\0') return false;
    size_t len = strlen(token);
    if (len != SESSION_TOKEN_HEX_LEN) return false;
    int64_t now_us = esp_timer_get_time();
    bool ok = false;
    taskENTER_CRITICAL(&s_sessions_mux);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0') continue;
        if (s_sessions[i].expires_us <= now_us) {
            /* Opportunistic purge */
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            continue;
        }
        /* Constant-time compare */
        unsigned char diff = 0;
        for (size_t k = 0; k < SESSION_TOKEN_HEX_LEN; k++) {
            diff |= (unsigned char)s_sessions[i].token[k] ^ (unsigned char)token[k];
        }
        if (diff == 0) { ok = true; /* keep scanning to purge, but can break */ break; }
    }
    taskEXIT_CRITICAL(&s_sessions_mux);
    return ok;
}

void session_destroy(const char *token) {
    if (!token || token[0] == '\0') return;
    taskENTER_CRITICAL(&s_sessions_mux);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0') continue;
        if (strncmp(s_sessions[i].token, token, SESSION_TOKEN_HEX_LEN) == 0) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            break;
        }
    }
    taskEXIT_CRITICAL(&s_sessions_mux);
}

bool session_extract_cookie(httpd_req_t *req, char *out, size_t out_len) {
    if (out_len < SESSION_TOKEN_HEX_LEN + 1) return false;
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hdr_len == 0 || hdr_len > 1024) return false;
    char *buf = malloc(hdr_len + 1);
    if (!buf) return false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, hdr_len + 1) != ESP_OK) {
        free(buf);
        return false;
    }
    bool ok = false;
    /* Scan for "session=" allowing leading whitespace/semicolons */
    const char *p = buf;
    while (p && *p) {
        while (*p == ' ' || *p == ';') p++;
        if (strncmp(p, "session=", 8) == 0) {
            p += 8;
            size_t n = 0;
            while (p[n] && p[n] != ';' && n < out_len - 1) { out[n] = p[n]; n++; }
            out[n] = '\0';
            ok = (n == SESSION_TOKEN_HEX_LEN);
            break;
        }
        /* Skip to next cookie */
        const char *next = strchr(p, ';');
        if (!next) break;
        p = next + 1;
    }
    free(buf);
    return ok;
}

bool check_session(httpd_req_t *req) {
    /* When auth is disabled, every request is granted. Secrets are still
     * redacted in API responses (see backup_get_handler, config_get_handler). */
    const app_config_t *cfg = app_config_get();
    if (cfg && !cfg->auth_enabled) return true;

    char tok[SESSION_TOKEN_HEX_LEN + 1];
    if (!session_extract_cookie(req, tok, sizeof(tok))) return false;
    return session_valid(tok);
}

/**
 * @brief Send an auth-required response.
 *
 * For browser page requests (URI does not start with /api/), issue a 302 redirect
 * to /login so the user lands on the login page. For API requests, return 401
 * with a JSON body so fetch() callers can detect and redirect client-side.
 */
esp_err_t send_auth_required(httpd_req_t *req) {
    if (strncmp(req->uri, "/api/", 5) != 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"authentication required\"}";
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

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
    config.stack_size = 16384;
    config.max_uri_handlers = 43;
    config.max_open_sockets = 16;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 3;
    config.keep_alive_count = 3;
    config.enable_so_linger = true;
    config.linger_timeout = 1;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server!");
        return;
    }

    static const httpd_uri_t routes[] = {
        { "/",                     HTTP_GET,  root_get_handler, NULL },
        { "/favicon.ico",          HTTP_GET,  favicon_get_handler, NULL },
        { "/api/config",           HTTP_GET,  config_get_handler, NULL },
        { "/api/config",           HTTP_POST, config_post_handler, NULL },
        { "/api/brightness",       HTTP_POST, brightness_post_handler, NULL },
        { "/api/color-brightness", HTTP_POST, color_brightness_post_handler, NULL },
        { "/api/theme",            HTTP_POST, theme_post_handler, NULL },
        { "/api/widget-style",     HTTP_POST, widget_style_post_handler, NULL },
        { "/api/reboot",           HTTP_POST, reboot_post_handler, NULL },
        { "/api/factory-reset",    HTTP_POST, factory_reset_post_handler, NULL },
        { "/api/screenshot",       HTTP_GET,  screenshot_get_handler, NULL },
        { "/api/page",             HTTP_POST, page_post_handler, NULL },
        { "/api/screen-rotation", HTTP_POST, screen_rotation_post_handler, NULL },
        { "/api/version",          HTTP_GET,  version_get_handler, NULL },
        { "/api/ota",              HTTP_POST, ota_post_handler, NULL },
        { "/api/perf",             HTTP_GET,  perf_get_handler, NULL },
        { "/api/perf/reset",       HTTP_POST, perf_reset_post_handler, NULL },
        { "/api/config/apply",     HTTP_POST, config_apply_handler, NULL },
        { "/api/config/revert",    HTTP_POST, config_revert_handler, NULL },
        { "/api/check-update",     HTTP_POST, check_update_post_handler, NULL },
        { "/api/check-update-json", HTTP_GET, check_update_json_handler, NULL },
        { "/api/ota-github",       HTTP_POST, ota_github_post_handler, NULL },
        { "/api/allsky-config",    HTTP_GET,  allsky_config_get_handler, NULL },
        { "/api/allsky-proxy",     HTTP_GET,  allsky_proxy_get_handler, NULL },
        { "/api/spotify/config",         HTTP_GET,  spotify_config_get_handler, NULL },
        { "/api/spotify/config",         HTTP_POST, spotify_config_post_handler, NULL },
        { "/api/spotify/callback",       HTTP_GET,  spotify_callback_get_handler, NULL },
        { "/api/spotify/token-exchange", HTTP_POST, spotify_token_exchange_post_handler, NULL },
        { "/api/spotify/logout",         HTTP_POST, spotify_logout_post_handler, NULL },
        { "/api/spotify/status",         HTTP_GET,  spotify_status_get_handler, NULL },
        { "/api/spotify/control",        HTTP_POST, spotify_control_post_handler, NULL },
        { "/api/config/backup",          HTTP_GET,  backup_get_handler,   NULL },
        { "/api/config/restore",         HTTP_POST, restore_post_handler, NULL },
        { "/api/status",                 HTTP_GET,  status_get_handler, NULL },
        { "/api/nina/status",            HTTP_GET,  nina_status_get_handler, NULL },
        { "/api/crash",                  HTTP_GET,  crash_get_handler, NULL },
        { "/api/admin-password",         HTTP_POST, admin_password_post_handler, NULL },
        { "/login",                      HTTP_GET,  login_page_get_handler, NULL },
        { "/api/login",                  HTTP_POST, login_post_handler, NULL },
        { "/api/logout",                 HTTP_POST, logout_post_handler, NULL },
    };

    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "Web server started with %d routes",
             (int)(sizeof(routes)/sizeof(routes[0])));
}
