/* Host test for main/rainviewer.h -- the pure decisions behind the worldwide
 * radar source (RainViewer tiles over a NASA GIBS coastline basemap): zoom and
 * palette validation, the Web-Mercator window, the tile range and wrap, the
 * tile and basemap URL shapes, the weather-maps.json parse, and the provider
 * rule (which needs main/radar_sites.c for the nearest WSR-88D site). Header
 * only, no ESP-IDF dependency; assert-style like test/host/test_radar_wms.c.
 * screen_size() comes from main/screen_geom.c, whose default is 720. */
#include "rainviewer.h"
#include "radar_sites.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void check_int(const char *label, int got, int expect) {
    printf("%-64s got=%-8d expect=%-8d %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-64s got=%-8s expect=%-8s %s\n", label,
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
    printf("%-64s got=%-8s expect=%-8s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

/* Pull the four BBOX terms out of a built GetMap URL. */
static bool parse_bbox(const char *url, long *minx, long *miny, long *maxx, long *maxy) {
    const char *b = (url != NULL) ? strstr(url, "BBOX=") : NULL;
    if (b == NULL) return false;
    return sscanf(b + 5, "%ld,%ld,%ld,%ld", minx, miny, maxx, maxy) == 4;
}

/* GIBS renders a GetMap at a very coarse level whenever the vertical
 * metres-per-pixel is coarser than the horizontal by more than about
 * 0.001 m/px, so every built URL must satisfy resx >= resy, which in whole
 * metres is (maxx-minx)*HEIGHT >= (maxy-miny)*WIDTH. The builder widens x by
 * whole metres to get there, at most 8 of them (one step adds HEIGHT to the
 * left-hand side). */
static void check_aspect(const char *label, const char *url) {
    long minx = 0, miny = 0, maxx = 0, maxy = 0;
    int W = 0, H = 0;
    const char *s = (url != NULL) ? strstr(url, "WIDTH=") : NULL;
    char lbl[96];
    bool got = parse_bbox(url, &minx, &miny, &maxx, &maxy) && s != NULL &&
               sscanf(s + 6, "%d&HEIGHT=%d", &W, &H) == 2 && W > 0 && H > 0;
    snprintf(lbl, sizeof(lbl), "%s: geometry parsed", label);
    check_bool(lbl, got, true);
    if (!got) return;
    long long cross = (long long)(maxx - minx) * H - (long long)(maxy - miny) * W;
    snprintf(lbl, sizeof(lbl), "%s: x pixels are not coarser than y", label);
    check_bool(lbl, cross >= 0, true);
    snprintf(lbl, sizeof(lbl), "%s: widened by at most 8 m", label);
    check_bool(lbl, cross <= 8LL * (long long)H, true);
}

/* A trimmed but real-shaped weather-maps.json: host, radar.past OLDEST FIRST,
 * plus a nowcast array and a satellite block the parser must ignore. */
static const char *k_maps_json =
    "{\"version\":\"2.0\",\"generated\":1756900800,"
    "\"host\":\"https://tilecache.rainviewer.com\","
    "\"radar\":{\"past\":["
    "{\"time\":1756897200,\"path\":\"/v2/radar/1756897200\"},"
    "{\"time\":1756897800,\"path\":\"/v2/radar/1756897800\"},"
    "{\"time\":1756898400,\"path\":\"/v2/radar/1756898400\"},"
    "{\"time\":1756899000,\"path\":\"/v2/radar/1756899000\"}],"
    "\"nowcast\":[{\"time\":1756899600,\"path\":\"/v2/radar/nowcast_x\"}]},"
    "\"satellite\":{\"infrared\":[{\"time\":1756898000,\"path\":\"/v2/satellite/x\"}]}}";

int main(void)
{
    /* ---- zoom and palette ---- */
    check_int("zoom clamp: 3 -> 4",  rainviewer_zoom_clamp(3), 4);
    check_int("zoom clamp: 5 -> 5",  rainviewer_zoom_clamp(5), 5);
    check_int("zoom clamp: 9 -> 7",  rainviewer_zoom_clamp(9), 7);
    check_bool("palette 1 valid",  rainviewer_palette_ok(1), true);
    check_bool("palette 2 valid",  rainviewer_palette_ok(2), true);
    check_bool("palette 3 invalid", rainviewer_palette_ok(3), false);
    check_bool("palette 4 valid",  rainviewer_palette_ok(4), true);
    check_bool("palette 6 valid",  rainviewer_palette_ok(6), true);
    check_bool("palette 8 valid",  rainviewer_palette_ok(8), true);
    check_bool("palette 0 invalid", rainviewer_palette_ok(0), false);

    /* ---- window: the centre pixel lands in the middle of the window ---- */
    rainviewer_win_t w;
    rainviewer_window(0.0f, 0.0f, 5, 720, &w);
    check_int("z5 world px", (int)w.world_px, 512 << 5);
    check_int("z5 (0,0) ox", (int)w.ox, (512 << 5) / 2 - 360);
    check_int("z5 (0,0) oy", (int)w.oy, (512 << 5) / 2 - 360);
    rainviewer_window(0.0f, 180.0f, 5, 720, &w);
    check_int("z5 lon 180 ox", (int)w.ox, (512 << 5) - 360);

    /* ---- tile range: 720 px over 512 px tiles is 2 or 3 per axis ---- */
    rainviewer_range_t r;
    /* origin exactly on a tile boundary -> 720 px spans tiles 0 and 1 */
    w.ox = 0; w.oy = 0; w.win_px = 720; w.world_px = 512 << 5; w.zoom = 5;
    rainviewer_tile_range(&w, &r);
    check_int("aligned window x tiles", r.x1 - r.x0 + 1, 2);
    check_int("aligned window y tiles", r.y1 - r.y0 + 1, 2);
    /* origin mid-tile: window covers pixels [300, 1019], still inside tiles
     * 0 and 1 (tile 1 runs [512, 1023]) -- 2 tiles, not 3. The brief's own
     * next two checks (x0=0, x1=1) already force this count; a "3" here would
     * be unsatisfiable simultaneously with those, so this is corrected from
     * the task brief's "3" (see task-2-report.md). */
    w.ox = 300; w.oy = 300;
    rainviewer_tile_range(&w, &r);
    check_int("offset window x tiles", r.x1 - r.x0 + 1, 2);
    check_int("offset window y tiles", r.y1 - r.y0 + 1, 2);
    check_int("offset window x0", r.x0, 0);
    check_int("offset window x1", r.x1, 1);   /* 300+719 = 1019 -> tile 1 */
    /* a negative origin (antimeridian) still floors correctly */
    w.ox = -100; w.oy = 100;
    rainviewer_tile_range(&w, &r);
    check_int("negative origin x0", r.x0, -1);
    check_int("negative origin x1", r.x1, 1);
    /* y is clamped to the world, x is not (it wraps) */
    w.ox = 0; w.oy = -400;
    rainviewer_tile_range(&w, &r);
    check_int("north-clamped y0", r.y0, 0);
    /* every zoom in range produces 2..3 tiles per axis for a 720 px window */
    for (uint8_t z = 4; z <= 7; z++) {
        rainviewer_window(-41.29f, 174.78f, z, 720, &w);
        rainviewer_tile_range(&w, &r);
        char lbl[80];
        snprintf(lbl, sizeof(lbl), "z%u Wellington x tiles in 2..3", (unsigned)z);
        int nx = r.x1 - r.x0 + 1;
        check_bool(lbl, nx >= 2 && nx <= 3, true);
        snprintf(lbl, sizeof(lbl), "z%u Wellington y tiles in 2..3", (unsigned)z);
        int ny = r.y1 - r.y0 + 1;
        check_bool(lbl, ny >= 2 && ny <= 3, true);
    }

    /* ---- x wrap ---- */
    check_int("wrap -1 at z4", rainviewer_wrap_x(-1, 4), 15);
    check_int("wrap 16 at z4", rainviewer_wrap_x(16, 4), 0);
    check_int("wrap 3 at z5",  rainviewer_wrap_x(3, 5), 3);

    /* ---- tile URL ---- */
    char url[RAINVIEWER_URL_MAX];
    check_bool("tile url built",
               rainviewer_tile_url(url, sizeof(url), "https://tilecache.rainviewer.com",
                                   "/v2/radar/1756899000", 5, 30, 19, 6), true);
    check_str("tile url shape",
              url,
              "https://tilecache.rainviewer.com/v2/radar/1756899000/512/5/30/19/6/1_1.png");
    check_bool("tile url rejects a non-https host",
               rainviewer_tile_url(url, sizeof(url), "http://evil.example",
                                   "/v2/radar/1", 5, 0, 0, 6), false);
    check_bool("tile url rejects a path without a leading slash",
               rainviewer_tile_url(url, sizeof(url), "https://tilecache.rainviewer.com",
                                   "v2/radar/1", 5, 0, 0, 6), false);
    check_bool("tile url rejects a query character in the path",
               rainviewer_tile_url(url, sizeof(url), "https://tilecache.rainviewer.com",
                                   "/v2/radar/1?x=1", 5, 0, 0, 6), false);
    check_bool("tile url rejects an unknown palette",
               rainviewer_tile_url(url, sizeof(url), "https://tilecache.rainviewer.com",
                                   "/v2/radar/1", 5, 0, 0, 3), false);
    check_bool("tile url rejects a tiny buffer",
               rainviewer_tile_url(url, 20, "https://tilecache.rainviewer.com",
                                   "/v2/radar/1", 5, 0, 0, 6), false);
    /* The host arrives in a remote JSON document, so it is pinned to the
     * publisher's own domain: userinfo, a look-alike domain and a host that
     * hides the real name in a path segment all have to be refused. */
    check_bool("tile url rejects a userinfo host",
               rainviewer_tile_url(url, sizeof(url),
                                   "https://tilecache.rainviewer.com@evil.example",
                                   "/v2/radar/1", 5, 0, 0, 6), false);
    check_bool("tile url rejects a host outside rainviewer.com",
               rainviewer_tile_url(url, sizeof(url), "https://tilecache.example.com",
                                   "/v2/radar/1", 5, 0, 0, 6), false);
    check_bool("tile url rejects rainviewer.com in the middle of a host",
               rainviewer_tile_url(url, sizeof(url), "https://rainviewer.com.evil.example",
                                   "/v2/radar/1", 5, 0, 0, 6), false);
    check_bool("tile url rejects rainviewer.com in a path position",
               rainviewer_tile_url(url, sizeof(url), "https://evil.example/.rainviewer.com",
                                   "/v2/radar/1", 5, 0, 0, 6), false);
    check_bool("tile url accepts another rainviewer.com subdomain",
               rainviewer_tile_url(url, sizeof(url), "https://tiles.rainviewer.com",
                                   "/v2/radar/1", 5, 0, 0, 6), true);

    /* ---- basemap URL ----
     * Wellington at z6 sits fully inside the world, so the request is the whole
     * window and the picture fills the panel. */
    rainviewer_window(-41.29f, 174.78f, 6, 720, &w);
    check_bool("basemap url built", rainviewer_basemap_url(url, sizeof(url), &w), true);
    check_contains("basemap uses the GIBS 3857 endpoint", url,
                   "gibs.earthdata.nasa.gov/wms/epsg3857/best/wms.cgi", true);
    check_contains("basemap layers", url,
                   "LAYERS=Coastlines_15m,Reference_Features_15m", true);
    check_contains("basemap crs", url, "CRS=EPSG:3857", true);
    check_contains("basemap size", url, "WIDTH=720&HEIGHT=720", true);
    check_contains("basemap black background", url, "BGCOLOR=0x000000", true);
    check_contains("basemap carries no TIME", url, "TIME=", false);
    check_aspect("inside z6", url);
    {
        rainviewer_basemap_rect_t br;
        check_bool("inside window has a rect", rainviewer_basemap_rect(&w, &br), true);
        check_int("inside rect x", (int)br.x, 0);
        check_int("inside rect y", (int)br.y, 0);
        check_int("inside rect w", (int)br.w, 720);
        check_int("inside rect h", (int)br.h, 720);
    }

    /* ---- basemap box clamped to the world edge ----
     * GIBS renders a GetMap whose BBOX runs past +-20037508 m at a very coarse
     * level (20 px coastline blocks, whole islands missing), so the basemap
     * request is clamped to the world and WIDTH/HEIGHT shrink with it, leaving
     * the window's metres-per-pixel unchanged. rainviewer_basemap_rect() says
     * where that smaller picture belongs inside the panel-sized window.
     * Wellington z5 720 px is the live case that showed the defect. */
    rainviewer_window(-41.29f, 174.78f, 5, 720, &w);
    check_bool("z5 Wellington basemap url built",
               rainviewer_basemap_url(url, sizeof(url), &w), true);
    check_contains("z5 Wellington WIDTH shrinks with the clamp", url,
                   "WIDTH=598&HEIGHT=720", true);
    check_aspect("z5 Wellington (east clamped)", url);
    {
        long minx = 0, miny = 0, maxx = 0, maxy = 0;
        rainviewer_basemap_rect_t br;
        check_bool("z5 Wellington bbox parsed",
                   parse_bbox(url, &minx, &miny, &maxx, &maxy), true);
        check_int("z5 Wellington maxx clamped to the world edge", (int)maxx, 20037508);
        check_bool("z5 Wellington minx left where it was", minx > -20037508L && minx < maxx, true);
        check_bool("z5 Wellington rect", rainviewer_basemap_rect(&w, &br), true);
        check_int("z5 Wellington rect x", (int)br.x, 0);
        check_int("z5 Wellington rect y", (int)br.y, 0);
        check_int("z5 Wellington rect w", (int)br.w, 598);
        check_int("z5 Wellington rect h", (int)br.h, 720);
    }

    /* the same overrun on the west side: the picture is pasted inboard so its
     * right edge lands on the window edge. */
    rainviewer_window(-41.29f, -179.5f, 5, 720, &w);
    check_bool("west-overrun basemap url built",
               rainviewer_basemap_url(url, sizeof(url), &w), true);
    check_aspect("west clamped", url);
    {
        long minx = 0, miny = 0, maxx = 0, maxy = 0;
        rainviewer_basemap_rect_t br;
        char want[32];
        int wn;
        check_bool("west-overrun bbox parsed",
                   parse_bbox(url, &minx, &miny, &maxx, &maxy), true);
        check_int("west-overrun minx clamped to the world edge", (int)minx, -20037508);
        check_bool("west-overrun rect", rainviewer_basemap_rect(&w, &br), true);
        check_bool("west-overrun picture is pasted inboard", br.x > 0, true);
        check_int("west-overrun picture ends at the window edge", (int)(br.x + br.w), 720);
        check_int("west-overrun rect y", (int)br.y, 0);
        check_int("west-overrun rect h", (int)br.h, 720);
        wn = snprintf(want, sizeof(want), "WIDTH=%d&HEIGHT=%d", (int)br.w, (int)br.h);
        check_bool("west-overrun size needle fits", wn > 0 && (size_t)wn < sizeof(want), true);
        check_contains("west-overrun WIDTH follows the rect", url, want, true);
    }

    /* a polar window overruns on y instead: HEIGHT shrinks and the picture is
     * pasted down from the top of the panel. */
    rainviewer_window(84.0f, 0.0f, 4, 720, &w);
    check_bool("polar basemap url built", rainviewer_basemap_url(url, sizeof(url), &w), true);
    check_aspect("polar (y clamped)", url);
    {
        long minx = 0, miny = 0, maxx = 0, maxy = 0;
        rainviewer_basemap_rect_t br;
        char want[32];
        int pn2;
        check_bool("polar bbox parsed", parse_bbox(url, &minx, &miny, &maxx, &maxy), true);
        check_int("polar maxy clamped to the world edge", (int)maxy, 20037508);
        check_bool("polar rect", rainviewer_basemap_rect(&w, &br), true);
        check_bool("polar HEIGHT shrinks below the panel", br.h < 720, true);
        check_bool("polar picture is pasted down from the top", br.y > 0, true);
        check_int("polar rect x", (int)br.x, 0);
        check_int("polar rect w", (int)br.w, 720);
        pn2 = snprintf(want, sizeof(want), "WIDTH=%d&HEIGHT=%d", (int)br.w, (int)br.h);
        check_bool("polar size needle fits", pn2 > 0 && (size_t)pn2 < sizeof(want), true);
        check_contains("polar HEIGHT follows the rect", url, want, true);
    }

    /* ---- date-line crossing: the WINDOW is not shifted, the BASEMAP BOX is
     * clamped ---- Unlike clouds_bbox() in clouds_wms.h, the RainViewer window
     * origin is left exactly where the raw world-pixel math puts it: tile x is
     * what wraps (modulo 2^z), so the rain still aligns with the map and the
     * location marker stays at the frame centre. Only the GIBS basemap request
     * built from that window is clamped to the +-20037508 m world edge, with
     * the picture pasted at its offset, because GIBS renders an overrunning box
     * at a very coarse level. Expected ox/tile values computed independently in
     * Python from the same merc_x/half_m/round formula rainviewer_window() uses
     * (see task-2-report.md for the derivation). */
    rainviewer_window(0.0f, 179.9f, 5, 720, &w);
    check_int("z5 lon 179.9 ox (unshifted, near the world's east edge)", (int)w.ox, 16019);
    rainviewer_tile_range(&w, &r);
    check_int("lon 179.9 raw x1 runs past the last valid tile (31)", r.x1, 32);
    check_int("lon 179.9 x1 wraps to tile 0", rainviewer_wrap_x(r.x1, 5), 0);
    check_bool("date-line basemap url built (east edge)",
               rainviewer_basemap_url(url, sizeof(url), &w), true);
    {
        long minx = 0, miny = 0, maxx = 0, maxy = 0;
        rainviewer_basemap_rect_t br;
        check_bool("basemap bbox parsed", parse_bbox(url, &minx, &miny, &maxx, &maxy), true);
        check_int("date-line basemap bbox maxx clamped to the world edge",
                  (int)maxx, 20037508);
        check_bool("date-line rect", rainviewer_basemap_rect(&w, &br), true);
        check_int("date-line picture starts at the window edge", (int)br.x, 0);
        check_bool("date-line picture stops short of the window edge", br.w < 720, true);
    }

    rainviewer_window(0.0f, -179.9f, 5, 720, &w);
    check_int("z5 lon -179.9 ox (unshifted, negative)", (int)w.ox, -355);
    rainviewer_tile_range(&w, &r);
    check_int("lon -179.9 raw x0 is negative", r.x0, -1);
    check_int("lon -179.9 x0 wraps to tile 31", rainviewer_wrap_x(r.x0, 5), 31);

    /* ---- provider rule ---- */
    /* Wellington NZ: nowhere near a WSR-88D -> worldwide */
    check_bool("Wellington auto -> RainViewer",
               rainviewer_selected(1, "", -41.29f, 174.78f), true);
    /* New York: KOKX is close -> the US source stays */
    check_bool("New York auto -> US",
               rainviewer_selected(1, "", 40.71f, -74.01f), false);
    /* an explicit US area always wins over the automatic rule */
    check_bool("explicit token -> US even from Wellington",
               rainviewer_selected(1, "KOKX", -41.29f, 174.78f), false);
    /* the WORLD area token forces worldwide, US location or not */
    check_bool("WORLD token -> RainViewer in New York",
               rainviewer_selected(1, RAINVIEWER_TOKEN_WORLD, 40.71f, -74.01f), true);
    check_bool("WORLD token -> RainViewer at Wellington",
               rainviewer_selected(0, RAINVIEWER_TOKEN_WORLD, -41.29f, 174.78f), true);
    /* map style 3 is gone: a stale 3 behaves like any other US style */
    check_bool("style 3 in New York -> US (no style rule left)",
               rainviewer_selected(3, "", 40.71f, -74.01f), false);
    /* no location set: unchanged US behaviour (the national view) */
    check_bool("no location -> US", rainviewer_selected(1, "", 0.0f, 0.0f), false);
    /* Honolulu is inside coverage (PHKI/PHMO), Reykjavik is not */
    check_bool("Honolulu auto -> US", rainviewer_selected(0, "", 21.31f, -157.86f), false);
    check_bool("Reykjavik auto -> RainViewer", rainviewer_selected(0, "", 64.15f, -21.94f), true);

    /* ---- weather-maps.json ---- */
    char host[RAINVIEWER_HOST_MAX];
    rainviewer_frame_t fr[RAINVIEWER_MAX_FRAMES];
    int n = rainviewer_parse_maps(k_maps_json, strlen(k_maps_json),
                                  host, sizeof(host), fr, RAINVIEWER_MAX_FRAMES);
    check_int("maps: four past frames", n, 4);
    check_str("maps: host", host, "https://tilecache.rainviewer.com");
    check_int("maps: [0] is the NEWEST time", (int)fr[0].time, 1756899000);
    check_str("maps: [0] path", fr[0].path, "/v2/radar/1756899000");
    check_int("maps: [3] is the oldest time", (int)fr[3].time, 1756897200);
    /* a cap smaller than the list keeps the NEWEST entries */
    n = rainviewer_parse_maps(k_maps_json, strlen(k_maps_json), host, sizeof(host), fr, 2);
    check_int("maps: capped count", n, 2);
    check_int("maps: capped [0] newest", (int)fr[0].time, 1756899000);
    check_int("maps: capped [1] second newest", (int)fr[1].time, 1756898400);
    /* garbage in, zero out, and nothing written */
    check_int("maps: not JSON", rainviewer_parse_maps("nonsense", 8, host, sizeof(host),
                                                      fr, RAINVIEWER_MAX_FRAMES), 0);
    {
        /* strlen(), not hand-counted literals: a wrong hand-counted length
         * truncates the JSON before cJSON ever sees the real content, so the
         * parse fails as malformed JSON and never reaches the branch the
         * assertion claims to exercise (review finding, fix round 1). */
        static const char *no_radar = "{\"host\":\"https://x.example\"}";
        static const char *bad_host = "{\"host\":\"ftp://x\",\"radar\":{\"past\":"
                                      "[{\"time\":1,\"path\":\"/a\"}]}}";
        check_int("maps: no radar block",
                  rainviewer_parse_maps(no_radar, strlen(no_radar),
                                        host, sizeof(host), fr, RAINVIEWER_MAX_FRAMES), 0);
        check_int("maps: host without https rejected",
                  rainviewer_parse_maps(bad_host, strlen(bad_host),
                                        host, sizeof(host), fr, RAINVIEWER_MAX_FRAMES), 0);
    }

    /* an empty past list is not malformed, just empty: 0 frames, and the
     * contract is host is NOT written (the copy only happens once count > 0,
     * same as every other 0-returning path above) -- proven by seeding a
     * sentinel and checking it survives the call untouched. */
    {
        static const char *empty_past =
            "{\"host\":\"https://tilecache.rainviewer.com\",\"radar\":{\"past\":[]}}";
        strcpy(host, "SENTINEL");
        check_int("maps: empty radar.past returns zero frames",
                  rainviewer_parse_maps(empty_past, strlen(empty_past),
                                        host, sizeof(host), fr, RAINVIEWER_MAX_FRAMES), 0);
        check_str("maps: empty radar.past leaves host untouched", host, "SENTINEL");
    }

    /* a host at or past RAINVIEWER_HOST_MAX is rejected outright (0 frames,
     * nothing written) -- both the host_sz check and rainviewer_host_ok()'s
     * own fragment_ok() length check would catch this; either is fine, the
     * observable contract is the same 0. */
    {
        char xs[80];
        memset(xs, 'x', sizeof(xs) - 1);
        xs[sizeof(xs) - 1] = '\0';
        char long_host_json[512];
        int hn = snprintf(long_host_json, sizeof(long_host_json),
                          "{\"host\":\"https://%s.example\",\"radar\":{\"past\":"
                          "[{\"time\":1,\"path\":\"/a\"}]}}", xs);
        check_bool("long-host fixture fits its buffer", hn > 0 && (size_t)hn < sizeof(long_host_json), true);
        check_int("maps: host longer than RAINVIEWER_HOST_MAX rejected",
                  rainviewer_parse_maps(long_host_json, strlen(long_host_json),
                                        host, sizeof(host), fr, RAINVIEWER_MAX_FRAMES), 0);
    }

    /* a path at or past RAINVIEWER_PATH_MAX fails rainviewer_path_ok() for
     * just that entry, which is skipped rather than aborting the whole
     * document; with only one entry in this fixture that still nets 0 frames
     * and (count stays 0) an untouched host. */
    {
        char ps[100];
        memset(ps, 'p', sizeof(ps) - 1);
        ps[sizeof(ps) - 1] = '\0';
        char long_path_json[512];
        int pn = snprintf(long_path_json, sizeof(long_path_json),
                          "{\"host\":\"https://tilecache.rainviewer.com\",\"radar\":{\"past\":"
                          "[{\"time\":1,\"path\":\"/%s\"}]}}", ps);
        check_bool("long-path fixture fits its buffer", pn > 0 && (size_t)pn < sizeof(long_path_json), true);
        strcpy(host, "SENTINEL");
        check_int("maps: path longer than RAINVIEWER_PATH_MAX skips that entry",
                  rainviewer_parse_maps(long_path_json, strlen(long_path_json),
                                        host, sizeof(host), fr, RAINVIEWER_MAX_FRAMES), 0);
        check_str("maps: path-too-long entry leaves host untouched (count stayed 0)", host, "SENTINEL");
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
