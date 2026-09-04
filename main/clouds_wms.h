#pragma once

/*
 * clouds_wms.h - pure URL, geometry and time decisions for the Clouds page
 * (a geostationary satellite picture under a selectable vector basemap overlay:
 * borders and roads, coastlines only, borders plus a graticule, or none).
 *
 * The satellite is whichever of the five rows below sits nearest the weather
 * longitude, so the page works anywhere on Earth: GOES-East and GOES-West from
 * NASA GIBS, Himawari from GIBS, and the two Meteosat services (MTG at 0 and
 * MSG IODC at 45.5E) from EUMETView. The provider decides the URL shape and
 * where the frame times come from.
 *
 * Header-only and free of ESP-IDF, FreeRTOS and LVGL, so the host test suite
 * (test/host/test_clouds_wms.c) can exercise the parts that are easy to get
 * wrong: the satellite pick, the Web-Mercator box and its date-line clamp, both
 * GetMap URL shapes, the DescribeDomains parser and the UTC calendar
 * arithmetic. Same precedent as radar_wms.h, whose Mercator helpers this file
 * reuses. Fetching and the frame ring live in image_page_poll.c and
 * ui/nina_image_page.c.
 *
 * Verified against the live server 2026-08-18 (scratchpad gibs-robustness.md).
 * Traps this header is written around:
 *
 *  - TIME is mandatory and must be YYYY-MM-DDThh:mm:ssZ on the 10-minute grid.
 *    Omitting it serves whatever the (stale) capabilities default names, and
 *    off-grid values floor silently. Never send Reference_Labels_15m: it blanks
 *    the whole JPEG.
 *
 *  - A future or missing slot is NOT an error: the server answers 200 image/jpeg
 *    with the vector overlay drawn over black. DescribeDomains (a ~320 B REST
 *    document, cache-busted by a to-the-second window start) is the only cheap
 *    source of the slots that exist, so the ring is fed from it, never by
 *    blind stepping. When it fails, floor(now,10min)-50min is the fallback
 *    newest (frames land 30-47 min behind wall clock; -50 keeps the guess at
 *    or behind the real newest so a real list heals it instead of leaving a
 *    blank frame stranded as "Latest" ahead of every real stamp).
 *
 *  - An HTTP 200 blank JPEG for a slot that DescribeDomains listed but the
 *    origin has not filled, and a partial frame whose missing tiles are black
 *    blocks, are caught by clouds_frame_incomplete() (black-sample fraction)
 *    before the frame reaches the ring; the poller re-fetches the stamp on the
 *    next poll. A PARTIALLY ingested frame (black tile rectangles under the
 *    vector basemap) is too light to trip that gate and is caught instead by
 *    clouds_frame_holes(), per cell against the neighbouring frame of the loop.
 *    ponytail: that needs a neighbour, so the very first frame into an empty
 *    ring is still accepted unjudged. Upgrade path: re-judge the head once the
 *    backfill has given it one (image_page_poll.c).
 *
 * All maths is float-only (the P4 FPU is single precision); the calendar
 * helpers are integer days-from-civil / civil-from-days, no localtime.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "radar_wms.h"    /* radar_wms_merc_x/y, radar_wms_find */
#include "screen_geom.h"  /* screen_size(): the frame is the panel width */
#include "sun_pos.h"      /* sun_elevation_deg(): day/night pick for a photo layer */

#define CLOUDS_URL_MAX        RADAR_WMS_URL_MAX   /* 512: the GIBS GetMap URL is ~290 chars, 302 worst
                                                    case; the EUMETView one ~347 worst case (longest
                                                    layer + longest basemap suffix + the 17-character
                                                    &bgcolor=0x000000 tail) */
#define CLOUDS_TIME_MAX       21                  /* "2026-08-18T04:00:00Z" + NUL */
#define CLOUDS_TIMES_MAX      10                  /* == max clouds_frames (ring capacity) */
#define CLOUDS_GIBS_PERIOD_S  600u                /* GOES/Himawari ABI-AHI cadence, PT10M; also the
                                                    DescribeDomains default when a segment omits PTnM */
#define CLOUDS_FALLBACK_LAG_S 3000u               /* 50 min behind wall clock when the domain fetch fails */
/* Frame size in pixels, which is the panel width. The GIBS GetMap WIDTH/HEIGHT
 * and the EPSG:3857 bbox half-width are BOTH derived from this number: change
 * one without the other and the map scale silently changes. */
#define CLOUDS_PX             (screen_size())
#define CLOUDS_ZOOM_MIN       5
#define CLOUDS_ZOOM_MAX       9
#define CLOUDS_DOMAIN_BACK_S  (3u * 3600u)        /* DescribeDomains window: now-3h .. now+1h */
#define CLOUDS_DOMAIN_FWD_S   3600u

#define CLOUDS_GIBS_BASE   "https://gibs.earthdata.nasa.gov/"
#define CLOUDS_EUMET_BASE  "https://view.eumetsat.int/geoserver/"
#define CLOUDS_SUN_MIN_EL_DEG 10.0f   /* visible-light layers need the sun this high */
#define CLOUDS_BLANK_PCT      90      /* near-black sample share above which a frame is a blank slot */
#define CLOUDS_EUMET_LAG_S    1800u   /* 30 min behind wall clock when the capabilities fetch fails */

/* Which product a frame is drawn from. The GeoColor-role channel resolves to
 * PHOTO_DAY or PHOTO_NIGHT per frame from the sun at the weather location at
 * that frame's own time, so a loop through dusk legitimately mixes styles. */
typedef enum {
    CLOUDS_ROLE_PHOTO_DAY   = 0,
    CLOUDS_ROLE_PHOTO_NIGHT = 1,
    CLOUDS_ROLE_IR          = 2,
    CLOUDS_ROLE_AIR         = 3,
} clouds_role_t;

typedef enum { CLOUDS_PROV_GIBS = 0, CLOUDS_PROV_EUMET = 1 } clouds_provider_t;

/* One geostationary satellite the page can draw from.
 *
 *   name        the caption word ("GOES", "Himawari", "Meteosat")
 *   sub_lon     sub-satellite longitude, degrees east; the pick is nearest
 *               wrap-around distance to the weather longitude
 *   period_s    the publishing grid, 600 or 900 seconds
 *   photo_day / photo_night
 *               the GeoColor-role layer with the sun up / down. EQUAL when the
 *               satellite publishes a true day-night composite (GOES GeoColor,
 *               MTG GeoColour); different when it does not, where the night
 *               half falls back to the infrared layer.
 *   photo_word  the caption word for photo_day ("GeoColor", "GeoColour",
 *               "Visible", "Natural"). The night word is this one when the
 *               composite is real, "Clean IR" otherwise.
 *   ws_photo / ws_air
 *               EUMETView workspace the photo/IR layers and the air-mass layer
 *               live in, which is where their advertised time is read from.
 *               NULL on a GIBS row, which uses DescribeDomains instead.
 *
 * Layer names are exactly the ones verified live on 2026-09-03 (design doc
 * section 1). A wrong name returns an XML ServiceException on both providers,
 * so a typo cannot silently serve the wrong picture. */
typedef struct {
    const char       *name;
    float             sub_lon;
    clouds_provider_t provider;
    uint16_t          period_s;
    const char       *photo_day;
    const char       *photo_night;
    const char       *clean_ir;
    const char       *air_mass;
    const char       *photo_word;
    const char       *ws_photo;
    const char       *ws_air;
} clouds_sat_t;

#define CLOUDS_SAT_COUNT 5

/* ORDER MATTERS at exactly one longitude: -106.2 is equidistant from GOES-East
 * and GOES-West, and clouds_sat_for_lon() keeps the FIRST row on a tie, which
 * preserves the pre-worldwide behaviour (-106.2 East, -106.3 West). */
static const clouds_sat_t s_clouds_sats[CLOUDS_SAT_COUNT] = {
    { "GOES",     -75.2f,  CLOUDS_PROV_GIBS,  600,
      "GOES-East_ABI_GeoColor", "GOES-East_ABI_GeoColor",
      "GOES-East_ABI_Band13_Clean_Infrared", "GOES-East_ABI_Air_Mass",
      "GeoColor",  NULL, NULL },
    { "GOES",     -137.2f, CLOUDS_PROV_GIBS,  600,
      "GOES-West_ABI_GeoColor", "GOES-West_ABI_GeoColor",
      "GOES-West_ABI_Band13_Clean_Infrared", "GOES-West_ABI_Air_Mass",
      "GeoColor",  NULL, NULL },
    { "Himawari",  140.7f, CLOUDS_PROV_GIBS,  600,
      "Himawari_AHI_Band3_Red_Visible_1km", "Himawari_AHI_Band13_Clean_Infrared",
      "Himawari_AHI_Band13_Clean_Infrared", "Himawari_AHI_Air_Mass",
      "Visible",   NULL, NULL },
    /* MTG-I1 at 0 degrees. Its air-mass product is not published for MTG, so
     * that channel comes from the MSG 0-degree service in the msg_fes
     * workspace, on a 15-minute grid; the server snaps a requested time to the
     * nearest real slot, so the 10-minute step-back still returns a picture. */
    { "Meteosat",  0.0f,   CLOUDS_PROV_EUMET, 600,
      "mtg_fd:rgb_geocolour", "mtg_fd:rgb_geocolour",
      "mtg_fd:ir105_hrfi", "msg_fes:rgb_airmass",
      "GeoColour", "mtg_fd", "msg_fes" },
    /* MSG Indian Ocean Data Coverage at 45.5 degrees east, 15-minute grid.
     * No GeoColor product: natural colour by day (rgb_natural is a colour RGB,
     * not a single monochrome band), IR 10.8 by night. */
    { "Meteosat",  45.5f,  CLOUDS_PROV_EUMET, 900,
      "msg_iodc:rgb_natural", "msg_iodc:ir108",
      "msg_iodc:ir108", "msg_iodc:rgb_airmass",
      "Natural",   "msg_iodc", "msg_iodc" },
};

/* Nearest satellite by wrap-around longitude distance. Never NULL. */
static inline const clouds_sat_t *clouds_sat_for_lon(float lon)
{
    const clouds_sat_t *best = &s_clouds_sats[0];
    float best_d = 360.0f;
    for (int i = 0; i < CLOUDS_SAT_COUNT; i++) {
        float d = s_clouds_sats[i].sub_lon - lon;
        d = fabsf(fmodf(d + 540.0f, 360.0f) - 180.0f);
        if (d < best_d) {            /* strict: a tie keeps the earlier row */
            best_d = d;
            best = &s_clouds_sats[i];
        }
    }
    return best;
}

/* Which product channel @p ch means for a frame at @p stamp. Channels 1 and 2
 * are fixed; channel 0 (GeoColor) asks the sun. An out-of-range channel falls
 * back to the GeoColor role rather than failing, so a config from a newer build
 * never blanks the page. */
static inline clouds_role_t clouds_role(uint8_t ch, float lat, float lon, uint32_t stamp)
{
    if (ch == 1) return CLOUDS_ROLE_IR;
    if (ch == 2) return CLOUDS_ROLE_AIR;
    return (sun_elevation_deg(lat, lon, stamp) >= CLOUDS_SUN_MIN_EL_DEG)
               ? CLOUDS_ROLE_PHOTO_DAY : CLOUDS_ROLE_PHOTO_NIGHT;
}

static inline const char *clouds_sat_layer(const clouds_sat_t *s, clouds_role_t role)
{
    if (s == NULL) return "";
    switch (role) {
        case CLOUDS_ROLE_PHOTO_NIGHT: return s->photo_night;
        case CLOUDS_ROLE_IR:          return s->clean_ir;
        case CLOUDS_ROLE_AIR:         return s->air_mass;
        case CLOUDS_ROLE_PHOTO_DAY:   break;
    }
    return s->photo_day;
}

/* The same layer as GeoServer names it INSIDE its workspace-scoped service.
 *
 * The table stores the workspace-qualified name ("mtg_fd:rgb_geocolour"), which
 * is what the GetMap layers= parameter on the global /geoserver/ows endpoint
 * needs. The per-workspace virtual service at /geoserver/{ws}/wms publishes the
 * SAME layer without the prefix (<Name>rgb_geocolour</Name>; the real mtg_fd
 * capture of 2026-09-03 contains zero occurrences of "mtg_fd:"), so a lookup in
 * that document must use the bare tail. Returns a pointer INTO @p layer, no
 * copy; a name with no ':' is already bare and comes back unchanged. */
static inline const char *clouds_layer_bare_name(const char *layer)
{
    if (layer == NULL) return NULL;
    const char *c = strchr(layer, ':');
    return (c != NULL) ? c + 1 : layer;
}

/* EUMETView workspace the layer for @p role lives in; NULL on a GIBS row. */
static inline const char *clouds_sat_workspace(const clouds_sat_t *s, clouds_role_t role)
{
    if (s == NULL) return NULL;
    return (role == CLOUDS_ROLE_AIR) ? s->ws_air : s->ws_photo;
}

/* Caption word for @p role on @p s. A night photo slot on a satellite with no
 * real day-night composite is the infrared layer, so it says so. The day/night
 * sameness is a strcmp, not a pointer compare: whether two identical string
 * literals in this table share one address is up to the compiler. */
static inline const char *clouds_role_word(const clouds_sat_t *s, clouds_role_t role)
{
    if (s == NULL) return "";
    switch (role) {
        case CLOUDS_ROLE_IR:  return "Clean IR";
        case CLOUDS_ROLE_AIR: return "Air Mass";
        case CLOUDS_ROLE_PHOTO_NIGHT:
            return (strcmp(s->photo_night, s->photo_day) == 0) ? s->photo_word : "Clean IR";
        case CLOUDS_ROLE_PHOTO_DAY:
            break;
    }
    return s->photo_word;
}

/* The layer one frame is drawn from: satellite by longitude, product by channel
 * and by the sun at @p stamp. */
static inline const char *clouds_layer(uint8_t ch, float lat, float lon, uint32_t stamp)
{
    const clouds_sat_t *s = clouds_sat_for_lon(lon);
    return clouds_sat_layer(s, clouds_role(ch, lat, lon, stamp));
}

/* Overlay-strip caption for one frame: "<satellite> <product>", e.g.
 * "Himawari Clean IR" or "Meteosat GeoColour". The frame time is written
 * separately by the page. false and "" when it would not fit. */
static inline bool clouds_caption(char *out, size_t sz, uint8_t ch, float lat,
                                  float lon, uint32_t stamp)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    const clouds_sat_t *s = clouds_sat_for_lon(lon);
    int n = snprintf(out, sz, "%s %s", s->name,
                     clouds_role_word(s, clouds_role(ch, lat, lon, stamp)));
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Basemap table (config field clouds_basemap, 0..3): the vector layers appended
 * after the raster channel in LAYERS, as a ready-made ",..." suffix (empty =
 * satellite alone). Never add Reference_Labels_15m here: it blanks the JPEG. */
/* Layer names verified live 2026-08-21 (5-layer GetMap incl. Coastlines_15m and
 * Graticule_15m returned 200 image/jpeg, scratchpad gibs-catalog.md). */
#define CLOUDS_BASEMAP_COUNT 4

static const char *const s_clouds_basemaps[CLOUDS_BASEMAP_COUNT] = {
    ",Reference_Features_15m",                  /* 0: borders and roads (default) */
    ",Coastlines_15m",                          /* 1: coastlines only */
    ",Reference_Features_15m,Graticule_15m",    /* 2: borders, roads and grid */
    "",                                         /* 3: none */
};

/* Suffix for @p bm; an out-of-range value falls back to 0, so a config from a
 * newer build never drops the overlay silently. */
static inline const char *clouds_basemap_suffix(uint8_t bm)
{
    return s_clouds_basemaps[(bm < CLOUDS_BASEMAP_COUNT) ? bm : 0];
}

/* The same four user choices mapped onto the closest EUMETView background
 * layers. Layer order is satellite first, overlays after, as with GIBS. */
static const char *const s_clouds_eumet_basemaps[CLOUDS_BASEMAP_COUNT] = {
    ",backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land",   /* 0: borders and roads */
    ",backgrounds:ne_10m_coastline",                                      /* 1: coastlines only */
    ",backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land"
        ",backgrounds:graticules-dark",                                   /* 2: borders and grid */
    "",                                                                   /* 3: none */
};

static inline const char *clouds_eumet_basemap_suffix(uint8_t bm)
{
    return s_clouds_eumet_basemaps[(bm < CLOUDS_BASEMAP_COUNT) ? bm : 0];
}

/* Half-width of the CLOUDS_PX box in Web-Mercator metres at Web-Mercator zoom
 * @p zoom (clamped 5..9): CLOUDS_PX px * (world circumference / (256 * 2^zoom)) / 2. */
static inline float clouds_half_m(uint8_t zoom)
{
    if (zoom < CLOUDS_ZOOM_MIN) zoom = CLOUDS_ZOOM_MIN;
    if (zoom > CLOUDS_ZOOM_MAX) zoom = CLOUDS_ZOOM_MAX;
    return ((float)CLOUDS_PX / 2.0f) * 40075016.686f / (256.0f * (float)(1u << zoom));
}

/* Full width of the Web-Mercator world in metres. A box that would run past
 * either edge is SHIFTED back inside, not shrunk, so the picture stays
 * CLOUDS_PX wide with no black band near the date line (spec 3.5). */
#define CLOUDS_MERC_MAX 20037508.0f

/* How far east the box has to move, in metres, to keep it inside the world:
 * positive near 180W, negative near 180E, 0 everywhere else. The ONE piece of
 * clamp arithmetic; clouds_bbox() and clouds_centre_dx_px() both read it, so
 * the picture and the location marker can never disagree. */
static inline float clouds_clamp_shift_m(float lon, uint8_t zoom)
{
    float cx = radar_wms_merc_x(lon);
    float half = clouds_half_m(zoom);
    if (cx - half < -CLOUDS_MERC_MAX) return -CLOUDS_MERC_MAX - (cx - half);
    if (cx + half >  CLOUDS_MERC_MAX) return  CLOUDS_MERC_MAX - (cx + half);
    return 0.0f;
}

/* Where the true location sits relative to the CENTRE of the frame, in pixels,
 * positive to the right. 0 unless the date-line clamp moved the box: the box
 * shifts but the location does not, so a marker drawn at the frame centre would
 * point at the wrong place (at zoom 5 a Wellington box shifts about 240 px).
 * @p lat is unused: only x is ever clamped. */
static inline int clouds_centre_dx_px(float lat, float lon, uint8_t zoom)
{
    (void)lat;
    float shift = clouds_clamp_shift_m(lon, zoom);
    if (shift == 0.0f) return 0;
    float m_per_px = 2.0f * clouds_half_m(zoom) / (float)CLOUDS_PX;
    return (int)lroundf(-shift / m_per_px);
}

/* Square EPSG:3857 box centred on lat/lon, clamped into the world in x.
 * Latitude is clamped to +-85 inside radar_wms_merc_y(). (0,0) is a valid
 * centre (Gulf of Guinea), no special case. */
static inline void clouds_bbox(float lat, float lon, uint8_t zoom,
                               float *minx, float *miny, float *maxx, float *maxy)
{
    float cx = radar_wms_merc_x(lon) + clouds_clamp_shift_m(lon, zoom);
    float cy = radar_wms_merc_y(lat);
    float half = clouds_half_m(zoom);
    if (minx) *minx = cx - half;
    if (miny) *miny = cy - half;
    if (maxx) *maxx = cx + half;
    if (maxy) *maxy = cy + half;
}

/* clouds_bbox(), rounded to whole metres for the GetMap BBOX and then widened
 * (radar_wms_square_pixels(), main/radar_wms.h) so the request is never
 * VERTICALLY coarser per pixel than horizontally by more than about
 * 0.001 m/px -- the same GIBS coarse-render rule already fixed for the radar
 * basemap in rainviewer.h. Whole-metre rounding alone is enough to land on
 * the wrong side: measured 2026-09-03, Wellington NZ (-41.29, 174.78) zoom 6,
 * the unwidened box has resx 2445.983333, resy 2445.984722 (+0.001389 m/px)
 * and GIBS served a blocky 26 KB frame; widening minx 2 m west flips the sign
 * to -0.001389 and GIBS served the crisp 73 KB frame at the identical
 * position (full URLs and byte counts in
 * .superpowers/sdd/2026-09-03-world-radar-phase2/clouds-dateline-investigation.md).
 * The world-edge side pinned by clouds_clamp_shift_m() above is read straight
 * off the UNROUNDED box so the pin test matches the clamp exactly. No
 * clouds_zoom this file offers can pin both x sides at once (zoom 5's
 * half_m tops out at ~1.76M m against a 40.0M m wide world), so the "trim
 * maxy" fallback in radar_wms_square_pixels() never fires here; it exists for
 * the shared helper's other caller, radar's worldwide basemap window, whose
 * box can be much larger relative to the world. */
static inline void clouds_bbox_rounded(float lat, float lon, uint8_t zoom,
                                       long *minx, long *miny, long *maxx, long *maxy)
{
    float fminx, fminy, fmaxx, fmaxy;
    clouds_bbox(lat, lon, zoom, &fminx, &fminy, &fmaxx, &fmaxy);
    if (minx) *minx = (long)lroundf(fminx);
    if (miny) *miny = (long)lroundf(fminy);
    if (maxx) *maxx = (long)lroundf(fmaxx);
    if (maxy) *maxy = (long)lroundf(fmaxy);
    if (minx == NULL || miny == NULL || maxx == NULL || maxy == NULL) return;
    const bool west_pinned = fminx <= -CLOUDS_MERC_MAX;
    const bool east_pinned = fmaxx >=  CLOUDS_MERC_MAX;
    radar_wms_square_pixels(minx, miny, maxx, maxy, CLOUDS_PX, CLOUDS_PX, west_pinned, east_pinned);
}

/* ---- UTC calendar arithmetic (proleptic Gregorian, no localtime) ---- */

/* Days since 1970-01-01 for a civil date (Howard Hinnant's days_from_civil). */
static inline int64_t clouds_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - (int)era * 400);
    const unsigned doy = (153u * (m > 2 ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Civil date from days since 1970-01-01 (Howard Hinnant's civil_from_days). */
static inline void clouds_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm = mp < 10u ? mp + 3u : mp - 9u;
    if (y) *y = (int)(yy + (mm <= 2u ? 1 : 0));
    if (m) *m = mm;
    if (d) *d = dd;
}

/* Format @p epoch (UTC seconds) as "YYYY-MM-DDThh:mm:ssZ". @p sz >= CLOUDS_TIME_MAX. */
static inline bool clouds_time_format(char *out, size_t sz, uint32_t epoch)
{
    if (out == NULL || sz == 0) return false;
    int y; unsigned m, d;
    clouds_civil_from_days((int64_t)(epoch / 86400u), &y, &m, &d);
    unsigned s = epoch % 86400u;
    int n = snprintf(out, sz, "%04d-%02u-%02uT%02u:%02u:%02uZ",
                     y, m, d, s / 3600u, (s / 60u) % 60u, s % 60u);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Parse @p n digits at @p s into *v; false on any non-digit. */
static inline bool clouds_parse_digits(const char *s, size_t n, unsigned *v)
{
    unsigned acc = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        acc = acc * 10u + (unsigned)(s[i] - '0');
    }
    *v = acc;
    return true;
}

/* Parse "YYYY-MM-DDThh:mm:ssZ" (or "YYYY-MM-DDThh:mmZ") from [s, s+len) into
 * UTC seconds. STRICT: an entry without 'T' (a bare date) is rejected, so a
 * caller never mistakes a date for a frame time. Trailing junk after 'Z' fails. */
static inline bool clouds_time_parse(const char *s, size_t len, uint32_t *out)
{
    unsigned Y, M, D, h, mi, se = 0;
    if (s == NULL || out == NULL) return false;
    if (len < 17) return false;                        /* "YYYY-MM-DDThh:mmZ" */
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':') return false;
    if (!clouds_parse_digits(s, 4, &Y) || !clouds_parse_digits(s + 5, 2, &M) ||
        !clouds_parse_digits(s + 8, 2, &D) || !clouds_parse_digits(s + 11, 2, &h) ||
        !clouds_parse_digits(s + 14, 2, &mi)) return false;
    size_t p = 16;
    if (s[p] == ':') {
        if (len < 20 || !clouds_parse_digits(s + 17, 2, &se)) return false;
        p = 19;
    }
    if (p >= len || s[p] != 'Z' || p + 1 != len) return false;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || se > 59) return false;
    if (Y < 1970 || Y > 2105) return false;              /* keeps the product inside uint32 */
    int64_t days = clouds_days_from_civil((int)Y, M, D);
    *out = (uint32_t)(days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + (int64_t)se);
    return true;
}

/* Bare "YYYY-MM-DD" (exactly 10 chars) -> UTC midnight. Used only for the START
 * of a start/end/period segment, where GIBS does write a bare date. */
static inline bool clouds_date_parse(const char *s, size_t len, uint32_t *out)
{
    unsigned Y, M, D;
    if (s == NULL || out == NULL || len != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    if (!clouds_parse_digits(s, 4, &Y) || !clouds_parse_digits(s + 5, 2, &M) ||
        !clouds_parse_digits(s + 8, 2, &D)) return false;
    if (M < 1 || M > 12 || D < 1 || D > 31 || Y < 1970 || Y > 2105) return false;
    *out = (uint32_t)(clouds_days_from_civil((int)Y, M, D) * 86400);
    return true;
}

static inline uint32_t clouds_floor_to(uint32_t t, uint32_t period_s)
{
    if (period_s == 0u) return t;
    return t - (t % period_s);
}

/* Newest stamp to assume when the GIBS DescribeDomains fetch fails. */
static inline uint32_t clouds_fallback_newest(uint32_t now)
{
    uint32_t t = clouds_floor_to(now, CLOUDS_GIBS_PERIOD_S);
    return (t > CLOUDS_FALLBACK_LAG_S) ? t - CLOUDS_FALLBACK_LAG_S : 0u;
}

/* Newest stamp to assume when the EUMETView capabilities fetch fails: now minus
 * 30 minutes, floored to the row's own period (spec 3.3). */
static inline uint32_t clouds_eumet_fallback_newest(uint32_t now, uint32_t period_s)
{
    uint32_t t = (now > CLOUDS_EUMET_LAG_S) ? now - CLOUDS_EUMET_LAG_S : 0u;
    return clouds_floor_to(t, period_s);
}

/* Frame @p i (0 = newest) counted back from @p newest on a @p period_s grid. */
static inline uint32_t clouds_time_step(uint32_t newest, int i, uint32_t period_s)
{
    uint32_t back = (uint32_t)(i > 0 ? i : 0) * period_s;
    return (newest > back) ? newest - back : 0u;
}

/* EUMETView GetMap for one frame. Lowercase parameter names as the service
 * documents them; EPSG:3857 is an easting/northing CRS so the WMS 1.3.0 axis
 * order is the same minx,miny,maxx,maxy as GIBS. @p stamp is formatted here,
 * never copied from a server string. Returns false and writes "" when it would
 * not fit.
 *
 * bgcolor=0x000000 is not optional: GeoServer paints anything it has no data
 * for WHITE by default, and this page is dark. Measured 2026-09-03 on the live
 * service: a box outside the MTG disc came back 100 % white, msg_iodc natural
 * colour at night 100 % white, and a dusk frame carried a white wedge. Black
 * matches GIBS, whose no-data is already black, and keeps the blank and holes
 * gates reading the same colour on both providers. */
static inline bool clouds_eumet_frame_url(char *out, size_t sz, const clouds_sat_t *sat,
                                          clouds_role_t role, float lat, float lon,
                                          uint8_t zoom, uint8_t basemap, uint32_t stamp)
{
    if (out == NULL || sz == 0 || sat == NULL) return false;
    out[0] = '\0';
    char tstr[CLOUDS_TIME_MAX];
    if (!clouds_time_format(tstr, sizeof(tstr), stamp)) return false;
    long minx, miny, maxx, maxy;
    clouds_bbox_rounded(lat, lon, zoom, &minx, &miny, &maxx, &maxy);
    int n = snprintf(out, sz,
                     CLOUDS_EUMET_BASE "ows?service=WMS&version=1.3.0&request=GetMap"
                     "&layers=%s%s&crs=EPSG:3857"
                     "&bbox=%ld,%ld,%ld,%ld&width=%d&height=%d&format=image/jpeg&time=%s"
                     "&bgcolor=0x000000",
                     clouds_sat_layer(sat, role), clouds_eumet_basemap_suffix(basemap),
                     minx, miny, maxx, maxy,
                     CLOUDS_PX, CLOUDS_PX, tstr);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* GetMap URL for one frame, on whichever provider owns the satellite nearest
 * @p lon. @p stamp is the frame time in UTC seconds; it is formatted here
 * (never copied from a server string), so no query-splitting character can
 * reach the URL. Layer order: raster first, vector overlay after. Returns false
 * and writes "" when the result would not fit. */
static inline bool clouds_frame_url(char *out, size_t sz, float lat, float lon,
                                    uint8_t zoom, uint8_t ch, uint8_t basemap,
                                    uint32_t stamp)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    const clouds_sat_t *sat = clouds_sat_for_lon(lon);
    clouds_role_t role = clouds_role(ch, lat, lon, stamp);
    if (sat->provider == CLOUDS_PROV_EUMET) {
        return clouds_eumet_frame_url(out, sz, sat, role, lat, lon, zoom, basemap, stamp);
    }
    char tstr[CLOUDS_TIME_MAX];
    if (!clouds_time_format(tstr, sizeof(tstr), stamp)) return false;
    long minx, miny, maxx, maxy;
    clouds_bbox_rounded(lat, lon, zoom, &minx, &miny, &maxx, &maxy);
    int n = snprintf(out, sz,
                     CLOUDS_GIBS_BASE "wms/epsg3857/best/wms.cgi?SERVICE=WMS&VERSION=1.3.0"
                     "&REQUEST=GetMap&LAYERS=%s%s&CRS=EPSG:3857"
                     "&BBOX=%ld,%ld,%ld,%ld&WIDTH=%d&HEIGHT=%d&FORMAT=image/jpeg&TIME=%s",
                     clouds_sat_layer(sat, role), clouds_basemap_suffix(basemap),
                     minx, miny, maxx, maxy,
                     CLOUDS_PX, CLOUDS_PX, tstr);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* WMTS DescribeDomains REST URL for @p layer over [now-3h, now+1h], both ends
 * to the second so CloudFront (max-age 1800 on this document) cannot hand back
 * a stale copy:
 *   https://gibs.earthdata.nasa.gov/wmts/epsg4326/best/1.0.0/{layer}/default/1km/all/{start}--{end}.xml */
static inline bool clouds_domains_url(char *out, size_t sz, const char *layer, uint32_t now)
{
    if (out == NULL || sz == 0 || layer == NULL) return false;
    out[0] = '\0';
    char a[CLOUDS_TIME_MAX], b[CLOUDS_TIME_MAX];
    uint32_t start = (now > CLOUDS_DOMAIN_BACK_S) ? now - CLOUDS_DOMAIN_BACK_S : 0u;
    if (!clouds_time_format(a, sizeof(a), start) ||
        !clouds_time_format(b, sizeof(b), now + CLOUDS_DOMAIN_FWD_S)) return false;
    int n = snprintf(out, sz, CLOUDS_GIBS_BASE "wmts/epsg4326/best/1.0.0/%s/default/1km/all/%s--%s.xml",
                     layer, a, b);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Trim ASCII whitespace off [a, b). */
static inline void clouds_trim(const char **a, const char **b)
{
    while (*a < *b && (**a == ' ' || **a == '\t' || **a == '\r' || **a == '\n')) (*a)++;
    while (*b > *a && ((*b)[-1] == ' ' || (*b)[-1] == '\t' || (*b)[-1] == '\r' || (*b)[-1] == '\n')) (*b)--;
}

#define CLOUDS_SEGMENTS_MAX 32   /* start/end pairs kept from one Domain list */

/* Parse a DescribeDomains body: the text of <Domain>...</Domain> is a
 * comma-separated ASCENDING list of segments, each "start/end/PT10M" or a
 * single "YYYY-MM-DDThh:mm:ssZ". Expands them NEWEST FIRST on the period grid
 * into @p out (UTC seconds), at most @p max_out (<= CLOUDS_TIMES_MAX). Rules:
 *   - a single entry without 'T' (bare date) is skipped;
 *   - a range whose END has no 'T' is skipped; a range whose START is a bare
 *     date starts at that midnight; any other unparsable start = open (0);
 *   - the period is CLOUDS_GIBS_PERIOD_S unless the segment says PTnM (n minutes).
 * Returns the count (0 when the element is missing or nothing parses). Every
 * scan is bounded by @p len: the body needs no NUL terminator. */
static inline int clouds_parse_domains(const char *xml, size_t len, uint32_t *out, int max_out)
{
    if (xml == NULL || out == NULL || max_out <= 0) return 0;
    if (max_out > CLOUDS_TIMES_MAX) max_out = CLOUDS_TIMES_MAX;

    const char *d = radar_wms_find(xml, len, "<Domain>");
    if (d == NULL) return 0;
    d += 8;
    const char *e = radar_wms_find(d, len - (size_t)(d - xml), "</Domain>");
    if (e == NULL) return 0;

    /* Pass 1: collect (start, end, period) per segment, in document order. */
    uint32_t seg_start[CLOUDS_SEGMENTS_MAX], seg_end[CLOUDS_SEGMENTS_MAX], seg_per[CLOUDS_SEGMENTS_MAX];
    int nseg = 0;
    const char *p = d;
    while (p < e) {
        if (nseg == CLOUDS_SEGMENTS_MAX) {        /* overflow: drop the OLDEST, keep collecting */
            memmove(seg_start, seg_start + 1, sizeof(seg_start) - sizeof(seg_start[0]));
            memmove(seg_end,   seg_end + 1,   sizeof(seg_end)   - sizeof(seg_end[0]));
            memmove(seg_per,   seg_per + 1,   sizeof(seg_per)   - sizeof(seg_per[0]));
            nseg--;
        }
        const char *comma = memchr(p, ',', (size_t)(e - p));
        const char *sa = p, *sb = comma ? comma : e;
        p = comma ? comma + 1 : e;
        clouds_trim(&sa, &sb);
        if (sa >= sb) continue;

        const char *sl1 = memchr(sa, '/', (size_t)(sb - sa));
        if (sl1 == NULL) {                        /* single instant */
            uint32_t t;
            if (clouds_time_parse(sa, (size_t)(sb - sa), &t)) {
                seg_start[nseg] = t; seg_end[nseg] = t; seg_per[nseg] = CLOUDS_GIBS_PERIOD_S; nseg++;
            }
            continue;
        }
        const char *sl2 = memchr(sl1 + 1, '/', (size_t)(sb - sl1 - 1));
        const char *end_b = sl2 ? sl2 : sb;
        uint32_t t_end, t_start = 0, per = CLOUDS_GIBS_PERIOD_S;
        if (!clouds_time_parse(sl1 + 1, (size_t)(end_b - sl1 - 1), &t_end)) continue;
        if (!clouds_time_parse(sa, (size_t)(sl1 - sa), &t_start) &&
            !clouds_date_parse(sa, (size_t)(sl1 - sa), &t_start)) t_start = 0;
        if (sl2 != NULL) {                        /* "PT10M" */
            const char *q = sl2 + 1;
            if ((size_t)(sb - q) >= 4 && q[0] == 'P' && q[1] == 'T') {
                unsigned n = 0; const char *r = q + 2;
                while (r < sb && *r >= '0' && *r <= '9') { n = n * 10u + (unsigned)(*r - '0'); r++; }
                if (r < sb && *r == 'M' && n > 0) per = n * 60u;
            }
        }
        if (t_start > t_end) t_start = t_end;
        seg_start[nseg] = t_start; seg_end[nseg] = t_end; seg_per[nseg] = per; nseg++;
    }

    /* Pass 2: newest segment first, each expanded end -> start. */
    int count = 0;
    for (int s = nseg - 1; s >= 0 && count < max_out; s--) {
        uint32_t t = seg_end[s];
        for (;;) {
            if (count > 0 && out[count - 1] == t) { /* duplicate boundary between segments */
            } else {
                out[count++] = t;
                if (count >= max_out) break;
            }
            if (t < seg_start[s] + seg_per[s]) break;
            t -= seg_per[s];
        }
    }
    return count;
}

/* ---- EUMETView time discovery ----
 *
 * EUMETView has no DescribeDomains. The cheap source of "what is the newest
 * slot" is the WORKSPACE-scoped GetCapabilities (about 35-55 KB, against
 * several megabytes for the unscoped one), whose per-layer
 * <Dimension name="time"> carries a default="..." attribute holding the newest
 * published stamp. History is then a plain step back on the LAYER's period: the
 * service advertises nearestValue="1", so a requested time between slots snaps
 * to a real one and a stepped-back time always returns a picture. */

/* Response cap for the capabilities fetch. Measured 2026-09-03: mtg_fd 34742 B,
 * msg_fes 50773 B, msg_iodc 54921 B. 98304 leaves the largest document 43 KB of
 * headroom for a layer the workspace gains later; the buffer is PSRAM, so the
 * slack costs nothing. (65536 would leave under 10 KB on msg_iodc, and a
 * truncated body parses to nothing and silently drops the page onto the
 * 30-minute fallback.) */
#define CLOUDS_EUMET_CAPS_MAX 98304u

#define CLOUDS_MSG_PERIOD_S   900u   /* every msg_* workspace publishes on 15 minutes */

/* Publishing grid of the layer @p role resolves to on @p s, which is a property
 * of the LAYER, not of the row.
 *
 * The MTG row is a 10-minute service, but MTG publishes no air-mass product, so
 * that channel borrows msg_fes:rgb_airmass from the 15-minute MSG 0-degree
 * service. Stepping its history back on the row's 600 s would land two of every
 * three frames on the same real slot, and the loop would show duplicates.
 * Keyed on the workspace prefix rather than a fourth table column: the msg_*
 * services publish on 15 minutes, while BOTH layers this table takes from
 * mtg_fd (rgb_geocolour and ir105_hrfi) are on 10. The mtg_fd workspace as a
 * whole is NOT uniform -- the real 2026-09-03 capture has siblings on other
 * grids, li_afa among them -- so this rule speaks for the layers in the table,
 * not for the workspace. A GIBS row has no workspace and keeps its own period. */
static inline uint32_t clouds_layer_period_s(const clouds_sat_t *s, clouds_role_t role)
{
    if (s == NULL) return CLOUDS_GIBS_PERIOD_S;
    const char *ws = clouds_sat_workspace(s, role);
    if (ws != NULL && strncmp(ws, "msg_", 4) == 0) return CLOUDS_MSG_PERIOD_S;
    return s->period_s;
}

/* Workspace-scoped GetCapabilities URL:
 *   https://view.eumetsat.int/geoserver/{workspace}/wms?service=WMS&version=1.3.0&request=GetCapabilities
 * @p workspace comes from the baked satellite table, never from user input. */
static inline bool clouds_eumet_caps_url(char *out, size_t sz, const char *workspace)
{
    if (out == NULL || sz == 0 || workspace == NULL) return false;
    out[0] = '\0';
    int n = snprintf(out, sz, CLOUDS_EUMET_BASE
                     "%s/wms?service=WMS&version=1.3.0&request=GetCapabilities", workspace);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Newest published stamp for @p layer_name from a WORKSPACE-SCOPED
 * GetCapabilities body. @p layer_name is the table name, prefix and all
 * ("mtg_fd:rgb_geocolour"); clouds_layer_bare_name() drops the prefix, because
 * the virtual service publishes <Name>rgb_geocolour</Name>. The search is then
 * bounded to that layer by radar_wms_layer_bounds() (an EXACT <Name> match,
 * which is the whole correctness of this function -- siblings in one workspace
 * advertise different grids: measured 2026-09-03, ir105_hrfi defaults to 19:40,
 * rgb_geocolour to 19:30 and li_afa to 19:45), and within it the first
 * <Dimension ...> carrying name="time" is read. That dimension may be
 * self-closing, and a non-time dimension may precede it.
 *
 * The attribute is a PLAIN stamp: 2026-09-03T19:30:00Z, verified 2026-09-03.
 * Only the extent text after the tag carries the .000Z fraction, and that text
 * is not what is read. The fraction splice below is tolerance for a server-side
 * change, not the expected form; either way nothing but digits, '-', ':', 'T'
 * and 'Z' from the server reaches clouds_time_parse().
 *
 * Every scan is bounded by @p len: the body needs no NUL terminator. Returns
 * false when the layer, the dimension or a parsable default is missing --
 * "current" and other symbolic defaults are rejected, not guessed at. */
static inline bool clouds_eumet_default_time(const char *xml, size_t len,
                                             const char *layer_name, uint32_t *out)
{
    if (out == NULL) return false;

    /* Stripped HERE rather than at the call site: this function only ever reads
     * a workspace-scoped document, so the conversion belongs where it cannot be
     * forgotten. Callers pass the table name as-is. */
    const char *p, *end;
    if (!radar_wms_layer_bounds(xml, len, clouds_layer_bare_name(layer_name), &p, &end)) {
        return false;
    }

    /* The time dimension's opening tag, self-closing or not. */
    const char *attrs = NULL, *attrs_end = NULL;
    while (p < end) {
        const char *d = radar_wms_find(p, (size_t)(end - p), "<Dimension");
        if (d == NULL) return false;
        const char *gt = memchr(d, '>', (size_t)(end - d));
        if (gt == NULL) return false;
        if (radar_wms_find(d, (size_t)(gt - d), "name=\"time\"") != NULL) {
            attrs = d;
            attrs_end = gt;
            break;
        }
        p = gt + 1;
    }
    if (attrs == NULL) return false;

    const char *a = radar_wms_find(attrs, (size_t)(attrs_end - attrs), "default=\"");
    if (a == NULL) return false;
    a += 9;                                     /* strlen("default=\"") */
    const char *q = memchr(a, '"', (size_t)(attrs_end - a));
    if (q == NULL) return false;

    /* Copy, dropping a fractional-second part if there is one. */
    char buf[CLOUDS_TIME_MAX];
    size_t n = (size_t)(q - a);
    const char *dot = memchr(a, '.', n);
    size_t zed = 0;                             /* the 'Z' a dropped fraction takes with it */
    if (dot != NULL) {
        n = (size_t)(dot - a);
        zed = 1;
    }
    if (n + zed + 1u > sizeof(buf)) return false;
    memcpy(buf, a, n);
    if (zed) buf[n++] = 'Z';
    buf[n] = '\0';
    return clouds_time_parse(buf, n, out);
}

/* ---- blank / partial frame detection ---- */

/* GIBS answers HTTP 200 for a slot whose tiles are missing: a BLANK frame is the
 * selected vector basemap overlay drawn over black (~98 % near black on the
 * sample grid, and black everywhere when the basemap is "none"). Sample every
 * CLOUDS_BLANK_STEP-th pixel of every CLOUDS_BLANK_STEP-th row (8100 samples of
 * a 720x720 frame) and call the frame incomplete when more than CLOUDS_BLANK_PCT of
 * them are near black (all channels below 24). Raw decoded pixels, before any
 * bake or Red Night remap.
 *
 * The test catches BLANK slots only: blackness ALONE cannot separate a partially
 * ingested frame from a real night frame (2026-08-24: a GeoColor night frame over
 * Missouri is 8 % near black on stb and the HW decoder crushes the dark night
 * side to exact zero, so an exact-0x0000 gate at 3 % rejected every real frame
 * and the page went black). Partial frames are caught by clouds_frame_holes()
 * instead, which needs a neighbouring frame to compare against.
 *
 * Measured on real 720x720 GetMaps (near-black share of the sample grid):
 *   GeoColor  day 1.5 %, night 8 %      Clean IR  0 %      Air Mass  20 %
 *   (author's earlier pure-black survey saw Air Mass up to 54 %)
 *   blank, any channel: 96 - 98 %
 * CLOUDS_BLANK_PCT 90 clears the worst real frame by 36 points and sits 6
 * under a blank; one bar for every GIBS satellite and channel.
 *
 * GIBS ONLY. The bar was calibrated on GIBS pictures and EUMETSAT's infrared
 * styles paint warm ground near black, so real EUMETView frames sit far over
 * it: measured 2026-09-03, msg_iodc infrared over Dubai is 98 % near black by
 * day and by night at zoom 9 and 89 % at zoom 7, and MTG infrared over the
 * Sahara is 82 % at zoom 7, while the GIBS infrared band measured 0 % in the
 * same probe. The gate also has nothing to catch there: EUMETView answers a
 * missing slot with an XML ServiceException, which the image probe already
 * rejects, never a 200 of black tiles. image_page_poll.c runs this on GIBS
 * rows only; the holes gate still runs on both. */
#define CLOUDS_BLANK_STEP 8      /* sample stride, pixels and rows */

/* Shared near-black predicate: every channel below 24 (R5 < 3, G6 < 6, B5 < 3).
 * NOT an exact-0x0000 test: the P4 hardware JPEG decoder crushes dark values to
 * 0 where stb keeps 1-3, so an exact-zero gate flips its verdict with the
 * decoder. Both gates below are built on this one predicate, so "lit" and
 * "black" mean the same thing on both sides of a comparison. */
static inline bool clouds_px_near_black(uint16_t v)
{
    return (v >> 11) < 3 && ((v >> 5) & 0x3Fu) < 6 && (v & 0x1Fu) < 3;
}

/* Missing-tile (partial ingest) detection, per CELL against the neighbouring
 * frame of the loop (10 min apart). A tile GIBS has not ingested is rendered
 * black, but the vector basemap is composited over the WHOLE canvas regardless,
 * so a hole keeps its border/road/graticule lines drawn over black -- which is
 * exactly what the reported partial frames look like.
 *
 * Split the frame into a CLOUDS_HOLE_CELLS x CLOUDS_HOLE_CELLS grid (6x6 = 120 px
 * cells on a 720 px frame; a GIBS hole is a large rectangle covering at least one
 * whole cell -- the pitch of the origin's internal tiles cannot be derived from
 * the GetMap bbox, since the server reprojects them into one flat render). Sample
 * on the CLOUDS_BLANK_STEP grid, and for each cell count the samples LIT in @p ref
 * that went NEAR BLACK in @p f. Flag the frame when ONE cell crosses the bar.
 *
 * Calibration (device pixels, ninadash4 2026-08-25, a complete GeoColor night
 * frame at zoom 6 over Missouri, basemap 2, on this same sample grid):
 *   - 57.6 % of samples near black, 42.4 % lit. The old global gate used an
 *     exact-zero numerator and measured only 15.0 % -- a ~4x undercount -- and
 *     then divided a localised hole by the WHOLE frame's lit count, so a
 *     35-45 % hole scored in the single digits. It never fired once on device.
 *   - Terminator, the one thing that can look like a hole: a cell that flips
 *     day -> night in 10 min keeps that same 42 % lit, so at most ~58 % of its
 *     ref-lit samples go dark.
 *   - A real hole keeps only the 1-2 px vector line cores lit (a few percent of
 *     the cell), so ~80-90 % of its ref-lit samples go dark.
 * CLOUDS_HOLE_CELL_PCT 75 sits between the two, with ~17 points of margin over
 * the terminator worst case. (The task brief suggested starting at 85; 85 sits
 * ON the hole floor and would keep missing the exact frames this fixes, so the
 * bar is set on the terminator side instead. A false positive costs one
 * re-fetch; a false negative is the bug.)
 *
 * A cell is judged only when at least 1/CLOUDS_HOLE_MIN_LIT_Q of its samples are
 * lit in @p ref: an ocean-at-night or blank-reference cell says nothing, and a
 * reference that is itself holed in the same place (consecutive partial slots)
 * falls under the floor rather than voting "clean". Both buffers w x h, same
 * stride. Pure: no allocation, sampling grid only, never reads every pixel. */
/* Grid side: 6x6 cells, 120 px each on a 720 px frame and 133 px on an 800 px
 * one. The two lit-sample thresholds below were tuned against the 120 px cell;
 * on an 800 px frame the same rule is slightly less sensitive because each cell
 * carries more samples. Accepted as-is for phase 1 rather than re-derived, and
 * listed as open verification item 6 in the design. */
#define CLOUDS_HOLE_CELLS      6
#define CLOUDS_HOLE_CELL_PCT   75   /* >= this % of a cell's ref-lit samples gone black = hole */
#define CLOUDS_HOLE_MIN_LIT_Q  4    /* ref cell needs >= samples/4 lit to be judged at all */
#define CLOUDS_HOLE_MIN_CELL_N 8    /* and at least this many samples (tiny frames say nothing) */
static inline bool clouds_frame_holes(const uint16_t *f, const uint16_t *ref,
                                      int w, int h, int stride_px)
{
    if (f == NULL || ref == NULL || w <= 0 || h <= 0) return false;
    if (stride_px < w) stride_px = w;
    enum { NCELL = CLOUDS_HOLE_CELLS * CLOUDS_HOLE_CELLS };
    uint32_t n[NCELL] = {0}, lit[NCELL] = {0}, hole[NCELL] = {0};
    for (int y = 0; y < h; y += CLOUDS_BLANK_STEP) {
        const uint16_t *fr = f   + (size_t)y * (size_t)stride_px;
        const uint16_t *rr = ref + (size_t)y * (size_t)stride_px;
        int row = (y * CLOUDS_HOLE_CELLS / h) * CLOUDS_HOLE_CELLS;
        for (int x = 0; x < w; x += CLOUDS_BLANK_STEP) {
            int c = row + x * CLOUDS_HOLE_CELLS / w;
            n[c]++;
            if (clouds_px_near_black(rr[x])) continue;
            lit[c]++;
            if (clouds_px_near_black(fr[x])) hole[c]++;
        }
    }
    for (int c = 0; c < NCELL; c++) {
        if (n[c] < CLOUDS_HOLE_MIN_CELL_N) continue;
        if (lit[c] * (uint32_t)CLOUDS_HOLE_MIN_LIT_Q < n[c]) continue;
        if (hole[c] * 100u >= lit[c] * (uint32_t)CLOUDS_HOLE_CELL_PCT) return true;
    }
    return false;
}

static inline bool clouds_frame_incomplete(const uint16_t *rgb565, int w, int h,
                                           int stride_px)
{
    if (rgb565 == NULL || w <= 0 || h <= 0) return false;
    if (stride_px < w) stride_px = w;
    uint32_t n = 0, black = 0;
    for (int y = 0; y < h; y += CLOUDS_BLANK_STEP) {
        const uint16_t *row = rgb565 + (size_t)y * (size_t)stride_px;
        for (int x = 0; x < w; x += CLOUDS_BLANK_STEP) {
            n++;
            if (clouds_px_near_black(row[x])) black++;   /* see clouds_px_near_black */
        }
    }
    return black * 100u > n * (uint32_t)CLOUDS_BLANK_PCT;
}
