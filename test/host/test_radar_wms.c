/* Host test for main/radar_wms.h -- the pure URL/geometry decisions behind the
 * road-free radar source (NCEP GeoServer WMS): region lookup, float Web-Mercator,
 * pixel sizing, GetMap/GetCapabilities URL shape, and the capabilities TIME list
 * parser. Also covers radar_site_coords() (main/radar_sites.c), which feeds the
 * site box. Header-only, no ESP-IDF dependency; assert-style like
 * test/host/test_radar_play.c. The parser is additionally run over the two real
 * capabilities documents captured 2026-08-17 in test/host/fixtures/. */
#include "radar_wms.h"
#include "radar_sites.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NINA_FIXTURE_DIR
#define NINA_FIXTURE_DIR "fixtures"
#endif

static int fails = 0;

static void check_int(const char *label, int got, int expect) {
    printf("%-64s got=%-6d expect=%-6d %s\n", label, got, expect,
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

/* Read a whole file into a malloc'd buffer with NO NUL terminator, so a parser
 * that leaned on strstr() would overrun instead of passing. */
static char *read_fixture(const char *name, size_t *len_out) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", NINA_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *len_out = (size_t)n;
    return buf;
}

/* Parse "&bbox=a,b,c,d" out of a URL. */
static bool url_bbox(const char *url, long *a, long *b, long *c, long *d) {
    const char *p = strstr(url, "&bbox=");
    if (!p) return false;
    return sscanf(p + 6, "%ld,%ld,%ld,%ld", a, b, c, d) == 4;
}

static const char synthetic_caps[] =
    "<?xml version=\"1.0\"?><WMS_Capabilities><Capability><Layer><Title>root</Title>"
    "<Layer queryable=\"1\"><Name>klsx_bdhc</Name><Title>decoy</Title>"
    "<Keyword>klsx_sr_bref</Keyword>"
    "<Dimension name=\"elevation\" units=\"m\"/>"
    "<Dimension name=\"time\" default=\"2026-01-01T00:20:00Z\" units=\"ISO8601\" nearestValue=\"1\">"
    "2026-01-01T00:00:00.000Z,2026-01-01T00:10:00.000Z,2026-01-01T00:20:00.000Z</Dimension>"
    "<Style><Name>radar_bdhc</Name></Style></Layer>\n"
    "<Layer queryable=\"1\"><Name>klsx_sr_bref</Name><Title>klsx_sr_bref</Title>"
    "<Keyword>klsx_sr_bref</Keyword><CRS>EPSG:3857</CRS>"
    "<Dimension name=\"time\" default=\"2026-08-17T22:46:06Z\" units=\"ISO8601\" nearestValue=\"1\">"
    "2026-08-17T20:50:22.000Z,2026-08-17T20:54:18.000Z,2026-08-17T21:03:07.000Z,"
    "2026-08-17T22:41:13.000Z,2026-08-17T22:46:06.000Z</Dimension>\n"
    "<Style><Name>radar_reflectivity</Name></Style></Layer>"
    "<Layer><Name>klsx_sr_bvel</Name>"
    "<Dimension name=\"time\" default=\"x\" units=\"ISO8601\">2030-01-01T00:00:00.000Z</Dimension>"
    "</Layer>"
    "<Layer><Name>klsx_boha</Name><Dimension name=\"elevation\">1,2,3</Dimension></Layer>"
    "<Layer><Name>klsx_interval</Name>"
    "<Dimension name=\"time\" units=\"ISO8601\">2026-01-01T00:00:00Z/2026-01-02T00:00:00Z/PT2M</Dimension>"
    "</Layer>"
    "<Layer><Name>klsx_empty</Name><Dimension name=\"time\" units=\"ISO8601\"></Dimension></Layer>"
    "<Layer><Name>klsx_long</Name><Dimension name=\"time\" units=\"ISO8601\">"
    "2026-01-01T00:00:00.000Z,2026-01-01T00:00:00.000000000000000000000Z</Dimension></Layer>"
    "<Layer><Name>klsx_ws</Name><Dimension name=\"time\" units=\"ISO8601\">\n  2026-01-01T00:00:00Z ,\n"
    " 2026-01-01T00:10:00Z\n</Dimension></Layer>"
    "</Layer></Capability></WMS_Capabilities>";

int main(void) {
    char url[RADAR_WMS_URL_MAX];
    char times[RADAR_WMS_TIMES_MAX][RADAR_WMS_TIME_MAX];

    /* -- region table --------------------------------------------------------- */
    {
        const radar_wms_region_t *r = radar_wms_region("NORTHEAST");
        check_bool("region: NORTHEAST found", r != NULL, true);
        if (r) {
            check_str("region: NORTHEAST ns", r->ns, "conus");
            check_str("region: NORTHEAST layer", r->layer, "conus_bref_qcd");
            check_near("region: NORTHEAST min_lon", r->min_lon, -81.0f, 0.001f);
        }
        check_bool("region: ALASKA found", radar_wms_region("ALASKA") != NULL, true);
        check_bool("region: KLSX is not a region", radar_wms_region("KLSX") == NULL, true);
        check_bool("region: NULL", radar_wms_region(NULL) == NULL, true);
        check_bool("region: lowercase miss", radar_wms_region("northeast") == NULL, true);
    }

    /* -- Mercator --------------------------------------------------------------- */
    check_near("merc_x(0) == 0", radar_wms_merc_x(0.0f), 0.0f, 0.001f);
    check_near("merc_y(0) == 0", radar_wms_merc_y(0.0f), 0.0f, 0.001f);
    check_near("merc_x(180) ~ 20037508", radar_wms_merc_x(180.0f), 20037508.34f, 1.0f);
    check_near("merc_x(-90) ~ -10018754", radar_wms_merc_x(-90.0f), -10018754.17f, 1.0f);
    check_near("merc_y(45) ~ 5621521", radar_wms_merc_y(45.0f), 5621521.49f, 2.0f);
    {
        int mono = 1;
        float prev = radar_wms_merc_y(-85.0f);
        for (int lat = -84; lat <= 85; lat++) {
            float y = radar_wms_merc_y((float)lat);
            if (!(y > prev)) mono = 0;
            prev = y;
        }
        check_int("merc_y monotonic -85..85", mono, 1);
        check_bool("merc_y clamps above 85", radar_wms_merc_y(89.0f) == radar_wms_merc_y(85.0f), true);
    }

    /* -- pixel size ------------------------------------------------------------- */
    {
        int w = 0, h = 0;
        radar_wms_pixel_size(2000.0f, 1000.0f, &w, &h);
        check_int("pixel: wider -> w", w, 600);
        check_int("pixel: wider -> h", h, 300);
        radar_wms_pixel_size(1000.0f, 2000.0f, &w, &h);
        check_int("pixel: taller -> w", w, 300);
        check_int("pixel: taller -> h", h, 600);
        radar_wms_pixel_size(1234.0f, 1234.0f, &w, &h);
        check_int("pixel: square -> w", w, 600);
        check_int("pixel: square -> h", h, 600);
        radar_wms_pixel_size(1.0e7f, 1.0f, &w, &h);
        check_int("pixel: degenerate -> h floor 1", h, 1);
        radar_wms_pixel_size(0.0f, 0.0f, &w, &h);
        check_int("pixel: zero span -> 600 w", w, 600);
        check_int("pixel: zero span -> 600 h", h, 600);
    }

    /* -- site coords ------------------------------------------------------------ */
    float klsx_lat = 0.0f, klsx_lon = 0.0f;
    check_bool("coords: KLSX found", radar_site_coords("KLSX", &klsx_lat, &klsx_lon), true);
    check_near("coords: KLSX lat", klsx_lat, 38.699f, 0.01f);
    check_near("coords: KLSX lon", klsx_lon, -90.683f, 0.01f);
    check_bool("coords: ZZZZ unknown", radar_site_coords("ZZZZ", &klsx_lat, &klsx_lon), false);
    check_bool("coords: NULL", radar_site_coords(NULL, &klsx_lat, &klsx_lon), false);
    check_bool("coords: NULL outputs tolerated", radar_site_coords("KTLX", NULL, NULL), true);
    radar_site_coords("KLSX", &klsx_lat, &klsx_lon);

    /* -- site frame URL, style 1 ------------------------------------------------ */
    {
        bool ok = radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, NULL);
        check_bool("url: KLSX style 1 builds", ok, true);
        printf("    %s\n", url);
        check_contains("url: layers+styles for style 1", url,
                       "layers=klsx:klsx_sr_bref,nws:state_boundary&styles=radar_reflectivity,boundary_gray&", true);
        check_contains("url: crs=EPSG:3857", url, "crs=EPSG:3857", true);
        check_contains("url: 600x600", url, "width=600&height=600", true);
        check_contains("url: no time when NULL", url, "time=", false);
        check_contains("url: no us_counties in style 1", url, "us_counties", false);
        check_contains("url: GetMap prefix", url,
                       "https://opengeo.ncep.noaa.gov/geoserver/ows?service=WMS&version=1.3.0&request=GetMap&", true);
        check_contains("url: format/bgcolor/transparent", url,
                       "&format=image/gif&bgcolor=0x000000&transparent=false", true);
        check_bool("url: shorter than RADAR_WMS_URL_MAX", strlen(url) < RADAR_WMS_URL_MAX, true);
        long a = 0, b = 0, c = 0, d = 0;
        check_bool("url: bbox parses", url_bbox(url, &a, &b, &c, &d), true);
        check_bool("url: bbox is square (within 2 m)", labs((c - a) - (d - b)) <= 2, true);
        check_bool("url: bbox x span ~ 10 deg at equator scale",
                   labs((c - a) - 1113195L) <= 4, true);
        check_bool("url: bbox centred on site x",
                   labs((a + c) / 2 - (long)lroundf(radar_wms_merc_x(klsx_lon))) <= 2, true);
        check_bool("url: bbox centred on site y",
                   labs((b + d) / 2 - (long)lroundf(radar_wms_merc_y(klsx_lat))) <= 2, true);

        ok = radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, "2026-08-17T20:50:22.000Z");
        check_bool("url: KLSX with time builds", ok, true);
        check_contains("url: time appended", url, "&time=2026-08-17T20:50:22.000Z", true);
        check_bool("url: time is the last thing", strcmp(url + strlen(url) - 24, "2026-08-17T20:50:22.000Z") == 0, true);
        ok = radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, "");
        check_bool("url: empty time builds", ok, true);
        check_contains("url: empty time omitted", url, "time=", false);
    }

    /* -- style 2 ----------------------------------------------------------------- */
    {
        bool ok = radar_wms_frame_url(url, sizeof(url), 2, "KLSX", klsx_lat, klsx_lon, NULL);
        check_bool("url: KLSX style 2 builds", ok, true);
        check_contains("url: style 2 layers", url,
                       "layers=klsx:klsx_sr_bref,nws:state_boundary,nws:us_counties&", true);
        check_contains("url: style 2 three styles", url,
                       "&styles=radar_reflectivity,boundary_gray,boundary_gray&", true);
    }

    /* -- regional URL ------------------------------------------------------------ */
    {
        bool ok = radar_wms_frame_url(url, sizeof(url), 1, "NORTHEAST", 0.0f, 0.0f, NULL);
        check_bool("url: NORTHEAST builds", ok, true);
        printf("    %s\n", url);
        check_contains("url: NORTHEAST layer", url, "layers=conus:conus_bref_qcd,nws:state_boundary&", true);
        long a = 0, b = 0, c = 0, d = 0;
        check_bool("url: NORTHEAST bbox parses", url_bbox(url, &a, &b, &c, &d), true);
        check_bool("url: NORTHEAST minx <= merc(-81) (padded outward)", a <= (long)lroundf(radar_wms_merc_x(-81.0f)), true);
        check_bool("url: NORTHEAST maxy = merc(50)", labs(d - (long)lroundf(radar_wms_merc_y(50.0f))) <= 1, true);
        /* landscape or square: the renderer scales to panel width, top-anchored */
        check_bool("url: NORTHEAST bbox xspan >= yspan (padded in x, 2 m rounding slack)", (c - a) + 2 >= (d - b), true);
        check_bool("url: NORTHEAST bbox padded symmetrically about merc(-73.5)",
                   labs((a + c) / 2 - (long)lroundf(radar_wms_merc_x(-73.5f))) <= 2, true);
        check_contains("url: NORTHEAST width 600", url, "&width=600&", true);
        {
            int hh = 0;
            const char *hp = strstr(url, "&height=");
            check_bool("url: NORTHEAST height parses", hp && sscanf(hp + 8, "%d", &hh) == 1, true);
            check_bool("url: NORTHEAST height <= 600", hh >= 1 && hh <= 600, true);
        }
        {
            static const char *const regs[] = { "SOUTHEAST", "UPPERMISSVLY", "SOUTHMISSVLY",
                "NORTHROCKIES", "SOUTHROCKIES", "PACNORTHWEST", "PACSOUTHWEST", "CENTGRLAKES",
                "ALASKA", "HAWAII", "CONUS" };
            int bad = 0;
            for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
                long ra = 0, rb = 0, rc = 0, rd = 0;
                if (!radar_wms_frame_url(url, sizeof(url), 1, regs[i], 0.0f, 0.0f, NULL)) { bad++; continue; }
                if (!url_bbox(url, &ra, &rb, &rc, &rd)) { bad++; continue; }
                if ((rc - ra) + 2 < (rd - rb)) bad++;   /* integer-rounded bbox: 2 m slack */
                if (!strstr(url, "&width=600&")) bad++;
            }
            check_int("url: every region/mosaic is width 600 with xspan >= yspan", bad, 0);
        }
        ok = radar_wms_frame_url(url, sizeof(url), 1, "CONUS", 0.0f, 0.0f, NULL);
        check_bool("url: CONUS builds", ok, true);
        check_contains("url: CONUS width 600 (wider than tall)", url, "&width=600&", true);
        check_contains("url: CONUS height < 600", url, "&height=600&", false);
        ok = radar_wms_frame_url(url, sizeof(url), 2, "ALASKA", 0.0f, 0.0f, "2026-08-17T22:50:56.000Z");
        check_bool("url: ALASKA style 2 with time builds", ok, true);
        check_contains("url: ALASKA layer", url, "layers=alaska:alaska_bref_qcd,nws:state_boundary,nws:us_counties&", true);
    }

    /* -- rejections --------------------------------------------------------------- */
    {
        strcpy(url, "sentinel");
        check_bool("url: style 0 -> false", radar_wms_frame_url(url, sizeof(url), 0, "KLSX", klsx_lat, klsx_lon, NULL), false);
        check_bool("url: style 0 -> empty", url[0] == '\0', true);
        check_bool("url: style 3 -> false", radar_wms_frame_url(url, sizeof(url), 3, "KLSX", klsx_lat, klsx_lon, NULL), false);
        check_bool("url: BAN KTLX_loop.gif?", radar_wms_frame_url(url, sizeof(url), 1, "KTLX_loop.gif?", 0.0f, 0.0f, NULL), false);
        check_bool("url: empty token", radar_wms_frame_url(url, sizeof(url), 1, "", 0.0f, 0.0f, NULL), false);
        check_bool("url: NULL token", radar_wms_frame_url(url, sizeof(url), 1, NULL, 0.0f, 0.0f, NULL), false);
        check_bool("url: 15-char token accepted", radar_wms_frame_url(url, sizeof(url), 1, "ABCDEFGHIJKLMNO", 0.0f, 0.0f, NULL), true);
        check_bool("url: 16-char token rejected", radar_wms_frame_url(url, sizeof(url), 1, "ABCDEFGHIJKLMNOP", 0.0f, 0.0f, NULL), false);
        check_bool("url: lowercase token rejected", radar_wms_frame_url(url, sizeof(url), 1, "klsx", 0.0f, 0.0f, NULL), false);
        check_bool("url: stamp with & rejected", radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, "2026-08-17T20:50:22.000Z&layers=x"), false);
        check_bool("url: stamp with ? rejected", radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, "x?y"), false);
        check_bool("url: stamp with space rejected", radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, "2026-08-17 20:50"), false);
        check_bool("url: stamp with < rejected", radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, "<x>"), false);
        check_bool("url: stamp with # rejected", radar_wms_frame_url(url, sizeof(url), 1, "KLSX", klsx_lat, klsx_lon, "a#b"), false);
        char tiny[64];
        strcpy(tiny, "sentinel");
        check_bool("url: too small buffer -> false", radar_wms_frame_url(tiny, sizeof(tiny), 1, "KLSX", klsx_lat, klsx_lon, NULL), false);
        check_bool("url: too small buffer -> empty", tiny[0] == '\0', true);
    }

    /* -- caps URL / layer name ------------------------------------------------------ */
    {
        check_bool("caps: KLSX builds", radar_wms_caps_url(url, sizeof(url), "KLSX"), true);
        check_str("caps: KLSX url", url,
                  "https://opengeo.ncep.noaa.gov/geoserver/klsx/ows?service=WMS&version=1.3.0&request=GetCapabilities");
        check_bool("caps: NORTHEAST builds", radar_wms_caps_url(url, sizeof(url), "NORTHEAST"), true);
        check_str("caps: NORTHEAST url uses conus", url,
                  "https://opengeo.ncep.noaa.gov/geoserver/conus/ows?service=WMS&version=1.3.0&request=GetCapabilities");
        check_bool("caps: ALASKA builds", radar_wms_caps_url(url, sizeof(url), "ALASKA"), true);
        check_contains("caps: ALASKA url uses alaska", url, "/geoserver/alaska/ows?", true);
        check_bool("caps: bad token", radar_wms_caps_url(url, sizeof(url), "KTLX_loop.gif?"), false);
        check_bool("caps: empty token", radar_wms_caps_url(url, sizeof(url), ""), false);

        char name[48];
        check_bool("layer: KLSX builds", radar_wms_layer_name(name, sizeof(name), "KLSX"), true);
        check_str("layer: KLSX", name, "klsx_sr_bref");
        check_bool("layer: HAWAII builds", radar_wms_layer_name(name, sizeof(name), "HAWAII"), true);
        check_str("layer: HAWAII", name, "hawaii_bref_qcd");
        check_bool("layer: bad token", radar_wms_layer_name(name, sizeof(name), "klsx"), false);
    }

    /* -- parser: synthetic --------------------------------------------------------- */
    {
        size_t len = strlen(synthetic_caps);
        int n = radar_wms_parse_times(synthetic_caps, len, "klsx_sr_bref", times, RADAR_WMS_TIMES_MAX);
        check_int("parse: target layer count", n, 5);
        if (n == 5) {
            check_str("parse: [0] newest", times[0], "2026-08-17T22:46:06.000Z");
            check_str("parse: [1]", times[1], "2026-08-17T22:41:13.000Z");
            check_str("parse: [2]", times[2], "2026-08-17T21:03:07.000Z");
            check_str("parse: [3]", times[3], "2026-08-17T20:54:18.000Z");
            check_str("parse: [4] oldest", times[4], "2026-08-17T20:50:22.000Z");
        }
        n = radar_wms_parse_times(synthetic_caps, len, "klsx_sr_bref", times, 3);
        check_int("parse: max_out 3 of 5", n, 3);
        if (n == 3) {
            check_str("parse: max_out [0] newest", times[0], "2026-08-17T22:46:06.000Z");
            check_str("parse: max_out [2] third newest", times[2], "2026-08-17T21:03:07.000Z");
        }
        n = radar_wms_parse_times(synthetic_caps, len, "klsx_bdhc", times, RADAR_WMS_TIMES_MAX);
        check_int("parse: decoy layer (elevation dim skipped) count", n, 3);
        if (n == 3) check_str("parse: decoy newest", times[0], "2026-01-01T00:20:00.000Z");
        n = radar_wms_parse_times(synthetic_caps, len, "klsx_sr_bvel", times, RADAR_WMS_TIMES_MAX);
        check_int("parse: single-entry layer", n, 1);
        if (n == 1) check_str("parse: single entry", times[0], "2030-01-01T00:00:00.000Z");
        check_int("parse: layer with only elevation dim -> 0",
                  radar_wms_parse_times(synthetic_caps, len, "klsx_boha", times, RADAR_WMS_TIMES_MAX), 0);
        check_int("parse: interval form -> 0",
                  radar_wms_parse_times(synthetic_caps, len, "klsx_interval", times, RADAR_WMS_TIMES_MAX), 0);
        check_int("parse: empty list -> 0",
                  radar_wms_parse_times(synthetic_caps, len, "klsx_empty", times, RADAR_WMS_TIMES_MAX), 0);
        check_int("parse: over-long entry -> 0",
                  radar_wms_parse_times(synthetic_caps, len, "klsx_long", times, RADAR_WMS_TIMES_MAX), 0);
        check_int("parse: missing layer -> 0",
                  radar_wms_parse_times(synthetic_caps, len, "klsx_nope", times, RADAR_WMS_TIMES_MAX), 0);
        check_int("parse: prefix of a name is not a match -> 0",
                  radar_wms_parse_times(synthetic_caps, len, "klsx_sr", times, RADAR_WMS_TIMES_MAX), 0);
        n = radar_wms_parse_times(synthetic_caps, len, "klsx_ws", times, RADAR_WMS_TIMES_MAX);
        check_int("parse: whitespace-padded list count", n, 2);
        if (n == 2) {
            check_str("parse: whitespace trimmed [0]", times[0], "2026-01-01T00:10:00Z");
            check_str("parse: whitespace trimmed [1]", times[1], "2026-01-01T00:00:00Z");
        }
        /* xml_len bound: truncate the body before the target's dimension closes */
        const char *cut = strstr(synthetic_caps, "2026-08-17T22:46:06.000Z</Dimension>");
        check_int("parse: truncated body (no </Dimension> in range) -> 0",
                  radar_wms_parse_times(synthetic_caps, (size_t)(cut - synthetic_caps) + 10, "klsx_sr_bref", times, RADAR_WMS_TIMES_MAX), 0);
        check_int("parse: max_out 0 -> 0",
                  radar_wms_parse_times(synthetic_caps, len, "klsx_sr_bref", times, 0), 0);
        check_int("parse: NULL xml -> 0",
                  radar_wms_parse_times(NULL, 0, "klsx_sr_bref", times, RADAR_WMS_TIMES_MAX), 0);
    }

    /* -- parser: real fixtures (captured 2026-08-17) --------------------------------- */
    {
        static const struct { const char *file; const char *layer; int min; } fx[] = {
            { "klsx_caps.xml",  "klsx_sr_bref",   20 },
            { "conus_caps.xml", "conus_bref_qcd", 40 },
        };
        for (size_t i = 0; i < 2; i++) {
            size_t len = 0;
            char *body = read_fixture(fx[i].file, &len);
            char label[96];
            snprintf(label, sizeof(label), "fixture: %s readable", fx[i].file);
            check_bool(label, body != NULL, true);
            if (!body) continue;
            /* Ask for more than the ring holds so the count reflects the document. */
            char big[64][RADAR_WMS_TIME_MAX];
            int n = radar_wms_parse_times(body, len, fx[i].layer, big, 64);
            snprintf(label, sizeof(label), "fixture: %s count >= %d", fx[i].file, fx[i].min);
            check_bool(label, n >= fx[i].min, true);
            printf("    count=%d newest=%s oldest=%s\n", n, n > 0 ? big[0] : "-", n > 0 ? big[n - 1] : "-");
            if (n >= 2) {
                snprintf(label, sizeof(label), "fixture: %s [0] > [1] (newest first)", fx[i].file);
                check_bool(label, strcmp(big[0], big[1]) > 0, true);
                int shape_ok = 1;
                for (int k = 0; k < n; k++) {
                    size_t sl = strlen(big[k]);
                    if (!(sl == 24 || sl == 20) || big[k][sl - 1] != 'Z') shape_ok = 0;
                    if (k > 0 && strcmp(big[k - 1], big[k]) <= 0) shape_ok = 0;
                }
                snprintf(label, sizeof(label), "fixture: %s every stamp 24/20 chars, ends Z, descending", fx[i].file);
                check_int(label, shape_ok, 1);
            }
            /* ring-sized request: exactly RADAR_WMS_TIMES_MAX newest */
            n = radar_wms_parse_times(body, len, fx[i].layer, times, RADAR_WMS_TIMES_MAX);
            snprintf(label, sizeof(label), "fixture: %s ring-sized count", fx[i].file);
            check_int(label, n, RADAR_WMS_TIMES_MAX);
            if (n == RADAR_WMS_TIMES_MAX) {
                snprintf(label, sizeof(label), "fixture: %s ring [0] == doc newest", fx[i].file);
                check_str(label, times[0], big[0]);
            }
            /* the decoy layers in the real klsx doc must not be picked up */
            if (i == 0) {
                n = radar_wms_parse_times(body, len, "klsx_bdsa", big, 64);
                check_int("fixture: klsx_bdsa has its own 13-entry list", n, 13);
            }
            free(body);
        }
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
