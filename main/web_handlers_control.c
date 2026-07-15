/**
 * @file web_handlers_control.c
 * @brief Generic Control API HTTP handlers.
 *
 * Endpoints (all query-string driven):
 *   GET  /api/control/list                       — enumerate all control items
 *   GET  /api/control/get?name=<name>            — read one item
 *   POST /api/control/toggle?name=<bool item>    — flip a bool item
 *   POST /api/control/cycle?name=<enum|int|page> — advance with wrap
 *   POST /api/control/set?name=<name>&value=<v>  — set absolute value
 *   POST /api/control/adjust?name=<name>&delta=<d> — add delta (no wrap)
 *
 * Config items use the standard snapshot+modify+save+apply path. The "page"
 * item is non-config and routes through the navigation arbiter.
 */

#include "web_server_internal.h"
#include "control_registry.h"
#include "ui/nina_nav_arbiter.h"   /* nav_arbiter_set_pin / nav_arbiter_is_pinned */
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"             /* esp_timer_get_time */
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdlib.h>
#include <limits.h>    /* INT_MAX, INT_MIN */

/* TAG is declared in web_server_internal.h; do not redeclare it. */

/* ===================================================================== */
/* Helpers                                                                */
/* ===================================================================== */

/* Parse an untrusted decimal query value with strtol, clamped to int range so a
 * huge/garbage input cannot trigger atoi() UB or silently wrap an int. */
static int ctrl_parse_int(const char *s)
{
    long lv = strtol(s, NULL, 10);
    if (lv > INT_MAX) {
        lv = INT_MAX;
    }
    if (lv < INT_MIN) {
        lv = INT_MIN;
    }
    return (int)lv;
}

/* Build one item JSON object (caller owns the returned cJSON*).
 *
 * The value is taken from @p cfg via the item's getter UNLESS @p value_override
 * is non-NULL, in which case *value_override is used verbatim. The override lets
 * the page set/cycle ops report the TARGET page id (the arbiter applies the move
 * asynchronously, so re-querying the live page would return the stale id). For
 * config items the caller passes a single pre-taken snapshot so a multi-item
 * list reflects one consistent view (no per-item re-snapshot TOCTOU). */
static cJSON *control_item_to_json(const control_item_t *it,
                                   const app_config_t *cfg,
                                   const int *value_override)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    int v = value_override ? *value_override : it->get(it, cfg);
    int emax = control_item_effective_max(it);

    cJSON_AddStringToObject(o, "name", it->name);

    const char *type_str = (it->type == CTRL_TYPE_BOOL) ? "bool" :
                           (it->type == CTRL_TYPE_PIN)  ? "bool" :
                           (it->type == CTRL_TYPE_INT)  ? "int"  :
                           (it->type == CTRL_TYPE_PAGE) ? "enum" : "enum";
    cJSON_AddStringToObject(o, "type", type_str);

    if (it->type == CTRL_TYPE_BOOL || it->type == CTRL_TYPE_PIN) {
        cJSON_AddBoolToObject(o, "value", v != 0);
    } else {
        cJSON_AddNumberToObject(o, "value", v);
    }

    const char *lbl = control_item_label(it, v);
    char numbuf[16];
    if (!lbl) {
        snprintf(numbuf, sizeof(numbuf), "%d", v);
        lbl = numbuf;
    }
    cJSON_AddStringToObject(o, "label", lbl);

    cJSON_AddNumberToObject(o, "min", it->vmin);
    cJSON_AddNumberToObject(o, "max", emax);
    cJSON_AddNumberToObject(o, "step", it->vstep);
    return o;
}

/* Read a query-string value for @p key into @p out. Returns false if absent. */
static bool ctrl_get_query_str(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char qbuf[256] = {0};
    out[0] = '\0';
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(qbuf, key, out, out_len) != ESP_OK) {
        return false;
    }
    return out[0] != '\0';
}

/* Send a pre-built item object as the JSON response (takes ownership: deletes
 * @p o before returning). */
static esp_err_t send_item_json(httpd_req_t *req, cJSON *o)
{
    if (!o) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *out = cJSON_PrintUnformatted(o);
    if (!out) {
        cJSON_Delete(o);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    cJSON_Delete(o);
    return ESP_OK;
}

/* Send one item using a fresh snapshot and its live getter value. */
static esp_err_t send_item(httpd_req_t *req, const control_item_t *it)
{
    /* app_config_t is ~20 KB; snapshot into PSRAM, never onto the httpd stack. */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);
    esp_err_t r = send_item_json(req, control_item_to_json(it, cfg, NULL));
    heap_caps_free(cfg);
    return r;
}

/* Send one item reporting an explicit @p value (used by page set/cycle to report
 * the target id, which the arbiter has not necessarily committed yet). */
static esp_err_t send_item_value(httpd_req_t *req, const control_item_t *it, int value)
{
    /* app_config_t is ~20 KB; snapshot into PSRAM, never onto the httpd stack. */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);
    esp_err_t r = send_item_json(req, control_item_to_json(it, cfg, &value));
    heap_caps_free(cfg);
    return r;
}

/* Commit a new value for a config item: clamp, snapshot, modify, save, apply. */
static void ctrl_commit_value(const control_item_t *it, int newval)
{
    int emax = control_item_effective_max(it);
    if (newval < it->vmin) {
        newval = it->vmin;
    }
    if (newval > emax) {
        newval = emax;
    }
    /* app_config_t is ~20 KB. Holding TWO copies on the HTTP task stack
     * overflows it (panic/reboot), so allocate both in PSRAM. Snapshot via the
     * out-param variant so no large stack return-temporary is materialized. */
    app_config_t *prev = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    app_config_t *cur  = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!prev || !cur) {
        heap_caps_free(prev);
        heap_caps_free(cur);
        ESP_LOGE(TAG, "ctrl_commit_value: PSRAM alloc failed");
        return;
    }
    app_config_get_snapshot_into(prev);
    *cur  = *prev;
    it->set(it, cur, newval);
    app_config_save(cur);
    if (it->apply) {
        it->apply(prev, cur);
    }
    heap_caps_free(prev);
    heap_caps_free(cur);
}

/* Read an item's current value from a fresh snapshot. */
static int ctrl_current_value(const control_item_t *it)
{
    /* app_config_t is ~20 KB; snapshot into PSRAM, never onto the httpd stack. */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        ESP_LOGE(TAG, "ctrl_current_value: PSRAM alloc failed");
        return it->vmin;
    }
    app_config_get_snapshot_into(cfg);
    int v = it->get(it, cfg);
    heap_caps_free(cfg);
    return v;
}

/* ===================================================================== */
/* Handlers                                                               */
/* ===================================================================== */

esp_err_t control_list_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Single snapshot for the whole list: every item reflects one consistent
     * config view (no per-item re-snapshot that could mix mid-write states).
     * app_config_t is ~20 KB; snapshot into PSRAM, never onto the httpd stack. */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        cJSON_Delete(arr);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    app_config_get_snapshot_into(cfg);

    for (int i = 0; i < control_registry_count(); i++) {
        const control_item_t *it = control_registry_get(i);
        if (!it) {
            continue;
        }
        cJSON *o = control_item_to_json(it, cfg, NULL);
        if (o) {
            cJSON_AddItemToArray(arr, o);
        }
    }

    heap_caps_free(cfg);

    char *out = cJSON_PrintUnformatted(arr);
    if (!out) {
        cJSON_Delete(arr);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    cJSON_Delete(arr);
    return ESP_OK;
}

esp_err_t control_get_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char name[64] = {0};
    if (!ctrl_get_query_str(req, "name", name, sizeof(name))) {
        return send_400(req, "missing name");
    }
    const control_item_t *it = control_registry_find(name);
    if (!it) {
        return send_400(req, "unknown name");
    }
    return send_item(req, it);
}

esp_err_t control_toggle_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char name[64] = {0};
    if (!ctrl_get_query_str(req, "name", name, sizeof(name))) {
        return send_400(req, "missing name");
    }
    const control_item_t *it = control_registry_find(name);
    if (!it) {
        return send_400(req, "unknown name");
    }
    if (it->type == CTRL_TYPE_PIN) {
        /* Non-config: flip the navigation pin via the arbiter (no snapshot/save). */
        nav_arbiter_set_pin(!nav_arbiter_is_pinned(), -1, -1,
                            esp_timer_get_time() / 1000);
        return send_item(req, it);
    }
    if (it->type != CTRL_TYPE_BOOL) {
        return send_400(req, "toggle requires a bool item");
    }
    int cur = ctrl_current_value(it);
    ctrl_commit_value(it, cur ? 0 : 1);
    return send_item(req, it);
}

esp_err_t control_cycle_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char name[64] = {0};
    if (!ctrl_get_query_str(req, "name", name, sizeof(name))) {
        return send_400(req, "missing name");
    }
    const control_item_t *it = control_registry_find(name);
    if (!it) {
        return send_400(req, "unknown name");
    }
    if (it->type == CTRL_TYPE_PAGE) {
        /* Report the target id the cycle navigated to, not the (async) live page. */
        int target = control_page_cycle_next();
        return send_item_value(req, it, target);
    }
    if (it->type != CTRL_TYPE_ENUM && it->type != CTRL_TYPE_INT) {
        return send_400(req, "cycle not supported for this item");
    }
    int cur = ctrl_current_value(it);
    int emax = control_item_effective_max(it);
    int newval = cur + it->vstep;
    if (newval > emax) {
        newval = it->vmin;  /* wrap */
    }
    ctrl_commit_value(it, newval);
    return send_item(req, it);
}

esp_err_t control_set_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char name[64] = {0};
    char valuebuf[32] = {0};
    if (!ctrl_get_query_str(req, "name", name, sizeof(name))) {
        return send_400(req, "missing name");
    }
    if (!ctrl_get_query_str(req, "value", valuebuf, sizeof(valuebuf))) {
        return send_400(req, "missing value");
    }
    const control_item_t *it = control_registry_find(name);
    if (!it) {
        return send_400(req, "unknown name");
    }

    int target;
    if (it->type == CTRL_TYPE_BOOL || it->type == CTRL_TYPE_PIN) {
        if (strcasecmp(valuebuf, "true") == 0) {
            target = 1;
        } else if (strcasecmp(valuebuf, "false") == 0) {
            target = 0;
        } else {
            target = (ctrl_parse_int(valuebuf) != 0) ? 1 : 0;
        }
    } else {
        target = ctrl_parse_int(valuebuf);
    }

    if (it->type == CTRL_TYPE_PIN) {
        /* Non-config: drive the navigation pin via the arbiter (no snapshot/save). */
        nav_arbiter_set_pin(target != 0, -1, -1, esp_timer_get_time() / 1000);
        return send_item(req, it);
    }

    if (it->type == CTRL_TYPE_PAGE) {
        if (!control_page_set_by_id(target)) {
            return send_400(req, "page not available");
        }
        /* Report the requested target id, not the (async) live page. */
        return send_item_value(req, it, target);
    }

    ctrl_commit_value(it, target);  /* clamps */
    return send_item(req, it);
}

esp_err_t control_adjust_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char name[64] = {0};
    char deltabuf[32] = {0};
    if (!ctrl_get_query_str(req, "name", name, sizeof(name))) {
        return send_400(req, "missing name");
    }
    if (!ctrl_get_query_str(req, "delta", deltabuf, sizeof(deltabuf))) {
        return send_400(req, "missing delta");
    }
    const control_item_t *it = control_registry_find(name);
    if (!it) {
        return send_400(req, "unknown name");
    }
    if (it->type == CTRL_TYPE_PAGE) {
        return send_400(req, "adjust not supported for page");
    }
    if (it->type == CTRL_TYPE_BOOL) {
        return send_400(req, "adjust not supported for bool");
    }
    if (it->type == CTRL_TYPE_PIN) {
        return send_400(req, "adjust not supported for nav_pinned");
    }

    int delta = ctrl_parse_int(deltabuf);
    int cur = ctrl_current_value(it);
    /* Compute in a wider type so cur+delta cannot signed-overflow before clamp. */
    long nv = (long)cur + (long)delta;
    if (nv > INT_MAX) {
        nv = INT_MAX;
    }
    if (nv < INT_MIN) {
        nv = INT_MIN;
    }
    ctrl_commit_value(it, (int)nv);  /* clamps to [vmin,emax], no wrap */
    return send_item(req, it);
}
