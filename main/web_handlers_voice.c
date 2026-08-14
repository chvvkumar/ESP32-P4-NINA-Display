/**
 * @file web_handlers_voice.c
 * @brief Web endpoints for custom voice clip upload / reset (voice_store).
 *
 * GET  /api/voice-clips          -> JSON inventory: store readiness, SPIFFS
 *                                   usage, custom-clip budget, and one entry
 *                                   per known clip {name, custom, size}.
 * POST /api/voice-clip?name=X    -> raw application/octet-stream body replaces
 *                                   the named clip. Rejected before buffering
 *                                   when the name is unknown or Content-Length
 *                                   exceeds the per-clip cap; voice_store_save
 *                                   errors map to specific 4xx/507 responses.
 * POST /api/voice-clip/reset?name=X  -> restore one clip to its built-in PCM.
 * POST /api/voice-clip/reset?all=1   -> restore every clip.
 */

#include "web_server_internal.h"
#include "voice_store.h"
#include "audio_alert.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

/* Receive step for the upload body; PSRAM destination, so keep the copies in
 * modest slices rather than one huge recv. */
#define VOICE_RECV_CHUNK 8192

/* True when @p name matches one of the built-in clip stems. */
static bool clip_name_known(const char *name)
{
    int n = audio_alert_clip_count();
    for (int i = 0; i < n; i++) {
        const char *c = audio_alert_clip_name(i);
        if (c && strcmp(c, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Send @p body as application/json with an explicit HTTP status line. */
static esp_err_t send_json_status(httpd_req_t *req, const char *status,
                                  const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* Map a negative voice_store_err_t to its HTTP error response. */
static esp_err_t send_store_error(httpd_req_t *req, int rc)
{
    switch (rc) {
        case VOICE_STORE_ERR_NOT_READY:
            return send_json_status(req, "503 Service Unavailable",
                                    "{\"error\":\"storage preparing\"}");
        case VOICE_STORE_ERR_UNKNOWN_CLIP:
            return send_400(req, "unknown clip");
        case VOICE_STORE_ERR_TOO_LARGE:
            return send_json_status(req, "413 Payload Too Large",
                                    "{\"error\":\"clip too large\"}");
        case VOICE_STORE_ERR_OVER_BUDGET:
            return send_json_status(req, "507 Insufficient Storage",
                                    "{\"error\":\"storage budget exceeded\"}");
        case VOICE_STORE_ERR_IO:
        default:
            return send_json_status(req, "500 Internal Server Error",
                                    "{\"error\":\"storage write failed\"}");
    }
}

// Handler listing every clip with its custom/built-in state and store stats
esp_err_t voice_clips_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    bool ready = voice_store_ready();
    size_t used = 0, total = 0, custom = 0;
    if (ready) {
        voice_store_stats(&used, &total, &custom);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON_AddBoolToObject(root, "formatting", voice_store_formatting());
    cJSON_AddNumberToObject(root, "spiffs_used", (double)used);
    cJSON_AddNumberToObject(root, "spiffs_total", (double)total);
    cJSON_AddNumberToObject(root, "custom_bytes", (double)custom);
    cJSON_AddNumberToObject(root, "budget", (double)VOICE_STORE_BUDGET);

    cJSON *clips = cJSON_AddArrayToObject(root, "clips");
    if (!clips) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int n = audio_alert_clip_count();
    for (int i = 0; i < n; i++) {
        const char *name = audio_alert_clip_name(i);
        if (!name) {
            continue;
        }
        cJSON *c = cJSON_CreateObject();
        if (!c) {
            break;
        }
        size_t sz = 0;
        bool is_custom = voice_store_is_custom(name, &sz);
        cJSON_AddStringToObject(c, "name", name);
        cJSON_AddBoolToObject(c, "custom", is_custom);
        cJSON_AddNumberToObject(c, "size", is_custom ? (double)sz : 0.0);
        cJSON_AddItemToArray(clips, c);
    }

    return send_json_response(req, root);
}

// Handler accepting a raw PCM upload that replaces the named clip
esp_err_t voice_clip_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char q[96] = {0}, name[32] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK ||
        name[0] == '\0') {
        return send_400(req, "missing name");
    }
    if (!clip_name_known(name)) {
        return send_400(req, "unknown clip");
    }
    if (!voice_store_ready()) {
        return send_json_status(req, "503 Service Unavailable",
                                "{\"error\":\"storage preparing\"}");
    }

    /* Reject oversized bodies from the Content-Length alone, BEFORE any
     * buffering. voice_store_save re-checks the same caps. */
    size_t cap = (strcmp(name, "boot_jingle") == 0) ? VOICE_STORE_CAP_JINGLE
                                                    : VOICE_STORE_CAP_CLIP;
    size_t len = req->content_len;
    if (len == 0) {
        return send_400(req, "missing body");
    }
    if (len & 1) {
        /* PCM16 bodies must be an even byte count; catch it here with a clear
         * 400 rather than letting voice_store map it to a misleading 413. */
        return send_400(req, "PCM length must be even");
    }
    if (len > cap) {
        return send_json_status(req, "413 Payload Too Large",
                                "{\"error\":\"clip too large\"}");
    }

    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t off = 0;
    int timeouts = 0;
    while (off < len) {
        size_t want = len - off;
        if (want > VOICE_RECV_CHUNK) {
            want = VOICE_RECV_CHUNK;
        }
        int r = httpd_req_recv(req, (char *)buf + off, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) {
            continue;  /* transient socket timeout; retry the same slice */
        }
        if (r <= 0) {
            heap_caps_free(buf);
            return ESP_FAIL;  /* connection aborted; httpd cleans up */
        }
        timeouts = 0;   /* progress made: only CONSECUTIVE timeouts abort */
        off += (size_t)r;
    }

    int rc = voice_store_save(name, buf, len);
    heap_caps_free(buf);
    if (rc != VOICE_STORE_OK) {
        return send_store_error(req, rc);
    }

    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"size\":%u}", (unsigned)len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// Handler restoring one clip (?name=X) or every clip (?all=1) to built-in PCM
esp_err_t voice_clip_reset_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char q[96] = {0}, val[32] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return send_400(req, "missing name or all");
    }

    int rc;
    if (httpd_query_key_value(q, "all", val, sizeof(val)) == ESP_OK &&
        strcmp(val, "1") == 0) {
        rc = voice_store_reset_all();
    } else if (httpd_query_key_value(q, "name", val, sizeof(val)) == ESP_OK &&
               val[0] != '\0') {
        if (!clip_name_known(val)) {
            return send_400(req, "unknown clip");
        }
        rc = voice_store_reset(val);
    } else {
        return send_400(req, "missing name or all");
    }

    if (rc != VOICE_STORE_OK) {
        return send_store_error(req, rc);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
