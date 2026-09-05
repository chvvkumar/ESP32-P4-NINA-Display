/**
 * @file rainviewer.c
 * @brief Device half of the worldwide Weather Radar source: the frame list,
 *        the coastline basemap, and the per-frame tile composite.
 *
 * ONE FRAME = one panel-sized RGB565 PSRAM buffer that starts as a copy of the
 * cached basemap and has 4 to 9 decoded RainViewer tiles alpha-pasted over it.
 * The result is exactly panel sized, so it enters the animation ring at native
 * size and playback needs no scale at all.
 *
 * THE BASEMAP IS CACHED, THE TILES ARE NOT. The map does not change between
 * loop frames, so it is fetched and decoded once per window and reused; the
 * rain does, so every frame fetches its own tiles. rainviewer_release() drops
 * the cache when the page parks or the window moves.
 *
 * MEMORY: the cache is one panel frame (about 1 MB at 720 px) and each build
 * allocates one more, plus a single decoded tile at a time (512 x 512 RGBA =
 * 1 MB) and its compressed bytes. Peak is therefore about 4 MB on top of the
 * ring -- and only about 3 MB once the basemap is cached, because the frame is
 * allocated AFTER the basemap decode transient has been released. That is the
 * same order as the JPEG decode transient the other image pages already carry,
 * and it is why the caller holds the shared fetch gate.
 *
 * A FAILED TILE IS NOT A FAILED FRAME. Rain coverage is patchy by nature and a
 * missing tile leaves bare map, which reads as "no rain there" rather than as
 * a broken picture. Only an allocation failure or an out-of-range index fails.
 *
 * BUT A FAILED TILE ENDS THE FRAME. RainViewer answers a no-rain tile with a
 * transparent 200, so a fetch that actually fails is a host-level problem and
 * every remaining tile of that frame would fail the same way. The loop stops at
 * the first failure rather than spending 10 s per tile, and a failed basemap is
 * not retried for RV_BASEMAP_RETRY_S. Both exist because the caller holds the
 * shared image fetch gate with portMAX_DELAY: time wasted here stalls all six
 * image pollers.
 */

#include "rainviewer.h"

#include "app_config.h"
#include "http_fetch.h"
#include "jpeg_utils.h"
#include "stb_image.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "rainviewer";

#define RV_MAPS_MAX_BYTES   16384    /* weather-maps.json is ~2 KB; 16 KB is generous */
#define RV_TILE_MAX_BYTES   (512u * 1024u)
#define RV_BASE_MAX_BYTES   (1024u * 1024u)
#define RV_HTTP_TIMEOUT_MS  10000
#define RV_USER_AGENT       "NINA-Display/1.0 (ESP32-P4 dashboard)"
/* How long a failed basemap fetch stays failed. Without this every frame build
 * pays another 10 s timeout for a map that is not coming back, while holding
 * the shared image fetch gate. Cleared when the window moves or the page
 * parks, so a settings change still retries at once. */
#define RV_BASEMAP_RETRY_S  300

/* Frame list and basemap cache. Touched only by the radar page's poll task
 * (rainviewer_refresh, rainviewer_build_frame and rainviewer_release all run
 * on it), so no lock is needed -- the same rule the anim_state_t table in
 * image_page_poll.c follows. */
static char               s_host[RAINVIEWER_HOST_MAX];
static rainviewer_frame_t s_frames[RAINVIEWER_MAX_FRAMES];
static int                s_nframes;

static uint16_t          *s_base;        /* cached basemap pixels, PSRAM, or NULL */
static rainviewer_win_t   s_base_win;    /* the window s_base was LAST TRIED for,
                                            whether that attempt succeeded or not */
static bool               s_base_valid;
static int64_t            s_base_retry_us;  /* esp_timer time before which a failed
                                               basemap is not fetched again; 0 = now */

/* PAGE-SCOPED KEEP-ALIVE for the tile host. A frame is 4 to 9 tiles from one
 * server and a visit is several frames, and a one-shot client paid a fresh
 * TCP+TLS handshake for every one of them: a New Zealand user's log shows 1.9
 * to 3.1 s per tile, about 45 s before the page could show anything. Owned by
 * the radar poll task alone (rainviewer_build_frame and rainviewer_release both
 * run on it) and destroyed on park, because a drained slot parks its socket
 * OPEN against this board's ~9 connection ceiling.
 *
 * NOT used for the basemap (a different host, GIBS) or for weather-maps.json
 * (api.rainviewer.com, one request per poll): both stay one-shot. */
static http_fetch_conn_t *s_tile_conn;

bool radar_use_rainviewer(const app_config_t *c)
{
    if (c == NULL) return false;
    return rainviewer_selected(c->radar_map_style, c->radar_token,
                               c->weather_lat, c->weather_lon);
}

static http_fetch_conn_t *rv_tile_conn(void)
{
    if (s_tile_conn == NULL) {
        s_tile_conn = http_fetch_conn_create();
        if (s_tile_conn != NULL) ESP_LOGI(TAG, "tile keep-alive connection opened");
    }
    return s_tile_conn;   /* NULL on alloc failure = one-shot, still correct */
}

static void rv_tile_conn_close(void)
{
    if (s_tile_conn == NULL) return;
    http_fetch_conn_destroy(s_tile_conn);
    s_tile_conn = NULL;
    ESP_LOGI(TAG, "tile keep-alive connection closed");
}

void rainviewer_release(void)
{
    rv_tile_conn_close();
    if (s_base != NULL) {
        heap_caps_free(s_base);
        s_base = NULL;
    }
    s_base_valid = false;
    s_base_retry_us = 0;
    memset(&s_base_win, 0, sizeof(s_base_win));
    s_nframes = 0;
    s_host[0] = '\0';
}

/* The window this configuration asks for. */
static void rv_window(const app_config_t *c, rainviewer_win_t *w)
{
    rainviewer_window(c->weather_lat, c->weather_lon, c->radar_zoom, RAINVIEWER_PX, w);
}

static bool rv_same_window(const rainviewer_win_t *a, const rainviewer_win_t *b)
{
    return a->ox == b->ox && a->oy == b->oy &&
           a->world_px == b->world_px && a->win_px == b->win_px && a->zoom == b->zoom;
}

int rainviewer_refresh(const app_config_t *c, uint32_t *stamps, int max_out)
{
    if (c == NULL || stamps == NULL || max_out <= 0) return 0;
    if (max_out > RAINVIEWER_MAX_FRAMES) max_out = RAINVIEWER_MAX_FRAMES;

    /* Declared before the failure goto below, which would otherwise jump over it. */
    char prev_host[RAINVIEWER_HOST_MAX];
    strlcpy(prev_host, s_host, sizeof(prev_host));

    http_fetch_opts_t opts = {
        .timeout_ms = RV_HTTP_TIMEOUT_MS,
        .use_tls_bundle = true,
        .max_redirects = 2,
        .max_response_bytes = RV_MAPS_MAX_BYTES,
        .user_agent = RV_USER_AGENT,
    };
    char  *body = NULL;
    size_t len = 0;
    if (http_fetch_text(RAINVIEWER_MAPS_URL, &opts, &body, &len) != ESP_OK) {
        ESP_LOGW(TAG, "weather-maps.json fetch failed; keeping %d cached frames", s_nframes);
        goto publish;
    }
    s_nframes = rainviewer_parse_maps(body, len, s_host, sizeof(s_host),
                                      s_frames, RAINVIEWER_MAX_FRAMES);
    heap_caps_free(body);
    /* A parked socket points at the host it was opened to. */
    if (strcmp(prev_host, s_host) != 0) rv_tile_conn_close();
    if (s_nframes == 0) {
        ESP_LOGW(TAG, "weather-maps.json had no usable radar frames");
        s_host[0] = '\0';
    } else {
        ESP_LOGI(TAG, "RainViewer: %d frames, newest %lu",
                 s_nframes, (unsigned long)s_frames[0].time);
    }

publish: {
        int n = s_nframes < max_out ? s_nframes : max_out;
        for (int i = 0; i < n; i++) stamps[i] = s_frames[i].time;
        return n;
    }
}

/* Mark the basemap unavailable and hold off the next attempt. */
static bool rv_basemap_fail(const char *why)
{
    ESP_LOGW(TAG, "%s; frames compose over black for the next %d s",
             why, RV_BASEMAP_RETRY_S);
    s_base_retry_us = esp_timer_get_time() + (int64_t)RV_BASEMAP_RETRY_S * 1000000;
    return false;
}

/* Fetch and decode the coastline basemap for @p w into the cache. Leaves the
 * cache empty (and returns false) on any failure, which composes the frames
 * over black; the next attempt waits RV_BASEMAP_RETRY_S so a dead GIBS does not
 * cost every frame build a 10 s timeout while the fetch gate is held. */
static bool rv_basemap_ensure(const rainviewer_win_t *w)
{
    if (s_base_valid && s_base != NULL && rv_same_window(&s_base_win, w)) return true;
    if (s_base != NULL) {
        heap_caps_free(s_base);
        s_base = NULL;
    }
    s_base_valid = false;

    /* A different window is a different picture, so a failure on the old one
     * says nothing about this one: try immediately. */
    if (!rv_same_window(&s_base_win, w)) s_base_retry_us = 0;
    s_base_win = *w;
    if (s_base_retry_us != 0 && esp_timer_get_time() < s_base_retry_us) return false;

    /* The request is clamped to the world edge, so the picture that comes back
     * can be smaller than the panel; rect says where it belongs. */
    rainviewer_basemap_rect_t rect;
    if (!rainviewer_basemap_rect(w, &rect)) return rv_basemap_fail("window is outside the world");

    char url[RAINVIEWER_URL_MAX];
    if (!rainviewer_basemap_url(url, sizeof(url), w)) return rv_basemap_fail("basemap URL too long");

    /* REJECT, not CLAMP: a truncated JPEG decodes to a torn or failed picture,
     * and an empty cache (black under the rain) beats half a map. */
    http_fetch_binary_opts_t opts = {
        .timeout_ms = RV_HTTP_TIMEOUT_MS,
        .use_tls_bundle = true,
        .max_redirects = 2,
        .max_size = RV_BASE_MAX_BYTES,
        .oversize = HTTP_BIN_OVERSIZE_REJECT,
        .shrink_to_fit = true,
        .user_agent = RV_USER_AGENT,
        .label = "Radar map",
    };
    uint8_t *jpg = NULL;
    size_t   jpg_len = 0;
    if (http_fetch_binary(url, &opts, &jpg, &jpg_len) != ESP_OK) {
        return rv_basemap_fail("basemap fetch failed");
    }

    uint8_t *pix = NULL;
    uint32_t bw = 0, bh = 0;
    size_t   bsz = 0;
    bool ok = jpeg_decode_rgb565(jpg, jpg_len, &pix, &bw, &bh, &bsz);
    heap_caps_free(jpg);
    if (!ok || pix == NULL) {
        if (pix != NULL) heap_caps_free(pix);
        return rv_basemap_fail("basemap decode failed");
    }
    if ((int32_t)bw != rect.w || (int32_t)bh != rect.h) {
        /* GIBS returns exactly the WIDTH/HEIGHT asked for; anything else is a
         * server change and would misalign the rain over the map, which is
         * worse than no map at all. */
        ESP_LOGW(TAG, "basemap is %ux%u, expected %dx%d",
                 (unsigned)bw, (unsigned)bh, (int)rect.w, (int)rect.h);
        heap_caps_free(pix);
        return rv_basemap_fail("basemap wrong size");
    }

    if (rect.x == 0 && rect.y == 0 && rect.w == w->win_px && rect.h == w->win_px) {
        s_base = (uint16_t *)pix;   /* the whole window: the decode IS the cache */
    } else {
        /* Clamped at the world edge: keep the cache panel sized and black
         * outside the world so a frame build stays one memcpy. The decode
         * transient and the cache overlap for the length of this copy, which is
         * still under the ~1.5 MB the decode itself peaked at. */
        size_t bytes = ((size_t)w->win_px * (size_t)w->win_px * 2u + 127u) & ~(size_t)127u;
        uint16_t *base = (uint16_t *)heap_caps_aligned_calloc(128, 1, bytes, MALLOC_CAP_SPIRAM);
        if (base == NULL) {
            heap_caps_free(pix);
            return rv_basemap_fail("no PSRAM for the basemap cache");
        }
        const size_t row = (size_t)rect.w * 2u;
        for (int32_t y = 0; y < rect.h; y++) {
            memcpy((uint8_t *)base + ((size_t)(rect.y + y) * (size_t)w->win_px
                                      + (size_t)rect.x) * 2u,
                   pix + (size_t)y * row, row);
        }
        heap_caps_free(pix);
        ESP_LOGI(TAG, "basemap clamped to the world: %dx%d pasted at %d,%d",
                 (int)rect.w, (int)rect.h, (int)rect.x, (int)rect.y);
        s_base = base;
    }
    s_base_valid = true;
    s_base_retry_us = 0;
    return true;
}

/* Source-over paste of one decoded RGBA tile onto the RGB565 frame. @p dx / @p
 * dy are the tile's top-left in frame pixels and may be negative. Integer only:
 * the 5/6/5 channels are expanded to 8 bits, blended, and re-packed. Pure --
 * nothing here touches the cache, the network or LVGL, so it lifts into a
 * header for a host test unchanged if one is ever wanted. */
static void rv_paste_tile(uint16_t *dst, int dst_px, const uint8_t *rgba,
                          int tw, int th, int dx, int dy)
{
    for (int y = 0; y < th; y++) {
        int py = dy + y;
        if (py < 0 || py >= dst_px) continue;
        const uint8_t *srow = rgba + (size_t)y * (size_t)tw * 4u;
        uint16_t      *drow = dst + (size_t)py * (size_t)dst_px;
        for (int x = 0; x < tw; x++) {
            int px = dx + x;
            if (px < 0 || px >= dst_px) continue;
            uint32_t a = srow[(size_t)x * 4u + 3u];
            if (a == 0u) continue;                       /* fully clear: keep the map */
            uint32_t sr = srow[(size_t)x * 4u + 0u];
            uint32_t sg = srow[(size_t)x * 4u + 1u];
            uint32_t sb = srow[(size_t)x * 4u + 2u];
            if (a < 255u) {
                uint16_t d  = drow[px];
                uint32_t dr = (((uint32_t)d >> 11) & 0x1Fu) * 255u / 31u;
                uint32_t dg = (((uint32_t)d >> 5) & 0x3Fu) * 255u / 63u;
                uint32_t db = ((uint32_t)d & 0x1Fu) * 255u / 31u;
                uint32_t na = 255u - a;
                sr = (sr * a + dr * na) / 255u;
                sg = (sg * a + dg * na) / 255u;
                sb = (sb * a + db * na) / 255u;
            }
            drow[px] = (uint16_t)(((sr & 0xF8u) << 8) | ((sg & 0xFCu) << 3) | (sb >> 3));
        }
    }
}

/* Fetch and paste one tile. Returns false when the tile could not be used;
 * the caller carries on with the next one. */
static bool rv_add_tile(uint16_t *frame, int frame_px, const app_config_t *c,
                        const rainviewer_win_t *w, const char *path,
                        int tx, int ty)
{
    char url[RAINVIEWER_URL_MAX];
    /* validate_config() clamps the palette on LOAD, so a hand-crafted
     * POST /api/config can leave a value RainViewer does not publish (3, 5, 7)
     * in the live config until the next boot; rainviewer_tile_url() would then
     * reject every tile and the page would show bare basemap. Fall back to the
     * default instead of going blind. */
    uint8_t palette = rainviewer_palette_ok(c->radar_palette) ? c->radar_palette
                                                              : RAINVIEWER_PALETTE_DEF;
    if (!rainviewer_tile_url(url, sizeof(url), s_host, path,
                             w->zoom, tx, ty, palette)) return false;

    /* REJECT for the same reason as the basemap: a clamped PNG will not decode,
     * so there is nothing to gain from keeping the truncated bytes. */
    http_fetch_binary_opts_t opts = {
        .timeout_ms = RV_HTTP_TIMEOUT_MS,
        .use_tls_bundle = true,
        .max_redirects = 2,
        .max_size = RV_TILE_MAX_BYTES,
        .oversize = HTTP_BIN_OVERSIZE_REJECT,
        .shrink_to_fit = true,
        .user_agent = RV_USER_AGENT,
        .conn = rv_tile_conn(),
        .label = "Radar tile",
    };
    uint8_t *png = NULL;
    size_t   png_len = 0;
    if (http_fetch_binary(url, &opts, &png, &png_len) != ESP_OK) return false;

    int tw = 0, th = 0, ch = 0;
    uint8_t *rgba = stbi_load_from_memory(png, (int)png_len, &tw, &th, &ch, 4);
    heap_caps_free(png);
    if (rgba == NULL) return false;
    /* The paste offset is computed from the 512 px grid, so a tile of any other
     * size would land skewed even though the loop below would happily walk it. */
    if (tw != RAINVIEWER_TILE_PX || th != RAINVIEWER_TILE_PX) {
        ESP_LOGW(TAG, "tile %d/%d is %dx%d, expected %dx%d",
                 tx, ty, tw, th, RAINVIEWER_TILE_PX, RAINVIEWER_TILE_PX);
        stbi_image_free(rgba);
        return false;
    }
    rv_paste_tile(frame, frame_px, rgba, tw, th,
                  tx * RAINVIEWER_TILE_PX - (int)w->ox,
                  ty * RAINVIEWER_TILE_PX - (int)w->oy);
    stbi_image_free(rgba);
    return true;
}

bool rainviewer_build_frame(const app_config_t *c, int i,
                            uint8_t **out_buf, uint16_t *out_w, uint16_t *out_h)
{
    if (c == NULL || out_buf == NULL || out_w == NULL || out_h == NULL) return false;
    *out_buf = NULL;
    *out_w = 0;
    *out_h = 0;
    if (i < 0 || i >= s_nframes) return false;

    rainviewer_win_t w;
    rv_window(c, &w);
    int px = (int)w.win_px;
    if (px < 1) return false;

    /* Basemap FIRST: its decode transient is ~1.5 MB and is released before the
     * frame is allocated, so the two never sit in PSRAM together. */
    bool have_base = rv_basemap_ensure(&w);

    /* 128-byte aligned so the PPA can read the frame straight out of the ring,
     * matching what the decoders hand back everywhere else. */
    size_t sz = ((size_t)px * (size_t)px * 2u + 127u) & ~(size_t)127u;
    uint16_t *frame = (uint16_t *)heap_caps_aligned_calloc(128, 1, sz, MALLOC_CAP_SPIRAM);
    if (frame == NULL) {
        ESP_LOGW(TAG, "no PSRAM for a %dx%d radar frame", px, px);
        return false;
    }

    if (have_base) {
        memcpy(frame, s_base, (size_t)px * (size_t)px * 2u);
    }
    /* else: calloc already left it black, which is a usable picture. */

    rainviewer_range_t r;
    rainviewer_tile_range(&w, &r);
    int got = 0, tried = 0;
    bool aborted = false;
    for (int ty = r.y0; ty <= r.y1 && !aborted; ty++) {
        for (int tx = r.x0; tx <= r.x1; tx++) {
            tried++;
            if (rv_add_tile(frame, px, c, &w, s_frames[i].path, tx, ty)) {
                got++;
                continue;
            }
            /* RainViewer answers a no-rain tile with a transparent 200, so a
             * real failure is host-level and every other tile of this frame
             * would burn another RV_HTTP_TIMEOUT_MS for the same answer. */
            aborted = true;
            break;
        }
    }
    ESP_LOGI(TAG, "frame %d (%lu): %d/%d tiles at z%u%s",
             i, (unsigned long)s_frames[i].time, got, tried, (unsigned)w.zoom,
             aborted ? " (aborted after first failure)" : "");

    *out_buf = (uint8_t *)frame;
    *out_w = (uint16_t)px;
    *out_h = (uint16_t)px;
    return true;
}
