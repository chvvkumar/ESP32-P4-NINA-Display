/**
 * @file octoprint_client.c
 * @brief Read-only OctoPrint REST client. See octoprint_client.h for scope.
 *
 * Every HTTP request goes through http_fetch (text for JSON, binary for
 * images) with one module-static keep-alive slot, exactly as json_client.c and
 * ha_client.c do. Image decode goes through jpeg_utils' stb-backed decoder,
 * which is format-agnostic (it feeds stbi_load_from_memory) and already lands
 * a 128-byte-aligned, cache-flushed RGB565 buffer in PSRAM -- the same path
 * goes_client.c uses to hand a frame to the Image Display page.
 *
 * Single-owner: only octoprint_poll_task calls octoprint_client_poll(), so the
 * tier/cache statics below (slow-tier timestamp, DLP latch, cached image key)
 * need no locking. The two exceptions: the published octoprint_data_t (its own
 * mutex) and the keep-alive conn slots, whose destruction is serialised by
 * s_conn_mux because page leave can require the UI coordinator task to destroy
 * them while the poll task sleeps (drained-parked slots hold open sockets).
 */

#include "octoprint_client.h"
#include "http_fetch.h"
#include "jpeg_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "octoprint";

#define OCTO_HTTP_TIMEOUT_MS   8000
#define OCTO_JSON_MAX          65536          /* /api/job and friends are tiny */
#define OCTO_IMAGE_MAX         (512 * 1024)   /* thumbnail ~45KB, webcam ~34KB */
#define OCTO_IMG_MAX_DIM       1024           /* reject anything larger        */
#define OCTO_CONN_TIER_MS      30000          /* /api/connection slow tier     */
#define OCTO_MAX_BASE_LEN      128            /* octoprint_url[128]            */
#define OCTO_MAX_GCODE_PATH    128            /* longest job path we will encode */

/* Persistent keep-alive slot for the JSON fetches; lazily created on the
 * first poll. A drained-parked slot holds an OPEN socket, and the whole poll
 * loop is page-gated, so both slots are destroyed once the page is left (see
 * conn_teardown_if_page_left) instead of living forever. */
static http_fetch_conn_t *s_conn = NULL;

/* Separate keep-alive slot for the binary image fetches. Not shared with
 * s_conn: the snapshot may live on a third-party host, and text/binary slots
 * must not be mixed (see http_fetch_binary_opts_t.conn). Created lazily on the
 * first image fetch, destroyed with s_conn on page leave. */
static http_fetch_conn_t *s_img_conn = NULL;

/* Serialises conn-slot destruction against a poll in flight. The poll task
 * holds this for the whole poll body; set_page_active() (UI coordinator task)
 * try-takes it with zero wait to destroy the slots when the page is left while
 * the poll task is asleep -- the only window the poll task's own exit teardown
 * cannot cover, because the page-gated loop never runs again. Created in
 * octoprint_client_init(). */
static SemaphoreHandle_t s_conn_mux = NULL;

/* Image pipeline gate. Set from the UI/coordinator task, read here -- atomic
 * because it crosses cores, same as the page-active flags in tasks.c. */
static _Atomic bool s_page_active = false;

/* DisplayLayerProgress availability latch: 0 = unprobed (ask this cycle),
 * 1 = available, -1 = 404'd (stop asking until the next reconnect). */
static int8_t s_dlp = 0;

/* Slow-tier bookkeeping for /api/connection. The last value is cached here so
 * the published conn_state survives the cycles that do not re-fetch it. */
static int64_t s_conn_tier_ms = 0;
static char    s_conn_state[OCTO_CONN_LEN] = "";

/* Was the previous cycle connected? A false->true edge re-arms the DLP probe. */
static bool s_was_connected = false;

/* Which gcode file the currently held thumbnail belongs to, and which image
 * source produced the held buffer. Both reset when the page deactivates.
 * Sized to match the gcode-path buffer so the copy can never truncate. */
static char  s_image_key[OCTO_MAX_GCODE_PATH + 1] = "";
static int8_t s_image_src = -1;

/* Bumped by every octoprint_client_note_image_source_switch(). A poll pass
 * snapshots it before its first fetch and re-reads it at publish time: a
 * difference means the source changed while the pass was in flight, so what it
 * fetched belongs to the old source and must be dropped, not published. */
static _Atomic uint32_t s_src_gen = 0;

/* Consecutive thumbnail failures for s_thumb_fail_key. s_image_key is stamped
 * only on a published success, so without this a file whose thumbnail is
 * genuinely missing would re-fetch metadata every cycle. The counter resets when
 * the printing file changes or the page is left. */
#define OCTO_THUMB_MAX_FAILS 3
static char   s_thumb_fail_key[OCTO_MAX_GCODE_PATH + 1] = "";
static int8_t s_thumb_fails = 0;

/* Forward declarations (prototype-before-use under -Werror). */
static void   reset_defaults(octoprint_data_t *data);
static bool   copy_base(const char *base_url, char *out, size_t out_len);
static void   node_str(const cJSON *node, char *out, size_t out_len);
static int    node_int(const cJSON *node, int def);
static float  node_float(const cJSON *node, float def);
static cJSON *fetch_json(const char *base, const char *hdr, const char *path,
                         int *status_out);
static bool   url_encode_segment(const char *src, char *dst, size_t dst_len);
static bool   fetch_image_bytes(const char *url, const char *hdr,
                                uint8_t **out_buf, size_t *out_len);
static bool   decode_image(const uint8_t *buf, size_t len, const char *what,
                           uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h);
static bool   resolve_thumbnail_path(const char *base, const char *hdr,
                                     const char *origin, const char *file_path,
                                     char *out, size_t out_len);
static bool   fetch_thumbnail(const char *base, const char *hdr,
                              const char *origin, const char *file_path,
                              uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h);
static bool   fetch_snapshot(const char *base, const char *hdr,
                             const char *snapshot_url,
                             uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h);
static void   conn_teardown_if_page_left(void);

// =============================================================================
// Mutex helpers (mirror json_client_init/lock/unlock)
// =============================================================================

/**
 * Zero the struct and install the "unknown" sentinels. Used both by init and
 * by each poll's scratch copy, so a field the printer stopped reporting falls
 * back to "--" instead of sticking at its last value. Leaves mutex NULL.
 */
static void reset_defaults(octoprint_data_t *data) {
    memset(data, 0, sizeof(octoprint_data_t));
    data->completion        = -1.0f;
    data->print_time_s      = -1;
    data->print_time_left_s = -1;
    data->layer_current     = -1;
    data->layer_total       = -1;
    data->nozzle_actual     = NAN;
    data->nozzle_target     = NAN;
    data->bed_actual        = NAN;
    data->bed_target        = NAN;
    data->image_src         = -1;   /* 0 is a real source, so -1 is "none" */
}

void octoprint_client_init(octoprint_data_t *data) {
    if (!data) {
        return;
    }
    reset_defaults(data);
    data->mutex = xSemaphoreCreateMutex();
    s_conn_mux  = xSemaphoreCreateMutex();
}

bool octoprint_client_lock(octoprint_data_t *data, int timeout_ms) {
    if (!data || !data->mutex) {
        return false;
    }
    return xSemaphoreTake(data->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void octoprint_client_unlock(octoprint_data_t *data) {
    if (data && data->mutex) {
        xSemaphoreGive(data->mutex);
    }
}

void octoprint_client_note_image_source_switch(octoprint_data_t *data) {
    /* Bump first and unconditionally: this is what a poll pass already in
     * flight checks, and it is the half that must not be lost if the lock
     * below times out. */
    atomic_fetch_add(&s_src_gen, 1);
    if (!data) {
        return;
    }

    /* Same swap-under-lock/free-after-unlock discipline as set_page_active:
     * a reader holding the lock must never see a pointer we already freed. */
    uint8_t *old = NULL;
    if (octoprint_client_lock(data, 200)) {
        old = data->image_buf;
        data->image_buf    = NULL;
        data->image_w      = 0;
        data->image_h      = 0;
        data->new_image    = false;
        data->image_status = OCTO_IMG_PENDING;
        data->image_src    = -1;
        octoprint_client_unlock(data);
    } else {
        /* The generation bump above still forces the in-flight pass to publish
         * a drop, so the stale frame cannot survive as the new source. */
        ESP_LOGW(TAG, "Source switch could not take the lock; drop deferred to the poll pass");
    }
    if (old) {
        heap_caps_free(old);
    }
}

void octoprint_client_set_page_active(bool active, octoprint_data_t *data) {
    atomic_store(&s_page_active, active);
    if (active) {
        /* Entering the page: nothing has been fetched for this visit yet, so
         * the slot is PENDING until the first pass writes a verdict. */
        octoprint_client_note_image_source_switch(data);
        return;
    }

    /* Leaving the page: release the decoded frame. Swap the pointer out under
     * the lock and free it after unlocking, so a reader that is mid-render
     * cannot be holding a pointer we already returned to the heap. */
    uint8_t *old = NULL;
    if (data && octoprint_client_lock(data, 200)) {
        old = data->image_buf;
        data->image_buf = NULL;
        data->image_w   = 0;
        data->image_h   = 0;
        data->new_image = false;
        octoprint_client_unlock(data);
    }
    if (old) {
        heap_caps_free(old);
    }
    s_image_key[0] = '\0';
    s_image_src    = -1;
    s_thumb_fail_key[0] = '\0';
    s_thumb_fails       = 0;

    /* Destroy the keep-alive slots so a drained-parked OPEN socket (or its
     * CLOSE_WAIT remnant after the server times it out) does not sit against
     * the ~9-connection ceiling for as long as the page stays hidden -- the
     * page-gated poll loop stops running, so nobody else would close it.
     * Zero-wait try-take: if a poll is in flight it owns the slots, and its
     * exit teardown (which re-reads the flag cleared above) destroys them
     * instead. Never destroy without this mutex -- a fetch may be mid-flight
     * on the handle. */
    if (s_conn_mux && xSemaphoreTake(s_conn_mux, 0) == pdTRUE) {
        conn_teardown_if_page_left();
        xSemaphoreGive(s_conn_mux);
    }
}

// =============================================================================
// Small cJSON accessors
// =============================================================================

/** Copy a string node into @p out; writes "" for a missing/non-string node. */
static void node_str(const cJSON *node, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (cJSON_IsString(node) && node->valuestring) {
        snprintf(out, out_len, "%s", node->valuestring);
    }
}

/**
 * Read an integer from a node that may be a JSON number OR a numeric string.
 * DisplayLayerProgress reports layer counts and the M73 percentage as strings
 * ("45", "313"), while the core API reports numbers -- one accessor covers both.
 * Returns @p def for a missing node or a string that is not a number.
 */
static int node_int(const cJSON *node, int def) {
    if (cJSON_IsNumber(node)) {
        return (int)node->valuedouble;
    }
    if (cJSON_IsString(node) && node->valuestring && node->valuestring[0] != '\0') {
        char *end = NULL;
        long v = strtol(node->valuestring, &end, 10);
        if (end != node->valuestring) {
            return (int)v;
        }
    }
    return def;
}

/** Float variant of node_int(). Returns @p def when absent/unparseable. */
static float node_float(const cJSON *node, float def) {
    if (cJSON_IsNumber(node)) {
        return (float)node->valuedouble;
    }
    if (cJSON_IsString(node) && node->valuestring && node->valuestring[0] != '\0') {
        char *end = NULL;
        float v = strtof(node->valuestring, &end);
        if (end != node->valuestring) {
            return v;
        }
    }
    return def;
}

// =============================================================================
// URL helpers
// =============================================================================

/** Copy @p base_url into @p out, stripping trailing slashes. False if too long. */
static bool copy_base(const char *base_url, char *out, size_t out_len) {
    if (!base_url || base_url[0] == '\0' || !out || out_len == 0) {
        return false;
    }
    size_t len = strlen(base_url);
    if (len >= out_len) {
        ESP_LOGW(TAG, "octoprint_url too long (%u chars)", (unsigned)len);
        return false;
    }
    memcpy(out, base_url, len + 1);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    return len > 0;
}

/**
 * Percent-encode a gcode file path for use inside a URL path. '/' is preserved
 * (OctoPrint file paths are folder-qualified); unreserved characters pass
 * through; everything else becomes %XX. Returns false when the source is too
 * long to encode into @p dst, so the caller skips the request rather than
 * sending a truncated path.
 */
static bool url_encode_segment(const char *src, char *dst, size_t dst_len) {
    static const char hex[] = "0123456789ABCDEF";
    if (!src || !dst || dst_len == 0) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (plain) {
            if (o + 1 >= dst_len) {
                return false;
            }
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= dst_len) {
                return false;
            }
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
    return true;
}

// =============================================================================
// JSON fetch
// =============================================================================

/**
 * GET {base}/{path} with the X-Api-Key header line in @p hdr and parse the body.
 *
 * @p path is root-relative WITHOUT a leading slash ("api/job").
 * @p status_out (optional) is pre-set to 0 and receives the upstream HTTP status
 * whenever headers were reached, so the caller can tell a transport failure (0)
 * from OctoPrint answering 409 (printer not operational) or 404 (plugin absent).
 * Returns the parsed JSON (caller cJSON_Delete) or NULL.
 */
static cJSON *fetch_json(const char *base, const char *hdr, const char *path,
                         int *status_out) {
    if (status_out) {
        *status_out = 0;
    }
    if (!base || !path) {
        return NULL;
    }

    /* url[640]: base (<=127) + '/' + the longest path any caller builds (<=415). */
    char url[640];
    int un = snprintf(url, sizeof(url), "%s/%s", base, path);
    if (un <= 0 || un >= (int)sizeof(url)) {
        ESP_LOGW(TAG, "URL truncated for path %s", path);
        return NULL;
    }

    http_fetch_opts_t opts = {
        .timeout_ms = OCTO_HTTP_TIMEOUT_MS,
        .use_tls_bundle = (strncasecmp(url, "https", 5) == 0),
        .max_redirects = 3,
        .max_attempts = 1,
        .max_response_bytes = OCTO_JSON_MAX,
        .extra_header = (hdr && hdr[0] != '\0') ? hdr : NULL,
        .conn = s_conn,
        .status_out = status_out,
    };

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_fetch_text(url, &opts, &body, &body_len);
    if (err != ESP_OK) {
        return NULL;   /* status_out carries 409/404/etc for the caller */
    }
    if (body_len == 0) {
        heap_caps_free(body);
        return NULL;
    }

    cJSON *json = cJSON_Parse(body);
    heap_caps_free(body);
    if (!json) {
        ESP_LOGW(TAG, "Failed to parse response for %s", path);
    }
    return json;
}

// =============================================================================
// Image pipeline
// =============================================================================

/** Stream an image body into a PSRAM buffer. Caller heap_caps_free()s it. */
static bool fetch_image_bytes(const char *url, const char *hdr,
                              uint8_t **out_buf, size_t *out_len) {
    if (!s_img_conn) {
        s_img_conn = http_fetch_conn_create();   /* NULL is fine: one-shot client */
    }
    /* hdr may be NULL here (third-party snapshot host, see fetch_snapshot's
     * authority check): the reused handle's previous X-Api-Key is scrubbed by
     * http_fetch before the request goes out, so sharing one slot across the
     * thumbnail and snapshot fetches cannot leak the key. */
    http_fetch_binary_opts_t opts = {
        .timeout_ms = OCTO_HTTP_TIMEOUT_MS,
        .use_tls_bundle = (strncasecmp(url, "https", 5) == 0),
        .max_redirects = 3,
        .max_size = OCTO_IMAGE_MAX,
        .oversize = HTTP_BIN_OVERSIZE_REJECT,
        .shrink_to_fit = true,
        .probe_overflow = true,
        .extra_header = (hdr && hdr[0] != '\0') ? hdr : NULL,
        .conn = s_img_conn,
        .label = "OctoPrint image",
    };
    return http_fetch_binary(url, &opts, out_buf, out_len) == ESP_OK;
}

/**
 * Validate and decode an encoded image into an RGB565 PSRAM buffer.
 *
 * jpeg_sw_decode_rgb565() is named for its first caller but is a plain
 * stb_image decode: PNG (gcode thumbnail) and JPEG (webcam snapshot) both go
 * through it, and it already delivers a 128-byte-aligned, cache-flushed PSRAM
 * buffer at the image's exact dimensions.
 */
static bool decode_image(const uint8_t *buf, size_t len, const char *what,
                         uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h) {
    if (!buf || len < 64) {
        ESP_LOGW(TAG, "%s: body too small (%u bytes)", what, (unsigned)len);
        return false;
    }

    /* Magic gate -- an auth redirect or error page must not reach the decoder. */
    bool is_png  = (buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47);
    bool is_jpeg = (buf[0] == 0xFF && buf[1] == 0xD8);
    if (!is_png && !is_jpeg) {
        ESP_LOGW(TAG, "%s: not a PNG/JPEG (magic %02X %02X)", what, buf[0], buf[1]);
        return false;
    }

    uint32_t pw = 0, ph = 0;
    if (jpeg_probe_dimensions(buf, len, &pw, &ph) &&
        (pw > OCTO_IMG_MAX_DIM || ph > OCTO_IMG_MAX_DIM)) {
        ESP_LOGW(TAG, "%s: too large %lux%lu (max %d)",
                 what, (unsigned long)pw, (unsigned long)ph, OCTO_IMG_MAX_DIM);
        return false;
    }

    uint8_t *rgb = NULL;
    uint32_t w = 0, h = 0;
    size_t out_size = 0;
    if (!jpeg_sw_decode_rgb565(buf, len, &rgb, &w, &h, &out_size) || !rgb) {
        ESP_LOGW(TAG, "%s: decode failed", what);
        if (rgb) {
            heap_caps_free(rgb);
        }
        return false;
    }

    *out_rgb = rgb;
    *out_w   = (uint16_t)w;
    *out_h   = (uint16_t)h;
    return true;
}

/**
 * GET the file metadata for the printing job and read its "thumbnail" property.
 *
 * The value is a root-relative URL supplied by a slicer-thumbnail plugin, e.g.
 * "plugin/prusaslicerthumbnails/thumbnail/foo.png?ts=1699...". Kept in its own
 * frame so the metadata path buffers do not stack with the image URL buffer.
 */
static bool resolve_thumbnail_path(const char *base, const char *hdr,
                                   const char *origin, const char *file_path,
                                   char *out, size_t out_len) {
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    if (!file_path || file_path[0] == '\0' || strlen(file_path) > OCTO_MAX_GCODE_PATH) {
        return false;
    }

    /* encoded[388] holds the worst case: every one of 128 chars percent-escaped. */
    char encoded[388];
    if (!url_encode_segment(file_path, encoded, sizeof(encoded))) {
        ESP_LOGW(TAG, "gcode path too long to encode");
        return false;
    }

    /* meta[416]: "api/files/" (10) + origin (<=15) + '/' + encoded (<=387). */
    char meta[416];
    int mn = snprintf(meta, sizeof(meta), "api/files/%s/%s", origin, encoded);
    if (mn <= 0 || mn >= (int)sizeof(meta)) {
        return false;
    }

    int status = 0;
    cJSON *json = fetch_json(base, hdr, meta, &status);
    if (!json) {
        ESP_LOGD(TAG, "File metadata unavailable (status %d)", status);
        return false;
    }

    /* node_str() would silently truncate; a truncated URL fetches a 404 that
     * looks like "no thumbnail exists". Reject instead so the caller reports a
     * real failure. */
    const cJSON *thumb = cJSON_GetObjectItem(json, "thumbnail");
    if (cJSON_IsString(thumb) && thumb->valuestring &&
        strlen(thumb->valuestring) >= out_len) {
        ESP_LOGW(TAG, "Thumbnail URL too long (%u chars, max %u); skipping",
                 (unsigned)strlen(thumb->valuestring), (unsigned)(out_len - 1));
        cJSON_Delete(json);
        return false;
    }
    node_str(thumb, out, out_len);
    cJSON_Delete(json);
    return out[0] != '\0';
}

/** Fetch + decode the gcode preview thumbnail for the current job's file. */
static bool fetch_thumbnail(const char *base, const char *hdr,
                            const char *origin, const char *file_path,
                            uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h) {
    char rel[256];
    if (!resolve_thumbnail_path(base, hdr, origin, file_path, rel, sizeof(rel))) {
        return false;
    }

    /* The plugin reports a root-relative path; tolerate a leading slash. */
    const char *tail = (rel[0] == '/') ? rel + 1 : rel;
    char url[640];
    int un = snprintf(url, sizeof(url), "%s/%s", base, tail);
    if (un <= 0 || un >= (int)sizeof(url)) {
        return false;
    }

    uint8_t *body = NULL;
    size_t body_len = 0;
    if (!fetch_image_bytes(url, hdr, &body, &body_len)) {
        return false;
    }
    bool ok = decode_image(body, body_len, "Thumbnail", out_rgb, out_w, out_h);
    heap_caps_free(body);
    return ok;
}

/** Fetch + decode the webcam snapshot JPEG. */
static bool fetch_snapshot(const char *base, const char *hdr,
                           const char *snapshot_url,
                           uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h) {
    char url[640];
    if (snapshot_url && snapshot_url[0] != '\0') {
        int un = snprintf(url, sizeof(url), "%s", snapshot_url);
        if (un <= 0 || un >= (int)sizeof(url)) {
            return false;
        }
    } else {
        int un = snprintf(url, sizeof(url), "%s/webcam/?action=snapshot", base);
        if (un <= 0 || un >= (int)sizeof(url)) {
            return false;
        }
    }

    /* Only send the API key to the OctoPrint host itself. A user-supplied
     * snapshot URL may point at a standalone mjpg-streamer (or any third-party
     * host) that has no business receiving the key.
     *
     * A bare prefix compare is not enough: base "http://octopi.lan" prefixes
     * "http://octopi.lan.attacker.com/x", which is a different host. The byte
     * after the prefix must therefore end the authority -- NUL, '/', '?' or '#'.
     * (base has its trailing slashes stripped by copy_base(), so no other
     * separator can appear there.) */
    size_t base_len = strlen(base);
    const char *send_hdr = NULL;
    if (strncmp(url, base, base_len) == 0) {
        char sep = url[base_len];   /* in bounds: the prefix matched, so at worst this is the NUL */
        if (sep == '\0' || sep == '/' || sep == '?' || sep == '#') {
            send_hdr = hdr;
        }
    }

    uint8_t *body = NULL;
    size_t body_len = 0;
    if (!fetch_image_bytes(url, send_hdr, &body, &body_len)) {
        return false;
    }
    bool ok = decode_image(body, body_len, "Snapshot", out_rgb, out_w, out_h);
    heap_caps_free(body);
    return ok;
}

/**
 * Destroy both keep-alive slots once the page has been left. A drained-parked
 * slot holds an OPEN socket, and the page-gated poll loop stops running when
 * the page is hidden, so the slots must not outlive it. Caller MUST hold
 * s_conn_mux. Two callers cover the two leave windows: every poll exit path
 * (a poll in flight when the page deactivates cleans up on its way out) and
 * set_page_active(false) via try-take (the poll task was asleep and will not
 * run again). No-op while the page is active.
 */
static void conn_teardown_if_page_left(void) {
    if (atomic_load(&s_page_active)) {
        return;
    }
    if (s_img_conn) {
        http_fetch_conn_destroy(s_img_conn);
        s_img_conn = NULL;
    }
    if (s_conn) {
        http_fetch_conn_destroy(s_conn);
        s_conn = NULL;
    }
}

// =============================================================================
// Public API -- one poll cycle
// =============================================================================

void octoprint_client_poll(const char *base_url, const char *api_key,
                           uint8_t image_source, const char *snapshot_url,
                           octoprint_data_t *data) {
    if (!data) {
        return;
    }

    char base[OCTO_MAX_BASE_LEN];
    if (!copy_base(base_url, base, sizeof(base))) {
        return;
    }

    /* hdr[96]: "X-Api-Key: " (11) + octoprint_api_key[64]. */
    char hdr[96];
    hdr[0] = '\0';
    if (api_key && api_key[0] != '\0') {
        int hn = snprintf(hdr, sizeof(hdr), "X-Api-Key: %s", api_key);
        if (hn <= 0 || hn >= (int)sizeof(hdr)) {
            ESP_LOGW(TAG, "API key too long, sending unauthenticated");
            hdr[0] = '\0';
        }
    }

    /* The conn slots are owned for the whole poll: set_page_active() try-takes
     * this mutex to destroy them, and must never win mid-fetch. Uncontended in
     * the common case; the UI holds it only for a microsecond destroy. */
    if (s_conn_mux) {
        xSemaphoreTake(s_conn_mux, portMAX_DELAY);
    }

    /* Lazily create the keep-alive slot. On failure fall through with NULL --
     * http_fetch then uses a one-shot client per request. */
    if (!s_conn) {
        s_conn = http_fetch_conn_create();
        if (!s_conn) {
            ESP_LOGW(TAG, "Failed to allocate keep-alive connection slot");
        }
    }

    /* Scratch copy published wholesale at the end. */
    octoprint_data_t local;
    reset_defaults(&local);   /* mutex stays NULL -- scratch is never locked */

    /* Everything this pass fetches is fetched under the source selected NOW.
     * Snapshot the generation before the first request so the publish block can
     * tell whether the user moved the toggle underneath us. */
    const uint32_t gen0 = atomic_load(&s_src_gen);

    // ---- Tier 1: /api/job (also the connectivity probe) --------------------
    int status = 0;
    cJSON *job = fetch_json(base, hdr, "api/job", &status);
    if (!job) {
        ESP_LOGW(TAG, "/api/job failed (status %d)", status);
        s_was_connected = false;
        s_dlp = 0;                 /* re-probe the plugin on the next connect */
        s_conn_state[0] = '\0';
        if (octoprint_client_lock(data, 100)) {
            data->connected = false;
            /* Everything else (data_valid, the last good values, image_status)
             * stays as published: the page dims stale data instead of blanking
             * it, and fail_count tells it how stale "stale" is. */
            if (data->fail_count < 0xFFFF) {
                data->fail_count++;
            }
            octoprint_client_unlock(data);
        }
        conn_teardown_if_page_left();
        if (s_conn_mux) {
            xSemaphoreGive(s_conn_mux);
        }
        return;
    }

    node_str(cJSON_GetObjectItem(job, "state"), local.job_state, sizeof(local.job_state));

    const cJSON *job_obj = cJSON_GetObjectItem(job, "job");
    const cJSON *file    = cJSON_GetObjectItem(job_obj, "file");

    /* The metadata request needs the storage-relative path and its origin. */
    char file_path[OCTO_MAX_GCODE_PATH + 1];
    char origin[16];
    const cJSON *pathn = cJSON_GetObjectItem(file, "path");
    node_str(cJSON_IsString(pathn) ? pathn : cJSON_GetObjectItem(file, "name"),
             file_path, sizeof(file_path));
    node_str(cJSON_GetObjectItem(file, "origin"), origin, sizeof(origin));
    if (origin[0] == '\0') {
        snprintf(origin, sizeof(origin), "local");
    }

    /* progress.completion is a PERCENTAGE 0..100 on a live server (the sample
     * in docs/octoprint api/job.rst shows a 0..1 fraction, which the running
     * API does not match -- verified against OctoPrint 1.11.x). Stored as sent;
     * the renderer must not multiply by 100. */
    const cJSON *prog = cJSON_GetObjectItem(job, "progress");
    local.completion        = node_float(cJSON_GetObjectItem(prog, "completion"), -1.0f);
    local.print_time_s      = node_int(cJSON_GetObjectItem(prog, "printTime"), -1);
    local.print_time_left_s = node_int(cJSON_GetObjectItem(prog, "printTimeLeft"), -1);
    cJSON_Delete(job);

    local.connected  = true;
    local.data_valid = true;
    local.fail_count = 0;

    /* First cycle of a fresh connection: re-arm the plugin probe. */
    if (!s_was_connected) {
        s_dlp = 0;
    }
    s_was_connected = true;

    // ---- Tier 1: /api/printer ----------------------------------------------
    /* 409 means OctoPrint is up but the printer is not operational (powered
     * off / not connected). That is a legitimate state, not a poll failure:
     * temperatures stay NAN and the flags stay false. */
    status = 0;
    cJSON *printer = fetch_json(base, hdr, "api/printer", &status);
    if (printer) {
        const cJSON *temp  = cJSON_GetObjectItem(printer, "temperature");
        const cJSON *tool0 = cJSON_GetObjectItem(temp, "tool0");
        const cJSON *bed   = cJSON_GetObjectItem(temp, "bed");
        local.nozzle_actual = node_float(cJSON_GetObjectItem(tool0, "actual"), NAN);
        local.nozzle_target = node_float(cJSON_GetObjectItem(tool0, "target"), NAN);
        local.bed_actual    = node_float(cJSON_GetObjectItem(bed, "actual"), NAN);
        local.bed_target    = node_float(cJSON_GetObjectItem(bed, "target"), NAN);

        const cJSON *state = cJSON_GetObjectItem(printer, "state");
        node_str(cJSON_GetObjectItem(state, "text"),
                 local.printer_state, sizeof(local.printer_state));
        node_str(cJSON_GetObjectItem(state, "error"),
                 local.error_text, sizeof(local.error_text));

        const cJSON *flags = cJSON_GetObjectItem(state, "flags");
        local.printing = cJSON_IsTrue(cJSON_GetObjectItem(flags, "printing"));
        local.paused   = cJSON_IsTrue(cJSON_GetObjectItem(flags, "paused"));
        local.error    = cJSON_IsTrue(cJSON_GetObjectItem(flags, "error"));
        cJSON_Delete(printer);
    } else if (status == 409) {
        ESP_LOGD(TAG, "/api/printer 409 - printer not operational");
    } else {
        ESP_LOGW(TAG, "/api/printer failed (status %d)", status);
    }

    // ---- Tier 2: /api/connection (~every 30 s) ------------------------------
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_conn_state[0] == '\0' || now_ms - s_conn_tier_ms >= OCTO_CONN_TIER_MS) {
        s_conn_tier_ms = now_ms;
        status = 0;
        cJSON *conn = fetch_json(base, hdr, "api/connection", &status);
        if (conn) {
            node_str(cJSON_GetObjectItem(cJSON_GetObjectItem(conn, "current"), "state"),
                     s_conn_state, sizeof(s_conn_state));
            cJSON_Delete(conn);
        } else {
            ESP_LOGD(TAG, "/api/connection failed (status %d)", status);
        }
    }
    snprintf(local.conn_state, sizeof(local.conn_state), "%s", s_conn_state);

    // ---- Tier 1: DisplayLayerProgress (while available) ---------------------
    if (s_dlp >= 0) {
        status = 0;
        cJSON *dlp = fetch_json(base, hdr, "plugin/DisplayLayerProgress/values", &status);
        if (dlp) {
            s_dlp = 1;
            const cJSON *layer = cJSON_GetObjectItem(dlp, "layer");
            local.layer_current = node_int(cJSON_GetObjectItem(layer, "current"), -1);
            local.layer_total   = node_int(cJSON_GetObjectItem(layer, "total"), -1);

            const cJSON *print = cJSON_GetObjectItem(dlp, "print");
            node_str(cJSON_GetObjectItem(print, "estimatedEndTime"),
                     local.eta, sizeof(local.eta));
            cJSON_Delete(dlp);
        } else if (status == 404) {
            ESP_LOGI(TAG, "DisplayLayerProgress not installed; skipping until reconnect");
            s_dlp = -1;
        }
        /* Any other failure leaves s_dlp untouched so a transient error does
         * not permanently disable the tier. */
    }
    local.dlp_available = (s_dlp == 1);

    // ---- Image (only while the page is visible) -----------------------------
    uint8_t *new_img = NULL;
    uint16_t new_w = 0, new_h = 0;
    bool drop_img  = false;   /* publish NULL: the held frame is from the old source */
    bool thumb_new = false;   /* the fetch that produced new_img was a thumbnail */
    const int8_t want_src = (int8_t)((image_source == 1) ? 1 : 0);
    /* Read once: the publish block below must use the same answer this pass
     * fetched (or did not fetch) under. */
    const bool page_active = atomic_load(&s_page_active);

    if (page_active) {
        /* Source switched while the page stayed open. The whole switch happens
         * in THIS pass: the held frame is dropped and the newly selected source
         * is fetched, decoded and published below, so the change costs one poll,
         * not one to drop plus another to fetch. If the new source has nothing
         * to show this cycle (preview selected with no job file loaded), the
         * drop still stands: otherwise the old webcam frame would sit on screen
         * as if it were the preview. The drop applies ONLY on a switch -- see
         * the empty-path note below the chain. */
        const bool src_switched = (s_image_src >= 0 && s_image_src != want_src);
        drop_img = src_switched;

        if (image_source == 1) {
            /* Webcam: refetch every cycle -- it is a live view. */
            if (!fetch_snapshot(base, hdr, snapshot_url, &new_img, &new_w, &new_h)) {
                new_img = NULL;
            }
        } else if (file_path[0] != '\0') {
            /* A different file -- or the user re-selecting this source -- resets
             * the give-up counter: an explicit switch must retry even when this
             * file's thumbnail already failed OCTO_THUMB_MAX_FAILS times. */
            if (src_switched || strcmp(s_thumb_fail_key, file_path) != 0) {
                snprintf(s_thumb_fail_key, sizeof(s_thumb_fail_key), "%s", file_path);
                s_thumb_fails = 0;
            }
            /* Thumbnail: refetch only when the printing file changed, or when
             * the source was switched while the page stayed open. */
            bool stale = (s_image_src != 0) || (strcmp(s_image_key, file_path) != 0);
            if (stale && s_thumb_fails < OCTO_THUMB_MAX_FAILS) {
                if (fetch_thumbnail(base, hdr, origin, file_path,
                                    &new_img, &new_w, &new_h)) {
                    thumb_new = true;
                } else {
                    new_img = NULL;
                    s_thumb_fails++;
                    if (s_thumb_fails >= OCTO_THUMB_MAX_FAILS) {
                        ESP_LOGW(TAG, "No thumbnail for %s after %d tries; "
                                 "not retrying until the file changes",
                                 file_path, OCTO_THUMB_MAX_FAILS);
                    }
                }
            }
        }
        /* else (preview selected, file_path empty -- no job loaded): the held
         * frame, the previous job's thumbnail, stays published on purpose as a
         * "last print" view (user ruling 2026-08-14). Not a stale-frame bug;
         * do not re-flag. */
    }

    // ---- Publish -------------------------------------------------------------
    /* The toggle moved after gen0 was taken, so whatever this pass fetched is
     * the OLD source's picture. Retire it here -- it is pass-local, nobody else
     * can be holding it -- and force the drop below. */
    const bool src_stale = (atomic_load(&s_src_gen) != gen0);
    if (src_stale && new_img) {
        heap_caps_free(new_img);
        new_img   = NULL;
        thumb_new = false;
    }

    uint8_t *retired = NULL;
    bool published = false;
    if (octoprint_client_lock(data, 200)) {
        /* Carry the image ownership across the wholesale copy: the new buffer
         * (retiring the old one), nothing at all (source switch), or the
         * existing one untouched. The carried buffer may legitimately be NULL
         * for the webcam source: the UI consumes (frees+NULLs) the original
         * under the lock once it has scaled it (see octoprint_client.h). */
        local.mutex = data->mutex;
        if (src_stale || new_img || drop_img) {
            /* local came from reset_defaults(), so its image fields are already
             * NULL/0/-1 -- publishing them as-is IS the drop. */
            retired = data->image_buf;
        }
        if (src_stale) {
            /* Forced drop: nothing this pass produced may be published, and the
             * held frame is the old source's. Image fields stay at their
             * reset_defaults() values. */
        } else if (new_img) {
            local.image_buf = new_img;
            local.image_w   = new_w;
            local.image_h   = new_h;
            local.new_image = true;
            local.image_src = want_src;
        } else if (!drop_img) {
            local.image_buf = data->image_buf;
            local.image_w   = data->image_w;
            local.image_h   = data->image_h;
            local.new_image = data->new_image;
            local.image_src = data->image_src;   /* the frame's own label */
        }

        /* Why the slot looks the way it does. A pass that ran with the page
         * inactive fetched nothing, so it carries the previous verdict --
         * local came from reset_defaults() and would otherwise reset it to
         * PENDING. Webcam has no "held" case for a fresh fetch: the UI frees
         * the original after scaling, so a NULL image_buf there is normal. */
        if (src_stale) {
            /* Back to "loading" until the next pass lands the new source. */
            local.image_status = OCTO_IMG_PENDING;
        } else if (page_active) {
            if (want_src == 1) {
                /* A failed refresh that still publishes a carried webcam frame
                 * is not a visible failure -- the status must describe what is
                 * actually drawn, and the live view keeps its last good frame
                 * through a hiccup. */
                local.image_status = (new_img ||
                                      (local.image_buf && local.image_src == 1))
                                     ? OCTO_IMG_OK : OCTO_IMG_WEBCAM_FAILED;
            } else if (local.image_buf) {
                local.image_status = OCTO_IMG_OK;   /* fetched now, or held */
            } else {
                local.image_status = (file_path[0] == '\0') ? OCTO_IMG_NO_JOB
                                                            : OCTO_IMG_NO_THUMBNAIL;
            }
        } else {
            local.image_status = data->image_status;
            /* image_src rides along with the carried buffer above. */
        }
        memcpy(data, &local, sizeof(*data));
        octoprint_client_unlock(data);
        published = true;
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for data update");
        retired = new_img;   /* nothing published it; do not leak it */
    }

    /* The held-image bookkeeping only advances once the frame is actually on the
     * published struct. Stamping it earlier would latch "no image" for the rest
     * of the print when a fetch or the publish itself failed. */
    /* A forced drop advances nothing: s_image_src still names the source the
     * held bookkeeping was built for, so the next pass sees a switch, drops
     * again if needed and refetches. */
    if (published && !src_stale) {
        if (new_img) {
            s_image_src = want_src;
            if (thumb_new) {
                snprintf(s_image_key, sizeof(s_image_key), "%s", file_path);
            }
        } else if (drop_img) {
            s_image_src    = -1;
            s_image_key[0] = '\0';
        }
    }

    /* Freed only after the lock is released, so no reader can hold it. */
    if (retired) {
        heap_caps_free(retired);
    }

    conn_teardown_if_page_left();
    if (s_conn_mux) {
        xSemaphoreGive(s_conn_mux);
    }

    ESP_LOGD(TAG, "poll OK state=%s completion=%.1f layer=%d/%d dlp=%d",
             local.job_state, (double)local.completion,
             local.layer_current, local.layer_total, (int)s_dlp);
}
