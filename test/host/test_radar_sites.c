/* Host test for main/radar_sites.c -- nearest WSR-88D site lookup used to
 * resolve an empty radar_token into a RIDGE image token. No ESP-IDF
 * dependency (only math.h); assert-style like test/host/test_poll_backoff.c. */
#include "radar_sites.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void check_str(const char *label, const char *got, const char *expect) {
    int ok = (got != NULL) && (strcmp(got, expect) == 0);
    printf("%-60s got=%-8s expect=%-8s %s\n", label,
           got ? got : "(null)", expect, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void check_not_str(const char *label, const char *got, const char *reject) {
    int ok = (got != NULL) && (strcmp(got, reject) != 0);
    printf("%-60s got=%-8s reject=%-8s %s\n", label,
           got ? got : "(null)", reject, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    /* -- a site resolves to itself from its own coordinates ------------------ */
    check_str("self: KTLX (Norman, OK)",        radar_site_nearest(35.33305f,  -97.27775f),  "KTLX");
    check_str("self: KABR (Aberdeen, SD)",      radar_site_nearest(45.45583f,  -98.41305f),  "KABR");
    check_str("self: KBOX (Boston, MA)",        radar_site_nearest(41.95577f,  -71.13686f),  "KBOX");
    check_str("self: PAEC (Nome, AK)",          radar_site_nearest(64.51139f, -165.29498f),  "PAEC");
    check_str("self: PHWA (South Shore, HI)",   radar_site_nearest(19.09500f, -155.56887f),  "PHWA");
    check_str("self: TJUA (San Juan, PR)",      radar_site_nearest(18.11566f,  -66.07817f),  "TJUA");
    check_str("self: PGUA (Guam, +lon)",        radar_site_nearest(13.45583f,  144.81112f),  "PGUA");

    /* -- a city resolves to the radar that actually serves it ---------------- */
    check_str("city: Oklahoma City -> KTLX",    radar_site_nearest(35.4676f,   -97.5164f),   "KTLX");
    check_str("city: Dallas -> KFWS",           radar_site_nearest(32.7767f,   -96.7970f),   "KFWS");
    check_str("city: Denver -> KFTG",           radar_site_nearest(39.7392f,  -104.9903f),   "KFTG");
    check_str("city: Seattle -> KATX",          radar_site_nearest(47.6062f,  -122.3321f),   "KATX");
    check_str("city: Miami -> KAMX",            radar_site_nearest(25.7617f,   -80.1918f),   "KAMX");
    check_str("city: Boston -> KBOX",           radar_site_nearest(42.3601f,   -71.0589f),   "KBOX");
    check_str("city: Honolulu -> PHMO",         radar_site_nearest(21.3069f,  -157.8583f),   "PHMO");

    /* -- longitude sign must be honoured -------------------------------------
     * KTLX's latitude with the longitude sign FLIPPED lands in western China.
     * Code that dropped the sign (or took |lon|) would answer KTLX; the real
     * nearest site to (35.33 N, 97.28 E) is Guam. */
    check_str("sign: +97.28E at KTLX latitude -> PGUA",
              radar_site_nearest(35.33305f, 97.27775f), "PGUA");
    check_not_str("sign: +97.28E must not answer KTLX",
                  radar_site_nearest(35.33305f, 97.27775f), "KTLX");

    /* -- lat/lon must not be swapped ----------------------------------------
     * Feeding KTLX's pair in the wrong order is an impossible position; any
     * answer is acceptable except the one a swap bug would produce. */
    check_not_str("swap: (lon,lat) argument order must not answer KTLX",
                  radar_site_nearest(-97.27775f, 35.33305f), "KTLX");

    /* -- antimeridian wrap ---------------------------------------------------
     * (52 N, 179 E) is ~19 degrees of longitude from Bethel, AK across the
     * dateline. Without the +/-360 wrap every US site looks ~340 degrees away
     * and Guam wins by default, so this pins the wrap. */
    check_str("wrap: (52N,179E) -> PABC (Bethel, AK)",
              radar_site_nearest(52.0f, 179.0f), "PABC");

    /* -- extreme coordinates still return a usable site ---------------------
     * At the poles the longitude scale collapses to ~0, so the pick is the
     * closest site by latitude alone: the northernmost (Fairbanks) and the
     * southernmost (Guam). No crash, no NULL, no out-of-table read. */
    check_str("extreme: north pole -> PAPD (northernmost)",
              radar_site_nearest(90.0f, 0.0f), "PAPD");
    check_str("extreme: south pole -> PGUA (southernmost)",
              radar_site_nearest(-90.0f, 0.0f), "PGUA");
    check_str("extreme: null island (0,0) -> TJUA",
              radar_site_nearest(0.0f, 0.0f), "TJUA");
    check_str("extreme: lon exactly -180 still resolves",
              radar_site_nearest(60.0f, -180.0f), "PAEC");
    check_str("extreme: lon exactly +180 matches -180",
              radar_site_nearest(60.0f, 180.0f), "PAEC");

    /* -- every answer is a 4-character NUL-terminated id --------------------- */
    {
        const char *id = radar_site_nearest(35.4676f, -97.5164f);
        int ok = (id != NULL) && (strlen(id) == 4);
        printf("%-60s len=%-6u %s\n", "shape: id is 4 chars",
               id ? (unsigned)strlen(id) : 0u, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
