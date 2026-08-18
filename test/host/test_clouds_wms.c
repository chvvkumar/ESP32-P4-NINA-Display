/* Host test for main/clouds_wms.h -- the pure URL/geometry/time decisions behind
 * the Clouds page (NASA GIBS WMS, GOES ABI GeoColor): satellite pick, square
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

/* Parse "&BBOX=a,b,c,d" out of a URL. */
static bool url_bbox(const char *url, long *a, long *b, long *c, long *d) {
    const char *p = strstr(url, "&BBOX=");
    if (!p) return false;
    return sscanf(p + 6, "%ld,%ld,%ld,%ld", a, b, c, d) == 4;
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

int main(void) {
    char url[CLOUDS_URL_MAX];
    char ts[CLOUDS_TIME_MAX];
    uint32_t times[CLOUDS_TIMES_MAX];

    /* -- satellite pick ---------------------------------------------------- */
    check_str("layer: St Louis (-90.7) -> East", clouds_layer(-90.689438f), CLOUDS_LAYER_EAST);
    check_str("layer: Denver (-105.0) -> East", clouds_layer(-105.0f), CLOUDS_LAYER_EAST);
    check_str("layer: -106.2 exactly -> East", clouds_layer(-106.2f), CLOUDS_LAYER_EAST);
    check_str("layer: -106.3 -> West", clouds_layer(-106.3f), CLOUDS_LAYER_WEST);
    check_str("layer: Seattle (-122.3) -> West", clouds_layer(-122.33f), CLOUDS_LAYER_WEST);
    check_str("layer: (0,0) -> East", clouds_layer(0.0f), CLOUDS_LAYER_EAST);

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

    /* -- GetMap URL -------------------------------------------------------- */
    {
        uint32_t stamp = 1787025600u;   /* 2026-08-18T04:00:00Z */
        check_bool("url: builds", clouds_frame_url(url, sizeof(url), 38.758331f, -90.689438f, 7, stamp), true);
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
        check_bool("url: Seattle builds", clouds_frame_url(url, sizeof(url), 47.6f, -122.33f, 6, stamp), true);
        check_contains("url: Seattle uses West", url, "LAYERS=GOES-West_ABI_GeoColor,", true);
        /* too small a buffer -> false and "" */
        char tiny[64];
        check_bool("url: tiny buffer fails", clouds_frame_url(tiny, sizeof(tiny), 1.0f, 2.0f, 7, stamp), false);
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
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 1));
        check_str("step: 09-01 00:00 back 10 min = 08-31 23:50", ts, "2026-08-31T23:50:00Z");
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 7));
        check_str("step: back 70 min = 08-31 22:50", ts, "2026-08-31T22:50:00Z");
        clouds_time_parse("2027-01-01T00:10:00Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_time_step(t0, 2));
        check_str("step: across the year = 2026-12-31 23:50", ts, "2026-12-31T23:50:00Z");
        check_int("step: i=0 is identity", (long)clouds_time_step(t0, 0), (long)t0);
        check_int("step: negative i is identity", (long)clouds_time_step(t0, -3), (long)t0);

        /* fallback newest: floor to 10 min, minus 50 min */
        clouds_time_parse("2026-08-18T04:17:33Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_fallback_newest(t0));
        check_str("fallback: 04:17:33 -> 03:20:00", ts, "2026-08-18T03:20:00Z");
        clouds_time_parse("2026-08-18T00:05:00Z", 20, &t0);
        clouds_time_format(ts, sizeof(ts), clouds_fallback_newest(t0));
        check_str("fallback: 00:05 -> 23:10 previous day", ts, "2026-08-17T23:10:00Z");
        check_int("floor_period exact", (long)clouds_floor_period(1787025600u), 1787025600L);
        check_int("floor_period +599", (long)clouds_floor_period(1787025600u + 599u), 1787025600L);
    }

    /* -- DescribeDomains URL ---------------------------------------------- */
    {
        uint32_t now = 1787025600u + 150u;   /* 04:02:30Z */
        check_bool("domains url builds", clouds_domains_url(url, sizeof(url), CLOUDS_LAYER_EAST, now), true);
        printf("    url=%s\n", url);
        check_str("domains url exact", url,
                  "https://gibs.earthdata.nasa.gov/wmts/epsg4326/best/1.0.0/GOES-East_ABI_GeoColor/default/1km/all/"
                  "2026-08-18T01:02:30Z--2026-08-18T05:02:30Z.xml");
        check_bool("domains url fits 160", strlen(url) < 160, true);
        char small[100];
        check_bool("domains url tiny fails", clouds_domains_url(small, sizeof(small), CLOUDS_LAYER_WEST, now), false);
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

    /* ---- blank / partial frame detection ---- */
    {
        enum { W = 96, H = 64 };
        static uint16_t px[W * H];
        memset(px, 0, sizeof(px));
        check_bool("incomplete: all black", clouds_frame_incomplete(px, W, H, W), true);
        for (int i = 0; i < W * H; i++) px[i] = 0x0841;          /* night navy */
        check_bool("incomplete: all dark navy", clouds_frame_incomplete(px, W, H, W), false);
        /* 25% black block: top-left quadrant */
        for (int y = 0; y < H / 2; y++)
            for (int x = 0; x < W / 2; x++) px[y * W + x] = 0;
        check_bool("incomplete: 25% black block", clouds_frame_incomplete(px, W, H, W), true);
        /* ~1% black: 1 of the 12x8 = 96 sampled pixels */
        for (int i = 0; i < W * H; i++) px[i] = 0x0841;
        px[0] = 0;
        check_bool("incomplete: 1% black", clouds_frame_incomplete(px, W, H, W), false);
        check_bool("incomplete: NULL is not incomplete", clouds_frame_incomplete(NULL, W, H, W), false);
    }

    printf("\n%s (%d failures)\n", fails == 0 ? "ALL PASSED" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}
