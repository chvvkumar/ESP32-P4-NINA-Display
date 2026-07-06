#include "web_server.h"
#include "web_server_internal.h"
#include "web_route_auth.h"
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ui/nina_setup_screen.h" /* is_setup_mode() */

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

    /* Cookie path: a valid session cookie grants access without touching the
     * lockout (a browser presenting a good cookie is not a brute-force attempt). */
    char tok[SESSION_TOKEN_HEX_LEN + 1];
    if (session_extract_cookie(req, tok, sizeof(tok)) && session_valid(tok)) {
        return true;
    }

    /* Header path (stateless clients: automation, macro keypads). Only reached
     * when there is no valid cookie. Carries the admin password in the
     * X-Auth-Password header and is fed into the SAME login lockout so it is
     * not an unthrottled brute-force bypass. */
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "X-Auth-Password");
    if (hdr_len == 0) return false;  /* header absent -> no lockout change */
    /* Cap to admin_password capacity; anything longer cannot match anyway. */
    if (hdr_len > sizeof(cfg->admin_password) - 1) return false;

    if (auth_is_locked_out()) return false;  /* behave as locked; do not compare */

    char hdr[sizeof(cfg->admin_password)];  /* 32 chars + NUL */
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Password",
                                    hdr, sizeof(hdr)) != ESP_OK) {
        return false;  /* truncation/error -> no lockout change */
    }

    /* Constant-time compare (same style as login_post_handler): iterate over
     * max(a,b) with clamped indexing so the loop count does not depend on the
     * submitted password length, and do not early-return on first mismatch. */
    size_t a = strlen(hdr);
    size_t b = strlen(cfg->admin_password);
    unsigned char diff = (a != b) ? 1 : 0;
    size_t maxn = (a > b) ? a : b;
    for (size_t i = 0; i < maxn; i++) {
        unsigned char ca = (i < a) ? (unsigned char)hdr[i] : 0;
        unsigned char cb = (i < b) ? (unsigned char)cfg->admin_password[i] : 0;
        diff |= ca ^ cb;
    }

    if (diff != 0 || cfg->admin_password[0] == '\0') {
        auth_note_failure();
        return false;
    }
    auth_note_success();
    return true;
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

/**
 * @brief One route table entry: the httpd_uri_t plus its auth classification.
 *
 * Aggregate-initializing only the first two/three fields of `uri` and
 * omitting `.auth` entirely yields ROUTE_AUTH_REQUIRED (enum value 0) --
 * a new route is secured by default unless explicitly marked otherwise.
 */
typedef struct {
    httpd_uri_t uri;
    route_auth_class_t auth;
} route_entry_t;

/**
 * @brief Single registration-point auth gate. Registered as the handler for
 * every route; the real handler is reached only after route_auth_allows()
 * says so. req->user_ctx points at the owning route_entry_t (set at
 * registration time below), so the classification and real handler are
 * always looked up together -- no per-handler auth code to forget.
 */
static esp_err_t auth_gate_handler(httpd_req_t *req)
{
    const route_entry_t *entry = (const route_entry_t *)req->user_ctx;
    bool setup_mode = is_setup_mode();
    bool authed = false;

    switch (entry->auth) {
        case ROUTE_PUBLIC:
            /* Never touch check_session() here: PUBLIC routes must not be
             * able to trip the X-Auth-Password lockout counter. */
            return entry->uri.handler(req);
        case ROUTE_SETUP_EXEMPT:
            if (setup_mode) {
                return entry->uri.handler(req);
            }
            authed = check_session(req);
            break;
        case ROUTE_AUTH_REQUIRED:
        default:
            authed = check_session(req);
            break;
    }

    if (!route_auth_allows(entry->auth, setup_mode, authed)) {
        return send_auth_required(req);
    }
    return entry->uri.handler(req);
}

void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;
    config.max_uri_handlers = 69;
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

    /* Auth classification: every entry defaults to ROUTE_AUTH_REQUIRED
     * (fail-closed) unless explicitly marked ROUTE_PUBLIC or
     * ROUTE_SETUP_EXEMPT below. See main/web_route_auth.h for the truth
     * table auth_gate_handler() evaluates via route_auth_allows(). */
    static const route_entry_t routes[] = {
        { { "/",                     HTTP_GET,  root_get_handler, NULL }, ROUTE_SETUP_EXEMPT },
        { { "/ui/fragment",          HTTP_GET,  ui_fragment_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/favicon.ico",          HTTP_GET,  favicon_get_handler, NULL }, ROUTE_PUBLIC },
        { { "/api/config",           HTTP_GET,  config_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/config",           HTTP_POST, config_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/brightness",       HTTP_POST, brightness_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/color-brightness", HTTP_POST, color_brightness_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/theme",            HTTP_POST, theme_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/widget-style",     HTTP_POST, widget_style_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/reboot",           HTTP_POST, reboot_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/factory-reset",    HTTP_POST, factory_reset_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/screenshot",       HTTP_GET,  screenshot_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/page",             HTTP_POST, page_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/screen-rotation", HTTP_POST, screen_rotation_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/version",          HTTP_GET,  version_get_handler, NULL }, ROUTE_PUBLIC },
        { { "/api/ota",              HTTP_POST, ota_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/perf",             HTTP_GET,  perf_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/perf/reset",       HTTP_POST, perf_reset_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/config/apply",     HTTP_POST, config_apply_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/config/revert",    HTTP_POST, config_revert_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/check-update",     HTTP_POST, check_update_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/check-update-json", HTTP_GET, check_update_json_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/ota-github",       HTTP_POST, ota_github_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/allsky-config",    HTTP_GET,  allsky_config_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/allsky-proxy",     HTTP_GET,  allsky_proxy_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/spotify/config",         HTTP_GET,  spotify_config_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/spotify/config",         HTTP_POST, spotify_config_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/spotify/callback",       HTTP_GET,  spotify_callback_get_handler, NULL }, ROUTE_PUBLIC },
        { { "/api/spotify/token-exchange", HTTP_POST, spotify_token_exchange_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/spotify/logout",         HTTP_POST, spotify_logout_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/spotify/status",         HTTP_GET,  spotify_status_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/spotify/control",        HTTP_POST, spotify_control_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/image-display-config",   HTTP_GET,  image_display_config_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/image-display-config",   HTTP_POST, image_display_config_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/config/backup",          HTTP_GET,  backup_get_handler,   NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/config/restore",         HTTP_POST, restore_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/status",                 HTTP_GET,  status_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/nina/status",            HTTP_GET,  nina_status_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/crash",                  HTTP_GET,  crash_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/weather",                HTTP_GET,  weather_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/events",                 HTTP_GET,  events_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/events/clear",           HTTP_POST, events_clear_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/admin-password",         HTTP_POST, admin_password_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/login",                      HTTP_GET,  login_page_get_handler, NULL }, ROUTE_PUBLIC },
        { { "/api/login",                  HTTP_POST, login_post_handler, NULL }, ROUTE_PUBLIC },
        { { "/api/logout",                 HTTP_POST, logout_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/wifi/scan",              HTTP_GET,  wifi_scan_get_handler, NULL }, ROUTE_SETUP_EXEMPT },
        { { "/api/wifi/setup",         HTTP_POST, wifi_setup_post_handler, NULL }, ROUTE_SETUP_EXEMPT },
        { { "/api/wifi/status",        HTTP_GET,  wifi_status_get_handler, NULL }, ROUTE_SETUP_EXEMPT },
        { { "/api/auth/status",        HTTP_GET,  auth_status_get_handler, NULL }, ROUTE_PUBLIC },
        { { "/api/logs",               HTTP_GET,  logs_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/logs/clear",         HTTP_POST, logs_clear_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/crashlog",           HTTP_GET,  crashlog_get_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/crashlog/clear",     HTTP_POST, crashlog_clear_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/coredump",           HTTP_GET,  coredump_get_handler,        NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/coredump/info",      HTTP_GET,  coredump_info_get_handler,   NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/coredump/clear",     HTTP_POST, coredump_clear_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/pages",              HTTP_GET,  pages_get_handler,     NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/navigate",           HTTP_GET,  navigate_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/navigate",           HTTP_POST, navigate_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/nav/pin",            HTTP_POST, nav_pin_post_handler,  NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/control/list",       HTTP_GET,  control_list_get_handler,    NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/control/get",        HTTP_GET,  control_get_get_handler,     NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/control/toggle",     HTTP_POST, control_toggle_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/control/cycle",      HTTP_POST, control_cycle_post_handler,  NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/control/set",        HTTP_POST, control_set_post_handler,    NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/control/adjust",     HTTP_POST, control_adjust_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
        { { "/api/image-display/refresh", HTTP_POST, image_display_refresh_post_handler, NULL }, ROUTE_AUTH_REQUIRED },
    };

    /* Keep config.max_uri_handlers (set to 69 above) in sync with the route
     * table; a route that overflows it would be silently dropped at
     * registration. Bump both together when adding routes. */
    _Static_assert(sizeof(routes) / sizeof(routes[0]) <= 69,
                   "max_uri_handlers too small for route table");

    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++) {
        httpd_uri_t gated = routes[i].uri;
        gated.handler = auth_gate_handler;
        gated.user_ctx = (void *)&routes[i];
        httpd_register_uri_handler(server, &gated);
    }

    ESP_LOGI(TAG, "Web server started with %d routes",
             (int)(sizeof(routes)/sizeof(routes[0])));
}
