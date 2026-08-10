/**
 * @file web_handlers_ha.c
 * @brief HTTP handlers for the Home Assistant page.
 *
 * Four routes, all ROUTE_AUTH_REQUIRED (see web_server.c) and each also
 * REQUIRE_AUTH() first-line (defense-in-depth):
 *   GET  /api/ha-config  -- return the 5 Home Assistant config fields
 *   POST /api/ha-config  -- validate, snapshot+save, invalidate cache, live-apply
 *                           ("preview":true applies live WITHOUT persisting)
 *   GET  /api/ha-probe   -- one-shot fetch of ONE entity (GET {base}/api/states/
 *                           <entity_id>) with Bearer auth, forward the entity JSON
 *   GET  /api/ha-test    -- one-shot credentials check (GET {base}/api/config),
 *                           reports a coarse verdict instead of the raw body
 *
 * Mirrors web_handlers_json.c (config get/post + proxy). The HA page fetches
 * per-entity (GET /api/states/<entity_id>) rather than a single URL, so the
 * probe takes ?entity=<entity_id> and honors X-HA-BASE / X-HA-TOKEN override
 * headers so the config UI can probe before Save (mirrors X-JSON-URL /
 * X-JSON-AUTH in the JSON proxy). The ~6 KB tiles blob stays off the main
 * /api/config payload by using its own endpoints.
 */

#include "web_server_internal.h"
#include "ha_client.h"            /* ha_client_fetch_entity / ha_client_invalidate_config_cache */
#include "ui/nina_ha.h"           /* ha_page_refresh_config */
#include "ui/nina_dashboard.h"    /* nina_dashboard_set_ha_enabled */
#include "ui/nina_nav_arbiter.h"  /* nav_arbiter_notify_topology_changed */
#include "tasks.h"                /* ha_ensure_task_running */
#include "display_defs.h"         /* LVGL_LOCK_TIMEOUT_MS */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"          /* bsp_display_lock / bsp_display_unlock */
#include "esp_heap_caps.h"
#include <string.h>
#include <strings.h>              /* strncasecmp */

/* POST body cap: the tiles blob is ha_tiles_config[6144]; JSON-escaped and
 * wrapped with base/token/interval fields it stays well under this. PSRAM-backed. */
#define HA_CONFIG_MAX_PAYLOAD  16384

/**
 * @brief Apply the current Home Assistant config to the live UI, without
 *        touching persistence.
 *
 * Shared spine for every path that changes HA page config: the POST handler
 * (save and preview) and the backup restore path. Reads the values it applies
 * from the live config (app_config_get / app_config_get_ha_tiles), so the caller
 * must have already committed them there via app_config_save(),
 * app_config_apply_preview(), or app_config_set_ha_tiles[_ram]().
 *
 * Steps, in order: drop the client's parsed-tiles cache so the next poll re-reads
 * the config; rebuild the page widget tree to the new rows/tiles; show/hide the
 * page per @p enabled; re-resolve navigation (the page just appeared/disappeared
 * from the arbiter ladder); start the poll task if the page is enabled.
 *
 * The refresh + enable calls touch LVGL and do NOT self-lock, so they run under
 * the display lock (matches json_page_refresh_config usage). A lock timeout is
 * not fatal -- the next poll picks the config up.
 */
void ha_page_apply_live(bool enabled)
{
    ha_client_invalidate_config_cache();
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        ha_page_refresh_config();
        nina_dashboard_set_ha_enabled(enabled);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "HA config: display lock timeout; page refresh deferred to next poll");
    }

    nav_arbiter_notify_topology_changed();

    /* A runtime enable must start the poll task without a reboot; idempotent
     * (no-op when disabled or already running). */
    if (enabled) {
        ha_ensure_task_running();
    }
}

/**
 * @brief GET /api/ha-config  -- return the Home Assistant config fields.
 *
 * Mirrors json_config_get_handler. Returns the raw token (as the JSON page
 * returns its raw auth header) so the config UI can round-trip it into the
 * password field.
 */
esp_err_t ha_config_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    app_config_t *cfg = app_config_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(root,   "ha_enabled",           cfg->ha_enabled);
    cJSON_AddStringToObject(root, "ha_base_url",           cfg->ha_base_url);
    cJSON_AddStringToObject(root, "ha_token",              cfg->ha_token);
    cJSON_AddNumberToObject(root, "ha_update_interval_s",  cfg->ha_update_interval_s);
    cJSON_AddStringToObject(root, "ha_tiles_config",       app_config_get_ha_tiles());

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
 * @brief POST /api/ha-config  -- persist the Home Assistant config fields and
 *        apply them live.
 *
 * Snapshot+save pattern (project rule: never field-write the live &s_config):
 * heap-alloc a PSRAM copy of the config, set the 5 fields, app_config_save(),
 * then live-apply (invalidate the client's parsed-tiles cache + rebuild the
 * page widget tree + show/hide the page + start the poll task). app_config_t
 * is ~7.6 KB, so the copy lives in PSRAM -- never on the httpd stack (two
 * copies overflow it -> panic).
 *
 * Preview: a body carrying "preview":true runs the identical validation and
 * live-apply path but swaps both persist calls for their RAM-only twins --
 * app_config_apply_preview() instead of app_config_save(),
 * app_config_set_ha_tiles_ram() instead of app_config_set_ha_tiles(). Nothing
 * reaches NVS, so the panel shows the candidate config while the saved one is
 * untouched. The live-apply helpers read app_config_get() /
 * app_config_get_ha_tiles(), which is exactly what those two RAM-only writes
 * update, so preview needs no separate apply path. Neither RAM-only write
 * touches the unsaved-changes flag, so a preview never raises the web UI's
 * "unsaved changes" bar.
 */
esp_err_t ha_config_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *root = receive_json_body(req, HA_CONFIG_MAX_PAYLOAD);
    if (root == NULL) {
        return ESP_OK;  /* error response already sent */
    }

    /* Length guards: reject over-long strings up front so a truncated blob is
     * never silently persisted. sizeof() reads the field width directly. */
    if (!validate_string_len(root, "ha_base_url", sizeof(((app_config_t *)0)->ha_base_url)) ||
        !validate_string_len(root, "ha_token", sizeof(((app_config_t *)0)->ha_token)) ||
        !validate_string_len(root, "ha_tiles_config", HA_TILES_CONFIG_MAX)) {
        cJSON_Delete(root);
        return send_400(req, "Home Assistant field too long");
    }
    /* A base URL, when present, must be an http(s) URL. Empty clears it. */
    cJSON *base_item = cJSON_GetObjectItem(root, "ha_base_url");
    if (cJSON_IsString(base_item) && base_item->valuestring[0] != '\0') {
        const char *u = base_item->valuestring;
        bool http_scheme = (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0);
        if (!http_scheme || !validate_url_format(u)) {
            cJSON_Delete(root);
            return send_400(req, "ha_base_url must start with http:// or https://");
        }
    }

    /* Preview: apply live, persist nothing. Absent/false => save as before. */
    bool preview = cJSON_IsTrue(cJSON_GetObjectItem(root, "preview"));

    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);

    JSON_TO_BOOL(root,   "ha_enabled",           cfg->ha_enabled);
    JSON_TO_STRING(root, "ha_base_url",          cfg->ha_base_url);
    JSON_TO_STRING(root, "ha_token",             cfg->ha_token);
    JSON_TO_INT(root,    "ha_update_interval_s", cfg->ha_update_interval_s);

    /* Tiles no longer live in app_config_t; persist to the dedicated NVS key via
     * the setter, but only when the key is present so a scalar-only POST does not
     * blank existing tiles (mirrors JSON_TO_STRING's key-present semantics). The
     * setter clamps to HA_TILES_CONFIG_MAX-1 and updates the getter cache so the
     * live-apply refresh below reads the new value. */
    cJSON *tiles_item = cJSON_GetObjectItem(root, "ha_tiles_config");
    if (cJSON_IsString(tiles_item) && tiles_item->valuestring) {
        esp_err_t te = preview ? app_config_set_ha_tiles_ram(tiles_item->valuestring)
                               : app_config_set_ha_tiles(tiles_item->valuestring);
        if (te != ESP_OK) ESP_LOGW(TAG, "ha tiles apply failed: %s", esp_err_to_name(te));
    }

    cJSON_Delete(root);

    /* Single atomic memcpy under the config mutex, + NVS persist unless preview.
     * validate_config (inside both save and apply) clamps ha_update_interval_s
     * to 5..300 and NUL-terminates the char arrays. The _preview variant also
     * leaves the unsaved-changes flag untouched. */
    if (preview) {
        app_config_apply_preview(cfg);
    } else {
        app_config_save(cfg);
    }

    /* Live apply (cache drop + widget rebuild + show/hide + nav + task). */
    ha_page_apply_live(cfg->ha_enabled);

    heap_caps_free(cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, preview ? "{\"success\":true,\"preview\":true}"
                                 : "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Buffer sizes for the resolved credentials, taken from the config fields. */
#define HA_BASE_BUF   sizeof(((app_config_t *)0)->ha_base_url)
#define HA_TOKEN_BUF  sizeof(((app_config_t *)0)->ha_token)

/**
 * Resolve the base URL + token for an outbound HA request: start from the saved
 * config, then override each with the live (possibly unsaved) value sent as an
 * X-HA-BASE / X-HA-TOKEN request header, so Probe and Test work before Save.
 * Headers (not query params) keep the bearer token out of any request-URI log.
 *
 * @p base must be HA_BASE_BUF bytes, @p token HA_TOKEN_BUF bytes; both are always
 * NUL-terminated on a true return. Returns false only when the PSRAM config copy
 * could not be allocated (caller sends 500). The config copy is heap-allocated in
 * PSRAM and freed before returning -- app_config_t is ~7.6 KB and must never sit
 * on the httpd stack.
 */
static bool ha_resolve_credentials(httpd_req_t *req, char *base, char *token)
{
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        return false;
    }
    app_config_get_snapshot_into(cfg);
    strlcpy(base, cfg->ha_base_url, HA_BASE_BUF);
    strlcpy(token, cfg->ha_token, HA_TOKEN_BUF);
    heap_caps_free(cfg);

    size_t hlen = httpd_req_get_hdr_value_len(req, "X-HA-BASE");
    if (hlen > 0 && hlen < HA_BASE_BUF) {
        char hval[HA_BASE_BUF];
        if (httpd_req_get_hdr_value_str(req, "X-HA-BASE", hval, sizeof(hval)) == ESP_OK) {
            strlcpy(base, hval, HA_BASE_BUF);
        }
    }
    hlen = httpd_req_get_hdr_value_len(req, "X-HA-TOKEN");
    if (hlen > 0 && hlen < HA_TOKEN_BUF) {
        char tval[HA_TOKEN_BUF];
        if (httpd_req_get_hdr_value_str(req, "X-HA-TOKEN", tval, sizeof(tval)) == ESP_OK) {
            strlcpy(token, tval, HA_TOKEN_BUF);
        }
    }
    return true;
}

/**
 * @brief GET /api/ha-probe?entity=<entity_id>  -- fetch one HA entity through
 *        the device and forward the parsed entity JSON.
 *
 * Lets the browser config UI's "Probe" button read a single entity (state +
 * attributes) to build the attribute dropdown, without a CORS problem and
 * without exposing the token to the browser. Base+token come from saved config,
 * overridden by X-HA-BASE / X-HA-TOKEN request headers when present so the
 * probe works before Save (mirrors X-JSON-URL / X-JSON-AUTH in json_proxy).
 */
esp_err_t ha_probe_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char base[HA_BASE_BUF];
    char token[HA_TOKEN_BUF];
    if (!ha_resolve_credentials(req, base, token)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Read the ?entity=<entity_id> query parameter. */
    char entity[160] = {0};
    char qbuf[256] = {0};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "entity", entity, sizeof(entity));
    }
    if (entity[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"entity parameter required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (base[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Home Assistant base URL not configured\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *ent = ha_client_fetch_entity(base, token, entity);
    if (ent == NULL) {
        ESP_LOGW(TAG, "HA probe: fetch failed for entity %s", entity);
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to reach Home Assistant\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const char *body = cJSON_PrintUnformatted(ent);
    if (body == NULL) {
        cJSON_Delete(ent);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);

    free((void *)body);
    cJSON_Delete(ent);
    return ESP_OK;
}

/**
 * @brief GET /api/ha-test  -- verify the Home Assistant base URL + token.
 *
 * Fetches GET {base}/api/config (the smallest authenticated HA endpoint that
 * identifies the server) and reports a coarse verdict instead of the raw body,
 * so the config UI can tell the three failure modes apart: host not reachable,
 * token rejected, and reachable-but-not-Home-Assistant. Base+token come from
 * saved config, overridden by X-HA-BASE / X-HA-TOKEN when present, so Test works
 * before Save (same resolution as Probe -- see ha_resolve_credentials).
 *
 * Always answers 200 with the verdict in the body (the one exception is an
 * unconfigured base URL -> 400 {"error":...}, matching ha-probe):
 *   {"ok":bool, "stage":"unreachable"|"auth"|"not_ha"|"ok",
 *    "status":<upstream HTTP status, 0 if none>, "version":"", "location_name":""}
 * version/location_name are populated only when stage=="ok". The token is never
 * echoed, in the response or in a log line.
 */
esp_err_t ha_test_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char base[HA_BASE_BUF];
    char token[HA_TOKEN_BUF];
    if (!ha_resolve_credentials(req, base, token)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (base[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Home Assistant base URL not configured\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* status 0 == no response at all (DNS/connect/timeout). */
    cJSON *upstream = NULL;
    int status = ha_client_fetch_config(base, token, &upstream);

    bool ok = false;
    const char *stage = "not_ha";
    const char *version = "";
    const char *location_name = "";

    if (status == 0) {
        stage = "unreachable";
    } else if (status == 401 || status == 403) {
        stage = "auth";
    } else if (status == 200) {
        /* A real HA /api/config always carries "version". Anything else on 200
         * (wrong host, captive portal, reverse proxy error page) is not_ha. */
        cJSON *v = upstream ? cJSON_GetObjectItem(upstream, "version") : NULL;
        if (cJSON_IsString(v) && v->valuestring != NULL && v->valuestring[0] != '\0') {
            ok = true;
            stage = "ok";
            version = v->valuestring;
            cJSON *ln = cJSON_GetObjectItem(upstream, "location_name");
            if (cJSON_IsString(ln) && ln->valuestring != NULL) {
                location_name = ln->valuestring;
            }
        }
    }

    ESP_LOGI(TAG, "HA test: %s (stage=%s, status=%d)", base, stage, status);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        if (upstream) {
            cJSON_Delete(upstream);
        }
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    /* AddString copies, so the borrowed pointers into `upstream` are safe here. */
    cJSON_AddBoolToObject(root,   "ok",            ok);
    cJSON_AddStringToObject(root, "stage",         stage);
    cJSON_AddNumberToObject(root, "status",        status);
    cJSON_AddStringToObject(root, "version",       version);
    cJSON_AddStringToObject(root, "location_name", location_name);
    if (upstream) {
        cJSON_Delete(upstream);
    }

    const char *out = cJSON_PrintUnformatted(root);
    if (out == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);

    free((void *)out);
    cJSON_Delete(root);
    return ESP_OK;
}
