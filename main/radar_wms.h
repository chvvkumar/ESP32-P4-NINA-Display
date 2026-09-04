#pragma once

/*
 * radar_wms.h - pure URL and geometry decisions for the road-free radar
 * source (NCEP GeoServer WMS, opengeo.ncep.noaa.gov), map styles 1 and 2 of
 * the Weather Radar page. Style 0 (RIDGE-2 GIFs) is untouched by this file.
 *
 * Header-only and free of ESP-IDF, FreeRTOS and LVGL, so the host test suite
 * can exercise the parts that are easy to get wrong (Mercator maths, bbox
 * shape, layer/style pairing, the capabilities TIME parser) without a device.
 * Same precedent as radar_play.h. Fetching and the frame ring live in
 * image_page_poll.c and ui/nina_image_page.c.
 *
 * Verified against the live server 2026-08-17; details in
 * docs/radar-road-free-source-plan.md. Traps this header is written around:
 *
 *  - An out-of-range or absent TIME returns HTTP 200 with a VALID GIF: with
 *    no time the server draws the newest frame (byte-identical to asking for
 *    the newest stamp), with a bad time it draws boundaries and an EMPTY radar
 *    layer. Never step time blindly; take the stamps from the layer's own
 *    GetCapabilities TIME dimension (radar_wms_parse_times) and hand them back
 *    verbatim. Only the newest frame may be requested without a time.
 *
 *  - The gray line style is a silent dependency. "boundary_gray" is ACCEPTED
 *    on nws:state_boundary and nws:us_counties but is not advertised for them
 *    in the capabilities. If it is ever unregistered the server still answers
 *    200 image/gif with the radar and NO lines: a black frame with echoes on
 *    it and no error to catch. Recorded here so a "lines vanished" report is
 *    read as a server-side style change, not a bbox bug.
 *
 *  - Parameter errors (unknown layer, bad crs) return HTTP 200 text/xml. The
 *    fetch path already rejects a payload without the GIF8 magic.
 *
 * bbox is EPSG:3857 (Web Mercator) METRES, computed with float-only
 * spherical Mercator (R = 6378137, tanf/logf) and printed as integers: a
 * pixel or so of error at the picture edge, and no double on the P4. Regions
 * use the RIDGE-2 extents measured in verify-report.md so the picture frames
 * like the RIDGE product it replaces (RIDGE draws them in 3857 too).
 *
 * The site box is SQUARE in metres, not lat/lon +-5: a 10x10 degree box is
 * taller than wide in Mercator north of the equator, and the display path
 * scales every frame to the panel width and centres it, so a non-square box
 * letterboxes. Half-width in metres is taken from +-5 degrees of longitude
 * at the site and reused as the half-height, which keeps the map conformal,
 * the radar disc round, and the frame exactly 600x600.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "radar_play.h"   /* radar_token_valid(): the token alphabet ban */

#define RADAR_WMS_URL_MAX   512   /* url buffer size the caller must provide */
#define RADAR_WMS_TIME_MAX  32    /* "2026-08-17T20:50:22.000Z" + NUL, with slack */
#define RADAR_WMS_TIMES_MAX 10    /* == max radar_frames (ring capacity) */

#define RADAR_WMS_BASE      "https://opengeo.ncep.noaa.gov/geoserver/"
#define RADAR_WMS_PX        600   /* long side of every frame */

/* Region / mosaic table row: token as the user config stores it (uppercase), the WMS
 * namespace and bare layer name, and the CRS:84 box in degrees. */
typedef struct {
    const char *token;      /* "CONUS", "ALASKA", "HAWAII", "NORTHEAST", ... (12 rows) */
    const char *ns;         /* "conus" | "alaska" | "hawaii" */
    const char *layer;      /* "conus_bref_qcd" | "alaska_bref_qcd" | "hawaii_bref_qcd" */
    float min_lon, min_lat, max_lon, max_lat;
} radar_wms_region_t;

/* Region table (degrees). AK/HI/CONUS = the mosaics' advertised extents; the nine
 * regionals = RIDGE-2 extents measured by state-line fit (verify-report.md). */
static const radar_wms_region_t s_radar_wms_regions[] = {
    { "CONUS",        "conus",  "conus_bref_qcd",  -130.0f, 20.0f,  -60.0f, 55.0f },
    { "ALASKA",       "alaska", "alaska_bref_qcd", -176.0f, 50.0f, -126.0f, 72.0f },
    { "HAWAII",       "hawaii", "hawaii_bref_qcd", -164.0f, 15.0f, -151.0f, 26.0f },
    { "NORTHEAST",    "conus",  "conus_bref_qcd",   -81.0f, 35.0f,  -66.0f, 50.0f },
    { "SOUTHEAST",    "conus",  "conus_bref_qcd",   -91.0f, 22.0f,  -75.0f, 37.0f },
    { "UPPERMISSVLY", "conus",  "conus_bref_qcd",  -103.0f, 35.0f,  -89.0f, 50.0f },
    { "SOUTHMISSVLY", "conus",  "conus_bref_qcd",   -98.0f, 25.0f,  -82.0f, 39.0f },
    { "NORTHROCKIES", "conus",  "conus_bref_qcd",  -117.0f, 35.0f, -100.0f, 50.0f },
    { "SOUTHROCKIES", "conus",  "conus_bref_qcd",  -120.0f, 26.0f, -102.0f, 40.0f },
    { "PACNORTHWEST", "conus",  "conus_bref_qcd",  -129.0f, 35.0f, -111.0f, 50.0f },
    { "PACSOUTHWEST", "conus",  "conus_bref_qcd",  -129.0f, 28.0f, -111.0f, 43.0f },
    { "CENTGRLAKES",  "conus",  "conus_bref_qcd",   -92.0f, 35.0f,  -77.0f, 49.0f },
};

/* Table lookup by exact token. NULL when the token is not a region/mosaic (=> a site id). */
static inline const radar_wms_region_t *radar_wms_region(const char *token)
{
    if (token == NULL) return NULL;
    size_t n = sizeof(s_radar_wms_regions) / sizeof(s_radar_wms_regions[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s_radar_wms_regions[i].token, token) == 0) return &s_radar_wms_regions[i];
    }
    return NULL;
}

#define RADAR_WMS_R_M       6378137.0f
#define RADAR_WMS_DEG2RAD   0.017453292f
#define RADAR_WMS_PI        3.14159265f

/* Web-Mercator (EPSG:3857) forward: metres. Float math only (tanf/logf). */
static inline float radar_wms_merc_x(float lon_deg)
{
    return RADAR_WMS_R_M * (lon_deg * RADAR_WMS_DEG2RAD);
}

static inline float radar_wms_merc_y(float lat_deg)
{
    if (lat_deg > 85.0f) lat_deg = 85.0f;      /* tanf blows up at the pole */
    if (lat_deg < -85.0f) lat_deg = -85.0f;
    float phi = lat_deg * RADAR_WMS_DEG2RAD;
    return RADAR_WMS_R_M * logf(tanf(RADAR_WMS_PI * 0.25f + phi * 0.5f));
}

/* Pixel size for a Mercator box: width 600 when the box is wider than tall (height =
 * 600*yspan/xspan), else height 600 and width = 600*xspan/yspan. Both >= 1, <= 600. */
static inline void radar_wms_pixel_size(float xspan_m, float yspan_m, int *w, int *h)
{
    int ww = RADAR_WMS_PX, hh = RADAR_WMS_PX;
    if (xspan_m > 0.0f && yspan_m > 0.0f) {
        if (xspan_m > yspan_m) {
            hh = (int)(RADAR_WMS_PX * yspan_m / xspan_m + 0.5f);
        } else if (yspan_m > xspan_m) {
            ww = (int)(RADAR_WMS_PX * xspan_m / yspan_m + 0.5f);
        }
    }
    if (ww < 1) ww = 1;
    if (hh < 1) hh = 1;
    if (ww > RADAR_WMS_PX) ww = RADAR_WMS_PX;
    if (hh > RADAR_WMS_PX) hh = RADAR_WMS_PX;
    if (w) *w = ww;
    if (h) *h = hh;
}

/* GIBS renders a GetMap at a very coarse level whenever the requested box's
 * VERTICAL metres-per-pixel is coarser than the horizontal by more than about
 * 0.001 m/px (whole-metre BBOX rounding can land on the wrong side of that by
 * itself). Widens the box, in whole metres, until
 * (maxx-minx)*@p h >= (maxy-miny)*@p w: on the x side that is not pinned to a
 * world/window edge when only one side is pinned, on maxx when neither side
 * is pinned, and by trimming maxy when both x sides are pinned (a box that
 * already spans the full window/world in x, so there is no x room left).
 * Capped at 8 steps, one whole-metre rounding error is at most a couple of
 * steps. Shared by rainviewer_basemap_url() (radar's worldwide basemap) and
 * clouds_bbox_rounded() (clouds_wms.h) so the two call sites the trap was
 * found on cannot drift apart; see rainviewer.h for the measured GIBS sweep
 * that established the 0.001 m/px threshold. */
static inline void radar_wms_square_pixels(long *minx, long *miny, long *maxx, long *maxy,
                                           int w, int h, bool west_pinned, bool east_pinned)
{
    if (minx == NULL || miny == NULL || maxx == NULL || maxy == NULL) return;
    for (int i = 0; i < 8; i++) {
        if ((int64_t)(*maxx - *minx) * (int64_t)h >=
            (int64_t)(*maxy - *miny) * (int64_t)w) break;
        if (west_pinned && east_pinned) {
            (*maxy)--;
        } else if (east_pinned) {
            (*minx)--;
        } else {
            (*maxx)++;
        }
    }
}

/* Lowercase a validated token into @p out (sz >= 16). false when the token fails
 * radar_token_valid(): that is the same alphabet ban the RIDGE builder applies, and
 * it also rules out any URL surgery through the site layer name. */
static inline bool radar_wms_lower_token(const char *token, char *out, size_t sz)
{
    if (!radar_token_valid(token)) return false;
    size_t n = strlen(token);
    if (n + 1 > sz) return false;
    for (size_t i = 0; i < n; i++) {
        char c = token[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = '\0';
    return true;
}

/* A stamp is copied verbatim into the query string, so refuse anything that could
 * split or terminate it. Length is bounded by the snprintf truncation check. */
static inline bool radar_wms_time_ok(const char *time)
{
    for (const char *p = time; *p; p++) {
        if (*p == '&' || *p == '?' || *p == '#' || *p == ' ' || *p == '<') return false;
    }
    return true;
}

/* Build the GetMap URL for one frame.
 *   style     1 = radar + nws:state_boundary, 2 = radar + nws:state_boundary + nws:us_counties
 *   token     resolved radar token (uppercase site id such as "KLSX", or a table token)
 *   site_lat/site_lon  used only when token is NOT in the region table (square Mercator
 *             box centred on the site, half-side = merc_x(lon+5)-merc_x(lon), 600x600)
 *   time      ISO-8601 stamp as advertised (e.g. "2026-08-17T20:50:22.000Z") or NULL /
 *             "" for the server default (= newest, verified 2026-08-17)
 * URL shape (crs=EPSG:3857, bbox in metres printed as integers, styles matched 1:1 to
 * layers: radar_reflectivity then boundary_gray[,boundary_gray]):
 *   https://opengeo.ncep.noaa.gov/geoserver/ows?service=WMS&version=1.3.0&request=GetMap
 *     &layers=klsx:klsx_sr_bref,nws:state_boundary&styles=radar_reflectivity,boundary_gray
 *     &crs=EPSG:3857&bbox=<minx>,<miny>,<maxx>,<maxy>&width=600&height=600
 *     &format=image/gif&bgcolor=0x000000&transparent=false[&time=<stamp>]
 * The site layer name is "<lower>:<lower>_sr_bref" (token lowercased). Returns false and
 * writes "" when style is 0, token is empty/invalid/too long, the stamp carries a query
 * or tag character, or the result would not fit in @p sz. */
static inline bool radar_wms_frame_url(char *out, size_t sz, uint8_t style, const char *token,
                                       float site_lat, float site_lon, const char *time)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    if (style != 1 && style != 2) return false;
    if (time != NULL && time[0] != '\0' && !radar_wms_time_ok(time)) return false;

    char layer[48];
    float minx, miny, maxx, maxy;
    const radar_wms_region_t *r = radar_wms_region(token);
    if (r != NULL) {
        int n = snprintf(layer, sizeof(layer), "%s:%s", r->ns, r->layer);
        if (n < 0 || (size_t)n >= sizeof(layer)) return false;
        minx = radar_wms_merc_x(r->min_lon);
        maxx = radar_wms_merc_x(r->max_lon);
        miny = radar_wms_merc_y(r->min_lat);
        maxy = radar_wms_merc_y(r->max_lat);
        /* The display path scales every frame to the panel WIDTH, top-anchored, so a
         * portrait frame overflows the bottom of the screen. Pad a taller-than-wide
         * region symmetrically in x so the picture is always 600 x <=600. */
        float pad = ((maxy - miny) - (maxx - minx)) * 0.5f;
        if (pad > 0.0f) {
            minx -= pad;
            maxx += pad;
        }
    } else {
        char lower[16];
        if (!radar_wms_lower_token(token, lower, sizeof(lower))) return false;
        int n = snprintf(layer, sizeof(layer), "%s:%s_sr_bref", lower, lower);
        if (n < 0 || (size_t)n >= sizeof(layer)) return false;
        float cx = radar_wms_merc_x(site_lon);
        float cy = radar_wms_merc_y(site_lat);
        float half = radar_wms_merc_x(site_lon + 5.0f) - cx;
        minx = cx - half;
        maxx = cx + half;
        miny = cy - half;
        maxy = cy + half;
    }

    int w, h;
    radar_wms_pixel_size(maxx - minx, maxy - miny, &w, &h);

    bool has_time = (time != NULL && time[0] != '\0');
    int n = snprintf(out, sz,
                     RADAR_WMS_BASE "ows?service=WMS&version=1.3.0&request=GetMap"
                     "&layers=%s,nws:state_boundary%s"
                     "&styles=radar_reflectivity,boundary_gray%s"
                     "&crs=EPSG:3857&bbox=%ld,%ld,%ld,%ld&width=%d&height=%d"
                     "&format=image/gif&bgcolor=0x000000&transparent=false%s%s",
                     layer,
                     (style == 2) ? ",nws:us_counties" : "",
                     (style == 2) ? ",boundary_gray" : "",
                     (long)lroundf(minx), (long)lroundf(miny),
                     (long)lroundf(maxx), (long)lroundf(maxy),
                     w, h,
                     has_time ? "&time=" : "",
                     has_time ? time : "");
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Namespace-scoped GetCapabilities URL:
 *   https://opengeo.ncep.noaa.gov/geoserver/<ns>/ows?service=WMS&version=1.3.0&request=GetCapabilities
 * ns = lowercased site id, or the table row's ns. false when token is empty/too long. */
static inline bool radar_wms_caps_url(char *out, size_t sz, const char *token)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    char lower[16];
    const char *ns;
    const radar_wms_region_t *r = radar_wms_region(token);
    if (r != NULL) {
        ns = r->ns;
    } else {
        if (!radar_wms_lower_token(token, lower, sizeof(lower))) return false;
        ns = lower;
    }
    int n = snprintf(out, sz, RADAR_WMS_BASE "%s/ows?service=WMS&version=1.3.0&request=GetCapabilities", ns);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Bare <Name> of the radar layer inside that namespace-scoped caps document:
 * "<lower>_sr_bref" for a site, or the table row's layer. false when token invalid. */
static inline bool radar_wms_layer_name(char *out, size_t sz, const char *token)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    int n;
    const radar_wms_region_t *r = radar_wms_region(token);
    if (r != NULL) {
        n = snprintf(out, sz, "%s", r->layer);
    } else {
        char lower[16];
        if (!radar_wms_lower_token(token, lower, sizeof(lower))) return false;
        n = snprintf(out, sz, "%s_sr_bref", lower);
    }
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Bounded memmem: first occurrence of NUL-terminated @p needle in [hay, hay+len).
 * Never reads past hay+len, so the body needs no NUL terminator. */
static inline const char *radar_wms_find(const char *hay, size_t len, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0 || len < nl) return NULL;
    for (size_t i = 0; i + nl <= len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nl) == 0) return hay + i;
    }
    return NULL;
}

/* Byte range of the <Layer> whose <Name> is EXACTLY @p layer_name in a WMS
 * GetCapabilities body: *begin just past "</Name>", *end at whichever of the
 * next "<Layer" or "</Layer>" comes first (or the body end).
 *
 * The exact-name match and that bound are the whole correctness of every
 * per-layer read built on this: one workspace document holds siblings whose
 * dimensions differ (measured 2026-09-03 in mtg_fd, some layers on a 10-minute
 * grid and others on a 15-minute one), so a scan that took the first matching
 * element in the document would answer with a neighbour's data. Bounded by
 * @p xml_len throughout: the body needs no NUL terminator. */
static inline bool radar_wms_layer_bounds(const char *xml, size_t xml_len,
                                          const char *layer_name,
                                          const char **begin, const char **end)
{
    if (xml == NULL || layer_name == NULL || begin == NULL || end == NULL) return false;

    char needle[64];
    int nn = snprintf(needle, sizeof(needle), "<Name>%s</Name>", layer_name);
    if (nn < 0 || (size_t)nn >= sizeof(needle)) return false;

    const char *p = radar_wms_find(xml, xml_len, needle);
    if (p == NULL) return false;
    p += (size_t)nn;

    const char *e = xml + xml_len;
    size_t rem = (size_t)(e - p);
    const char *lc = radar_wms_find(p, rem, "</Layer>");
    const char *lo = radar_wms_find(p, rem, "<Layer");
    if (lc != NULL && lc < e) e = lc;
    if (lo != NULL && lo < e) e = lo;

    *begin = p;
    *end = e;
    return true;
}

/* Parse the enumerated TIME list of `layer_name` from a GetCapabilities body.
 * Bounds the layer with radar_wms_layer_bounds(), then finds, within it, the next
 * "<Dimension" whose attributes contain name="time",
 * then the text between its '>' and "</Dimension>": a comma-separated ASCENDING list of
 * ISO stamps (real shape 2026-08-17: <Dimension name="time" default="..." units="ISO8601"
 * nearestValue="1">stamp,stamp,...</Dimension>, entries like 2026-08-17T20:50:22.000Z).
 * Copies up to max_out stamps, NEWEST FIRST: out[0] = last list entry, out[1] = the one
 * before it, ... Each NUL-terminated, whitespace trimmed, entries longer than
 * RADAR_WMS_TIME_MAX-1 rejected (return 0). Returns the count; 0 when the layer or
 * dimension is missing, the content contains '/' (interval form), or the list is empty.
 * Every scan is bounded by xml_len: the body does NOT need a NUL terminator. */
static inline int radar_wms_parse_times(const char *xml, size_t xml_len, const char *layer_name,
                                        char (*out)[RADAR_WMS_TIME_MAX], int max_out)
{
    if (out == NULL || max_out <= 0) return 0;

    const char *p, *end;
    if (!radar_wms_layer_bounds(xml, xml_len, layer_name, &p, &end)) return 0;

    const char *content = NULL;
    size_t clen = 0;
    while (p < end) {
        const char *d = radar_wms_find(p, (size_t)(end - p), "<Dimension");
        if (d == NULL) return 0;
        const char *gt = memchr(d, '>', (size_t)(end - d));
        if (gt == NULL) return 0;
        if (gt > d && gt[-1] == '/') {   /* self-closing <Dimension .../>: no content */
            p = gt + 1;
            continue;
        }
        const char *close = radar_wms_find(gt + 1, (size_t)(end - gt - 1), "</Dimension>");
        if (close == NULL) return 0;
        if (radar_wms_find(d, (size_t)(gt - d), "name=\"time\"") != NULL) {
            content = gt + 1;
            clen = (size_t)(close - content);
            break;
        }
        p = close + 12;   /* strlen("</Dimension>") */
    }
    if (content == NULL) return 0;
    if (memchr(content, '/', clen) != NULL) return 0;   /* start/end/period form */

    /* Walk the list from the END so out[0] is the newest. */
    int count = 0;
    const char *e = content + clen;
    while (e > content && count < max_out) {
        const char *s = e;
        while (s > content && s[-1] != ',') s--;
        const char *a = s;
        const char *b = e;
        while (a < b && (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
        size_t len = (size_t)(b - a);
        if (len > 0) {
            if (len > RADAR_WMS_TIME_MAX - 1) return 0;
            memcpy(out[count], a, len);
            out[count][len] = '\0';
            count++;
        }
        e = (s > content) ? s - 1 : content;   /* skip the comma */
    }
    return count;
}
