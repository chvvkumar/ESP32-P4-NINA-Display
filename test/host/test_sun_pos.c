/* Host test for main/sun_pos.h -- single-precision solar elevation.
 * Epoch values are derived from days-from-civil: 2026-01-01 is day 20454
 * since 1970-01-01 (2026-08-18 is 20683, and 2026-08-18 is day-of-year 230).
 * Header-only, no ESP-IDF dependency, assert-style like test_clouds_wms.c. */
#include "sun_pos.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void check_int(const char *label, long got, long expect) {
    printf("%-60s got=%-10ld expect=%-10ld %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

/* |got - expect| <= tol, printed in hundredths of a degree so no %f is used. */
static void check_near(const char *label, float got, float expect, float tol) {
    float d = got - expect;
    if (d < 0.0f) d = -d;
    bool ok = d <= tol;
    printf("%-60s got=%-10ld expect=%-10ld (x100) %s\n", label,
           (long)lroundf(got * 100.0f), (long)lroundf(expect * 100.0f),
           ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-60s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void) {
    /* -- day of year / hour split ----------------------------------------- */
    {
        int doy = 0; float hour = 0.0f;
        sun_utc_doy_hour(1787025600u, &doy, &hour);        /* 2026-08-18T04:00:00Z */
        check_int("doy: 2026-08-18 is day 230", doy, 230);
        check_near("hour: 04:00Z", hour, 4.0f, 0.001f);
        sun_utc_doy_hour(20454u * 86400u, &doy, &hour);    /* 2026-01-01T00:00:00Z */
        check_int("doy: 2026-01-01 is day 1", doy, 1);
        check_near("hour: 00:00Z", hour, 0.0f, 0.001f);
        sun_utc_doy_hour(1780617600u + 86399u, &doy, &hour);
        check_bool("hour: 23:59:59Z is under 24", hour < 24.0f, true);
        /* leap year: 2024-03-01 is day 61 (2024-01-01 is day 19723) */
        sun_utc_doy_hour((19723u + 60u) * 86400u, &doy, &hour);
        check_int("doy: 2024-03-01 (leap) is day 61", doy, 61);
        /* non-leap: 2026-03-01 is day 60 */
        sun_utc_doy_hour((20454u + 59u) * 86400u, &doy, &hour);
        check_int("doy: 2026-03-01 is day 60", doy, 60);
    }

    /* -- equator at the March equinox: sun overhead at local solar noon ----- */
    {
        /* 2026-03-20T12:00:00Z = day 20532 * 86400 + 43200 */
        float el = sun_elevation_deg(0.0f, 0.0f, 1774008000u);
        printf("    equator 2026-03-20T12:00Z el x100 = %ld\n", (long)lroundf(el * 100.0f));
        check_bool("equinox noon at (0,0) is near overhead", el > 86.0f && el <= 90.01f, true);
        /* 12 h later the same point is on the night side */
        float night = sun_elevation_deg(0.0f, 0.0f, 1774051200u);
        printf("    equator 2026-03-21T00:00Z el x100 = %ld\n", (long)lroundf(night * 100.0f));
        check_bool("equinox midnight at (0,0) is near nadir", night < -85.0f && night >= -90.01f, true);
    }

    /* -- New York, summer solstice, local solar noon ------------------------
     * lat 40.71, lon -74.01: solar noon is 12:00Z + 74.01/15 h = 16:56Z, and
     * the equation of time on 21 June is about -1.6 min, so 16:58Z. Peak
     * elevation is 90 - (lat - declination) = 90 - (40.71 - 23.44) = 72.73. */
    {
        float el = sun_elevation_deg(40.71f, -74.01f, 1782061080u);
        check_near("New York solstice solar noon = 72.7 deg", el, 72.73f, 1.5f);
    }

    /* -- Wellington NZ (-41.29, 174.78), the spec's day/night pair ----------
     * Solar noon is 12:00Z - 174.78/15 h = 00:21Z, declination on 3 Sep is
     * about +7.5, so the peak is 90 - (41.29 + 7.5) = 41.2 and the antipodal
     * hour angle gives about -55. */
    {
        float day   = sun_elevation_deg(-41.29f, 174.78f, 1788397200u);   /* 01:00Z */
        float night = sun_elevation_deg(-41.29f, 174.78f, 1788440400u);   /* 13:00Z */
        check_near("Wellington 2026-09-03T01:00Z (local afternoon)", day, 40.6f, 2.0f);
        check_near("Wellington 2026-09-03T13:00Z (local night)", night, -55.1f, 2.0f);
        check_bool("Wellington 01:00Z is above the 10 deg gate", day >= 10.0f, true);
        check_bool("Wellington 13:00Z is below the 10 deg gate", night < 10.0f, true);
    }

    /* -- sunrise within 2 degrees ------------------------------------------
     * Wellington 2026-09-03: cos(ha0) = -tan(lat)tan(decl) = 0.1157, so
     * ha0 = 83.36 deg = 5 h 33 m before solar noon (00:21Z) = 18:48Z on 09-02. */
    {
        float el = sun_elevation_deg(-41.29f, 174.78f, 1788374880u);
        check_near("Wellington sunrise 2026-09-02T18:48Z is at the horizon", el, 0.0f, 2.0f);
    }

    /* -- range and continuity ---------------------------------------------- */
    {
        bool in_range = true, crossed = false;
        float prev = sun_elevation_deg(51.5f, -0.13f, 1788307200u);
        for (uint32_t t = 1788307200u; t < 1788307200u + 86400u; t += 600u) {
            float el = sun_elevation_deg(51.5f, -0.13f, t);
            if (el < -90.01f || el > 90.01f) in_range = false;
            if ((prev < 0.0f) != (el < 0.0f)) crossed = true;
            prev = el;
        }
        check_bool("London: a whole day stays inside +-90", in_range, true);
        check_bool("London: a whole day crosses the horizon", crossed, true);
    }

    printf("\n%s (%d failures)\n", fails == 0 ? "ALL PASSED" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}
