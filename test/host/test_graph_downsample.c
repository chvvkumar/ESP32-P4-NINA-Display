/* Host test for main/ui/graph_downsample.c — pure stride/range math extracted
 * from nina_graph_overlay.c. No LVGL/ESP dependency; assert-style like
 * test/moon/test_moon_compute.c. */
#include "graph_downsample.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;

static void check_int(const char *label, int got, int expect) {
    printf("%-40s got=%d expect=%d %s\n", label, got, expect, got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_downsample(const char *label, int count, int expect_stride, int expect_disp) {
    graph_downsample_t ds = graph_downsample_compute(count);
    printf("%-40s count=%d stride=%d(exp %d) disp=%d(exp %d) %s\n",
           label, count, ds.stride, expect_stride, ds.disp_count, expect_disp,
           (ds.stride == expect_stride && ds.disp_count == expect_disp) ? "OK" : "FAIL");
    if (ds.stride != expect_stride || ds.disp_count != expect_disp) fails++;
}

int main(void) {
    /* -- Downsample stride/disp_count -------------------------------------- */

    /* count <= GRAPH_MAX_DISPLAY_POINTS (360): no decimation. */
    check_downsample("count=1 (min)", 1, 1, 1);
    check_downsample("count=360 (== max display)", GRAPH_MAX_DISPLAY_POINTS, 1, GRAPH_MAX_DISPLAY_POINTS);

    /* count just above the threshold: stride becomes 2, disp shrinks. */
    check_downsample("count=361 (== max+1)", GRAPH_MAX_DISPLAY_POINTS + 1, 2, 181);

    /* count=500 (GRAPH_MAX_POINTS, the real-world ceiling after the caller's
     * own `if (count > GRAPH_MAX_POINTS) count = GRAPH_MAX_POINTS;` clamp). */
    check_downsample("count=500 (GRAPH_MAX_POINTS)", 500, 2, 250);

    /* stride rounding boundary: count=501 -> stride=ceil(501/360)=2,
     * disp=ceil(501/2)=251. */
    check_downsample("count=501 (stride rounding)", 501, 2, 251);

    /* stride rounding boundary further out: count=720 exactly divides at
     * stride=2 -> disp=360. count=721 pushes stride to 3. */
    check_downsample("count=720 (exact stride=2)", 720, 2, 360);
    check_downsample("count=721 (stride bumps to 3)", 721, 3, 241);

    /* -- RMS Y range --------------------------------------------------------- */

    /* All-zero samples: floor is the 0.5" minimum visible range ->
     * range = (int)(0.5*120+50) = 110, still clamped to >=100 (no-op here). */
    {
        float ra[3] = {0, 0, 0};
        float dec[3] = {0, 0, 0};
        check_int("rms range: all-zero", graph_rms_y_range(ra, dec, 3), 110);
    }

    /* Negative values must use fabsf: -2.0 arcsec dominates.
     * range = (int)(2.0*120+50) = 290. */
    {
        float ra[2] = {-2.0f, 0.1f};
        float dec[2] = {0.0f, -0.05f};
        check_int("rms range: negative dominates (fabsf)", graph_rms_y_range(ra, dec, 2), 290);
    }

    /* Tiny magnitude: max_val stays at the 0.5" default since the sample is
     * smaller, so the range is the same as the all-zero case (110); the
     * explicit range<100 clamp is never reachable given the 0.5f seed. */
    {
        float ra[1] = {0.01f};
        float dec[1] = {0.01f};
        check_int("rms range: tiny values use 0.5\" default", graph_rms_y_range(ra, dec, 1), 110);
    }

    /* Large magnitude forces a floor-exceeding range: max_val=10.0 ->
     * range=(int)(10*120+50)=1250. */
    {
        float ra[1] = {10.0f};
        float dec[1] = {-3.0f};
        check_int("rms range: large magnitude", graph_rms_y_range(ra, dec, 1), 1250);
    }

    /* All-equal (padding) case: every sample identical, no branch taken past
     * the first that sets max_val == that value; result must match the
     * scalar formula exactly, not degrade due to ties. */
    {
        float ra[4] = {1.5f, 1.5f, 1.5f, 1.5f};
        float dec[4] = {1.5f, 1.5f, 1.5f, 1.5f};
        /* max_val=1.5 -> (int)(1.5*120+50) = (int)230.0 = 230 */
        check_int("rms range: all-equal values", graph_rms_y_range(ra, dec, 4), 230);
    }

    /* -- HFR Y range ---------------------------------------------------------- */

    /* All-zero HFR: floor is the 1.0 minimum visible range ->
     * range=(int)(1.0*120+0.5)=120, clamped to >=200. */
    {
        float hfr[3] = {0, 0, 0};
        float sum = -1.0f;
        int range = graph_hfr_y_range(hfr, 3, &sum);
        check_int("hfr range: all-zero (floors at 200)", range, 200);
        check_int("hfr range: all-zero sum", (int)sum, 0);
    }

    /* Values above the 1.0 default push the range past the 200 floor.
     * max_val=5.0 -> range=(int)(5*120+0.5)=600. sum=5+3+1=9. */
    {
        float hfr[3] = {5.0f, 3.0f, 1.0f};
        float sum = 0;
        int range = graph_hfr_y_range(hfr, 3, &sum);
        check_int("hfr range: above floor", range, 600);
        check_int("hfr range: sum", (int)sum, 9);
    }

    /* All-equal (padding) case for HFR too. max_val=2.5 ->
     * (int)(2.5*120+0.5) = (int)300.5 = 300. sum=2.5*4=10. */
    {
        float hfr[4] = {2.5f, 2.5f, 2.5f, 2.5f};
        float sum = 0;
        int range = graph_hfr_y_range(hfr, 4, &sum);
        check_int("hfr range: all-equal values", range, 300);
        check_int("hfr range: all-equal sum", (int)sum, 10);
    }

    /* out_sum may be NULL (caller-optional): must not crash.
     * max_val: 1.0 default, hfr[0]=1.0 (not >, stays default), hfr[1]=2.0
     * (>1.0 -> max_val=2.0). range=(int)(2.0*120+0.5)=(int)240.5=240. */
    {
        float hfr[2] = {1.0f, 2.0f};
        int range = graph_hfr_y_range(hfr, 2, NULL);
        check_int("hfr range: NULL out_sum is safe", range, 240);
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
