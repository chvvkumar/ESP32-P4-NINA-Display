#pragma once

/*
 * clouds_wms.h - pure URL, geometry and time decisions for the Clouds page
 * (NASA GIBS WMS, a GOES ABI channel under a selectable vector basemap
 * overlay: borders and roads, coastlines only, borders plus a graticule, or none).
 *
 * Header-only and free of ESP-IDF, FreeRTOS and LVGL, so the host test suite
 * (test/host/test_clouds_wms.c) can exercise the parts that are easy to get
 * wrong: the East/West pick, the Web-Mercator box, the GetMap URL shape, the
 * DescribeDomains parser and the UTC calendar arithmetic. Same precedent as
 * radar_wms.h, whose Mercator helpers this file reuses. Fetching and the frame
 * ring live in image_page_poll.c and ui/nina_image_page.c.
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
 *    next poll. ponytail: a partial frame that leaves less than the channel's
 *    blank_pct of the samples black still slips through and is only healed by the
 *    same-stamp replace on the next poll. Upgrade path: also compare the
 *    tile grid against the previous frame for the same stamp.
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

#include "radar_wms.h"   /* radar_wms_merc_x/y, radar_wms_find */

#define CLOUDS_URL_MAX        RADAR_WMS_URL_MAX   /* 512: the GetMap URL is ~290 chars, 302 worst case
                                                    (longest channel + longest basemap suffix) */
#define CLOUDS_TIME_MAX       21                  /* "2026-08-18T04:00:00Z" + NUL */
#define CLOUDS_TIMES_MAX      10                  /* == max clouds_frames (ring capacity) */
#define CLOUDS_PERIOD_S       600u                /* GOES ABI cadence, PT10M on every channel */
#define CLOUDS_FALLBACK_LAG_S 3000u               /* 50 min behind wall clock when the domain fetch fails */
#define CLOUDS_PX             720                 /* frame size, native panel size */
#define CLOUDS_ZOOM_MIN       5
#define CLOUDS_ZOOM_MAX       9
#define CLOUDS_DOMAIN_BACK_S  (3u * 3600u)        /* DescribeDomains window: now-3h .. now+1h */
#define CLOUDS_DOMAIN_FWD_S   3600u

#define CLOUDS_GIBS_BASE   "https://gibs.earthdata.nasa.gov/"
#define CLOUDS_SPLIT_LON   (-106.2f)              /* midpoint of the two sub-satellite longitudes */

/* Channel table (config field clouds_channel, 0..2). All six layer names and
 * their PT10M DescribeDomains lists verified live 2026-08-18; a wrong name
 * returns a 460 B XML ServiceException, so a typo cannot silently serve the
 * wrong picture. blank_pct is the near-black sample fraction above which
 * clouds_frame_incomplete() calls the frame a blank slot (see the measurements
 * at that function). */
typedef struct {
    const char *east;       /* GIBS layer name, GOES-East */
    const char *west;       /* GIBS layer name, GOES-West */
    const char *label;      /* overlay strip caption */
    uint8_t     blank_pct;  /* > this % of samples near black = blank (not ingested) frame */
} clouds_channel_t;

#define CLOUDS_CHANNEL_COUNT 3

static const clouds_channel_t s_clouds_channels[CLOUDS_CHANNEL_COUNT] = {
    { "GOES-East_ABI_GeoColor",              "GOES-West_ABI_GeoColor",              "GeoColor", 90 },
    { "GOES-East_ABI_Band13_Clean_Infrared", "GOES-West_ABI_Band13_Clean_Infrared", "Clean IR", 90 },
    { "GOES-East_ABI_Air_Mass",              "GOES-West_ABI_Air_Mass",              "Air Mass", 90 },
};

/* Row for @p ch; an out-of-range value falls back to GeoColor rather than
 * failing, so a config from a newer build never blanks the page. */
static inline const clouds_channel_t *clouds_channel(uint8_t ch)
{
    return &s_clouds_channels[(ch < CLOUDS_CHANNEL_COUNT) ? ch : 0];
}

/* Satellite by longitude only: west of the -75.2/-137.2 midpoint gets GOES-West. */
static inline const char *clouds_layer(uint8_t ch, float lon)
{
    const clouds_channel_t *c = clouds_channel(ch);
    return (lon < CLOUDS_SPLIT_LON) ? c->west : c->east;
}

static inline const char *clouds_channel_label(uint8_t ch)
{
    return clouds_channel(ch)->label;
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

/* Half-width of the 720 px box in Web-Mercator metres at Web-Mercator zoom
 * @p zoom (clamped 5..9): 720 px * (world circumference / (256 * 2^zoom)) / 2. */
static inline float clouds_half_m(uint8_t zoom)
{
    if (zoom < CLOUDS_ZOOM_MIN) zoom = CLOUDS_ZOOM_MIN;
    if (zoom > CLOUDS_ZOOM_MAX) zoom = CLOUDS_ZOOM_MAX;
    return 360.0f * 40075016.686f / (256.0f * (float)(1u << zoom));
}

/* Square EPSG:3857 box centred on lat/lon. Latitude is clamped to +-85 inside
 * radar_wms_merc_y(). (0,0) is a valid centre (Gulf of Guinea), no special case. */
static inline void clouds_bbox(float lat, float lon, uint8_t zoom,
                               float *minx, float *miny, float *maxx, float *maxy)
{
    float cx = radar_wms_merc_x(lon);
    float cy = radar_wms_merc_y(lat);
    float half = clouds_half_m(zoom);
    if (minx) *minx = cx - half;
    if (miny) *miny = cy - half;
    if (maxx) *maxx = cx + half;
    if (maxy) *maxy = cy + half;
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

static inline uint32_t clouds_floor_period(uint32_t t)
{
    return t - (t % CLOUDS_PERIOD_S);
}

/* Newest stamp to assume when DescribeDomains is unavailable. */
static inline uint32_t clouds_fallback_newest(uint32_t now)
{
    uint32_t t = clouds_floor_period(now);
    return (t > CLOUDS_FALLBACK_LAG_S) ? t - CLOUDS_FALLBACK_LAG_S : 0u;
}

/* Frame @p i (0 = newest) counted back from @p newest on the period grid. */
static inline uint32_t clouds_time_step(uint32_t newest, int i)
{
    uint32_t back = (uint32_t)(i > 0 ? i : 0) * CLOUDS_PERIOD_S;
    return (newest > back) ? newest - back : 0u;
}

/* GetMap URL for one frame. @p stamp is the frame time in UTC seconds; it is
 * formatted here (never copied from a server string), so no query-splitting
 * character can reach the URL. Layer order: raster first, vector overlay after.
 * Returns false and writes "" when the result would not fit. */
static inline bool clouds_frame_url(char *out, size_t sz, float lat, float lon,
                                    uint8_t zoom, uint8_t ch, uint8_t basemap,
                                    uint32_t stamp)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    char tstr[CLOUDS_TIME_MAX];
    if (!clouds_time_format(tstr, sizeof(tstr), stamp)) return false;
    float minx, miny, maxx, maxy;
    clouds_bbox(lat, lon, zoom, &minx, &miny, &maxx, &maxy);
    int n = snprintf(out, sz,
                     CLOUDS_GIBS_BASE "wms/epsg3857/best/wms.cgi?SERVICE=WMS&VERSION=1.3.0"
                     "&REQUEST=GetMap&LAYERS=%s%s&CRS=EPSG:3857"
                     "&BBOX=%ld,%ld,%ld,%ld&WIDTH=%d&HEIGHT=%d&FORMAT=image/jpeg&TIME=%s",
                     clouds_layer(ch, lon), clouds_basemap_suffix(basemap),
                     (long)lroundf(minx), (long)lroundf(miny),
                     (long)lroundf(maxx), (long)lroundf(maxy),
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
 *   - the period is CLOUDS_PERIOD_S unless the segment says PTnM (n minutes).
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
                seg_start[nseg] = t; seg_end[nseg] = t; seg_per[nseg] = CLOUDS_PERIOD_S; nseg++;
            }
            continue;
        }
        const char *sl2 = memchr(sl1 + 1, '/', (size_t)(sb - sl1 - 1));
        const char *end_b = sl2 ? sl2 : sb;
        uint32_t t_end, t_start = 0, per = CLOUDS_PERIOD_S;
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

/* ---- blank / partial frame detection ---- */

/* GIBS answers HTTP 200 for a slot whose tiles are missing: a BLANK frame is the
 * selected vector basemap overlay drawn over black (~98 % near black on the
 * sample grid, and black everywhere when the basemap is "none"). Sample every
 * CLOUDS_BLANK_STEP-th pixel of every CLOUDS_BLANK_STEP-th row (8100 samples of
 * a 720x720 frame) and call the frame incomplete when more than blank_pct of
 * them are near black (all channels below 24). Raw decoded pixels, before any
 * bake or Red Night remap.
 *
 * The test catches BLANK slots only. It does not try to catch partially
 * ingested frames (one black quadrant): blackness cannot separate those from
 * a real night frame (2026-08-24: a GeoColor night frame over Missouri is 8 %
 * near black on stb and the HW decoder crushes the dark night side to exact
 * zero, so an exact-0x0000 gate at 3 % rejected every real frame and the page
 * went black). Partials self-heal instead: the poller re-fetches the newest
 * two slots every poll and a same-stamp slot is replaced when its hash changes.
 *
 * Measured on real 720x720 GetMaps (near-black share of the sample grid):
 *   GeoColor  day 1.5 %, night 8 %      Clean IR  0 %      Air Mass  20 %
 *   (author's earlier pure-black survey saw Air Mass up to 54 %)
 *   blank, any channel: 96 - 98 %
 * 90 clears the worst real frame by 36 points and sits 6 under a blank. The
 * per-channel table column stays (it is the layer table anyway) so a channel
 * can still be tuned individually. */
#define CLOUDS_BLANK_STEP 8      /* sample stride, pixels and rows */

/* Missing-tile (partial ingest) detection. A tile GIBS has not ingested yet is
 * rendered EXACTLY black (0x0000 from either decoder), while the same area in
 * the neighbouring frame of the loop (10 min apart) is lit. Night sky is dark
 * in both, so it never trips this. Count, on the CLOUDS_BLANK_STEP grid, the
 * samples that are lit in @p ref (any channel >= 24) and exactly zero in @p f;
 * call @p f partial when they exceed CLOUDS_HOLE_PCT of the lit samples.
 *
 * Threshold math (zoom 6, 720 px): the day/night terminator moves ~90 px in
 * the 10 min between frames, so at dusk up to ~12 % of samples go lit -> dark
 * (and the HW decoder crushes dark to exact zero). ONE missing 256 px tile is
 * also ~12 %. Those two cannot be told apart, so the bar sits at 30: catches
 * the multi-tile holes seen after a channel change (45-55 % of the frame),
 * never a terminator; a single-tile hole is left to the re-fetch to heal.
 * Needs at least 1/20 of the grid lit in @p ref to say anything (a blank
 * reference is no reference). Both buffers w x h, same stride. */
#define CLOUDS_HOLE_PCT 30
static inline bool clouds_frame_holes(const uint16_t *f, const uint16_t *ref,
                                      int w, int h, int stride_px)
{
    if (f == NULL || ref == NULL || w <= 0 || h <= 0) return false;
    if (stride_px < w) stride_px = w;
    uint32_t n = 0, lit = 0, hole = 0;
    for (int y = 0; y < h; y += CLOUDS_BLANK_STEP) {
        const uint16_t *fr = f   + (size_t)y * (size_t)stride_px;
        const uint16_t *rr = ref + (size_t)y * (size_t)stride_px;
        for (int x = 0; x < w; x += CLOUDS_BLANK_STEP) {
            n++;
            uint16_t v = rr[x];
            bool is_lit = (v >> 11) >= 3 || ((v >> 5) & 0x3F) >= 6 || (v & 0x1F) >= 3;
            if (!is_lit) continue;
            lit++;
            if (fr[x] == 0x0000u) hole++;
        }
    }
    if (lit * 20u < n) return false;
    return hole * 100u > lit * (uint32_t)CLOUDS_HOLE_PCT;
}

static inline bool clouds_frame_incomplete(const uint16_t *rgb565, int w, int h,
                                           int stride_px, uint8_t ch)
{
    if (rgb565 == NULL || w <= 0 || h <= 0) return false;
    if (stride_px < w) stride_px = w;
    uint32_t n = 0, black = 0;
    for (int y = 0; y < h; y += CLOUDS_BLANK_STEP) {
        const uint16_t *row = rgb565 + (size_t)y * (size_t)stride_px;
        for (int x = 0; x < w; x += CLOUDS_BLANK_STEP) {
            n++;
            /* NEAR black (every channel < 24, i.e. R5 < 3, G6 < 6, B5 < 3),
             * not exactly 0x0000: the P4 hardware JPEG path crushes dark
             * values to 0 where stb keeps 1-3, so an exact-zero test read a
             * real night frame as 7-8 % black on HW vs <1 % on stb and the
             * thresholds flipped with the decoder. Both decoders agree on
             * "below 24". */
            uint16_t v = row[x];
            if ((v >> 11) < 3 && ((v >> 5) & 0x3F) < 6 && (v & 0x1F) < 3) black++;
        }
    }
    return black * 100u > n * (uint32_t)clouds_channel(ch)->blank_pct;
}
