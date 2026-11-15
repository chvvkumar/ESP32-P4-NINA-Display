/* Host test for main/sky_stars.h: the star table, the star patterns and the
 * equatorial to world rotation. Header-only, no ESP-IDF dependency. */
#include "sky_stars.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
static int checks = 0;

static void check_true(const char *label, int got)
{
    checks++;
    if (!got) {
        printf("FAIL %s\n", label);
        fails++;
    }
}

static int find_near(float ra_h, float dec_deg)
{
    for (int i = 0; i < SKY_STAR_N; i++) {
        if (fabsf(SKY_STARS[i].ra_h - ra_h) < 0.001f && fabsf(SKY_STARS[i].dec_deg - dec_deg) < 0.01f) {
            return i;
        }
    }
    return -1;
}

static void test_table_shape(void)
{
    int sirius = 0;
    check_true("star count 237", SKY_STAR_N == 237);
    for (int i = 0; i < SKY_STAR_N; i++) {
        const sky_star_t *s = &SKY_STARS[i];
        check_true("ra range", s->ra_h >= 0.0f && s->ra_h < 24.0f);
        check_true("dec range", s->dec_deg >= -90.0f && s->dec_deg <= 90.0f);
        check_true("mag range", s->mag >= -1.5f && s->mag <= 5.0f);
        if (s->mag < SKY_STARS[sirius].mag) {
            sirius = i;
        }
        for (int j = i + 1; j < SKY_STAR_N; j++) {
            check_true("no duplicate position",
                       !(s->ra_h == SKY_STARS[j].ra_h && s->dec_deg == SKY_STARS[j].dec_deg));
        }
    }
    check_true("brightest is Sirius", fabsf(SKY_STARS[sirius].mag + 1.46f) < 0.001f);
    check_true("Sirius ra", fabsf(SKY_STARS[sirius].ra_h - 6.7525f) < 0.0001f);
    check_true("tint breaks", sky_star_tint(-0.1f) == 0 && sky_star_tint(0.2f) == 1 && sky_star_tint(0.5f) == 2 &&
                              sky_star_tint(1.0f) == 3 && sky_star_tint(1.85f) == 4);
}

static void test_polaris_at_pole(void)
{
    int p = -1;
    const float lsts[4] = { 0.0f, 6.0f, 12.0f, 18.0f };
    for (int i = 0; i < SKY_STAR_N; i++) {
        if (SKY_STARS[i].dec_deg > 89.0f) {
            p = i;
        }
    }
    check_true("polaris found", p >= 0);
    if (p < 0) {
        return;
    }
    for (int k = 0; k < 4; k++) {
        sky_m3_t m = sky_eq_to_world(lsts[k], 41.9f);
        sky_v3_t w = sky_m3_apply(&m, sky_eq_vec(SKY_STARS[p].ra_h, SKY_STARS[p].dec_deg));
        float alt, az;
        sky_vec_to_altaz(w, &alt, &az);
        check_true("polaris altitude", fabsf(alt - 41.9f) <= 0.8f);
        check_true("polaris azimuth", fabsf(sky_wrap180(az)) <= 1.2f);
    }
    {
        sky_m3_t m = sky_eq_to_world(3.0f, -30.0f);
        sky_v3_t w = sky_m3_apply(&m, sky_eq_vec(SKY_STARS[p].ra_h, SKY_STARS[p].dec_deg));
        float alt, az;
        sky_vec_to_altaz(w, &alt, &az);
        check_true("polaris south altitude", fabsf(alt + 30.0f) <= 0.8f);
    }
}

static void test_matrix_matches_hadec(void)
{
    const float lsts[5] = { 0.0f, 3.3f, 9.7f, 15.2f, 22.9f };
    const float lats[5] = { 41.9f, -33.5f, 0.0f, 65.0f, -80.0f };
    for (int a = 0; a < 5; a++) {
        for (int b = 0; b < 5; b++) {
            sky_m3_t m = sky_eq_to_world(lsts[a], lats[b]);
            for (int i = 0; i < SKY_STAR_N; i += 12) {
                const sky_star_t *s = &SKY_STARS[i];
                sky_v3_t g = sky_m3_apply(&m, sky_eq_vec(s->ra_h, s->dec_deg));
                sky_v3_t e = sky_hadec_to_vec(sky_ha_deg(lsts[a], s->ra_h), s->dec_deg, lats[b]);
                check_true("matrix x", fabsf(g.x - e.x) < 1e-4f);
                check_true("matrix y", fabsf(g.y - e.y) < 1e-4f);
                check_true("matrix z", fabsf(g.z - e.z) < 1e-4f);
            }
        }
    }
}

static void test_known_separations(void)
{
    int dubhe = find_near(11.0621f, 61.751f);
    int merak = find_near(11.0307f, 56.382f);
    int betel = find_near(5.9195f, 7.407f);
    int rigel = find_near(5.2423f, -8.202f);
    float d1, d2;
    check_true("named stars found", dubhe >= 0 && merak >= 0 && betel >= 0 && rigel >= 0);
    if (dubhe < 0 || merak < 0 || betel < 0 || rigel < 0) {
        return;
    }
    d1 = sky_radec_sep_deg(SKY_STARS[dubhe].ra_h, SKY_STARS[dubhe].dec_deg,
                           SKY_STARS[merak].ra_h, SKY_STARS[merak].dec_deg);
    d2 = sky_radec_sep_deg(SKY_STARS[betel].ra_h, SKY_STARS[betel].dec_deg,
                           SKY_STARS[rigel].ra_h, SKY_STARS[rigel].dec_deg);
    check_true("Dubhe Merak 5.4", fabsf(d1 - 5.4f) <= 0.2f);
    check_true("Betelgeuse Rigel 18.6", fabsf(d2 - 18.6f) <= 0.4f);
}

static void test_figures(void)
{
    int segs = 0;
    unsigned seen = 0;
    float ra, dec;
    check_true("run count 31", SKY_FIG_RUN_N == 31);
    for (int i = 0; i < SKY_FIG_RUN_N; i++) {
        const sky_fig_run_t *r = &SKY_FIG_RUNS[i];
        check_true("run has 2+ stars", r->n >= 2);
        check_true("fig id", r->fig < SKY_FIG_N);
        seen |= 1u << r->fig;
        segs += r->n - 1;
        for (int j = 0; j < r->n; j++) {
            check_true("index in range", SKY_FIG_IDX[r->first + j] < SKY_STAR_N);
        }
        for (int j = 1; j < r->n; j++) {
            const sky_star_t *a = &SKY_STARS[SKY_FIG_IDX[r->first + j - 1]];
            const sky_star_t *b = &SKY_STARS[SKY_FIG_IDX[r->first + j]];
            check_true("segment under 40 deg", sky_radec_sep_deg(a->ra_h, a->dec_deg, b->ra_h, b->dec_deg) <= 40.0f);
        }
    }
    check_true("107 segments", segs == 107);
    check_true("index table size", SKY_FIG_RUNS[SKY_FIG_RUN_N - 1].first + SKY_FIG_RUNS[SKY_FIG_RUN_N - 1].n == 138);
    check_true("fig ids cover 0..15", seen == 0xFFFFu);
    sky_fig_centre(0, &ra, &dec);
    check_true("Big Dipper dec 50..62", dec >= 50.0f && dec <= 62.0f);
}

int main(void)
{
    test_table_shape();
    test_polaris_at_pole();
    test_matrix_matches_hadec();
    test_known_separations();
    test_figures();
    printf("sky_stars: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
