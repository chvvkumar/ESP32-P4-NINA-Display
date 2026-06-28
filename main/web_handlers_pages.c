#include "web_server_internal.h"
#include "ui/page_registry.h"
#include "ui/nina_nav_arbiter.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdlib.h>

static const char *PAGES_TAG __attribute__((unused)) = "web_pages";

/* Map a page_ref_kind_t to its stable external string. */
static const char *page_kind_str(page_ref_kind_t kind)
{
    switch (kind) {
        case PAGE_REF_KIND_IMAGE_SOURCE: return "image_source";
        case PAGE_REF_KIND_OVERLAY:      return "overlay";
        case PAGE_REF_KIND_PAGE:
        default:                         return "page";
    }
}

/* GET /api/pages — enumerate the full page registry as a JSON array. */
esp_err_t pages_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    for (int i = 0; i < page_ref_count(); i++) {
        const page_ref_entry_t *e = page_ref_get(i);
        if (!e) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(arr);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        cJSON_AddNumberToObject(o, "id", (double)e->id);
        cJSON_AddStringToObject(o, "slug", e->slug ? e->slug : "");
        cJSON_AddStringToObject(o, "label", e->label ? e->label : "");
        cJSON_AddStringToObject(o, "kind", page_kind_str(e->kind));
        cJSON_AddStringToObject(o, "category", e->category ? e->category : "");
        cJSON_AddBoolToObject(o, "targetable", e->targetable);
        cJSON_AddBoolToObject(o, "available", page_ref_is_available(e->id));
        cJSON_AddItemToArray(arr, o);
    }

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

/* GET|POST /api/navigate?ref=<slug>  or  ?id=<page_ref id>
 * Issues a USER-claim navigation to the named page. Query-string based, so the
 * same handler serves both methods. */
esp_err_t navigate_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char qbuf[160] = {0};
    char ref[64] = {0};
    char idbuf[16] = {0};
    bool have_ref = false;
    bool have_id = false;

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        if (httpd_query_key_value(qbuf, "ref", ref, sizeof(ref)) == ESP_OK &&
            ref[0] != '\0') {
            have_ref = true;
        }
        if (httpd_query_key_value(qbuf, "id", idbuf, sizeof(idbuf)) == ESP_OK &&
            idbuf[0] != '\0') {
            have_id = true;
        }
    }

    const page_ref_entry_t *entry = NULL;
    if (have_ref) {
        entry = page_ref_by_slug(ref);
    } else if (have_id) {
        entry = page_ref_by_id((page_ref_t)atoi(idbuf));
    } else {
        return send_400(req, "missing ref or id");
    }

    if (!entry) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown page");
        return ESP_OK;
    }

    if (!entry->targetable) {
        return send_400(req, "page is not a navigation target");
    }

    if (!page_ref_is_available(entry->id)) {
        return send_400(req, "page is not currently available");
    }

    page_ref_navigate(entry->id);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* POST /api/nav/pin?on=<1|0|true|false>[&id=<page_ref id>]
 * Engage or release the in-memory navigation pin. While pinned the arbiter holds
 * the selected page and suspends all automatic page changes. RUNTIME ONLY: the
 * pin resets to off on reboot (no config field). Optional id selects the page to
 * pin (resolved via page_ref_navigate); omitted pins the current page. */
esp_err_t nav_pin_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char qbuf[160] = {0};
    char onbuf[16] = {0};
    char idbuf[16] = {0};
    bool have_on = false;
    bool have_id = false;

    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        if (httpd_query_key_value(qbuf, "on", onbuf, sizeof(onbuf)) == ESP_OK &&
            onbuf[0] != '\0') {
            have_on = true;
        }
        if (httpd_query_key_value(qbuf, "id", idbuf, sizeof(idbuf)) == ESP_OK &&
            idbuf[0] != '\0') {
            have_id = true;
        }
    }

    if (!have_on) {
        return send_400(req, "missing on");
    }

    bool on;
    if (strcasecmp(onbuf, "true") == 0 || strcmp(onbuf, "1") == 0) {
        on = true;
    } else if (strcasecmp(onbuf, "false") == 0 || strcmp(onbuf, "0") == 0) {
        on = false;
    } else {
        return send_400(req, "invalid on");
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (on) {
        if (have_id) {
            /* Bounds-checked parse: reject non-numeric (endptr) and out-of-range
             * ids before casting to page_ref_t. Mirrors ctrl_parse_int's strtol
             * style; atoi() would treat garbage as 0 and is UB on overflow. */
            char *end = NULL;
            long idv = strtol(idbuf, &end, 10);
            if (end == idbuf || *end != '\0' || idv < 0 ||
                idv >= (long)PAGE_REF_ID_MAX) {
                return send_400(req, "invalid id");
            }
            /* Move the USER selection to the requested page first, then pin it. */
            if (!page_ref_navigate((page_ref_t)idv)) {
                return send_400(req, "page not available");
            }
        }
        nav_arbiter_set_pin(true, -1, -1, now_ms);
    } else {
        nav_arbiter_set_pin(false, -1, -1, now_ms);
    }

    char resp[24];
    snprintf(resp, sizeof(resp), "{\"pinned\":%s}",
             nav_arbiter_is_pinned() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
