/* Host test for main/sky_night_math.h: twilight, sky colours, Sun glow, star
 * look, Moon strip and glow mask, guiding ring, HFR, time-left parsing,
 * compass labels and hill noise. Assert-style like test_sky_scene_math.c. */
#include "sky_night_math.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static int checks = 0;

static void check_near(const char *label, float got, float expect, float tol)
{
    float d = got - expect;
    if (d < 0.0f) d = -d;
    int ok = (d <= tol);
    checks++;
    printf("%-60s got=%-10ld expect=%-10ld %s\n", label,
           (long)lroundf(got * 1000.0f), (long)lroundf(expect * 1000.0f), ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void check_true(const char *label, int got)
{
    checks++;
    printf("%-60s %s\n", label, got ? "OK" : "FAIL");
    if (!got) fails++;
}

static void check_int(const char *label, int got, int expect)
{
    int ok = (got == expect);
    checks++;
    printf("%-60s got=%d expect=%d %s\n", label, got, expect, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static int fin(float v) { return isfinite(v) ? 1 : 0; }

static void test_twilight_ramp(void)
{
    check_near("tw -5", sky_twilight(-5.0f), 1.0f, 1e-5f);
    check_near("tw -18", sky_twilight(-18.0f), 0.0f, 1e-5f);
    check_near("tw -11.5", sky_twilight(-11.5f), 0.5f, 1e-5f);
    check_near("tw +30", sky_twilight(30.0f), 1.0f, 1e-5f);
    check_near("tw -60", sky_twilight(-60.0f), 0.0f, 1e-5f);
    int mono = 1;
    float prev = -1.0f;
    for (float a = -30.0f; a <= 10.0f; a += 0.5f) {
        float v = sky_twilight(a);
        if (v < prev) mono = 0;
        prev = v;
    }
    check_true("tw monotonic", mono);
    check_true("tw NaN finite", fin(sky_twilight(NAN)));
    check_near("moon light high", sky_moon_light(0.8f, 60.0f), 0.8f, 1e-5f);
    check_near("moon light below horizon", sky_moon_light(0.8f, -3.0f), 0.0f, 1e-6f);
    check_near("moon light alt 15", sky_moon_light(1.0f, 15.0f), 0.5f, 1e-5f);
    check_near("moon wash alt -1", sky_moon_wash(1.0f, -1.0f), 0.0f, 1e-6f);
    check_near("moon wash alt 19", sky_moon_wash(1.0f, 19.0f), 1.0f, 1e-5f);
    check_near("moon wash alt 9", sky_moon_wash(0.5f, 9.0f), 0.25f, 1e-5f);
}

static void test_sky_colours_night(void)
{
    const uint32_t top = 0x03050d, glow = 0x121d33, td = 0x0b1636, gd = 0x2a4872;
    const uint32_t tm = 0x0c1730, gm = 0x22365a, haze = 0x5f7fb0;
    sky_grad_t g = sky_colours(0.0f, 0.0f, top, glow, td, gd, tm, gm, haze);
    check_true("night top", g.top == top);
    check_true("night mid", g.mid == sky_rgb_mix(top, glow, 0.4f));
    check_true("night low", g.low == sky_rgb_mix(glow, haze, 0.15f));
    sky_grad_t d = sky_colours(1.0f, 0.0f, top, glow, td, gd, tm, gm, haze);
    check_true("dusk top 90 percent", d.top == sky_rgb_mix(top, td, 0.9f));
    sky_grad_t m = sky_colours(0.0f, 1.0f, top, glow, td, gd, tm, gm, haze);
    check_true("moon top", m.top == tm);
    check_true("mix k clamp", sky_rgb_mix(0x000000, 0xffffff, 5.0f) == 0xffffff);
    check_true("mix NaN k", sky_rgb_mix(0x102030, 0xffffff, NAN) == 0x102030);
    check_true("haze at tw 0", sky_haze_colour(0.0f, haze, 0x8a6a78) == haze);
    check_true("haze at tw 1", sky_haze_colour(1.0f, haze, 0x8a6a78) == sky_rgb_mix(haze, 0x8a6a78, 0.5f));
}

static void test_sun_glow_profile(void)
{
    uint32_t rgb = 0;
    float a = -1.0f;
    sky_sun_glow(0.0f, 1.0f, 0xb06a44, 0x5a3c52, &rgb, &a);
    check_near("glow d0", a, 0.36f, 1e-5f);
    check_true("glow near colour", rgb == 0xb06a44);
    sky_sun_glow(0.45f, 1.0f, 0xb06a44, 0x5a3c52, &rgb, &a);
    check_near("glow d0.45", a, 0.15f, 1e-5f);
    check_true("glow far colour", rgb == 0x5a3c52);
    sky_sun_glow(1.0f, 1.0f, 0xb06a44, 0x5a3c52, &rgb, &a);
    check_near("glow d1", a, 0.0f, 1e-5f);
    sky_sun_glow(3.0f, 1.0f, 0xb06a44, 0x5a3c52, &rgb, &a);
    check_near("glow d3", a, 0.0f, 1e-5f);
    sky_sun_glow(0.0f, 0.0f, 0xb06a44, 0x5a3c52, &rgb, &a);
    check_near("glow tw0", a, 0.0f, 1e-6f);
    sky_sun_glow(0.2f, 0.5f, 0xb06a44, 0x5a3c52, NULL, NULL);
    int mono = 1;
    float prev = 2.0f;
    for (float d = 0.0f; d <= 1.2f; d += 0.02f) {
        sky_sun_glow(d, 1.0f, 0xb06a44, 0x5a3c52, &rgb, &a);
        if (a > prev + 1e-6f) mono = 0;
        prev = a;
    }
    check_true("glow monotonic in d", mono);
    sky_sun_glow(NAN, NAN, 0, 0, &rgb, &a);
    check_true("glow NaN finite", fin(a));
}

static void test_sun_side_x(void)
{
    check_near("side inside", sky_sun_side_x(true, 300.0f, 10.0f, 720.0f), 300.0f, 1e-3f);
    check_near("side clamp low", sky_sun_side_x(true, -900.0f, -20.0f, 720.0f), -72.0f, 1e-3f);
    check_near("side clamp high", sky_sun_side_x(true, 5000.0f, 20.0f, 720.0f), 792.0f, 1e-3f);
    check_near("side far left", sky_sun_side_x(true, 300.0f, -120.0f, 720.0f), -72.0f, 1e-3f);
    check_near("side far right", sky_sun_side_x(true, 300.0f, 120.0f, 720.0f), 792.0f, 1e-3f);
    check_near("side behind left", sky_sun_side_x(false, 300.0f, -10.0f, 720.0f), -72.0f, 1e-3f);
    check_near("side behind right", sky_sun_side_x(false, 300.0f, 10.0f, 720.0f), 792.0f, 1e-3f);
    check_true("side NaN finite", fin(sky_sun_side_x(true, NAN, NAN, 720.0f)));
}

static void test_star_appearance(void)
{
    check_near("limit night", sky_star_limit(0.0f, 0.0f), 4.6f, 1e-5f);
    check_near("limit twilight", sky_star_limit(1.0f, 0.0f), 2.2f, 1e-5f);
    check_near("limit twilight wash", sky_star_limit(1.0f, 1.0f), 0.8f, 1e-5f);
    check_near("alpha past limit", sky_star_alpha(4.7f, 4.6f), 0.0f, 1e-6f);
    check_near("alpha mag -1", sky_star_alpha(-1.0f, 4.6f), 0.72f, 1e-5f);
    check_near("alpha at limit", sky_star_alpha(4.6f, 4.6f), 0.18f * 0.72f, 1e-5f);
    check_near("radius Sirius", sky_star_radius(-1.46f), 3.1f, 1e-5f);
    check_near("radius mag 4.5", sky_star_radius(4.5f), 0.65f, 1e-5f);
    check_near("moon fade far", sky_star_moon_fade(0.7f, 30.0f), 1.0f, 1e-5f);
    check_near("moon fade near", sky_star_moon_fade(0.7f, 2.0f), 0.3f, 1e-5f);
    check_true("alpha NaN finite", fin(sky_star_alpha(NAN, NAN)) && fin(sky_star_radius(NAN)));
}

static void test_seg_dist(void)
{
    check_near("seg interior", sky_seg_dist(5.0f, 3.0f, 0.0f, 0.0f, 10.0f, 0.0f), 3.0f, 1e-5f);
    check_near("seg past a", sky_seg_dist(-3.0f, 4.0f, 0.0f, 0.0f, 10.0f, 0.0f), 5.0f, 1e-5f);
    check_near("seg past b", sky_seg_dist(13.0f, 4.0f, 0.0f, 0.0f, 10.0f, 0.0f), 5.0f, 1e-5f);
    check_near("seg zero length", sky_seg_dist(3.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f), 5.0f, 1e-5f);
    check_true("seg NaN finite", fin(sky_seg_dist(NAN, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f)));
}

static float strip_area(float limb[9][2], float term[9][2])
{
    float x[18], y[18];
    int n = 0;
    for (int i = 0; i < 9; i++) { x[n] = limb[i][0]; y[n] = limb[i][1]; n++; }
    for (int i = 8; i >= 0; i--) { x[n] = term[i][0]; y[n] = term[i][1]; n++; }
    float s = 0.0f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        s += x[i] * y[j] - x[j] * y[i];
    }
    return fabsf(s) * 0.5f;
}

static void test_moon_lit_strip(void)
{
    const float ills[3] = { 0.1f, 0.5f, 0.9f };
    for (int k = 0; k < 3; k++) {
        float limb[9][2], term[9][2];
        char lab[48];
        sky_moon_lit_strip(ills[k], 100.0f, 0.7f, 50.0f, 60.0f, limb, term);
        float area = strip_area(limb, term);
        float want = ills[k] * 3.14159265f * 100.0f * 100.0f;
        snprintf(lab, sizeof lab, "lit area illum %.1f within 6 percent", (double)ills[k]);
        check_true(lab, fabsf(area - want) <= 0.06f * want);
        int inside = 1;
        for (int i = 0; i < 9; i++) {
            float d1 = hypotf(limb[i][0] - 50.0f, limb[i][1] - 60.0f);
            float d2 = hypotf(term[i][0] - 50.0f, term[i][1] - 60.0f);
            if (d1 > 100.01f || d2 > 100.01f) inside = 0;
        }
        snprintf(lab, sizeof lab, "lit points within r illum %.1f", (double)ills[k]);
        check_true(lab, inside);
    }
    /* Rotation: ang 0 versus ang 90 degrees is a 90 degree turn about the centre. */
    float a0[9][2], t0[9][2], a1[9][2], t1[9][2];
    sky_moon_lit_strip(0.3f, 50.0f, 0.0f, 0.0f, 0.0f, a0, t0);
    sky_moon_lit_strip(0.3f, 50.0f, 1.5707963f, 0.0f, 0.0f, a1, t1);
    int ok = 1;
    for (int i = 0; i < 9; i++) {
        if (fabsf(a1[i][0] - (-a0[i][1])) > 0.01f || fabsf(a1[i][1] - a0[i][0]) > 0.01f) ok = 0;
        if (fabsf(t1[i][0] - (-t0[i][1])) > 0.01f || fabsf(t1[i][1] - t0[i][0]) > 0.01f) ok = 0;
    }
    check_true("lit strip turns 90 degrees", ok);
    sky_moon_lit_strip(NAN, NAN, NAN, NAN, NAN, a0, t0);
    ok = 1;
    for (int i = 0; i < 9; i++) ok &= fin(a0[i][0]) && fin(a0[i][1]) && fin(t0[i][0]) && fin(t0[i][1]);
    check_true("lit strip NaN finite", ok);
}

static void test_glow_a8(void)
{
    const int size = 100;
    const float inner = 20.0f, peak = 0.14f, dith = 3.0f;
    int outside_zero = 1, never_over = 1, near_inner = 1, mid_ok = 1;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = (float)x - 49.5f, dy = (float)y - 49.5f;
            float d = sqrtf(dx * dx + dy * dy);
            int v = sky_glow_a8(x, y, size, inner, peak, dith);
            if (d >= 50.0f && v != 0) outside_zero = 0;
            if ((int)v > 255) never_over = 0;
            if (d <= inner && fabsf((float)v - peak * 255.0f) > dith) near_inner = 0;
            if (d > inner && d < 50.0f) {
                float want = peak * 255.0f * (1.0f - (d - inner) / (50.0f - inner));
                if (fabsf((float)v - want) > dith) mid_ok = 0;
            }
        }
    }
    check_true("glow a8 zero outside", outside_zero);
    check_true("glow a8 within dither at inner", near_inner);
    check_true("glow a8 falls linearly", mid_ok);
    check_true("glow a8 never above 255", never_over);
    check_int("glow a8 bad size", sky_glow_a8(0, 0, 0, 1.0f, 1.0f, 1.0f), 0);
}

static void test_guide_shape(void)
{
    const float flat[8] = { 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f };
    sky_guide_t g = sky_guide_shape(flat, 8, 0.4f, 0.55f, 1.15f);
    check_near("flat amp", g.amp_px, 0.0f, 1e-6f);
    check_near("flat poor", g.poor, 0.0f, 1e-6f);
    g = sky_guide_shape(flat, 8, 1.15f, 0.55f, 1.15f);
    check_near("ok_max poor", g.poor, 1.0f, 1e-5f);
    check_true("ok_max amp >= 2", g.amp_px >= 2.0f);
    const float wild[10] = { 0.1f, 3.0f, 0.2f, 4.0f, 0.1f, 5.0f, 0.2f, 6.0f, 0.1f, 7.0f };
    g = sky_guide_shape(wild, 10, 9.0f, 0.55f, 1.15f);
    check_true("amp capped at 3.5", g.amp_px <= 3.5f);
    check_near("amp is multiple of 0.5", fmodf(g.amp_px * 2.0f, 1.0f), 0.0f, 1e-5f);
    int mult = 1;
    for (float r = 0.0f; r < 2.0f; r += 0.07f) {
        sky_guide_t q = sky_guide_shape(wild, 10, r, 0.55f, 1.15f);
        if (fmodf(q.amp_px * 2.0f, 1.0f) > 1e-5f || q.amp_px > 3.5f) mult = 0;
    }
    check_true("amp always 0.5 steps", mult);
    /* fallback thresholds: 0 and inverted pair behave as 0.55 / 1.15 */
    g = sky_guide_shape(flat, 8, 1.15f, 0.0f, 0.0f);
    check_near("fallback zero pair", g.poor, 1.0f, 1e-5f);
    g = sky_guide_shape(flat, 8, 0.55f, 2.0f, 1.0f);
    check_near("fallback inverted pair", g.poor, 0.0f, 1e-5f);
    g = sky_guide_shape(NULL, 0, 0.8f, 0.55f, 1.15f);
    check_true("no history finite", fin(g.amp_px) && fin(g.poor));
    g = sky_guide_shape(NULL, -5, NAN, NAN, NAN);
    check_true("garbage finite", fin(g.amp_px) && fin(g.poor));
}

static void test_ring_radius(void)
{
    int flat = 1;
    float sum = 0.0f, ext = 0.0f;
    for (int i = 0; i < 64; i++) {
        float a = (float)i / 64.0f * 6.2831853f;
        if (fabsf(sky_ring_radius(24.0f, 0.0f, a, 1.1f, 2.3f) - 24.0f) > 1e-5f) flat = 0;
        float r = sky_ring_radius(24.0f, 3.0f, a, 1.1f, 2.3f);
        sum += r;
        if (fabsf(r - 24.0f) > ext) ext = fabsf(r - 24.0f);
    }
    check_true("ring amp 0 is round", flat);
    check_near("ring mean", sum / 64.0f, 24.0f, 0.05f);
    check_true("ring extreme within amp", ext <= 3.0f + 1e-4f);
    check_true("ring NaN finite", fin(sky_ring_radius(24.0f, NAN, NAN, NAN, NAN)));
}

static void test_hfr_soft(void)
{
    check_near("hfr zero", sky_hfr_soft(0.0f, 2.0f, 3.0f), 0.0f, 1e-6f);
    check_near("hfr negative", sky_hfr_soft(-1.0f, 2.0f, 3.0f), 0.0f, 1e-6f);
    check_near("hfr at good", sky_hfr_soft(2.0f, 2.0f, 3.0f), 0.0f, 1e-6f);
    check_near("hfr mid", sky_hfr_soft(2.5f, 2.0f, 3.0f), 0.5f, 1e-5f);
    check_near("hfr at ok", sky_hfr_soft(3.0f, 2.0f, 3.0f), 1.0f, 1e-5f);
    check_near("hfr fallback mid", sky_hfr_soft(3.4f, 0.0f, 0.0f), 0.5f, 1e-5f);
    check_near("hfr fallback inverted", sky_hfr_soft(4.2f, 3.0f, 2.0f), 1.0f, 1e-5f);
    check_true("hfr NaN finite", fin(sky_hfr_soft(NAN, NAN, NAN)));
}

static void test_gathered(void)
{
    check_near("gather idle", sky_gathered(false, 0.7f, -1.0f), 0.0f, 1e-6f);
    check_near("gather exposing", sky_gathered(true, 0.6f, -1.0f), 0.6f, 1e-6f);
    check_near("gather clamp", sky_gathered(true, 4.0f, -1.0f), 1.0f, 1e-6f);
    check_near("gather drain start", sky_gathered(false, 0.0f, 0.0f), 1.0f, 1e-5f);
    check_near("gather drain half", sky_gathered(false, 0.0f, SKY_DRAIN_S * 0.5f), 0.5f, 1e-5f);
    check_near("gather drain end", sky_gathered(false, 0.0f, SKY_DRAIN_S), 0.0f, 1e-5f);
    check_near("gather drain past", sky_gathered(false, 0.0f, 5.0f), 0.0f, 1e-6f);
    check_near("gather larger wins", sky_gathered(true, 0.9f, SKY_DRAIN_S * 0.5f), 0.9f, 1e-5f);
    check_true("gather NaN finite", fin(sky_gathered(true, NAN, NAN)));
}

static void test_parse_minutes(void)
{
    check_int("parse 2:05", sky_parse_minutes("2:05"), 125);
    check_int("parse 0:07", sky_parse_minutes("0:07"), 7);
    check_int("parse 2h 05m", sky_parse_minutes("2h 05m"), 125);
    check_int("parse 0h 00m", sky_parse_minutes("0h 00m"), 0);
    check_int("parse 12h 30m", sky_parse_minutes("12h 30m"), 750);
    check_int("parse empty", sky_parse_minutes(""), -1);
    check_int("parse --", sky_parse_minutes("--"), -1);
    check_int("parse abc", sky_parse_minutes("abc"), -1);
    check_int("parse NULL", sky_parse_minutes(NULL), -1);
    check_int("parse 1:75", sky_parse_minutes("1:75"), -1);
    check_int("parse 2h 75m", sky_parse_minutes("2h 75m"), -1);
    check_int("parse 2:5", sky_parse_minutes("2:5"), -1);
    check_int("parse 2:05x", sky_parse_minutes("2:05x"), -1);
    check_int("parse 2h05m", sky_parse_minutes("2h05m"), -1);
    check_int("parse -1:00", sky_parse_minutes("-1:00"), -1);
    check_int("parse 99999:00", sky_parse_minutes("99999:00"), -1);
    check_int("parse colon only", sky_parse_minutes(":"), -1);
}

static void test_compass_label(void)
{
    char b[16];
    int rk = -1;
    check_true("N", strcmp(sky_compass_label(0, b, sizeof b, &rk), "N") == 0 && rk == 2);
    check_true("E", strcmp(sky_compass_label(90, b, sizeof b, &rk), "E") == 0 && rk == 2);
    check_true("S", strcmp(sky_compass_label(180, b, sizeof b, &rk), "S") == 0 && rk == 2);
    check_true("W", strcmp(sky_compass_label(270, b, sizeof b, &rk), "W") == 0 && rk == 2);
    check_true("NE", strcmp(sky_compass_label(45, b, sizeof b, &rk), "NE") == 0 && rk == 1);
    check_true("SE", strcmp(sky_compass_label(135, b, sizeof b, &rk), "SE") == 0 && rk == 1);
    check_true("SW", strcmp(sky_compass_label(225, b, sizeof b, &rk), "SW") == 0 && rk == 1);
    check_true("NW", strcmp(sky_compass_label(315, b, sizeof b, &rk), "NW") == 0 && rk == 1);
    check_true("30 degrees", strcmp(sky_compass_label(30, b, sizeof b, &rk), "30\xC2\xB0") == 0 && rk == 0);
    check_true("360 wraps to N", strcmp(sky_compass_label(360, b, sizeof b, &rk), "N") == 0 && rk == 2);
    check_true("-90 wraps to W", strcmp(sky_compass_label(-90, b, sizeof b, &rk), "W") == 0 && rk == 2);
    check_true("rank NULL ok", strcmp(sky_compass_label(30, b, sizeof b, NULL), "30\xC2\xB0") == 0);
    check_true("NULL buf safe", sky_compass_label(30, NULL, 0, &rk)[0] == '\0');
    check_true("tiny buf safe", sky_compass_label(300, b, 3, &rk)[0] == '\0');
}

static void test_noise_wraps(void)
{
    const int cells[3] = { 19, 45, 144 };
    for (int c = 0; c < 3; c++) {
        char lab[48];
        check_near("noise 0 equals 360", sky_vnoise_wrap(0.0f, cells[c], 7u),
                   sky_vnoise_wrap(360.0f, cells[c], 7u), 1e-5f);
        float maxstep = 0.0f, wrapstep = 0.0f, prev = sky_vnoise_wrap(0.0f, cells[c], 7u);
        int in_range = 1;
        for (int i = 1; i <= 720; i++) {
            float v = sky_vnoise_wrap((float)i * 0.5f, cells[c], 7u);
            float s = fabsf(v - prev);
            if (v < 0.0f || v > 1.0f) in_range = 0;
            if (i <= 4 || i >= 717) {
                if (s > wrapstep) wrapstep = s;
            } else if (s > maxstep) {
                maxstep = s;
            }
            prev = v;
        }
        float wrap_in = fabsf(sky_vnoise_wrap(359.5f, cells[c], 7u) - sky_vnoise_wrap(0.5f, cells[c], 7u));
        snprintf(lab, sizeof lab, "noise %d cells in [0,1]", cells[c]);
        check_true(lab, in_range);
        snprintf(lab, sizeof lab, "noise %d cells wrap step small", cells[c]);
        check_true(lab, wrap_in <= 2.0f * maxstep + 1e-4f && wrapstep <= maxstep + 1e-4f);
    }
    check_true("noise negative az", fin(sky_vnoise_wrap(-725.0f, 19, 3u)));
    check_true("noise NaN az", fin(sky_vnoise_wrap(NAN, 19, 3u)));
    check_true("noise bad cells", fin(sky_vnoise_wrap(10.0f, 0, 3u)) && fin(sky_vnoise_wrap(10.0f, -4, 3u)));
    int hr = 1;
    for (uint32_t i = 0; i < 2000u; i++) {
        float h = sky_hash01(i);
        if (h < 0.0f || h >= 1.0f) hr = 0;
    }
    check_true("hash01 in [0,1)", hr);
}

static void test_hill_frac(void)
{
    float sm = 0.0f, sn = 0.0f;
    int in_range = 1;
    for (int i = 0; i < 360; i++) {
        float m = sky_hill_frac((float)i, 1);
        float n = sky_hill_frac((float)i, 2);
        if (m < 0.05f || m > 0.9f || n < 0.05f || n > 0.9f) in_range = 0;
        sm += m;
        sn += n;
    }
    check_true("hill frac in [0.05, 0.9]", in_range);
    check_true("middle layer mean above near", sm / 360.0f > sn / 360.0f);
    check_true("hill NaN finite", fin(sky_hill_frac(NAN, 1)) && fin(sky_hill_frac(NAN, 2)));
}

int main(void)
{
    test_twilight_ramp();
    test_sky_colours_night();
    test_sun_glow_profile();
    test_sun_side_x();
    test_star_appearance();
    test_seg_dist();
    test_moon_lit_strip();
    test_glow_a8();
    test_guide_shape();
    test_ring_radius();
    test_hfr_soft();
    test_gathered();
    test_parse_minutes();
    test_compass_label();
    test_noise_wraps();
    test_hill_frac();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
