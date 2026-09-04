/* Host test for main/clouds_wms.h -- the pure URL/geometry/time decisions behind
 * the Clouds page (NASA GIBS WMS, a GOES ABI channel): satellite pick, square
 * Web-Mercator box, GetMap and DescribeDomains URL shape, the DescribeDomains
 * time-list parser and the UTC calendar arithmetic. Header-only, no ESP-IDF
 * dependency; assert-style like test/host/test_radar_wms.c. */
#include "clouds_wms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

static void check_int(const char *label, long got, long expect) {
    printf("%-64s got=%-12ld expect=%-12ld %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-64s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_str(const char *label, const char *got, const char *expect) {
    int ok = (got != NULL) && (strcmp(got, expect) == 0);
    printf("%-64s %s\n    got=%s\n", label, ok ? "OK" : "FAIL", got ? got : "(null)");
    if (!ok) fails++;
}

static void check_contains(const char *label, const char *hay, const char *needle, bool expect) {
    bool got = (hay != NULL) && (strstr(hay, needle) != NULL);
    printf("%-64s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

/* |got - expect| <= tol, printed as long so no %f is needed */
static void check_near(const char *label, float got, float expect, float tol) {
    float d = got - expect;
    if (d < 0.0f) d = -d;
    bool ok = d <= tol;
    printf("%-64s got=%-10ld expect=%-10ld %s\n", label,
           (long)lroundf(got), (long)lroundf(expect), ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

/* ---- synthetic frames for clouds_frame_holes() ----
 * LIT_PX is imagery, DARK_PX is night sky just under the near-black gate, and
 * LINE_PX (~40 grey) is a basemap vector line: GIBS draws those over the whole
 * canvas, holes included, so they survive inside a missing tile. */
#define LIT_PX  0x5aacu
#define DARK_PX 0x0841u
#define LINE_PX 0x2945u

/* Night GeoColor stand-in: ~45 % of samples lit (city glow, cloud, basemap)
 * over near-black sky, matching the 42.4 % lit measured on the device's own
 * complete night frame. Patterned on the 8 px JPEG block so the stride-8
 * sample grid sees the same mix a whole cell does. */
static void fill_night(uint16_t *p, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[y * w + x] = ((((x >> 3) * 37 + (y >> 3) * 17) & 0xFF) < 115) ? LIT_PX : DARK_PX;
}

/* Darken @p k of every 5 blocks inside cell (@p cx, @p cy): k/5 of that cell's
 * samples, independent of the fill_night pattern. */
static void darken_cell(uint16_t *p, int w, int cx, int cy, int cell, int k) {
    for (int y = cy * cell; y < (cy + 1) * cell; y++)
        for (int x = cx * cell; x < (cx + 1) * cell; x++)
            if (((x >> 3) + (y >> 3)) % 5 < k) p[y * w + x] = DARK_PX;
}

/* A missing tile: the whole cell rendered flat black. */
static void black_cell(uint16_t *p, int w, int cx, int cy, int cell) {
    for (int y = cy * cell; y < (cy + 1) * cell; y++)
        for (int x = cx * cell; x < (cx + 1) * cell; x++) p[y * w + x] = 0x0000u;
}

/* Parse "&BBOX=a,b,c,d" out of a URL. */
static bool url_bbox(const char *url, long *a, long *b, long *c, long *d) {
    const char *p = strstr(url, "&BBOX=");
    if (!p) return false;
    return sscanf(p + 6, "%ld,%ld,%ld,%ld", a, b, c, d) == 4;
}

/* Parse one integer query parameter (e.g. "&WIDTH=") out of a URL; -1 if absent. */
static long url_int(const char *url, const char *key) {
    const char *p = strstr(url, key);
    long v = -1;
    if (p == NULL) return -1;
    if (sscanf(p + strlen(key), "%ld", &v) != 1) return -1;
    return v;
}

/* One arbitrary but real 10-minute GIBS slot, shared by the URL cases so the
 * TIME= term is stable. */
static const uint32_t stamp_ref = 1787025600u;

/* The exact strings this header produced BEFORE the date-line split existed
 * (captured from the ec7d2f7 build). A single-request location must keep
 * producing them byte for byte. */
#define URL_MISSOURI_Z6 \
    "https://gibs.earthdata.nasa.gov/wms/epsg3857/best/wms.cgi?SERVICE=WMS&VERSION=1.3.0" \
    "&REQUEST=GetMap&LAYERS=GOES-East_ABI_GeoColor,Reference_Features_15m&CRS=EPSG:3857" \
    "&BBOX=-11144212,3783982,-9383102,5545091&WIDTH=720&HEIGHT=720&FORMAT=image/jpeg" \
    "&TIME=2026-08-18T04:00:00Z"
#define URL_WELLINGTON_Z7 \
    "https://gibs.earthdata.nasa.gov/wms/epsg3857/best/wms.cgi?SERVICE=WMS&VERSION=1.3.0" \
    "&REQUEST=GetMap&LAYERS=Himawari_AHI_Band3_Red_Visible_1km,Reference_Features_15m" \
    "&CRS=EPSG:3857&BBOX=19016142,-5495489,19896698,-4614934&WIDTH=720&HEIGHT=720" \
    "&FORMAT=image/jpeg&TIME=2026-08-18T04:00:00Z"
#define URL_LONDON_Z7 \
    "https://view.eumetsat.int/geoserver/ows?service=WMS&version=1.3.0&request=GetMap" \
    "&layers=mtg_fd:rgb_geocolour,backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land" \
    "&crs=EPSG:3857&bbox=-454749,6269943,425806,7150498&width=720&height=720" \
    "&format=image/jpeg&time=2026-08-18T04:00:00Z&bgcolor=0x000000"
#define URL_DUBAI_Z7 \
    "https://view.eumetsat.int/geoserver/ows?service=WMS&version=1.3.0&request=GetMap" \
    "&layers=msg_iodc:rgb_natural,backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land" \
    "&crs=EPSG:3857&bbox=5715690,2460054,6596245,3340608&width=720&height=720" \
    "&format=image/jpeg&time=2026-08-18T04:00:00Z&bgcolor=0x000000"

/* Which panel column the true location falls in, once the parts are pasted
 * side by side. This is what clouds_centre_dx_px() used to measure an offset
 * from; it must now always be the panel centre. -1 when the location is in
 * neither part, which would be a bug in clouds_split(). */
static float split_loc_col(const clouds_split_t *sp, float lon) {
    float cx = radar_wms_merc_x(lon);
    for (int i = 0; i < sp->parts; i++) {
        float mpp = (sp->maxx[i] - sp->minx[i]) / (float)sp->px_w[i];
        if (cx >= sp->minx[i] - 1.0f && cx <= sp->maxx[i] + 1.0f) {
            return (float)sp->px_x[i] + (cx - sp->minx[i]) / mpp;
        }
    }
    return -1.0f;
}

/* Everything that must hold for EVERY split, whatever the panel or zoom. */
static void check_split_invariants(const char *what, const clouds_split_t *sp, int px) {
    char lbl[128];
    snprintf(lbl, sizeof(lbl), "%s: two parts", what);
    check_int(lbl, sp->parts, 2);
    snprintf(lbl, sizeof(lbl), "%s: part widths sum to the panel", what);
    check_int(lbl, sp->px_w[0] + sp->px_w[1], px);
    snprintf(lbl, sizeof(lbl), "%s: part 0 pastes at column 0", what);
    check_int(lbl, sp->px_x[0], 0);
    snprintf(lbl, sizeof(lbl), "%s: part 1 pastes right after part 0", what);
    check_int(lbl, sp->px_x[1], sp->px_w[0]);
    snprintf(lbl, sizeof(lbl), "%s: neither part is empty", what);
    check_bool(lbl, sp->px_w[0] >= 1 && sp->px_w[1] >= 1, true);
    snprintf(lbl, sizeof(lbl), "%s: part 0 ends on the antimeridian", what);
    check_int(lbl, lroundf(sp->maxx[0]), 20037508L);
    snprintf(lbl, sizeof(lbl), "%s: part 1 starts on the antimeridian", what);
    check_int(lbl, lroundf(sp->minx[1]), -20037508L);
    snprintf(lbl, sizeof(lbl), "%s: both parts share the y box", what);
    check_bool(lbl, sp->maxy > sp->miny, true);
}

/* The GetMap this part actually sends: right WIDTH/HEIGHT, square-or-wider
 * pixels after the widening, and the widening moved only the free x side. */
static void check_split_part_url(const char *what, const clouds_split_t *sp, int i,
                                 float lat, float lon, uint8_t zoom) {
    char u[CLOUDS_URL_MAX], lbl[160];
    snprintf(lbl, sizeof(lbl), "%s part %d: URL builds", what, i);
    bool built = clouds_frame_url_part(u, sizeof(u), lat, lon, zoom, 0, 0, stamp_ref, sp, i);
    check_bool(lbl, built, true);
    if (!built) return;
    long x0, y0, x1, y1;
    snprintf(lbl, sizeof(lbl), "%s part %d: URL bbox parses", what, i);
    bool parsed = url_bbox(u, &x0, &y0, &x1, &y1);
    check_bool(lbl, parsed, true);
    if (!parsed) return;
    long w = url_int(u, "&WIDTH=");
    snprintf(lbl, sizeof(lbl), "%s part %d: WIDTH is the part width", what, i);
    check_int(lbl, w, sp->px_w[i]);
    snprintf(lbl, sizeof(lbl), "%s part %d: HEIGHT is the panel", what, i);
    check_int(lbl, url_int(u, "&HEIGHT="), CLOUDS_PX);
    snprintf(lbl, sizeof(lbl), "%s part %d: square-or-wider pixels", what, i);
    check_bool(lbl, (long long)(x1 - x0) * (long long)CLOUDS_PX >=
                    (long long)(y1 - y0) * (long long)w, true);
    snprintf(lbl, sizeof(lbl), "%s part %d: pinned side stays on the antimeridian", what, i);
    check_int(lbl, (i == 0) ? x1 : x0, (i == 0) ? 20037508L : -20037508L);
}

/* Position of needle in hay, or -1. Used to assert query parameter order. */
static long pos_of(const char *hay, const char *needle) {
    const char *p = strstr(hay, needle);
    return p ? (long)(p - hay) : -1;
}

/* Three ascending segments (the GOES-West shape observed 2026-08-18: two gaps),
 * plus a bare-date single entry that must be skipped, plus whitespace. */
static const char sample_domains[] =
    "<Domains xmlns:ows='http://www.opengis.net/ows/1.1'><SpaceDomain>"
    "<BoundingBox miny='-90' maxx='180' crs='urn:ogc:def:crs:OGC:2:84' maxy='90' minx='-180'/>"
    "</SpaceDomain><DimensionDomain><ows:Identifier>time</ows:Identifier>"
    "<Domain>2026-08-17, 2026-08-18T01:40:00Z/2026-08-18T01:50:00Z/PT10M,\n"
    " 2026-08-18T02:10:00Z/2026-08-18T02:20:00Z/PT10M ,"
    "2026-08-18T02:40:00Z/2026-08-18T03:00:00Z/PT10M</Domain>"
    "<Size>4</Size></DimensionDomain></Domains>";

/* Read a fixture from test/host/fixtures into a malloc'd buffer. NULL on any
 * failure; *len gets the byte count. Caller frees. */
static char *read_fixture(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", NINA_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *len = got;
    return buf;
}

int main(void) {
    char url[CLOUDS_URL_MAX];
    char ts[CLOUDS_TIME_MAX];
    uint32_t times[CLOUDS_TIMES_MAX];

    /* -- satellite pick ---------------------------------------------------- */
    const char *geo_e = "GOES-East_ABI_GeoColor";
    const char *geo_w = "GOES-West_ABI_GeoColor";
    /* A daytime stamp for the Americas: 2026-08-18T18:00:00Z. */
    const uint32_t noon_us = 1787025600u + 14u * 3600u;
    /* A daytime stamp for the eastern hemisphere: 2026-08-18T02:00:00Z. */
    const uint32_t noon_ap = 1787025600u - 2u * 3600u;

    check_str("sat: New York (-74.0) -> GOES", clouds_sat_for_lon(-74.0f)->name, "GOES");
    check_near("sat: New York sub_lon", clouds_sat_for_lon(-74.0f)->sub_lon, -75.2f, 0.01f);
    check_near("sat: Seattle (-122.3) sub_lon", clouds_sat_for_lon(-122.33f)->sub_lon, -137.2f, 0.01f);
    check_near("sat: -106.2 exactly stays East", clouds_sat_for_lon(-106.2f)->sub_lon, -75.2f, 0.01f);
    check_near("sat: -106.3 is West", clouds_sat_for_lon(-106.3f)->sub_lon, -137.2f, 0.01f);
    check_str("sat: Wellington (174.8) -> Himawari", clouds_sat_for_lon(174.78f)->name, "Himawari");
    check_str("sat: Sydney (151.2) -> Himawari", clouds_sat_for_lon(151.21f)->name, "Himawari");
    check_str("sat: Tokyo (139.7) -> Himawari", clouds_sat_for_lon(139.69f)->name, "Himawari");
    check_str("sat: Mumbai (72.9) -> Meteosat", clouds_sat_for_lon(72.88f)->name, "Meteosat");
    check_near("sat: Mumbai is IODC", clouds_sat_for_lon(72.88f)->sub_lon, 45.5f, 0.01f);
    check_near("sat: Dubai (55.3) is IODC", clouds_sat_for_lon(55.27f)->sub_lon, 45.5f, 0.01f);
    check_str("sat: London (-0.13) -> Meteosat", clouds_sat_for_lon(-0.13f)->name, "Meteosat");
    check_near("sat: London is MTG (0.0)", clouds_sat_for_lon(-0.13f)->sub_lon, 0.0f, 0.01f);
    check_near("sat: Cape Town (18.4) is MTG", clouds_sat_for_lon(18.42f)->sub_lon, 0.0f, 0.01f);
    check_near("sat: Reykjavik (-21.9) is MTG", clouds_sat_for_lon(-21.94f)->sub_lon, 0.0f, 0.01f);
    /* wrap-around: Honolulu is nearer GOES-West than Himawari the long way */
    check_near("sat: Honolulu (-157.9) is GOES-West", clouds_sat_for_lon(-157.86f)->sub_lon, -137.2f, 0.01f);
    check_near("sat: Anchorage (-149.9) is GOES-West", clouds_sat_for_lon(-149.9f)->sub_lon, -137.2f, 0.01f);
    /* wrap-around the other way: 179.9E is nearer Himawari than GOES-West */
    check_near("sat: 179.9E is Himawari", clouds_sat_for_lon(179.9f)->sub_lon, 140.7f, 0.01f);
    check_near("sat: -179.9E is Himawari", clouds_sat_for_lon(-179.9f)->sub_lon, 140.7f, 0.01f);

    /* -- layer pick per channel and per frame time ------------------------- */
    check_str("layer: St Louis GeoColor -> East", clouds_layer(0, 38.6f, -90.689438f, noon_us), geo_e);
    check_str("layer: Seattle GeoColor -> West", clouds_layer(0, 47.6f, -122.33f, noon_us), geo_w);
    check_str("layer: channel 1 -> East Clean IR", clouds_layer(1, 38.6f, -90.0f, noon_us),
              "GOES-East_ABI_Band13_Clean_Infrared");
    check_str("layer: channel 2 -> East Air Mass", clouds_layer(2, 38.6f, -90.0f, noon_us),
              "GOES-East_ABI_Air_Mass");
    check_str("layer: channel 9 (invalid) falls back to GeoColor",
              clouds_layer(9, 38.6f, -90.0f, noon_us), geo_e);
    /* GOES has a true day-night composite: the same layer at night */
    check_str("layer: GOES GeoColor at night is still GeoColor",
              clouds_layer(0, 38.6f, -90.0f, noon_us + 12u * 3600u), geo_e);
    /* Himawari has no GeoColor: visible by day, Clean IR by night */
    check_str("layer: Wellington GeoColor by day = visible",
              clouds_layer(0, -41.29f, 174.78f, 1788397200u),
              "Himawari_AHI_Band3_Red_Visible_1km");
    check_str("layer: Wellington GeoColor by night = Clean IR",
              clouds_layer(0, -41.29f, 174.78f, 1788440400u),
              "Himawari_AHI_Band13_Clean_Infrared");
    check_str("layer: MTG GeoColour by day",
              clouds_layer(0, 51.5f, -0.13f, 1788307200u + 12u * 3600u), "mtg_fd:rgb_geocolour");
    /* MTG publishes a true day-night composite, so the SAME layer after dark.
     * 1788307200 is a midnight UTC, which is the middle of the night over
     * London; the assertion under it proves the sun really is down. */
    check_str("layer: MTG GeoColour by night",
              clouds_layer(0, 51.5f, -0.13f, 1788307200u), "mtg_fd:rgb_geocolour");
    check_bool("layer: MTG night stamp really is night",
               sun_elevation_deg(51.5f, -0.13f, 1788307200u) < CLOUDS_SUN_MIN_EL_DEG, true);
    check_str("layer: MTG channel 1", clouds_layer(1, 51.5f, -0.13f, noon_ap), "mtg_fd:ir105_hrfi");
    check_str("layer: MTG channel 2 comes from MSG",
              clouds_layer(2, 51.5f, -0.13f, noon_ap), "msg_fes:rgb_airmass");
    check_str("layer: IODC channel 1", clouds_layer(1, 25.2f, 55.27f, noon_ap), "msg_iodc:ir108");
    check_str("layer: IODC channel 2", clouds_layer(2, 25.2f, 55.27f, noon_ap), "msg_iodc:rgb_airmass");

    /* -- captions ---------------------------------------------------------- */
    {
        char cap[48];
        check_bool("caption builds", clouds_caption(cap, sizeof(cap), 0, 38.6f, -90.0f, noon_us), true);
        check_str("caption: GOES GeoColor", cap, "GOES GeoColor");
        clouds_caption(cap, sizeof(cap), 1, 38.6f, -90.0f, noon_us);
        check_str("caption: GOES Clean IR", cap, "GOES Clean IR");
        clouds_caption(cap, sizeof(cap), 2, 38.6f, -90.0f, noon_us);
        check_str("caption: GOES Air Mass", cap, "GOES Air Mass");
        clouds_caption(cap, sizeof(cap), 0, -41.29f, 174.78f, 1788397200u);
        check_str("caption: Himawari Visible by day", cap, "Himawari Visible");
        clouds_caption(cap, sizeof(cap), 0, -41.29f, 174.78f, 1788440400u);
        check_str("caption: Himawari Clean IR by night", cap, "Himawari Clean IR");
        clouds_caption(cap, sizeof(cap), 0, 51.5f, -0.13f, noon_ap);
        check_str("caption: Meteosat GeoColour", cap, "Meteosat GeoColour");
        clouds_caption(cap, sizeof(cap), 2, 51.5f, -0.13f, noon_ap);
        check_str("caption: Meteosat Air Mass", cap, "Meteosat Air Mass");
        /* The Indian Ocean daytime product is a natural-colour RGB, not a
         * single monochrome band, so its word is "Natural", not "Visible".
         * Mumbai at 07:00Z on 18 Aug is early afternoon local solar time. */
        {
            const uint32_t mumbai_day   = 1787025600u + 3u * 3600u;
            const uint32_t mumbai_night = 1787025600u + 15u * 3600u;
            check_bool("caption: Mumbai day stamp really is day",
                       sun_elevation_deg(19.08f, 72.88f, mumbai_day) >= CLOUDS_SUN_MIN_EL_DEG, true);
            clouds_caption(cap, sizeof(cap), 0, 19.08f, 72.88f, mumbai_day);
            check_str("caption: Meteosat Natural by day (IODC)", cap, "Meteosat Natural");
            check_str("layer: IODC GeoColor by day = natural colour",
                      clouds_layer(0, 19.08f, 72.88f, mumbai_day), "msg_iodc:rgb_natural");
            check_bool("caption: Mumbai night stamp really is night",
                       sun_elevation_deg(19.08f, 72.88f, mumbai_night) < CLOUDS_SUN_MIN_EL_DEG, true);
            clouds_caption(cap, sizeof(cap), 0, 19.08f, 72.88f, mumbai_night);
            check_str("caption: Meteosat Clean IR by night (IODC)", cap, "Meteosat Clean IR");
        }
        char tiny[6];
        check_bool("caption: tiny buffer fails", clouds_caption(tiny, sizeof(tiny), 0, 38.6f, -90.0f, noon_us), false);
        check_str("caption: tiny buffer empty", tiny, "");
    }

    /* -- bbox -------------------------------------------------------------- */
    {
        const float lat = 38.758331f, lon = -90.689438f;
        float minx, miny, maxx, maxy;
        clouds_bbox(lat, lon, 7, &minx, &miny, &maxx, &maxy);
        float cx = radar_wms_merc_x(lon), cy = radar_wms_merc_y(lat);
        float half7 = 360.0f * 40075016.686f / (256.0f * 128.0f);
        check_near("bbox z7: half-width", clouds_half_m(7), half7, 1.0f);
        check_near("bbox z7: half-width ~440277 m", clouds_half_m(7), 440277.28f, 2.0f);
        check_near("bbox z7: symmetric in x", (maxx - cx) - (cx - minx), 0.0f, 2.0f);
        check_near("bbox z7: symmetric in y", (maxy - cy) - (cy - miny), 0.0f, 2.0f);
        check_near("bbox z7: square", (maxx - minx) - (maxy - miny), 0.0f, 2.0f);
        check_near("bbox z7: width = 2*half", maxx - minx, 2.0f * half7, 4.0f);
        check_near("bbox z7: centre x", (minx + maxx) * 0.5f, cx, 2.0f);
        check_near("bbox z7: centre y", (miny + maxy) * 0.5f, cy, 2.0f);
        /* one zoom step halves the box; the clamp keeps 4 and 10 on the rails */
        check_near("half z8 = half z7 / 2", clouds_half_m(8), half7 * 0.5f, 1.0f);
        check_near("half z5 = half z7 * 4", clouds_half_m(5), half7 * 4.0f, 1.0f);
        check_near("half z4 clamps to z5", clouds_half_m(4), clouds_half_m(5), 0.5f);
        check_near("half z10 clamps to z9", clouds_half_m(10), clouds_half_m(9), 0.5f);
        /* lat beyond +-85 is clamped inside merc_y, so no inf/nan */
        clouds_bbox(89.0f, 0.0f, 7, &minx, &miny, &maxx, &maxy);
        check_bool("bbox: lat 89 clamped (finite)", isfinite(miny) && isfinite(maxy), true);
    }

    /* -- date-line clamp (spec 3.5): the box SHIFTS, it never shrinks ------- */
    {
        const float world = 20037508.0f;
        float minx, miny, maxx, maxy;
        clouds_bbox(0.0f, 179.9f, 5, &minx, &miny, &maxx, &maxy);
        check_bool("clamp 179.9E: maxx at the world edge", maxx <= world + 1.0f, true);
        check_bool("clamp 179.9E: minx inside the world", minx >= -world - 1.0f, true);
        check_near("clamp 179.9E: full width kept", maxx - minx, 2.0f * clouds_half_m(5), 4.0f);
        clouds_bbox(0.0f, -179.9f, 5, &minx, &miny, &maxx, &maxy);
        check_bool("clamp -179.9E: minx at the world edge", minx >= -world - 1.0f, true);
        check_bool("clamp -179.9E: maxx inside the world", maxx <= world + 1.0f, true);
        check_near("clamp -179.9E: full width kept", maxx - minx, 2.0f * clouds_half_m(5), 4.0f);
        /* an interior box is untouched */
        clouds_bbox(38.76f, -90.69f, 7, &minx, &miny, &maxx, &maxy);
        check_near("clamp: interior box centre unmoved", (minx + maxx) * 0.5f,
                   radar_wms_merc_x(-90.69f), 2.0f);
    }

    /* -- the window is never shifted; a crossing is served by two GetMaps ----
     * REPLACES the phase-1 "where the location sits once the clamp has moved
     * the box" cases, all of which asserted clouds_centre_dx_px(): that helper
     * told the marker how far the clamped box had carried the location off
     * centre (about +241 px at Wellington zoom 5). The box is no longer moved
     * at all -- a crossing is split at the antimeridian into two requests
     * pasted side by side -- so the location is the centre pixel at every
     * longitude and the helper is gone. The property those cases were really
     * about is asserted directly here, as split_loc_col() == CLOUDS_PX/2. */
    {
        const float wlat = -41.29f, wlon = 174.78f;
        clouds_split_t sp;

        /* Wellington crosses +180 at zoom 5 and 6 (the two zooms that showed
         * the black strip on glass). Pixel widths come from the investigation's
         * arithmetic: overrun 241.2 px at z5, 122.4 px at z6, on a 720 panel. */
        clouds_split(wlat, wlon, 5, &sp);
        check_split_invariants("split Wellington z5", &sp, 720);
        check_int("split Wellington z5: part 0 width", sp.px_w[0], 479);
        check_int("split Wellington z5: part 1 width", sp.px_w[1], 241);
        check_near("split Wellington z5: location at the panel centre",
                   split_loc_col(&sp, wlon), 360.0f, 1.0f);
        check_split_part_url("split Wellington z5", &sp, 0, wlat, wlon, 5);
        check_split_part_url("split Wellington z5", &sp, 1, wlat, wlon, 5);

        clouds_split(wlat, wlon, 6, &sp);
        check_split_invariants("split Wellington z6", &sp, 720);
        check_int("split Wellington z6: part 0 width", sp.px_w[0], 598);
        check_int("split Wellington z6: part 1 width", sp.px_w[1], 122);
        check_near("split Wellington z6: location at the panel centre",
                   split_loc_col(&sp, wlon), 360.0f, 1.0f);
        check_split_part_url("split Wellington z6", &sp, 0, wlat, wlon, 6);
        check_split_part_url("split Wellington z6", &sp, 1, wlat, wlon, 6);

        /* zoom 7 is a narrow enough box that it never reaches the edge: one
         * request, and the URL is byte for byte the pre-change string. */
        clouds_split(wlat, wlon, 7, &sp);
        check_int("split: Wellington z7 does not cross", sp.parts, 1);
        check_int("split: Wellington z7 part 0 is the whole panel", sp.px_w[0], 720);
        check_bool("split: Wellington z7 URL builds",
                   clouds_frame_url(url, sizeof(url), wlat, wlon, 7, 0, 0, stamp_ref), true);
        check_str("split: Wellington z7 URL unchanged", url, URL_WELLINGTON_Z7);

        /* The 800 px panel: the same crossing, wider parts. screen_geom_set()
         * is what board_profile_init() calls on the round 3.4in board. */
        screen_geom_set(800, 0, 0);
        clouds_split(wlat, wlon, 5, &sp);
        check_split_invariants("split Wellington z5 @800", &sp, 800);
        check_int("split Wellington z5 @800: part 0 width", sp.px_w[0], 519);
        check_int("split Wellington z5 @800: part 1 width", sp.px_w[1], 281);
        check_near("split Wellington z5 @800: location at the panel centre",
                   split_loc_col(&sp, wlon), 400.0f, 1.0f);
        check_split_part_url("split Wellington z5 @800", &sp, 0, wlat, wlon, 5);
        check_split_part_url("split Wellington z5 @800", &sp, 1, wlat, wlon, 5);
        clouds_split(wlat, wlon, 6, &sp);
        check_split_invariants("split Wellington z6 @800", &sp, 800);
        check_near("split Wellington z6 @800: location at the panel centre",
                   split_loc_col(&sp, wlon), 400.0f, 1.0f);
        check_split_part_url("split Wellington z6 @800", &sp, 0, wlat, wlon, 6);
        check_split_part_url("split Wellington z6 @800", &sp, 1, wlat, wlon, 6);
        screen_geom_set(720, 0, 0);

        /* Just WEST of 180 (western Aleutians): the crossing runs the other
         * way, so the wrapped EAST end of the world is what lands on the LEFT
         * of the panel. The Himawari/GOES-West boundary sits at -178.25, so
         * -178.0 must still resolve to GOES-West; a case at -179 would
         * silently be testing Himawari instead. */
        const float alat = 51.9f, alon = -178.0f;
        check_str("split: -178.0 is still GOES-West",
                  clouds_sat_layer(clouds_sat_for_lon(alon), CLOUDS_ROLE_IR),
                  "GOES-West_ABI_Band13_Clean_Infrared");
        clouds_split(alat, alon, 5, &sp);
        check_split_invariants("split Aleutians z5", &sp, 720);
        check_int("split Aleutians z5: part 0 width (wrapped east end)", sp.px_w[0], 314);
        check_near("split Aleutians z5: location at the panel centre",
                   split_loc_col(&sp, alon), 360.0f, 1.0f);
        check_split_part_url("split Aleutians z5", &sp, 0, alat, alon, 5);
        check_split_part_url("split Aleutians z5", &sp, 1, alat, alon, 5);
        /* zoom 7 is still wide enough to cross here: its half-width is 3.96
         * degrees of longitude against the 2.0 degrees left to the edge.
         * Zoom 9 (0.99 degrees) is the case where a narrower box stops
         * crossing and the page goes back to one request. */
        clouds_split(alat, alon, 7, &sp);
        check_int("split: Aleutians z7 still crosses", sp.parts, 2);
        check_near("split Aleutians z7: location at the panel centre",
                   split_loc_col(&sp, alon), 360.0f, 1.0f);
        clouds_split(alat, alon, 9, &sp);
        check_int("split: Aleutians z9 does not cross", sp.parts, 1);
        check_int("split: Aleutians z9 part 0 is the whole panel", sp.px_w[0], 720);

        /* EUMETView rows can never reach a world edge (design section E), and
         * their URL shape is untouched by this change. */
        clouds_split(51.5f, -0.13f, 7, &sp);
        check_int("split: London z7 does not cross", sp.parts, 1);
        check_bool("split: London z7 URL builds",
                   clouds_frame_url(url, sizeof(url), 51.5f, -0.13f, 7, 0, 0, stamp_ref), true);
        check_str("split: London z7 URL unchanged", url, URL_LONDON_Z7);
        clouds_split(25.2f, 55.3f, 7, &sp);
        check_int("split: Dubai z7 does not cross", sp.parts, 1);
        check_bool("split: Dubai z7 URL builds",
                   clouds_frame_url(url, sizeof(url), 25.2f, 55.3f, 7, 0, 0, stamp_ref), true);
        check_str("split: Dubai z7 URL unchanged", url, URL_DUBAI_Z7);

        /* The single-request guarantee: an ordinary GIBS location's URL is
         * byte for byte what it was before the split existed. */
        clouds_split(38.6f, -92.2f, 6, &sp);
        check_int("split: Missouri z6 does not cross", sp.parts, 1);
        check_bool("split: Missouri z6 URL builds",
                   clouds_frame_url(url, sizeof(url), 38.6f, -92.2f, 6, 0, 0, stamp_ref), true);
        check_str("split: Missouri z6 URL byte for byte unchanged", url, URL_MISSOURI_Z6);
        /* and clouds_frame_url() really is the part-0 builder underneath */
        char part0[CLOUDS_URL_MAX];
        check_bool("split: Missouri z6 part-0 builder agrees",
                   clouds_frame_url_part(part0, sizeof(part0), 38.6f, -92.2f, 6, 0, 0,
                                         stamp_ref, &sp, 0), true);
        check_str("split: Missouri z6 part 0 == clouds_frame_url", part0, URL_MISSOURI_Z6);
    }

    /* -- square-or-wider pixels: GIBS renders a coarse frame whenever a
     * request's vertical metres-per-pixel is coarser than the horizontal by
     * more than about 0.001 m/px, and whole-metre BBOX rounding alone can
     * land there. clouds_bbox_rounded() widens the box so it never does, the
     * same rule and shared helper (radar_wms_square_pixels(), main/radar_wms.h)
     * already fixed for the radar basemap in rainviewer.h. Measured against
     * Wellington NZ (-41.29, 174.78), the device's own Cloud Cover request:
     * at zoom 6 the plain-rounded box has resx 2445.983333, resy 2445.984722
     * (+0.001389 m/px, GIBS served a blocky 26 KB frame); the widened box
     * flips that to -0.001389 and GIBS served the crisp 73 KB frame at the
     * identical position. Zoom 5 and 7 already landed on the crisp side
     * before this fix, so their unwidened and widened boxes agree. */
    {
        const float wlat = -41.29f, wlon = 174.78f;
        for (uint8_t z = 5; z <= 7; z++) {
            long minx, miny, maxx, maxy;
            clouds_bbox_rounded(wlat, wlon, z, &minx, &miny, &maxx, &maxy);
            char lbl[80];
            snprintf(lbl, sizeof(lbl), "square pixels: Wellington z%u widened box x-span >= y-span", z);
            check_bool(lbl, (maxx - minx) >= (maxy - miny), true);

            /* the plain-rounded box clouds_frame_url() used to send before this
             * fix: z6 alone must FAIL the aspect test, proving the bug existed
             * (the RED case); z5/z7 already passed it unwidened. */
            float fminx, fminy, fmaxx, fmaxy;
            clouds_bbox(wlat, wlon, z, &fminx, &fminy, &fmaxx, &fmaxy);
            long ominx = (long)lroundf(fminx), ominy = (long)lroundf(fminy);
            long omaxx = (long)lroundf(fmaxx), omaxy = (long)lroundf(fmaxy);
            snprintf(lbl, sizeof(lbl), "square pixels: Wellington z%u unwidened box was %s",
                     z, (z == 6) ? "coarse (regression guard)" : "already crisp");
            check_bool(lbl, (omaxx - ominx) >= (omaxy - ominy), z != 6);

            /* the fix never moves an edge by more than the 8 m step cap */
            snprintf(lbl, sizeof(lbl), "square pixels: Wellington z%u minx moved <= 8m", z);
            check_bool(lbl, labs(minx - ominx) <= 8, true);
            snprintf(lbl, sizeof(lbl), "square pixels: Wellington z%u miny moved <= 8m", z);
            check_bool(lbl, labs(miny - ominy) <= 8, true);
            snprintf(lbl, sizeof(lbl), "square pixels: Wellington z%u maxx moved <= 8m", z);
            check_bool(lbl, labs(maxx - omaxx) <= 8, true);
            snprintf(lbl, sizeof(lbl), "square pixels: Wellington z%u maxy moved <= 8m", z);
            check_bool(lbl, labs(maxy - omaxy) <= 8, true);
        }
        /* the GIBS GetMap URL itself (uppercase BBOX/WIDTH/HEIGHT) carries the
         * widened box, not the raw one */
        check_bool("square pixels: z6 GIBS URL builds",
                   clouds_frame_url(url, sizeof(url), wlat, wlon, 6, 0, 0, 1787025600u), true);
        long ux0, uy0, ux1, uy1;
        check_bool("square pixels: z6 GIBS URL bbox parses", url_bbox(url, &ux0, &uy0, &ux1, &uy1), true);
        /* WIDTH is read from the URL rather than assumed to be the panel:
         * Wellington z6 crosses the date line, so clouds_frame_url() now
         * builds part 0 of a split and its request is 598 px wide. The rule
         * itself is per pixel, so it is x-span/WIDTH >= y-span/HEIGHT. */
        {
            long uw = url_int(url, "&WIDTH="), uh = url_int(url, "&HEIGHT=");
            check_bool("square pixels: z6 GIBS URL pixels are square or wider",
                       (long long)(ux1 - ux0) * uh >= (long long)(uy1 - uy0) * uw, true);
        }
    }

    /* -- EUMETView basemap suffixes ---------------------------------------- */
    check_str("eumet basemap 0", clouds_eumet_basemap_suffix(0),
              ",backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land");
    check_str("eumet basemap 1", clouds_eumet_basemap_suffix(1), ",backgrounds:ne_10m_coastline");
    check_str("eumet basemap 2", clouds_eumet_basemap_suffix(2),
              ",backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land,backgrounds:graticules-dark");
    check_str("eumet basemap 3", clouds_eumet_basemap_suffix(3), "");
    check_str("eumet basemap 9 falls back to 0", clouds_eumet_basemap_suffix(9),
              ",backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land");

    /* -- EUMETView GetMap URL ---------------------------------------------- */
    {
        uint32_t stamp = 1787025600u;   /* 2026-08-18T04:00:00Z */
        check_bool("eumet url: London GeoColour builds",
                   clouds_frame_url(url, sizeof(url), 51.5f, -0.13f, 7, 0, 0, stamp), true);
        printf("    url=%s\n", url);
        check_contains("eumet url: geoserver ows endpoint", url,
                       "https://view.eumetsat.int/geoserver/ows?service=WMS&version=1.3.0&request=GetMap", true);
        check_contains("eumet url: MTG geocolour layer", url, "&layers=mtg_fd:rgb_geocolour,", true);
        check_contains("eumet url: basemap 0 suffix", url,
                       ",backgrounds:ne_10m_coastline,backgrounds:ne_boundary_lines_land&", true);
        check_contains("eumet url: crs", url, "&crs=EPSG:3857&", true);
        check_contains("eumet url: 720x720", url, "&width=720&height=720&", true);
        check_contains("eumet url: jpeg", url, "&format=image/jpeg&", true);
        check_contains("eumet url: time", url, "&time=2026-08-18T04:00:00Z", true);
        /* GeoServer paints no-data WHITE by default, which is a bright wedge on
         * a dark page; black matches GIBS. */
        check_contains("eumet url: black background", url, "&bgcolor=0x000000", true);
        check_bool("eumet url: layer before overlays",
                   pos_of(url, "&layers=") < pos_of(url, "backgrounds:"), true);
        check_bool("eumet url: fits CLOUDS_URL_MAX", strlen(url) < CLOUDS_URL_MAX, true);
        /* basemap 3 = satellite alone */
        check_bool("eumet url: basemap 3 builds",
                   clouds_frame_url(url, sizeof(url), 51.5f, -0.13f, 7, 0, 3, stamp), true);
        check_contains("eumet url: basemap 3 is bare", url, "&layers=mtg_fd:rgb_geocolour&", true);
        /* worst case: longest layer + longest suffix */
        check_bool("eumet url: worst case builds",
                   clouds_frame_url(url, sizeof(url), 25.2f, 55.27f, 5, 2, 2, stamp), true);
        check_bool("eumet url: worst case < CLOUDS_URL_MAX", strlen(url) < CLOUDS_URL_MAX, true);
        char tiny[64];
        check_bool("eumet url: tiny buffer fails",
                   clouds_frame_url(tiny, sizeof(tiny), 51.5f, -0.13f, 7, 0, 0, stamp), false);
        check_str("eumet url: tiny buffer empty", tiny, "");
    }

    /* -- period-parameterised time helpers --------------------------------- */
    {
        uint32_t t0;
        clouds_time_parse("2026-09-01T00:00:00Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 1, 900u));
        check_str("step: 15 min grid back 1 = 08-31 23:45", ts, "2026-08-31T23:45:00Z");
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 4, 900u));
        check_str("step: 15 min grid back 4 = 08-31 23:00", ts, "2026-08-31T23:00:00Z");
        check_int("floor_to 600 exact", (long)clouds_floor_to(1787025600u, 600u), 1787025600L);
        check_int("floor_to 900 of +899", (long)clouds_floor_to(1787025600u + 899u, 900u), 1787025600L);
        clouds_time_parse("2026-08-18T04:17:33Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_eumet_fallback_newest(t0, 600u));
        check_str("eumet fallback: 04:17:33 -> 03:40 (10 min grid)", ts, "2026-08-18T03:40:00Z");
        clouds_time_format(ts, sizeof(ts), clouds_eumet_fallback_newest(t0, 900u));
        check_str("eumet fallback: 04:17:33 -> 03:45 (15 min grid)", ts, "2026-08-18T03:45:00Z");
    }

    /* -- GetMap URL -------------------------------------------------------- */
    {
        uint32_t stamp = 1787025600u;   /* 2026-08-18T04:00:00Z */
        check_bool("url: builds", clouds_frame_url(url, sizeof(url), 38.758331f, -90.689438f, 7, 0, 0, stamp), true);
        printf("    url=%s\n", url);
        check_contains("url: https gibs host + epsg3857 endpoint", url,
                       "https://gibs.earthdata.nasa.gov/wms/epsg3857/best/wms.cgi?", true);
        check_contains("url: SERVICE/VERSION/REQUEST", url, "SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap", true);
        check_contains("url: layers East + features", url,
                       "&LAYERS=GOES-East_ABI_GeoColor,Reference_Features_15m&", true);
        check_contains("url: never Reference_Labels", url, "Reference_Labels", false);
        check_contains("url: CRS", url, "&CRS=EPSG:3857&", true);
        check_contains("url: 720x720", url, "&WIDTH=720&HEIGHT=720&", true);
        check_contains("url: jpeg", url, "&FORMAT=image/jpeg&", true);
        check_contains("url: TIME :00Z", url, "&TIME=2026-08-18T04:00:00Z", true);
        /* GIBS already draws no-data black, and TIME stays last here */
        check_contains("url: no bgcolor on GIBS", url, "bgcolor", false);
        long a, b, c, d;
        check_bool("url: bbox parses", url_bbox(url, &a, &b, &c, &d), true);
        /* reference values from double maths; float-only merc is within a metre or two */
        check_near("url: bbox minx", (float)a, -10535779.0f, 3.0f);
        check_near("url: bbox miny", (float)b, 4246836.0f, 3.0f);
        check_near("url: bbox maxx", (float)c, -9655225.0f, 3.0f);
        check_near("url: bbox maxy", (float)d, 5127391.0f, 3.0f);
        /* parameter order as the spec writes it */
        long p_l = pos_of(url, "&LAYERS="), p_c = pos_of(url, "&CRS="), p_b = pos_of(url, "&BBOX="),
             p_w = pos_of(url, "&WIDTH="), p_h = pos_of(url, "&HEIGHT="), p_f = pos_of(url, "&FORMAT="),
             p_t = pos_of(url, "&TIME=");
        check_bool("url: param order LAYERS<CRS<BBOX<WIDTH<HEIGHT<FORMAT<TIME",
                   p_l >= 0 && p_l < p_c && p_c < p_b && p_b < p_w && p_w < p_h && p_h < p_f && p_f < p_t, true);
        check_bool("url: TIME is last", url[strlen(url) - 1] == 'Z' && strstr(url, "&TIME=") + 6 + 20 == url + strlen(url), true);
        /* West pick flows into the URL */
        check_bool("url: Seattle builds", clouds_frame_url(url, sizeof(url), 47.6f, -122.33f, 6, 0, 0, stamp), true);
        check_contains("url: Seattle uses West", url, "LAYERS=GOES-West_ABI_GeoColor,", true);
        /* basemap suffix selects the vector overlay layers */
        check_bool("url: basemap 1 builds", clouds_frame_url(url, sizeof(url), 38.758331f, -90.689438f, 7, 0, 1, stamp), true);
        check_contains("url: basemap 1 = coastlines only", url,
                       "&LAYERS=GOES-East_ABI_GeoColor,Coastlines_15m&", true);
        check_bool("url: basemap 2 builds", clouds_frame_url(url, sizeof(url), 38.758331f, -90.689438f, 7, 0, 2, stamp), true);
        check_contains("url: basemap 2 = features + graticule", url,
                       "&LAYERS=GOES-East_ABI_GeoColor,Reference_Features_15m,Graticule_15m&", true);
        check_bool("url: basemap 3 builds", clouds_frame_url(url, sizeof(url), 38.758331f, -90.689438f, 7, 0, 3, stamp), true);
        check_contains("url: basemap 3 = satellite alone", url,
                       "&LAYERS=GOES-East_ABI_GeoColor&", true);
        check_bool("url: basemap 9 builds", clouds_frame_url(url, sizeof(url), 38.758331f, -90.689438f, 7, 0, 9, stamp), true);
        check_contains("url: basemap 9 (invalid) falls back to 0", url,
                       "&LAYERS=GOES-East_ABI_GeoColor,Reference_Features_15m&", true);
        /* worst case (longest channel + longest suffix) still fits the buffer */
        check_bool("url: longest channel + basemap 2 builds",
                   clouds_frame_url(url, sizeof(url), 38.758331f, -122.33f, 5, 1, 2, stamp), true);
        check_bool("url: worst case < CLOUDS_URL_MAX", strlen(url) < CLOUDS_URL_MAX, true);

        /* too small a buffer -> false and "" */
        char tiny[64];
        check_bool("url: tiny buffer fails", clouds_frame_url(tiny, sizeof(tiny), 1.0f, 2.0f, 7, 0, 0, stamp), false);
        check_str("url: tiny buffer empty", tiny, "");
    }

    /* -- calendar / time --------------------------------------------------- */
    {
        check_int("days_from_civil 1970-01-01", (long)clouds_days_from_civil(1970, 1, 1), 0);
        check_int("days_from_civil 2000-03-01", (long)clouds_days_from_civil(2000, 3, 1), 11017);
        check_int("days_from_civil 2026-08-18", (long)clouds_days_from_civil(2026, 8, 18), 20683);
        int y; unsigned m, d;
        clouds_civil_from_days(20683, &y, &m, &d);
        check_int("civil_from_days 20683 y", y, 2026);
        check_int("civil_from_days 20683 m", m, 8);
        check_int("civil_from_days 20683 d", d, 18);
        clouds_civil_from_days(clouds_days_from_civil(2024, 2, 29), &y, &m, &d);
        check_bool("leap day round trip", y == 2024 && m == 2 && d == 29, true);

        check_bool("format 04:00", clouds_time_format(ts, sizeof(ts), 1787025600u), true);
        check_str("format 2026-08-18T04:00:00Z", ts, "2026-08-18T04:00:00Z");
        uint32_t t;
        check_bool("parse 2026-08-18T04:00:00Z", clouds_time_parse("2026-08-18T04:00:00Z", 20, &t), true);
        check_int("parse -> epoch", (long)t, 1787025600L);
        check_bool("parse hh:mmZ form", clouds_time_parse("2026-08-18T04:00Z", 17, &t), true);
        check_int("parse hh:mmZ -> epoch", (long)t, 1787025600L);
        check_bool("parse bare date rejected", clouds_time_parse("2026-08-18", 10, &t), false);
        check_bool("parse no Z rejected", clouds_time_parse("2026-08-18T04:00:00", 19, &t), false);
        check_bool("parse trailing junk rejected", clouds_time_parse("2026-08-18T04:00:00Zx", 21, &t), false);
        check_bool("parse garbage rejected", clouds_time_parse("garbage-in-here-nowZ", 20, &t), false);
        check_bool("date parse 2026-08-18", clouds_date_parse("2026-08-18", 10, &t), true);
        check_int("date parse -> midnight", (long)t, 20683L * 86400L);

        /* stepping across midnight and a month boundary */
        uint32_t t0;
        clouds_time_parse("2026-09-01T00:00:00Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 1, 600u));
        check_str("step: 09-01 00:00 back 10 min = 08-31 23:50", ts, "2026-08-31T23:50:00Z");
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 7, 600u));
        check_str("step: back 70 min = 08-31 22:50", ts, "2026-08-31T22:50:00Z");
        clouds_time_parse("2027-01-01T00:10:00Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 2, 600u));
        check_str("step: across the year = 2026-12-31 23:50", ts, "2026-12-31T23:50:00Z");
        check_int("step: i=0 is identity", (long)clouds_time_step(t0, 0, 600u), (long)t0);
        check_int("step: negative i is identity", (long)clouds_time_step(t0, -3, 600u), (long)t0);

        /* fallback newest: floor to 10 min, minus 50 min */
        clouds_time_parse("2026-08-18T04:17:33Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_fallback_newest(t0));
        check_str("fallback: 04:17:33 -> 03:20:00", ts, "2026-08-18T03:20:00Z");
        clouds_time_parse("2026-08-18T00:05:00Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_fallback_newest(t0));
        check_str("fallback: 00:05 -> 23:10 previous day", ts, "2026-08-17T23:10:00Z");
        check_int("floor_to 600 exact (10 min grid)", (long)clouds_floor_to(1787025600u, 600u), 1787025600L);
        check_int("floor_to 600 of +599", (long)clouds_floor_to(1787025600u + 599u, 600u), 1787025600L);
    }

    /* -- DescribeDomains URL ---------------------------------------------- */
    {
        uint32_t now = 1787025600u + 150u;   /* 04:02:30Z */
        check_bool("domains url builds", clouds_domains_url(url, sizeof(url), geo_e, now), true);
        printf("    url=%s\n", url);
        check_str("domains url exact", url,
                  "https://gibs.earthdata.nasa.gov/wmts/epsg4326/best/1.0.0/GOES-East_ABI_GeoColor/default/1km/all/"
                  "2026-08-18T01:02:30Z--2026-08-18T05:02:30Z.xml");
        check_bool("domains url fits 160", strlen(url) < 160, true);
        char small[100];
        check_bool("domains url tiny fails", clouds_domains_url(small, sizeof(small), geo_w, now), false);
    }

    /* -- DescribeDomains parser -------------------------------------------- */
    {
        int n = clouds_parse_domains(sample_domains, strlen(sample_domains), times, CLOUDS_TIMES_MAX);
        check_int("domains: count (3+2+2, bare date skipped)", n, 7);
        const char *expect[] = {
            "2026-08-18T03:00:00Z", "2026-08-18T02:50:00Z", "2026-08-18T02:40:00Z",
            "2026-08-18T02:20:00Z", "2026-08-18T02:10:00Z",
            "2026-08-18T01:50:00Z", "2026-08-18T01:40:00Z",
        };
        for (int i = 0; i < n && i < 7; i++) {
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "domains: times[%d]", i);
            clouds_time_format(ts, sizeof(ts), times[i]);
            check_str(lbl, ts, expect[i]);
        }
        /* cap */
        n = clouds_parse_domains(sample_domains, strlen(sample_domains), times, 3);
        check_int("domains: capped at 3", n, 3);
        clouds_time_format(ts, sizeof(ts), times[2]);
        check_str("domains: capped [2] = 02:40", ts, "2026-08-18T02:40:00Z");

        /* the real East shape: one segment with a bare-date start */
        const char east[] = "<Domain>2026-08-18/2026-08-18T03:50:00Z/PT10M</Domain>";
        n = clouds_parse_domains(east, strlen(east), times, 4);
        check_int("domains: bare-date start, 4 of many", n, 4);
        clouds_time_format(ts, sizeof(ts), times[0]);
        check_str("domains: newest 03:50", ts, "2026-08-18T03:50:00Z");
        clouds_time_format(ts, sizeof(ts), times[3]);
        check_str("domains: [3] = 03:20", ts, "2026-08-18T03:20:00Z");
        /* bare-date start bounds the walk at midnight */
        const char short_seg[] = "<Domain>2026-08-18/2026-08-18T00:10:00Z/PT10M</Domain>";
        n = clouds_parse_domains(short_seg, strlen(short_seg), times, 10);
        check_int("domains: midnight-bounded segment = 2", n, 2);
        /* single instants and a non-default period */
        const char singles[] = "<Domain>2026-08-18T01:00:00Z,2026-08-18T01:30:00Z/2026-08-18T02:30:00Z/PT30M</Domain>";
        n = clouds_parse_domains(singles, strlen(singles), times, 10);
        check_int("domains: single + PT30M range = 4", n, 4);
        clouds_time_format(ts, sizeof(ts), times[1]);
        check_str("domains: PT30M step [1] = 02:00", ts, "2026-08-18T02:00:00Z");
        clouds_time_format(ts, sizeof(ts), times[3]);
        check_str("domains: single last = 01:00", ts, "2026-08-18T01:00:00Z");
        /* missing element / empty / bare-date only */
        check_int("domains: no <Domain> = 0", clouds_parse_domains("<Domains></Domains>", 19, times, 10), 0);
        check_int("domains: empty = 0", clouds_parse_domains("<Domain></Domain>", 17, times, 10), 0);
        check_int("domains: bare date only = 0", clouds_parse_domains("<Domain>2026-08-18</Domain>", 27, times, 10), 0);
        /* range whose end has no 'T' is skipped, the rest survives */
        const char bad_end[] = "<Domain>2026-08-18T01:00:00Z/2026-08-18/PT10M,2026-08-18T02:00:00Z</Domain>";
        n = clouds_parse_domains(bad_end, strlen(bad_end), times, 10);
        check_int("domains: bad end skipped = 1", n, 1);
        /* body without a NUL terminator: only len bytes are read */
        char nonul[64];
        memcpy(nonul, "<Domain>2026-08-18T05:00:00Z</Domain>XXXXXXXXXXXXXXXXXXXXXXXXXX", 64);
        n = clouds_parse_domains(nonul, 37, times, 10);
        check_int("domains: bounded by len", n, 1);
    }

    /* -- EUMETView capabilities URL + default= parser ---------------------- */
    {
        check_bool("eumet caps url builds", clouds_eumet_caps_url(url, sizeof(url), "mtg_fd"), true);
        check_str("eumet caps url exact", url,
                  "https://view.eumetsat.int/geoserver/mtg_fd/wms?service=WMS&version=1.3.0&request=GetCapabilities");
        check_bool("eumet caps url msg_fes",
                   clouds_eumet_caps_url(url, sizeof(url), "msg_fes"), true);
        check_contains("eumet caps url uses the workspace", url, "/geoserver/msg_fes/wms?", true);
        check_bool("eumet caps url NULL workspace fails",
                   clouds_eumet_caps_url(url, sizeof(url), NULL), false);
        char small[40];
        check_bool("eumet caps url tiny fails", clouds_eumet_caps_url(small, sizeof(small), "mtg_fd"), false);

        size_t len = 0;
        char *xml = read_fixture("eumet_caps_mtg_fd.xml", &len);
        check_bool("eumet caps fixture loads", xml != NULL, true);
        if (xml != NULL) {
            uint32_t t = 0;
            /* Callers pass the TABLE name, prefix and all; the workspace-scoped
             * document names the same layer bare. The real capture of
             * 2026-09-03 contains no prefixed name at all, so this pair of
             * assertions is what stands between the page and a permanent
             * fallback: before the strip, every lookup missed and newest stayed
             * 0 on every poll. */
            check_bool("eumet default: geocolour found by its TABLE name",
                       clouds_eumet_default_time(xml, len, "mtg_fd:rgb_geocolour", &t), true);
            clouds_time_format(ts, sizeof(ts), t);
            check_str("eumet default: geocolour 19:30 (real capture)", ts, "2026-09-03T19:30:00Z");
            check_bool("eumet default: ir105 found by its TABLE name",
                       clouds_eumet_default_time(xml, len, "mtg_fd:ir105_hrfi", &t), true);
            clouds_time_format(ts, sizeof(ts), t);
            check_str("eumet default: ir105 19:40 (real capture)", ts, "2026-09-03T19:40:00Z");
            /* A bare name is already bare and must still work, so the strip
             * cannot be a blind "skip N characters". */
            check_bool("eumet default: bare name works unchanged",
                       clouds_eumet_default_time(xml, len, "rgb_geocolour", &t), true);
            clouds_time_format(ts, sizeof(ts), t);
            check_str("eumet default: bare name 19:30", ts, "2026-09-03T19:30:00Z");
            /* Sibling layers in one workspace default to DIFFERENT slots, so an
             * exact <Name> match is the whole correctness of this function: a
             * first-Dimension-wins parser would answer 19:40 for all three. */
            check_bool("eumet default: off-grid sibling li_afa found",
                       clouds_eumet_default_time(xml, len, "mtg_fd:li_afa", &t), true);
            clouds_time_format(ts, sizeof(ts), t);
            check_str("eumet default: li_afa 19:45, not a neighbour's stamp", ts,
                      "2026-09-03T19:45:00Z");
            /* The <Style> inside ir105_hrfi is named mtg_fd_ir105_hrfi_grayscale.
             * Once the needle is bare it is "<Name>ir105_hrfi</Name>", which must
             * not be satisfied by that neighbour: a substring match would bound
             * the search to a Style element, past its layer's time dimension. */
            check_bool("eumet default: style name is not a layer name",
                       clouds_eumet_default_time(xml, len, "mtg_fd_ir105_hrfi_grayscale", &t), false);
            check_bool("eumet default: unknown layer",
                       clouds_eumet_default_time(xml, len, "mtg_fd:nope", &t), false);
            /* The extent TEXT carries .000Z while the attribute does not, so
             * this asserts the fixture still reproduces that asymmetry: if a
             * future edit drops the fraction from the text, the tolerance
             * branch below stops being exercised by anything real. */
            check_bool("eumet fixture: extent text still carries .000Z",
                       memchr(xml, '.', len) != NULL, true);
            /* and the attribute itself must stay fraction-free, which is the
             * form the buffer bound has to have room for */
            check_contains("eumet fixture: attribute has no fraction", xml,
                           "default=\"2026-09-03T19:40:00Z\"", true);
            /* bounded by len, no NUL needed */
            check_bool("eumet default: truncated body",
                       clouds_eumet_default_time(xml, 400, "mtg_fd:rgb_geocolour", &t), false);
            free(xml);
        }
        /* A layer with no time dimension at all, and a non-time dimension that
         * must be stepped over: neither appears in the real mtg_fd capture (its
         * every layer carries exactly one time dimension), so they are covered
         * here rather than fabricated into the fixture. */
        const char no_time[] = "<Layer><Name>y</Name><Title>t</Title></Layer>";
        uint32_t nt = 0;
        check_bool("eumet default: layer with no time dimension",
                   clouds_eumet_default_time(no_time, strlen(no_time), "ws:y", &nt), false);
        const char skip_dim[] =
            "<Layer><Name>y</Name><Dimension name=\"elevation\" units=\"EPSG:5030\"/>"
            "<Dimension name=\"time\" default=\"2026-09-03T19:40:00Z\" units=\"ISO8601\"/></Layer>";
        check_bool("eumet default: steps over a self-closing non-time dimension",
                   clouds_eumet_default_time(skip_dim, strlen(skip_dim), "ws:y", &nt), true);
        clouds_time_format(ts, sizeof(ts), nt);
        check_str("eumet default: skipped-dimension layer 19:40", ts, "2026-09-03T19:40:00Z");
        /* Tolerance only: EUMETView writes the attribute WITHOUT a fraction
         * (verified 2026-09-03), but a fractional value must not break it. */
        const char frac[] =
            "<Layer><Name>y</Name>"
            "<Dimension name=\"time\" default=\"2026-09-03T10:00:00.000Z\" units=\"ISO8601\">"
            "2026-09-03T10:00:00.000Z</Dimension></Layer>";
        uint32_t t = 0;
        check_bool("eumet default: fractional value tolerated",
                   clouds_eumet_default_time(frac, strlen(frac), "x:y", &t), true);
        clouds_time_format(ts, sizeof(ts), t);
        check_str("eumet default: fractional value 10:00", ts, "2026-09-03T10:00:00Z");
        /* a malformed default is rejected, not guessed at */
        const char bad[] =
            "<Layer><Name>y</Name>"
            "<Dimension name=\"time\" default=\"current\" units=\"ISO8601\"/></Layer>";
        check_bool("eumet default: 'current' rejected",
                   clouds_eumet_default_time(bad, strlen(bad), "x:y", &t), false);
    }

    /* -- workspace-scoped lookup name -------------------------------------- */
    {
        check_str("bare name: strips the workspace prefix",
                  clouds_layer_bare_name("mtg_fd:rgb_geocolour"), "rgb_geocolour");
        check_str("bare name: msg_fes air mass", clouds_layer_bare_name("msg_fes:rgb_airmass"),
                  "rgb_airmass");
        check_str("bare name: an unprefixed name is unchanged",
                  clouds_layer_bare_name("GOES-East_ABI_GeoColor"), "GOES-East_ABI_GeoColor");
        check_bool("bare name: NULL is NULL", clouds_layer_bare_name(NULL) == NULL, true);
        /* underscores are not prefixes: a GIBS layer must survive intact */
        check_str("bare name: underscored GIBS layer unchanged",
                  clouds_layer_bare_name("Himawari_AHI_Band13_Clean_Infrared"),
                  "Himawari_AHI_Band13_Clean_Infrared");
    }

    /* -- per-layer publishing period --------------------------------------- */
    {
        /* The grid is a property of the LAYER's workspace, not of the row: the
         * MTG row is a 10-minute service whose air-mass layer is borrowed from
         * the 15-minute msg_fes one. Stepping history back on the row's 600 s
         * would repeat every third Air Mass frame over Europe. */
        const clouds_sat_t *mtg  = clouds_sat_for_lon(-0.13f);   /* London */
        const clouds_sat_t *iodc = clouds_sat_for_lon(55.27f);   /* Dubai */
        const clouds_sat_t *goes = clouds_sat_for_lon(-90.0f);   /* St Louis */
        check_int("period: MTG photo = 600", clouds_layer_period_s(mtg, CLOUDS_ROLE_PHOTO_DAY), 600);
        check_int("period: MTG night photo = 600", clouds_layer_period_s(mtg, CLOUDS_ROLE_PHOTO_NIGHT), 600);
        check_int("period: MTG IR = 600", clouds_layer_period_s(mtg, CLOUDS_ROLE_IR), 600);
        check_int("period: MTG Air Mass = 900 (msg_fes)", clouds_layer_period_s(mtg, CLOUDS_ROLE_AIR), 900);
        check_int("period: IODC photo = 900", clouds_layer_period_s(iodc, CLOUDS_ROLE_PHOTO_DAY), 900);
        check_int("period: IODC Air Mass = 900", clouds_layer_period_s(iodc, CLOUDS_ROLE_AIR), 900);
        check_int("period: GIBS row = 600", clouds_layer_period_s(goes, CLOUDS_ROLE_AIR), 600);
        check_int("period: NULL row = GIBS default", clouds_layer_period_s(NULL, CLOUDS_ROLE_IR),
                  CLOUDS_GIBS_PERIOD_S);
        /* the fallback floors to the LAYER's grid, not the row's */
        uint32_t nowish = 1788000000u + 137u;    /* deliberately off both grids */
        check_int("fallback: floors to 900 for MTG Air Mass",
                  (long)(clouds_eumet_fallback_newest(nowish, clouds_layer_period_s(mtg, CLOUDS_ROLE_AIR)) % 900u), 0);
        check_int("fallback: floors to 600 for MTG photo",
                  (long)(clouds_eumet_fallback_newest(nowish, clouds_layer_period_s(mtg, CLOUDS_ROLE_PHOTO_DAY)) % 600u), 0);
    }

    /* ---- blank / partial frame detection ---- */
    {
        enum { W = 96, H = 64 };            /* 12 x 8 = 96 sampled pixels */
        static uint16_t px[W * H];
        memset(px, 0, sizeof(px));
        check_bool("incomplete: all black", clouds_frame_incomplete(px, W, H, W), true);
        for (int i = 0; i < W * H; i++) px[i] = 0x0004;          /* night navy, RGB(0,0,32): above the near-black gate */
        check_bool("incomplete: all dark navy", clouds_frame_incomplete(px, W, H, W), false);
        for (int i = 0; i < W * H; i++) px[i] = 0x0841;          /* RGB(8,8,8): HW crushes this to 0, stb keeps it; both count as near black */
        check_bool("incomplete: all crushed-dark counts as black", clouds_frame_incomplete(px, W, H, W), true);
        for (int i = 0; i < W * H; i++) px[i] = 0x0004;
        /* 25% black block (one missing quadrant) is NOT rejected any more: the
         * gate only catches blank slots; partials self-heal on the next poll. */
        for (int y = 0; y < H / 2; y++)
            for (int x = 0; x < W / 2; x++) px[y * W + x] = 0;
        check_bool("incomplete: 25% black block passes (GeoColor)", clouds_frame_incomplete(px, W, H, W), false);
        /* 95% black = blank slot with a few basemap lines */
        for (int i = 0; i < W * H; i++) px[i] = 0;
        for (int x = 0; x < W; x += 8) px[x] = 0x0004;               /* 12 of 96 samples lit = 87.5% black */
        check_bool("incomplete: 87% black passes", clouds_frame_incomplete(px, W, H, W), false);
        for (int x = 8; x < W; x += 8) px[x] = 0;                     /* 1 of 96 lit = 99% black */
        check_bool("incomplete: 99% black is blank", clouds_frame_incomplete(px, W, H, W), true);
        /* ~1% black: 1 of the 96 sampled pixels */
        for (int i = 0; i < W * H; i++) px[i] = 0x0004;
        px[0] = 0;
        check_bool("incomplete: 1% black", clouds_frame_incomplete(px, W, H, W), false);
        check_bool("incomplete: NULL is not incomplete", clouds_frame_incomplete(NULL, W, H, W), false);
    }

    /* ---- missing-tile (partial ingest) detection, per cell ---- */
    {
        /* Full 720x720, so a cell is the real 120 px and one missing tile fills
         * one, as on the device. Static, not stack: 1 MB each. */
        enum { FW = 720, FH = 720, CELL = FW / CLOUDS_HOLE_CELLS };
        static uint16_t cand[FW * FH], base[FW * FH];

        fill_night(base, FW, FH);
        memcpy(cand, base, sizeof(base));
        check_bool("holes: identical night frames", clouds_frame_holes(cand, base, FW, FH, FW), false);

        /* 10 min of weather motion: 1 sample in 8 goes dark, scattered. */
        for (int y = 0; y < FH; y++)
            for (int x = 0; x < FW; x++)
                if ((((x >> 3) + (y >> 3)) & 7) == 0) cand[y * FW + x] = DARK_PX;
        check_bool("holes: night frame, 12% scattered dimming", clouds_frame_holes(cand, base, FW, FH, FW), false);

        /* Terminator crossing one whole cell. It takes the imagery with it, but
         * the night side keeps the ~42 % lit measured on the device, so about
         * 60 % of that cell's ref-lit samples go dark: under the bar. */
        memcpy(cand, base, sizeof(base));
        darken_cell(cand, FW, 4, 1, CELL, 3);            /* 3 blocks in 5 = 60 % */
        check_bool("holes: terminator, 60% of a cell dimmed", clouds_frame_holes(cand, base, FW, FH, FW), false);
        memcpy(cand, base, sizeof(base));
        darken_cell(cand, FW, 4, 1, CELL, 4);            /* 80 % */
        check_bool("holes: 80% of a cell dark is a hole", clouds_frame_holes(cand, base, FW, FH, FW), true);

        /* One missing 120 px tile, rendered flat black. */
        memcpy(cand, base, sizeof(base));
        black_cell(cand, FW, 4, 1, CELL);
        check_bool("holes: one 120px cell zeroed", clouds_frame_holes(cand, base, FW, FH, FW), true);

        /* Same cell, dark in the REFERENCE too (ocean at night, or a reference
         * that is itself holed there): the per-cell lit floor says nothing. */
        black_cell(base, FW, 4, 1, CELL);
        check_bool("holes: cell dark in the reference too", clouds_frame_holes(cand, base, FW, FH, FW), false);

        /* The real device case: GIBS composites the vector basemap over the
         * whole canvas, so a hole keeps its border/road/graticule lines (~40
         * grey) drawn over black. Those survivors must not dilute the cell
         * below the bar. One sample column in 13 here, the density of 1-2 px
         * vectors on a 720 px frame. */
        fill_night(base, FW, FH);
        memcpy(cand, base, sizeof(base));
        black_cell(cand, FW, 4, 1, CELL);
        for (int y = CELL; y < 2 * CELL; y++)
            for (int x = 4 * CELL; x < 5 * CELL; x++)
                if (((x >> 3) % 13) == 0) cand[y * FW + x] = LINE_PX;
        check_bool("holes: hole keeps its overlay lines", clouds_frame_holes(cand, base, FW, FH, FW), true);

        /* Day frame (fully lit) missing a whole quadrant. */
        for (int i = 0; i < FW * FH; i++) { base[i] = LIT_PX; cand[i] = LIT_PX; }
        for (int y = 0; y < FH / 2; y++)
            for (int x = 0; x < FW / 2; x++) cand[y * FW + x] = 0x0000;
        check_bool("holes: day frame missing a quadrant", clouds_frame_holes(cand, base, FW, FH, FW), true);

        /* Degenerate inputs. */
        memset(base, 0, sizeof(base));
        check_bool("holes: blank reference says nothing", clouds_frame_holes(cand, base, FW, FH, FW), false);
        check_bool("holes: NULL frame", clouds_frame_holes(NULL, base, FW, FH, FW), false);
        check_bool("holes: NULL reference", clouds_frame_holes(cand, NULL, FW, FH, FW), false);
    }

    printf("\n%s (%d failures)\n", fails == 0 ? "ALL PASSED" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}
