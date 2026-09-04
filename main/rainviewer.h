#pragma once

/*
 * rainviewer.h - pure URL, geometry and parsing decisions for the WORLDWIDE
 * Weather Radar source (Radar Area set to Worldwide, that is radar_token
 * RAINVIEWER_TOKEN_WORLD, and the automatic rule for a location outside US
 * radar coverage).
 *
 * Source: the RainViewer public API. One small JSON document
 * (https://api.rainviewer.com/public/weather-maps.json, ~2 KB) lists the host
 * and the past radar frames on a 10-minute grid, about two hours of history.
 * Each frame is a transparent PNG tile pyramid at
 *   {host}{path}/512/{z}/{x}/{y}/{palette}/1_1.png
 * carrying RAIN ONLY: there is no map under it. The map is one NASA GIBS
 * GetMap of Coastlines_15m + Reference_Features_15m over black, fetched once
 * per loop rebuild and copied under every frame (main/rainviewer.c).
 *
 * Header-only and free of ESP-IDF, FreeRTOS and LVGL, so the host suite can
 * exercise the parts that are easy to get wrong without a device. Same
 * precedent as radar_wms.h and clouds_wms.h; the fetching, PNG decoding and
 * compositing live in main/rainviewer.c and the frame ring in
 * ui/nina_image_page.c.
 *
 * Traps this header is written around:
 *
 *  - `host` and `path` come from a REMOTE JSON document, and both are pasted
 *    into a URL. rainviewer_tile_url() re-validates them (an https:// host with
 *    no userinfo and no path, whose name ends in ".rainviewer.com"; a path
 *    starting with '/'; no query/fragment/space/quote characters in either) so
 *    a changed or compromised API answer cannot send the fetch to another host
 *    or smuggle a query string past the fetcher.
 *
 *  - The tile grid is 512 px, not the 256 px most slippy-map maths assumes.
 *    World size is 512 << zoom, and the world pixel of a location must be
 *    computed against that, or every window lands at the wrong place.
 *
 *  - x wraps at the antimeridian, y does NOT: a y outside [0, 2^z) is off the
 *    top or bottom of the world and must be skipped, not wrapped. A Wellington
 *    or Reykjavik window is exactly where that matters.
 *
 *  - Integer division truncates towards zero in C, so a negative window origin
 *    (which happens whenever the window straddles longitude 180) needs a real
 *    floor division to find its first tile.
 *
 *  - A GIBS GetMap whose BBOX runs past the +-20037508 m world edge is NOT
 *    rendered black outside and correct inside: GIBS drops the WHOLE picture to
 *    a very coarse level (20 px coastline blocks, whole islands missing).
 *    rainviewer_basemap_url() therefore clamps the box to the world and shrinks
 *    WIDTH/HEIGHT with it, so the metres per pixel are unchanged, and
 *    rainviewer_basemap_rect() says where that smaller picture belongs inside
 *    the panel-sized window (the rest stays black). The TILE window is NOT
 *    clamped or shifted: tile x wraps at the antimeridian instead.
 *
 * All Mercator maths is float only (radar_wms_merc_x/_y), so nothing on this
 * path drops the P4 to soft-float.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "app_config.h"           /* app_config_t: the device-half prototypes below
                                     take a pointer to it, same precedent as
                                     settings_table.h and ui/nina_image_page.h */
#include "clouds_wms.h"           /* CLOUDS_GIBS_BASE, and radar_wms.h + screen_geom.h through it */
#include "radar_sites.h"          /* radar_site_nearest / radar_site_coords */
#include "adsb_geom.h"            /* adsb_haversine_nm: the one float great-circle helper in the tree */

#define RAINVIEWER_TILE_PX      512
#define RAINVIEWER_PX           (screen_size())   /* the window is the panel: 720 or 800 */
#define RAINVIEWER_ZOOM_MIN     4
#define RAINVIEWER_ZOOM_MAX     7
#define RAINVIEWER_ZOOM_DEF     6
#define RAINVIEWER_PALETTE_DEF  6
#define RAINVIEWER_MAX_FRAMES   RADAR_RING_MAX    /* 10: the ring capacity */
#define RAINVIEWER_HOST_MAX     64
#define RAINVIEWER_PATH_MAX     64
/* 512, not 256: a basemap URL prints four SIGNED 9-digit bbox values (the
 * Web-Mercator world edge is +-20037508 m) plus the longest GIBS query
 * string, which alone runs past 270 chars in the worst case (measured by
 * hand: 274 with 800 px WIDTH/HEIGHT and full 8-digit-plus-sign bbox terms).
 * Matches RADAR_WMS_URL_MAX / CLOUDS_URL_MAX rather than reusing their number
 * directly, since this header has no other reason to depend on radar_wms.h's
 * constant name. */
#define RAINVIEWER_URL_MAX      512
#define RAINVIEWER_MAPS_URL     "https://api.rainviewer.com/public/weather-maps.json"
/* The radar_token value that means "worldwide, centred on the weather
 * location". Passes radar_token_valid() (A-Z0-9, 3-15 chars) like a site id,
 * so it stores and validates on the existing path. */
#define RAINVIEWER_TOKEN_WORLD  "WORLD"

/* How far from the nearest WSR-88D site a location may be and still be served
 * by the US radar sources. Beyond this the page switches to RainViewer on its
 * own, without the user picking a map style. */
#define RADAR_US_REACH_KM       500.0f
#define RAINVIEWER_KM_PER_NM    1.852f

/* One past frame from weather-maps.json. */
typedef struct {
    uint32_t time;                        /* Unix seconds, the frame's own time */
    char     path[RAINVIEWER_PATH_MAX];   /* "/v2/radar/1756899000" */
} rainviewer_frame_t;

/* The panel-sized window in world pixels at one zoom. */
typedef struct {
    int32_t ox, oy;      /* top-left of the window, world pixels (may be negative) */
    int32_t world_px;    /* RAINVIEWER_TILE_PX << zoom */
    int32_t win_px;      /* panel size, the window is square */
    uint8_t zoom;
} rainviewer_win_t;

/* Inclusive tile range the window touches. x may be negative or >= 2^zoom and
 * must be passed through rainviewer_wrap_x() before it goes in a URL; y is
 * already clamped to the world. */
typedef struct {
    int x0, x1, y0, y1;
} rainviewer_range_t;

static inline uint8_t rainviewer_zoom_clamp(uint8_t z)
{
    if (z < RAINVIEWER_ZOOM_MIN) return RAINVIEWER_ZOOM_MIN;
    if (z > RAINVIEWER_ZOOM_MAX) return RAINVIEWER_ZOOM_MAX;
    return z;
}

/* The five colour schemes RainViewer publishes. Not a range: 3, 5 and 7 do not
 * exist, so a stored value is checked against the set, never clamped. */
static inline bool rainviewer_palette_ok(uint8_t p)
{
    return p == 1u || p == 2u || p == 4u || p == 6u || p == 8u;
}

/* Floor division for a possibly negative numerator; C's / truncates toward
 * zero, which puts a window straddling longitude 180 one tile too far east. */
static inline int32_t rainviewer_floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* The panel-sized window centred on @p lat / @p lon at @p zoom. */
static inline void rainviewer_window(float lat, float lon, uint8_t zoom, int win_px,
                                     rainviewer_win_t *w)
{
    if (w == NULL) return;
    zoom = rainviewer_zoom_clamp(zoom);
    if (win_px < 1) win_px = 1;
    int32_t world = (int32_t)RAINVIEWER_TILE_PX << zoom;
    /* Web-Mercator metres -> [0,1] -> world pixels. RADAR_WMS_R_M * pi is the
     * half-extent radar_wms_merc_x() produces at longitude 180. */
    const float half_m = RADAR_WMS_R_M * RADAR_WMS_PI;
    float px = ((radar_wms_merc_x(lon) + half_m) / (2.0f * half_m)) * (float)world;
    float py = ((half_m - radar_wms_merc_y(lat)) / (2.0f * half_m)) * (float)world;
    w->zoom     = zoom;
    w->world_px = world;
    w->win_px   = (int32_t)win_px;
    w->ox = (int32_t)lroundf(px) - (int32_t)win_px / 2;
    w->oy = (int32_t)lroundf(py) - (int32_t)win_px / 2;
}

/* Inclusive tile range the window covers. y is clamped to the world; x is left
 * unwrapped so the caller knows the paste position before wrapping the id. */
static inline void rainviewer_tile_range(const rainviewer_win_t *w, rainviewer_range_t *r)
{
    if (w == NULL || r == NULL) return;
    const int32_t tp = RAINVIEWER_TILE_PX;
    r->x0 = (int)rainviewer_floordiv(w->ox, tp);
    r->x1 = (int)rainviewer_floordiv(w->ox + w->win_px - 1, tp);
    r->y0 = (int)rainviewer_floordiv(w->oy, tp);
    r->y1 = (int)rainviewer_floordiv(w->oy + w->win_px - 1, tp);
    int n = 1 << w->zoom;
    if (r->y0 < 0) r->y0 = 0;
    if (r->y1 > n - 1) r->y1 = n - 1;
}

/* Tile x wrapped into [0, 2^zoom). */
static inline int rainviewer_wrap_x(int tx, uint8_t zoom)
{
    int n = 1 << rainviewer_zoom_clamp(zoom);
    int m = tx % n;
    return (m < 0) ? m + n : m;
}

/* A remote-supplied URL fragment is safe to paste only if it cannot open a
 * query, a fragment, or a second parameter. */
static inline bool rainviewer_fragment_ok(const char *s, size_t max_len)
{
    if (s == NULL) return false;
    size_t n = strlen(s);
    if (n == 0 || n >= max_len) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '?' || c == '#' || c == '&' || c == ' ' || c == '"' ||
            c == '\'' || c == '<' || c == '>' || c == '\\') return false;
        if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7e) return false;
    }
    return true;
}

/* The host comes out of a remote document and is pasted straight into a URL, so
 * it is pinned to the publisher's own domain. Checked, in order: printable
 * ASCII with no URL-structural character (rainviewer_fragment_ok), an "https://"
 * scheme, no '@' after it (which would turn everything before it into userinfo
 * and hand the fetch to a different host), no '/' after it (so a real host name
 * cannot be hidden in a path segment), and a name ending in ".rainviewer.com"
 * exactly, byte for byte — RainViewer publishes it lower case. A port is
 * refused as a side effect: it lands after the suffix. */
static inline bool rainviewer_host_ok(const char *host)
{
    if (!rainviewer_fragment_ok(host, RAINVIEWER_HOST_MAX)) return false;
    if (strncmp(host, "https://", 8) != 0) return false;
    const char *h = host + 8;
    if (strchr(h, '@') != NULL || strchr(h, '/') != NULL) return false;
    const char *sfx = ".rainviewer.com";
    size_t hn = strlen(h);
    size_t sn = strlen(sfx);
    return hn > sn && strcmp(h + hn - sn, sfx) == 0;
}

static inline bool rainviewer_path_ok(const char *path)
{
    return rainviewer_fragment_ok(path, RAINVIEWER_PATH_MAX) && path[0] == '/';
}

/* One tile: {host}{path}/512/{z}/{x}/{y}/{palette}/1_1.png
 * "1_1" is RainViewer's options pair: smoothed pixels, snow shown. */
static inline bool rainviewer_tile_url(char *out, size_t sz, const char *host, const char *path,
                                       uint8_t zoom, int tx, int ty, uint8_t palette)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    if (!rainviewer_host_ok(host) || !rainviewer_path_ok(path)) return false;
    if (!rainviewer_palette_ok(palette)) return false;
    int n = snprintf(out, sz, "%s%s/%d/%u/%d/%d/%u/1_1.png",
                     host, path, RAINVIEWER_TILE_PX,
                     (unsigned)rainviewer_zoom_clamp(zoom),
                     rainviewer_wrap_x(tx, zoom), ty, (unsigned)palette);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Where the basemap picture sits inside the panel-sized window, in window
 * pixels: the whole window (0, 0, win_px, win_px) unless the window runs past
 * the world edge, in which case the request shrinks and the rest stays black. */
typedef struct {
    int32_t x, y, w, h;
} rainviewer_basemap_rect_t;

/* The window clipped to the world, expressed as an offset and size inside the
 * window. false when nothing of the window is inside the world (nothing to
 * request, the frame composes over black). Pure; shared with the URL builder so
 * the two can never disagree about the size that was asked for. */
static inline bool rainviewer_basemap_rect(const rainviewer_win_t *w,
                                           rainviewer_basemap_rect_t *r)
{
    if (w == NULL || r == NULL || w->world_px <= 0 || w->win_px <= 0) return false;
    r->x = 0;
    r->y = 0;
    r->w = 0;
    r->h = 0;
    int32_t x0 = (w->ox < 0) ? 0 : w->ox;
    int32_t y0 = (w->oy < 0) ? 0 : w->oy;
    int32_t x1 = w->ox + w->win_px;
    int32_t y1 = w->oy + w->win_px;
    if (x1 > w->world_px) x1 = w->world_px;
    if (y1 > w->world_px) y1 = w->world_px;
    if (x1 <= x0 || y1 <= y0) return false;
    r->x = x0 - w->ox;
    r->y = y0 - w->oy;
    r->w = x1 - x0;
    r->h = y1 - y0;
    return true;
}

/* The map under the rain: one GIBS GetMap for the window's EPSG:3857 box
 * CLAMPED TO THE WORLD EDGE, coastlines plus borders and roads over black,
 * JPEG (so the hardware decoder takes it). No TIME: both layers are static.
 * WIDTH/HEIGHT are the clamped extent in window pixels, so the metres per pixel
 * match the window whether or not the clamp bit; rainviewer_basemap_rect()
 * gives the same numbers plus the paste offset. An overrunning box is not an
 * option: GIBS answers one at a very coarse level (see the trap list above).
 *
 * SQUARE-OR-WIDER PIXELS, second coarse trigger, measured 2026-09-03 over a
 * 23-request sweep: GIBS also drops to the coarse level whenever the VERTICAL
 * metres-per-pixel is coarser than the horizontal by more than about
 * 0.001 m/px. The clamped Wellington z5 box 18574810,-5936406,20037508,-4175296
 * at 598x720 has resx 2445.98328 against resy 2445.98611 and came back blocky
 * at 8626 bytes; moving minx 2 m west (resx 2445.98662) or miny 2 m north
 * (resy 2445.98333) returned the crisp 18639-byte picture. Every sample with
 * resx >= resy was crisp, even with resx larger by 0.007; every sample with
 * resy - resx > 0.001 was coarse. Whole-metre rounding of the four bbox terms
 * is enough to land on the wrong side, so after rounding the box is widened in
 * x by WHOLE METRES until (maxx-minx)*HEIGHT >= (maxy-miny)*WIDTH, on whichever
 * x side is not sitting on the world edge. A widening step cannot re-create an
 * overrun: an unpinned edge is at least one world pixel (156 m at z7, more at
 * every lower zoom) inside the world. The shift is under a thousandth of a
 * pixel, and the paste rect and pixel size do not change. */
static inline bool rainviewer_basemap_url(char *out, size_t sz, const rainviewer_win_t *w)
{
    if (out == NULL || sz == 0 || w == NULL || w->world_px <= 0) return false;
    out[0] = '\0';
    rainviewer_basemap_rect_t r;
    if (!rainviewer_basemap_rect(w, &r)) return false;
    const float mpp = (2.0f * RADAR_WMS_R_M * RADAR_WMS_PI) / (float)w->world_px;
    const float hw  = (float)w->world_px * 0.5f;
    float minx = ((float)(w->ox + r.x) - hw) * mpp;
    float maxx = ((float)(w->ox + r.x + r.w) - hw) * mpp;
    float maxy = (hw - (float)(w->oy + r.y)) * mpp;
    float miny = (hw - (float)(w->oy + r.y + r.h)) * mpp;
    long bminx = (long)lroundf(minx);
    long bminy = (long)lroundf(miny);
    long bmaxx = (long)lroundf(maxx);
    long bmaxy = (long)lroundf(maxy);

    /* Square-or-wider pixels (see above), via the shared widening step in
     * radar_wms.h (radar_wms_square_pixels()): widen x on the side that is
     * free to move; if the box already spans the whole world (both x sides
     * pinned, which needs a window wider than the world and so cannot happen
     * at zoom >= 4) trim y instead. */
    const bool west_pinned = (w->ox + r.x) <= 0;
    const bool east_pinned = (w->ox + r.x + r.w) >= w->world_px;
    radar_wms_square_pixels(&bminx, &bminy, &bmaxx, &bmaxy, (int)r.w, (int)r.h,
                            west_pinned, east_pinned);

    int n = snprintf(out, sz,
                     CLOUDS_GIBS_BASE "wms/epsg3857/best/wms.cgi?SERVICE=WMS&VERSION=1.3.0"
                     "&REQUEST=GetMap&LAYERS=Coastlines_15m,Reference_Features_15m"
                     "&CRS=EPSG:3857&BBOX=%ld,%ld,%ld,%ld&WIDTH=%d&HEIGHT=%d"
                     "&FORMAT=image/jpeg&BGCOLOR=0x000000&TRANSPARENT=FALSE",
                     bminx, bminy, bmaxx, bmaxy,
                     (int)r.w, (int)r.h);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Which radar source serves this configuration.
 *   token RAINVIEWER_TOKEN_WORLD -> always worldwide (the Radar Area select's
 *                                   own Worldwide entry)
 *   another non-empty area token -> always the US sources (a named site)
 *   no weather location          -> the US sources, exactly as before
 *   otherwise (the Automatic     -> worldwide when the nearest WSR-88D site is
 *   area entry)                     more than RADAR_US_REACH_KM away
 * @p map_style takes no part in the choice: the Map style select is a US-only
 * control. It stays in the signature so every caller and the host test are
 * unchanged.
 * Pure: takes the three config values rather than the struct, so the host test
 * can drive it. radar_use_rainviewer() in rainviewer.c is the config wrapper. */
static inline bool rainviewer_selected(uint8_t map_style, const char *token, float lat, float lon)
{
    (void)map_style;
    if (token != NULL && strcmp(token, RAINVIEWER_TOKEN_WORLD) == 0) return true;
    if (token != NULL && token[0] != '\0') return false;
    if (lat == 0.0f && lon == 0.0f) return false;
    float slat = 0.0f, slon = 0.0f;
    if (!radar_site_coords(radar_site_nearest(lat, lon), &slat, &slon)) return false;
    float km = adsb_haversine_nm(lat, lon, slat, slon) * RAINVIEWER_KM_PER_NM;
    return km > RADAR_US_REACH_KM;
}

/* Parse weather-maps.json: copy the host into @p host and up to @p max_out of
 * the NEWEST radar.past entries into @p out, NEWEST FIRST (the document lists
 * them oldest first). Returns the count, 0 on any failure, and writes nothing
 * on 0. The nowcast and satellite blocks are ignored: nowcast frames are a
 * forecast, not an observation, and mixing them into the history loop would
 * show weather that has not happened. */
static inline int rainviewer_parse_maps(const char *json, size_t len,
                                        char *host, size_t host_sz,
                                        rainviewer_frame_t *out, int max_out)
{
    if (json == NULL || host == NULL || host_sz == 0 || out == NULL || max_out <= 0) return 0;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) return 0;

    int count = 0;
    const cJSON *jhost = cJSON_GetObjectItem(root, "host");
    const cJSON *radar = cJSON_GetObjectItem(root, "radar");
    const cJSON *past  = radar ? cJSON_GetObjectItem(radar, "past") : NULL;
    if (!cJSON_IsString(jhost) || !cJSON_IsArray(past)) goto done;
    if (strlen(jhost->valuestring) + 1 > host_sz) goto done;
    if (!rainviewer_host_ok(jhost->valuestring)) goto done;

    int total = cJSON_GetArraySize(past);
    if (total <= 0) goto done;

    /* Walk backwards from the end so out[0] is the newest. */
    for (int i = total - 1; i >= 0 && count < max_out; i--) {
        const cJSON *e = cJSON_GetArrayItem(past, i);
        const cJSON *jt = e ? cJSON_GetObjectItem(e, "time") : NULL;
        const cJSON *jp = e ? cJSON_GetObjectItem(e, "path") : NULL;
        if (!cJSON_IsNumber(jt) || !cJSON_IsString(jp)) continue;
        if (jt->valuedouble <= 0.0) continue;
        if (!rainviewer_path_ok(jp->valuestring)) continue;
        out[count].time = (uint32_t)jt->valuedouble;
        /* Length already bounded by rainviewer_path_ok(). */
        memcpy(out[count].path, jp->valuestring, strlen(jp->valuestring) + 1);
        count++;
    }
    if (count > 0) memcpy(host, jhost->valuestring, strlen(jhost->valuestring) + 1);

done:
    cJSON_Delete(root);
    return count;
}

/* ── Device half (main/rainviewer.c) ── */

#ifdef __cplusplus
extern "C" {
#endif

/* Config wrapper for rainviewer_selected(): true when THIS configuration is
 * served by the worldwide source. Read live wherever the radar page's
 * behaviour differs (crop, dark mode, the partial-frame gate, the page label,
 * the location marker). */
bool radar_use_rainviewer(const app_config_t *c);

/* Refresh the frame list from weather-maps.json and publish the newest
 * @p max_out frame times into @p stamps (newest first). Returns the count, 0
 * on failure. The paths and host stay inside rainviewer.c. Network call:
 * the caller holds the shared image fetch gate. */
int rainviewer_refresh(const app_config_t *c, uint32_t *stamps, int max_out);

/* Build loop frame @p i (0 = newest, indexing the list the last
 * rainviewer_refresh() produced) into a fresh panel-sized RGB565 PSRAM buffer
 * the caller owns and frees with heap_caps_free(). Fetches the coastline
 * basemap on first use and caches it for the window; each frame is a copy of
 * that basemap with the RainViewer tiles alpha-pasted over it. A tile that
 * fails to fetch or decode leaves bare basemap and the frame is still
 * returned. false only when the frame index is out of range or the frame
 * buffer could not be allocated. Network calls: the caller holds the shared
 * image fetch gate. */
bool rainviewer_build_frame(const app_config_t *c, int i,
                            uint8_t **out_buf, uint16_t *out_w, uint16_t *out_h);

/* Drop the cached basemap and the frame list (page park, or a settings change
 * that moves the window). Frees PSRAM; safe to call when nothing is cached. */
void rainviewer_release(void);

#ifdef __cplusplus
}
#endif
