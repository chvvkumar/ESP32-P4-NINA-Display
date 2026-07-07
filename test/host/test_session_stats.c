/* Host test: exercises main/ui/nina_session_stats.c (PSRAM ring buffers for
 * RMS/HFR/temperature/star-count session analytics) against the host shims.
 * Style follows test/moon/test_moon_compute.c: main() with numbered sections,
 * printf progress lines, and a running failure counter (no test framework). */
#include "../../main/ui/nina_session_stats.h"
#include "../../main/app_config.h" /* MAX_NINA_INSTANCES */
#include "esp_timer.h"             /* shim_set_time_us / shim_reset_time */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

static int fails = 0;

#define CHECK(cond, fmt, ...) \
    do { \
        int _ok = (cond); \
        printf("  %s: " fmt "\n", _ok ? "OK  " : "FAIL", ##__VA_ARGS__); \
        if (!_ok) fails++; \
    } while (0)

int main(void)
{
    /* ── 1. Init / allocation sanity ─────────────────────────────────── */
    printf("1. Init/allocation\n");
    shim_reset_time();
    nina_session_stats_init();

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        const session_stats_t *st = nina_session_stats_get(i);
        CHECK(st != NULL, "instance %d: get() returns non-null", i);
        CHECK(st->points != NULL, "instance %d: PSRAM buffer allocated", i);
        CHECK(st->count == 0, "instance %d: count==0 after init", i);
        CHECK(st->write_index == 0, "instance %d: write_index==0 after init", i);
        CHECK(st->capacity == SESSION_MAX_POINTS, "instance %d: capacity==%d", i, SESSION_MAX_POINTS);
        CHECK(st->rms_min == FLT_MAX, "instance %d: rms_min primed to FLT_MAX", i);
        CHECK(st->hfr_min == FLT_MAX, "instance %d: hfr_min primed to FLT_MAX", i);
        CHECK(st->session_start_ms == 0, "instance %d: session_start_ms==0 (no records yet)", i);
    }

    /* Out-of-range instance indices must be handled defensively (no crash). */
    CHECK(nina_session_stats_get(-1) == NULL, "get(-1) returns NULL");
    CHECK(nina_session_stats_get(MAX_NINA_INSTANCES) == NULL, "get(MAX) returns NULL");
    {
        session_stats_t copy;
        CHECK(nina_session_stats_get_copy(-1, &copy) == false, "get_copy(-1) returns false");
        CHECK(nina_session_stats_get_copy(0, NULL) == false, "get_copy(0, NULL) returns false");
    }

    /* ── 2. Appending fewer than capacity preserves order + count ──────── */
    printf("2. Sub-capacity append order\n");
    const int inst = 0;
    const int n_small = 10;
    shim_set_time_us(1000 * 1000); /* t = 1000 ms */
    for (int i = 0; i < n_small; i++) {
        /* rms increases monotonically so we can check ordering via the ring
         * buffer contents directly (no accessor exposes indexed reads, so we
         * read the const pointer's ->points array, which is documented as
         * legitimate for nina_session_stats_get() callers -- unlike get_copy,
         * whose contract says the returned points ptr must not be dereferenced). */
        nina_session_stats_record(inst, (float)(i + 1), (float)(i + 1) * 0.1f,
                                   10.0f + i, 50 + i, 0.5f);
        shim_set_time_us((1000 + (i + 1) * 500) * 1000); /* advance 500ms/sample */
    }
    {
        const session_stats_t *st = nina_session_stats_get(inst);
        CHECK(st->count == n_small, "count == %d after %d appends", n_small, n_small);
        CHECK(st->write_index == n_small, "write_index == %d", n_small);
        for (int i = 0; i < n_small; i++) {
            CHECK(st->points[i].rms_total == (float)(i + 1),
                  "point[%d].rms_total == %d (insertion order preserved)", i, i + 1);
        }
    }

    /* ── 3. Exactly-at-capacity then one more: wraparound ───────────────── */
    printf("3. Capacity + wraparound\n");
    nina_session_stats_reset(inst);
    shim_set_time_us(2000ULL * 1000);
    for (int i = 0; i < SESSION_MAX_POINTS; i++) {
        nina_session_stats_record(inst, (float)(i + 1), 1.0f, 10.0f, 1, 0.0f);
        shim_set_time_us((2000ULL + (i + 1)) * 1000);
    }
    {
        const session_stats_t *st = nina_session_stats_get(inst);
        CHECK(st->count == SESSION_MAX_POINTS, "count clamps at capacity (%d) after exactly-full", SESSION_MAX_POINTS);
        CHECK(st->write_index == 0, "write_index wraps to 0 after exactly filling capacity");
        CHECK(st->points[0].rms_total == 1.0f, "point[0] still holds oldest sample (rms=1) before overflow");
    }
    /* One more record: oldest (rms==1, at slot 0) must be overwritten/dropped. */
    nina_session_stats_record(inst, 9999.0f, 1.0f, 10.0f, 1, 0.0f);
    {
        const session_stats_t *st = nina_session_stats_get(inst);
        CHECK(st->count == SESSION_MAX_POINTS,
              "count still clamped at capacity (%d), not capacity+1", SESSION_MAX_POINTS);
        CHECK(st->write_index == 1, "write_index advances to 1 after wraparound write");
        CHECK(st->points[0].rms_total == 9999.0f,
              "oldest sample (slot 0) overwritten by the wraparound write");
        CHECK(st->points[1].rms_total == 2.0f,
              "point[1] (originally 2nd-oldest) is now the logically-oldest surviving sample");
    }

    /* ── 4. Timestamps monotonic non-decreasing per the shim clock ──────── */
    printf("4. Timestamp monotonicity\n");
    nina_session_stats_reset(inst);
    {
        int64_t sim_ms[5] = { 5000, 5100, 5100, 5300, 6000 }; /* includes a tie */
        for (int i = 0; i < 5; i++) {
            shim_set_time_us(sim_ms[i] * 1000);
            nina_session_stats_record(inst, 1.0f, 1.0f, 10.0f, 1, 0.0f);
        }
        const session_stats_t *st = nina_session_stats_get(inst);
        CHECK(st->count == 5, "5 records taken for timestamp check");
        int monotonic = 1;
        for (int i = 0; i < 5; i++) {
            CHECK(st->points[i].timestamp_ms == sim_ms[i],
                  "point[%d].timestamp_ms == %lld (shim clock)", i, (long long)sim_ms[i]);
            if (i > 0 && st->points[i].timestamp_ms < st->points[i - 1].timestamp_ms) monotonic = 0;
        }
        CHECK(monotonic, "timestamps are non-decreasing across the recorded run");
        CHECK(st->session_start_ms == sim_ms[0], "session_start_ms == timestamp of first record");
    }

    /* ── 5. Aggregate (running min/max/sum -> avg) verification ─────────── */
    printf("5. Aggregate min/max/avg\n");
    nina_session_stats_reset(inst);
    {
        /* Known dataset. Include a <=0 rms/hfr value to verify the
         * skip-zero/invalid guard in nina_session_stats_record(). */
        float rms_vals[] = { 2.0f, 0.0f, 5.0f, 1.0f, 3.0f };   /* 0.0f must be skipped */
        float hfr_vals[] = { -1.0f, 2.5f, 4.5f, 3.0f, 2.0f };  /* -1.0f must be skipped */
        int nvals = 5;
        shim_set_time_us(10000 * 1000);
        for (int i = 0; i < nvals; i++) {
            nina_session_stats_record(inst, rms_vals[i], hfr_vals[i], 10.0f, 1, 0.0f);
        }
        const session_stats_t *st = nina_session_stats_get(inst);

        /* Hand-computed over the valid (>0) subset only. */
        float rms_expect_min = 1.0f, rms_expect_max = 5.0f;
        float rms_expect_sum = 2.0f + 5.0f + 1.0f + 3.0f; /* skip the 0.0f */
        int   rms_expect_count = 4;
        float hfr_expect_min = 2.0f, hfr_expect_max = 4.5f;
        float hfr_expect_sum = 2.5f + 4.5f + 3.0f + 2.0f; /* skip the -1.0f */
        int   hfr_expect_count = 4;

        CHECK(fabsf(st->rms_min - rms_expect_min) < 1e-6f, "rms_min == %.3f", rms_expect_min);
        CHECK(fabsf(st->rms_max - rms_expect_max) < 1e-6f, "rms_max == %.3f", rms_expect_max);
        CHECK(st->rms_count == rms_expect_count, "rms_count == %d (zero sample excluded)", rms_expect_count);
        CHECK(fabsf(st->rms_sum - rms_expect_sum) < 1e-4f, "rms_sum == %.3f", rms_expect_sum);
        CHECK(fabsf((st->rms_sum / st->rms_count) - (rms_expect_sum / rms_expect_count)) < 1e-4f,
              "rms avg == %.4f", rms_expect_sum / rms_expect_count);

        CHECK(fabsf(st->hfr_min - hfr_expect_min) < 1e-6f, "hfr_min == %.3f", hfr_expect_min);
        CHECK(fabsf(st->hfr_max - hfr_expect_max) < 1e-6f, "hfr_max == %.3f", hfr_expect_max);
        CHECK(st->hfr_count == hfr_expect_count, "hfr_count == %d (negative sample excluded)", hfr_expect_count);
        CHECK(fabsf(st->hfr_sum - hfr_expect_sum) < 1e-4f, "hfr_sum == %.3f", hfr_expect_sum);

        /* Exposure accumulation is a separate running aggregate; sanity-check it too. */
        nina_session_stats_add_exposure(inst, 30.0f);
        nina_session_stats_add_exposure(inst, 45.5f);
        st = nina_session_stats_get(inst);
        CHECK(st->total_exposures == 2, "total_exposures == 2 after two add_exposure calls");
        CHECK(fabsf(st->total_exposure_time_s - 75.5f) < 1e-4f, "total_exposure_time_s == 75.5");
    }

    /* ── 6. Reset returns to post-init empty state ───────────────────────── */
    printf("6. Reset semantics\n");
    nina_session_stats_reset(inst);
    {
        const session_stats_t *st = nina_session_stats_get(inst);
        CHECK(st->count == 0, "count==0 after reset");
        CHECK(st->write_index == 0, "write_index==0 after reset");
        CHECK(st->capacity == SESSION_MAX_POINTS, "capacity preserved across reset (allocation kept)");
        CHECK(st->rms_min == FLT_MAX, "rms_min re-primed to FLT_MAX after reset");
        CHECK(st->hfr_min == FLT_MAX, "hfr_min re-primed to FLT_MAX after reset");
        CHECK(st->rms_max == 0.0f, "rms_max cleared to 0 after reset");
        CHECK(st->session_start_ms == 0, "session_start_ms cleared after reset");
        CHECK(st->total_exposures == 0, "total_exposures cleared after reset");

        /* Fresh append after reset behaves like a brand-new buffer. */
        shim_set_time_us(20000 * 1000);
        nina_session_stats_record(inst, 7.0f, 7.0f, 15.0f, 20, 1.0f);
        st = nina_session_stats_get(inst);
        CHECK(st->count == 1, "count==1 after first append post-reset");
        CHECK(st->points[0].rms_total == 7.0f, "post-reset first sample lands at slot 0");
        CHECK(st->session_start_ms == 20000, "session_start_ms re-set on first post-reset record");
    }

    /* ── 7. Double-init: reachable and must not crash / must not leak refs ── */
    printf("7. Double-init safety\n");
    {
        /* nina_session_stats_init() is documented "call once at startup" but
         * nothing prevents a caller from invoking it twice (e.g. a hypothetical
         * re-init path); the implementation memset()s the whole array and
         * reallocates every instance's PSRAM buffer unconditionally, which
         * means a second init() call leaks the previous heap_caps_calloc()
         * allocation (the old `points` pointer is discarded by memset(0)
         * before the new heap_caps_calloc() call overwrites it).
         * This is a plausible hardening gap (leak on repeated init), not
         * something we fix here -- we just document current behavior.
         * See main/ui/nina_session_stats.c:27-44 (nina_session_stats_init). */
        nina_session_stats_init();
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            const session_stats_t *st = nina_session_stats_get(i);
            CHECK(st->points != NULL, "instance %d: buffer non-null after 2nd init()", i);
            CHECK(st->count == 0, "instance %d: count==0 after 2nd init() (state fully reset)", i);
        }
    }

    /* ── 8. NaN / Inf / huge-magnitude pass-through (no guard exists) ───── */
    printf("8. NaN/Inf passthrough (documents current behavior, no guard exists)\n");
    {
        const int inst2 = 1;
        nina_session_stats_reset(inst2);
        shim_set_time_us(30000 * 1000);

        /* rms_total = NAN: the guard is `if (rms_total > 0.0f)`, and any
         * comparison against NAN is false in IEEE-754, so NAN correctly
         * fails to update rms_min/max/sum -- but it IS still stored verbatim
         * in the ring buffer point itself (no clamp on the raw sample). */
        nina_session_stats_record(inst2, NAN, INFINITY, 1e30f, 1, -INFINITY);
        const session_stats_t *st = nina_session_stats_get(inst2);
        CHECK(st->count == 1, "record with NAN/INFINITY still counts as one sample");
        CHECK(isnan(st->points[0].rms_total), "raw NAN rms_total is stored verbatim (no clamp/guard)");
        CHECK(isinf(st->points[0].hfr) && st->points[0].hfr > 0, "raw +INFINITY hfr stored verbatim");
        CHECK(st->points[0].temperature == 1e30f, "huge-magnitude temperature stored verbatim");
        CHECK(isinf(st->points[0].cooler_power) && st->points[0].cooler_power < 0,
              "raw -INFINITY cooler_power stored verbatim");
        /* Aggregate guard behavior differs between NAN and INFINITY:
         * NAN fails `rms_total > 0.0f` (any comparison with NAN is false in
         * IEEE-754), so the NAN rms sample is correctly excluded from
         * rms_min/max/sum/count. But INFINITY *passes* `hfr > 0.0f` (IEEE-754:
         * +INFINITY > 0.0f is true), so the +INFINITY hfr sample DOES enter
         * the running aggregate: hfr_max becomes +INFINITY and hfr_sum becomes
         * +INFINITY, silently poisoning every future average/graph-scale
         * computation derived from hfr_max or hfr_sum for the rest of the
         * session (until the next nina_session_stats_reset()). This looks
         * like a real hardening gap: the guard `if (hfr > 0.0f)` only filters
         * zero/negative values, not non-finite ones -- a `isfinite()` check
         * would be needed to also exclude INFINITY/-INFINITY.
         * See main/ui/nina_session_stats.c:82-88 (hfr aggregate update) and
         * :74-80 (rms aggregate update, same pattern, same gap for +INFINITY
         * rms samples). Documented here, not fixed. */
        CHECK(st->rms_count == 0, "NAN rms_total does not enter the running aggregate (guard rejects NAN)");
        CHECK(st->hfr_count == 1,
              "+INFINITY hfr DOES enter the running aggregate (guard `> 0.0f` does not reject non-finite values)");
        CHECK(isinf(st->hfr_max) && st->hfr_max > 0,
              "hfr_max is poisoned to +INFINITY by the unguarded sample -- possible hardening gap");
        CHECK(isinf(st->hfr_sum) && st->hfr_sum > 0,
              "hfr_sum is poisoned to +INFINITY, corrupting any future average until next reset()");
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
