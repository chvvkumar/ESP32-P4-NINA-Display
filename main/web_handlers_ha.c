/**
 * @file web_handlers_ha.c
 * @brief HTTP handlers for the Home Assistant page.
 *
 * Three routes, all ROUTE_AUTH_REQUIRED (see web_server.c) and each also
 * REQUIRE_AUTH() first-line (defense-in-depth):
 *   GET  /api/ha-config  -- return the 5 Home Assistant config fields
 *   POST /api/ha-config  -- validate, snapshot+save, invalidate cache, live-apply
 *   GET  /api/ha-probe   -- one-shot fetch of ONE entity (GET {base}/api/states/
 *                           <entity_id>) with Bearer auth, forward the entity JSON
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
        esp_err_t te = app_config_set_ha_tiles(tiles_item->valuestring);
        if (te != ESP_OK) ESP_LOGW(TAG, "ha tiles persist failed: %s", esp_err_to_name(te));
    }

    cJSON_Delete(root);

    /* Single atomic memcpy under the config mutex + NVS persist. validate_config
     * (inside save) clamps ha_update_interval_s to 5..300 and NUL-terminates
     * the char arrays. */
    app_config_save(cfg);

    /* Live apply. The parsed-tiles cache in ha_client must be dropped so the
     * next poll re-reads the new config; the page widget tree is rebuilt to the
     * new rows/tiles; and the page is shown/hidden per ha_enabled. The refresh
     * + enable calls touch LVGL, so they run under the display lock (they do NOT
     * self-lock -- matches json_page_refresh_config usage). */
    ha_client_invalidate_config_cache();
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        ha_page_refresh_config();
        nina_dashboard_set_ha_enabled(cfg->ha_enabled);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "HA config: display lock timeout; page refresh deferred to next poll");
    }

    /* Re-resolve navigation so the arbiter immediately re-evaluates the HA
     * page's availability (it just appeared/disappeared from the ladder). */
    nav_arbiter_notify_topology_changed();

    /* A runtime enable must start the poll task without a reboot; idempotent
     * (no-op when disabled or already running). */
    if (cfg->ha_enabled) {
        ha_ensure_task_running();
    }

    heap_caps_free(cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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

    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);
    char base[sizeof(cfg->ha_base_url)];
    char token[sizeof(cfg->ha_token)];
    strlcpy(base, cfg->ha_base_url, sizeof(base));
    strlcpy(token, cfg->ha_token, sizeof(token));
    heap_caps_free(cfg);

    /* Override base/token with the live (possibly unsaved) values sent as
     * request headers, so Probe works before Save. Headers (not query params)
     * keep the bearer token out of any request-URI log. Fall back to saved. */
    size_t hlen = httpd_req_get_hdr_value_len(req, "X-HA-BASE");
    if (hlen > 0 && hlen < sizeof(base)) {
        char hval[sizeof(base)];
        if (httpd_req_get_hdr_value_str(req, "X-HA-BASE", hval, sizeof(hval)) == ESP_OK) {
            strlcpy(base, hval, sizeof(base));
        }
    }
    hlen = httpd_req_get_hdr_value_len(req, "X-HA-TOKEN");
    if (hlen > 0 && hlen < sizeof(token)) {
        char tval[sizeof(token)];
        if (httpd_req_get_hdr_value_str(req, "X-HA-TOKEN", tval, sizeof(tval)) == ESP_OK) {
            strlcpy(token, tval, sizeof(token));
        }
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
