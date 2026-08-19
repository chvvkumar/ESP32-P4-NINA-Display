/* Host test for main/adsb_geom.h -- the pure geometry behind the ADS-B page:
 * curved-earth elevation, great-circle distance/bearing, angular separation on
 * the sky, and the two screen projections (Sky Dome truncated stereographic,
 * Radar Scope linear). Header-only, no ESP-IDF dependency; assert-style like
 * test/host/test_clouds_wms.c.
 *
 * NOT covered here: main/adsb_client.c's JSON parser. It reaches tasks.h ->
 * freertos/event_groups.h, for which test/host/shims has no stand-in, and it
 * calls poll_loop_run/psram_task_ensure/http_fetch_text, so linking it would
 * need a new shim header plus four mocks. Parser field fallbacks are covered
 * by the on-device checks in the spec's section 8 instead.
 *
 * Expected values were computed in double precision with python (math.atan2 /
 * haversine over R = 6371000 m) and are asserted with a tolerance wide enough
 * for the float-only implementation.
 */
#include "adsb_geom.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

/* |got - expect| <= tol. Printed in millis so no %f rounding surprises. */
static void check_near(const char *label, float got, float expect, float tol)
{
    float d = got - expect;
    if (d < 0.0f) d = -d;
    int ok = (d <= tol);
    printf("%-62s got=%-12ld expect=%-12ld %s\n", label,
           (long)lroundf(got * 1000.0f), (long)lroundf(expect * 1000.0f),
           ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void check_true(const char *label, int got)
{
    printf("%-62s %s\n", label, got ? "OK" : "FAIL");
    if (!got) fails++;
}

int main(void)
{
    /* ---- angle wrap ---- */
    check_near("wrap360(-10) == 350", adsb_wrap360(-10.0f), 350.0f, 1e-3f);
    check_near("wrap360(370) == 10", adsb_wrap360(370.0f), 10.0f, 1e-3f);
    check_near("wrap360(0) == 0", adsb_wrap360(0.0f), 0.0f, 1e-3f);
    check_near("wrap180(350) == -10", adsb_wrap180(350.0f), -10.0f, 1e-3f);
    check_near("wrap180(190) == -170", adsb_wrap180(190.0f), -170.0f, 1e-3f);

    /* ---- elevation, curved earth (R = 6371 km, no refraction) ----
     * python: d=30*1852; h=35000*0.3048; degrees(atan2(h - d*d/(2*6371000), d))
     *       = 10.627861 deg                                                   */
    check_near("el(30nm, 35000ft, rx 0m) == 10.628",
               adsb_elevation_deg(30.0f, 35000.0f, 0.0f), 10.6279f, 0.01f);

    /* Receiver 300 m up sees the same aircraft slightly lower: 10.328722 deg */
    check_near("el(30nm, 35000ft, rx 300m) == 10.329",
               adsb_elevation_deg(30.0f, 35000.0f, 300.0f), 10.3287f, 0.01f);

    /* Overhead: zero ground distance, positive height => straight up. */
    check_near("el(0nm, 35000ft) == 90 (overhead)",
               adsb_elevation_deg(0.0f, 35000.0f, 0.0f), 90.0f, 1e-3f);

    /* Curvature bites at range: at 100 nm the earth has dropped 1.45 km, so
     * the curved answer (2.466089) sits well below flat trig (3.296742). */
    {
        float curved = adsb_elevation_deg(100.0f, 35000.0f, 0.0f);
        check_near("el(100nm, 35000ft) curved == 2.466", curved, 2.4661f, 0.01f);
        check_true("curvature lowers el at 100nm vs flat-earth 3.297",
                   curved < 3.2967f - 0.5f);
        /* ...and barely at all at 1 nm, where the drop is 0.15 m. */
        check_true("curvature negligible at 1nm",
                   adsb_elevation_deg(1.0f, 35000.0f, 0.0f) > 79.0f);
    }

    /* A low contact far out goes below the horizon. */
    check_true("el < 0 for 200nm at 5000ft",
               adsb_elevation_deg(200.0f, 5000.0f, 0.0f) < 0.0f);

    /* ---- haversine + bearing ----
     * python: 1 deg of latitude = 60.040457 nm, bearing due north = 0. */
    check_near("haversine 1 deg lat == 60.040 nm",
               adsb_haversine_nm(38.0f, -90.0f, 39.0f, -90.0f), 60.0405f, 0.02f);
    check_near("bearing due north == 0",
               adsb_bearing_deg(38.0f, -90.0f, 39.0f, -90.0f), 0.0f, 0.01f);
    check_near("haversine same point == 0",
               adsb_haversine_nm(38.0f, -90.0f, 38.0f, -90.0f), 0.0f, 1e-3f);

    /* Real contact from scratchpad/flights/aircraft.json (hex abb3ec) against a
     * receiver at 38,-90: python haversine 104.214640 nm, bearing 245.472253. */
    check_near("haversine rx(38,-90) -> ac(37.2627,-91.9842) == 104.215 nm",
               adsb_haversine_nm(38.0f, -90.0f, 37.262692f, -91.984226f),
               104.2146f, 0.05f);
    check_near("bearing rx(38,-90) -> ac(37.2627,-91.9842) == 245.472",
               adsb_bearing_deg(38.0f, -90.0f, 37.262692f, -91.984226f),
               245.4723f, 0.02f);

    /* Bearing is always wrapped into [0,360). */
    check_true("bearing due west in [0,360)",
               adsb_bearing_deg(38.0f, -90.0f, 38.0f, -91.0f) > 269.0f &&
               adsb_bearing_deg(38.0f, -90.0f, 38.0f, -91.0f) < 271.0f);

    /* ---- sky separation ---- */
    check_near("sep(0,90 vs 137,90) == 0 (both at zenith)",
               adsb_sep_deg(0.0f, 90.0f, 137.0f, 90.0f), 0.0f, 0.1f);
    check_near("sep identical directions == 0",
               adsb_sep_deg(212.0f, 34.0f, 212.0f, 34.0f), 0.0f, 0.1f);
    check_near("sep on horizon 90 deg apart == 90",
               adsb_sep_deg(0.0f, 0.0f, 90.0f, 0.0f), 90.0f, 0.05f);
    check_near("sep horizon to zenith == 90",
               adsb_sep_deg(0.0f, 0.0f, 0.0f, 90.0f), 90.0f, 0.05f);
    check_near("sep opposite horizons == 180",
               adsb_sep_deg(0.0f, 0.0f, 180.0f, 0.0f), 180.0f, 0.05f);
    check_near("sep same azimuth, 20 deg of elevation apart == 20",
               adsb_sep_deg(45.0f, 30.0f, 45.0f, 50.0f), 20.0f, 0.05f);

    /* ---- Sky Dome projection (cx,cy = 360,360, radius 356, gate 10) ---- */
    {
        const float cx = 360.0f, cy = 360.0f, R = 356.0f, gate = 10.0f;
        float x, y;

        adsb_sky_project(0.0f, 90.0f, gate, 0.0f, cx, cy, R, &x, &y);
        check_near("sky: el=90 lands at centre x", x, cx, 0.01f);
        check_near("sky: el=90 lands at centre y", y, cy, 0.01f);

        /* el == gate is exactly on the rim, whatever the azimuth. */
        adsb_sky_project(0.0f, gate, gate, 0.0f, cx, cy, R, &x, &y);
        check_near("sky: el=gate at az=up sits on the rim, straight up",
                   y, cy - R, 0.02f);
        check_near("sky: el=gate at az=up keeps x centred", x, cx, 0.02f);

        adsb_sky_project(90.0f, gate, gate, 0.0f, cx, cy, R, &x, &y);
        check_near("sky: az=90 (east) with north up goes right", x, cx + R, 0.02f);
        check_near("sky: az=90 stays on the horizontal", y, cy, 0.02f);

        /* Rotation: the azimuth equal to up_az always points straight up. */
        adsb_sky_project(215.0f, 40.0f, gate, 215.0f, cx, cy, R, &x, &y);
        check_true("sky: az == up_az points up (y < cy)", y < cy - 1.0f);
        check_near("sky: az == up_az keeps x centred", x, cx, 0.02f);

        /* Radius grows monotonically as elevation drops, and a contact below
         * the gate projects OUTSIDE the rim so the caller can drop it. */
        float y_hi, y_lo;
        adsb_sky_project(0.0f, 60.0f, gate, 0.0f, cx, cy, R, &x, &y_hi);
        adsb_sky_project(0.0f, 30.0f, gate, 0.0f, cx, cy, R, &x, &y_lo);
        check_true("sky: lower elevation projects further out",
                   (cy - y_lo) > (cy - y_hi));
        adsb_sky_project(0.0f, 5.0f, gate, 0.0f, cx, cy, R, &x, &y);
        check_true("sky: below-gate contact falls outside the rim",
                   (cy - y) > R);

        /* Stereographic, not linear: 50 deg (midway between gate 10 and 90)
         * must NOT land at half the radius. python: tan(20)/tan(40) = 0.4337 */
        adsb_sky_project(0.0f, 50.0f, gate, 0.0f, cx, cy, R, &x, &y);
        check_near("sky: el=50 radius is stereographic (0.4337 R)",
                   cy - y, R * 0.43373f, 0.5f);
    }

    /* ---- Radar Scope projection ---- */
    {
        const float cx = 360.0f, cy = 360.0f, R = 356.0f, range = 50.0f;
        float x, y;

        adsb_scope_project(0.0f, 0.0f, range, 0.0f, cx, cy, R, &x, &y);
        check_near("scope: dist=0 at centre x", x, cx, 0.01f);
        check_near("scope: dist=0 at centre y", y, cy, 0.01f);

        adsb_scope_project(0.0f, range, range, 0.0f, cx, cy, R, &x, &y);
        check_near("scope: dist=range sits on the rim due north", y, cy - R, 0.02f);
        check_near("scope: dist=range keeps x centred", x, cx, 0.02f);

        adsb_scope_project(180.0f, range, range, 0.0f, cx, cy, R, &x, &y);
        check_near("scope: bearing 180 sits on the rim due south", y, cy + R, 0.02f);

        /* Linear in distance: half the range is half the radius. */
        adsb_scope_project(90.0f, range * 0.5f, range, 0.0f, cx, cy, R, &x, &y);
        check_near("scope: half range = half radius (east)", x, cx + R * 0.5f, 0.02f);

        /* Same rotation convention as the Sky Dome. */
        adsb_scope_project(215.0f, range, range, 215.0f, cx, cy, R, &x, &y);
        check_near("scope: bearing == up_az points up", y, cy - R, 0.02f);
        check_near("scope: bearing == up_az keeps x centred", x, cx, 0.02f);
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
