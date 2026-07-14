/**
 * @file web_handlers_json.c
 * @brief HTTP handlers for the JSON Display page.
 *
 * Three routes, all ROUTE_AUTH_REQUIRED (see web_server.c) and each also
 * REQUIRE_AUTH() first-line (defense-in-depth):
 *   GET  /api/json-config  -- return the 5 JSON Display config fields
 *   POST /api/json-config  -- validate, snapshot+save, invalidate cache, live-apply
 *   GET  /api/json-proxy   -- one-shot fetch of the configured URL (with auth
 *                             header), forward the raw JSON body to the browser
 *
 * Mirrors web_handlers_allsky.c (get + proxy) and web_handlers_image_display.c
 * (snapshot+save+live-apply POST). Uses its own endpoints so the ~6 KB tiles
 * blob stays off the main /api/config payload.
 */

#include "web_server_internal.h"
#include "http_fetch.h"
#include "json_client.h"          /* json_client_invalidate_config_cache */
#include "ui/nina_json.h"         /* json_page_refresh_config */
#include "ui/nina_dashboard.h"    /* nina_dashboard_set_json_enabled */
#include "ui/nina_nav_arbiter.h"  /* nav_arbiter_notify_topology_changed */
#include "tasks.h"                /* json_ensure_task_running */
#include "display_defs.h"         /* LVGL_LOCK_TIMEOUT_MS */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"          /* bsp_display_lock / bsp_display_unlock */
#include "esp_heap_caps.h"
#include <string.h>
#include <strings.h>              /* strncasecmp */

/* POST body cap: the tiles blob is json_tiles_config[6144]; JSON-escaped and
 * wrapped with url/header fields it stays well under this. PSRAM-backed. */
#define JSON_CONFIG_MAX_PAYLOAD  16384

/* Proxy response buffer (PSRAM), protective cap matching the device poll path.
 * Point the page at a CURATED endpoint (single HA entity or a template sensor),
 * NOT the full `/api/states` dump (~1.5 MB on a real HA). A larger response is
 * truncated; the config UI then reports a parse failure rather than the device
 * buffering megabytes on an httpd worker. */
#define JSON_PROXY_BUF_SIZE      262144

/**
 * @brief GET /api/json-config  -- return the JSON Display config fields.
 *
 * Mirrors allsky_config_get_handler.
 */
esp_err_t json_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root,   "json_enabled",           cfg->json_enabled);
    cJSON_AddStringToObject(root, "json_url",               cfg->json_url);
    cJSON_AddStringToObject(root, "json_auth_header",       cfg->json_auth_header);
    cJSON_AddNumberToObject(root, "json_update_interval_s", cfg->json_update_interval_s);
    cJSON_AddStringToObject(root, "json_tiles_config",      app_config_get_json_tiles());

    const char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
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

/**
 * @brief POST /api/json-config  -- persist the JSON Display config fields and
 *        apply them live.
 *
 * Snapshot+save pattern (project rule: never field-write the live &s_config):
 * heap-alloc a PSRAM copy of the config, set the 5 fields, app_config_save(),
 * then live-apply (invalidate the client's parsed-tiles cache + rebuild the
 * page widget tree + show/hide the page). app_config_t is ~7.6 KB, so the copy
 * lives in PSRAM -- never on the httpd stack (two copies overflow it -> panic).
 */
esp_err_t json_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = receive_json_body(req, JSON_CONFIG_MAX_PAYLOAD);
    if (root == NULL) {
        return ESP_OK;  /* error response already sent */
    }

    /* Length guards: reject over-long strings up front so a truncated blob is
     * never silently persisted. sizeof() reads the field width directly. */
    if (!validate_string_len(root, "json_url", sizeof(((app_config_t *)0)->json_url)) ||
        !validate_string_len(root, "json_auth_header", sizeof(((app_config_t *)0)->json_auth_header)) ||
        !validate_string_len(root, "json_tiles_config", JSON_TILES_CONFIG_MAX)) {
        cJSON_Delete(root);
        return send_400(req, "JSON Display field too long");
    }
    /* A URL, when present, must be an http(s) URL. Empty clears it. */
    cJSON *url_item = cJSON_GetObjectItem(root, "json_url");
    if (cJSON_IsString(url_item) && url_item->valuestring[0] != '\0') {
        const char *u = url_item->valuestring;
        bool http_scheme = (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0);
        if (!http_scheme || !validate_url_format(u)) {
            cJSON_Delete(root);
            return send_400(req, "json_url must start with http:// or https://");
        }
    }

    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);

    JSON_TO_BOOL(root,   "json_enabled",           cfg->json_enabled);
    JSON_TO_STRING(root, "json_url",               cfg->json_url);
    JSON_TO_STRING(root, "json_auth_header",       cfg->json_auth_header);
    JSON_TO_INT(root,    "json_update_interval_s", cfg->json_update_interval_s);

    /* Tiles no longer live in app_config_t; persist to the dedicated NVS key via
     * the setter, but only when the key is present so a scalar-only POST does not
     * blank existing tiles (mirrors JSON_TO_STRING's key-present semantics). The
     * setter clamps to JSON_TILES_CONFIG_MAX-1 and updates the getter cache so
     * the live-apply refresh below reads the new value. */
    cJSON *tiles_item = cJSON_GetObjectItem(root, "json_tiles_config");
    if (cJSON_IsString(tiles_item) && tiles_item->valuestring) {
        esp_err_t te = app_config_set_json_tiles(tiles_item->valuestring);
        if (te != ESP_OK) ESP_LOGW(TAG, "json tiles persist failed: %s", esp_err_to_name(te));
    }

    cJSON_Delete(root);

    /* Single atomic memcpy under the config mutex + NVS persist. validate_config
     * (inside save) clamps json_update_interval_s to 5..300 and NUL-terminates
     * the char arrays. */
    app_config_save(cfg);

    /* Live apply. The parsed-tiles cache in json_client must be dropped so the
     * next poll re-reads the new config; the page widget tree is rebuilt to the
     * new rows/tiles; and the page is shown/hidden per json_enabled. The refresh
     * + enable calls touch LVGL, so they run under the display lock (they do NOT
     * self-lock -- matches allsky_page_refresh_config usage). */
    json_client_invalidate_config_cache();
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        json_page_refresh_config();
        nina_dashboard_set_json_enabled(cfg->json_enabled);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "JSON config: display lock timeout; page refresh deferred to next poll");
    }

    /* Re-resolve navigation so the arbiter immediately re-evaluates the JSON
     * page's availability (it just appeared/disappeared from the ladder). */
    nav_arbiter_notify_topology_changed();

    /* A runtime enable must start the poll task without a reboot; idempotent
     * (no-op when disabled or already running). */
    if (cfg->json_enabled) {
        json_ensure_task_running();
    }

    heap_caps_free(cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief GET /api/json-proxy  -- fetch the configured JSON URL through the
 *        device (with the configured auth header) and forward the raw body.
 *
 * Lets the browser config UI's "Fetch JSON" button read the endpoint without a
 * CORS problem, and without exposing the auth header to the browser. Mirrors
 * allsky_proxy_get_handler: snapshot url+header into locals under a PSRAM
 * config copy, free it, then one-shot fetch (conn=NULL) on the httpd worker.
 *
 * Auth header: the configured "Name: value" line is forwarded verbatim via
 * opts.extra_header, so any scheme works ("Authorization: Bearer <token>",
 * "X-API-Key: <key>", etc.), matching the device poll path in json_client.
 */
esp_err_t json_proxy_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);
    char url[sizeof(cfg->json_url)];
    char auth_header[sizeof(cfg->json_auth_header)];
    strlcpy(url, cfg->json_url, sizeof(url));
    strlcpy(auth_header, cfg->json_auth_header, sizeof(auth_header));
    heap_caps_free(cfg);

    /* Let the config UI fetch a URL the user has typed but not yet Saved: the
     * "Fetch JSON" button sends the live field values as request headers
     * (X-JSON-URL / X-JSON-Auth), which override the saved config here. Passing
     * them as headers (not query params) keeps the bearer token out of any
     * request-URI log. Fall back to the saved config when absent. */
    size_t hlen = httpd_req_get_hdr_value_len(req, "X-JSON-URL");
    if (hlen > 0 && hlen < sizeof(url)) {
        char hval[sizeof(url)];
        if (httpd_req_get_hdr_value_str(req, "X-JSON-URL", hval, sizeof(hval)) == ESP_OK) {
            strlcpy(url, hval, sizeof(url));
        }
    }
    hlen = httpd_req_get_hdr_value_len(req, "X-JSON-AUTH");
    if (hlen > 0 && hlen < sizeof(auth_header)) {
        char aval[sizeof(auth_header)];
        if (httpd_req_get_hdr_value_str(req, "X-JSON-AUTH", aval, sizeof(aval)) == ESP_OK) {
            strlcpy(auth_header, aval, sizeof(auth_header));
        }
    }

    if (url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"JSON URL not configured\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool https = (strncmp(url, "https://", 8) == 0);
    http_fetch_opts_t opts = {
        .timeout_ms         = 3000,
        .use_tls_bundle     = https,
        .max_redirects      = 3,
        .max_response_bytes = JSON_PROXY_BUF_SIZE,
        /* Forward the configured auth header verbatim (any "Name: value" line,
         * not just Authorization: Bearer). http_fetch splits at the first ':'. */
        .extra_header       = (auth_header[0] != '\0') ? auth_header : NULL,
    };

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_fetch_text(url, &opts, &body, &body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JSON proxy: fetch failed for %s (%s)", url, esp_err_to_name(err));
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to reach JSON source\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, body_len);

    heap_caps_free(body);
    return ESP_OK;
}
