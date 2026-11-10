/* Host test for main/sky_scene_math.h -- the pure geometry and camera logic
 * behind NINA page layouts 8 and 9: sky directions, sidereal time, the horizon
 * outline, the target's path, the equatorial mount pose, the telescope model,
 * the pinhole camera fit, edge arrows, picture counts and the activity word.
 * Header-only, no ESP-IDF dependency; assert-style like test_adsb_geom.c.
 *
 * Reference numbers were computed in double precision with python and are
 * asserted with tolerances wide enough for the float-only implementation.
 */
#include "sky_scene_math.h"
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
    printf("%-66s got=%-12ld expect=%-12ld %s\n", label,
           (long)lroundf(got * 1000.0f), (long)lroundf(expect * 1000.0f),
           ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void check_true(const char *label, int got)
{
    checks++;
    printf("%-66s %s\n", label, got ? "OK" : "FAIL");
    if (!got) fails++;
}

static void check_v3(const char *label, sky_v3_t got, float x, float y, float z, float tol)
{
    char buf[96];
    snprintf(buf, sizeof buf, "%s .x", label);
    check_near(buf, got.x, x, tol);
    snprintf(buf, sizeof buf, "%s .y", label);
    check_near(buf, got.y, y, tol);
    snprintf(buf, sizeof buf, "%s .z", label);
    check_near(buf, got.z, z, tol);
}

static sky_frame_t rect_frame(float x0, float y0, float x1, float y1, float px, float py)
{
    sky_frame_t f;
    memset(&f, 0, sizeof f);
    f.round = false;
    f.x0 = x0;
    f.y0 = y0;
    f.x1 = x1;
    f.y1 = y1;
    f.px = px;
    f.py = py;
    return f;
}

static sky_frame_t disc_frame(float cx, float cy, float r, float px, float py)
{
    sky_frame_t f;
    memset(&f, 0, sizeof f);
    f.round = true;
    f.cx = cx;
    f.cy = cy;
    f.r = r;
    f.px = px;
    f.py = py;
    return f;
}

static sky_cam_t make_cam(float yaw, float pitch, float focal, const sky_frame_t *fr)
{
    sky_cam_pose_t p = { yaw, pitch, focal };
    sky_cam_t c;
    sky_cam_setup(&c, &p, fr);
    return c;
}

/* Every given direction and point projects inside @p fr shrunk by @p m. */
static int all_inside(const sky_cam_pose_t *pose, const sky_frame_t *fr,
                      const sky_v3_t *dirs, int nd, const sky_v3_t *pts, int np, float m)
{
    sky_cam_t c;
    sky_cam_setup(&c, pose, fr);
    for (int i = 0; i < nd + np; i++) {
        sky_v3_t v = (i < nd) ? sky_cam_dir(&c, dirs[i]) : sky_cam_point(&c, pts[i - nd]);
        float sx, sy;
        if (!sky_cam_screen(&c, v, &sx, &sy)) return 0;
        if (!sky_frame_contains(fr, sx, sy, m)) return 0;
    }
    return 1;
}

static void test_directions_time(void)
{
    float alt, az;
    sky_v3_t v = sky_hadec_to_vec(0.0f, -5.39f, 41.9f);
    sky_vec_to_altaz(v, &alt, &az);
    check_near("hadec(0,-5.39,41.9) alt", alt, 42.71f, 0.01f);
    check_near("hadec(0,-5.39,41.9) az", az, 180.0f, 0.01f);
    check_v3("hadec(0,-5.39,41.9) vec", v, 0.0f, -0.7348f, 0.6783f, 1e-3f);

    sky_vec_to_altaz(sky_hadec_to_vec(-30.0f, -5.39f, 41.9f), &alt, &az);
    check_near("hadec(-30,-5.39) alt", alt, 35.381f, 0.02f);
    check_near("hadec(-30,-5.39) az", az, 142.371f, 0.02f);
    sky_vec_to_altaz(sky_hadec_to_vec(45.0f, 20.0f, 41.9f), &alt, &az);
    check_near("hadec(45,20) alt", alt, 46.301f, 0.02f);
    check_near("hadec(45,20) az", az, 254.108f, 0.02f);
    sky_vec_to_altaz(sky_hadec_to_vec(0.0f, 70.0f, 41.9f), &alt, &az);
    check_near("hadec(0,70) alt", alt, 61.9f, 0.02f);
    check_near("hadec(0,70) az (wrap180)", sky_wrap180(az), 0.0f, 0.02f);
    sky_vec_to_altaz(sky_hadec_to_vec(180.0f, 70.0f, 41.9f), &alt, &az);
    check_near("hadec(180,70) alt", alt, 21.9f, 0.02f);
    check_near("hadec(0,41.9,41.9) z", sky_hadec_to_vec(0.0f, 41.9f, 41.9f).z, 1.0f, 1e-5f);
    check_v3("hadec(-90,0,41.9)", sky_hadec_to_vec(-90.0f, 0.0f, 41.9f), 1.0f, 0.0f, 0.0f, 1e-5f);

    check_v3("pole(41.9)", sky_pole(41.9f), 0.0f, 0.74431f, 0.66783f, 1e-4f);
    check_v3("pole(-30)", sky_pole(-30.0f), 0.0f, -0.86603f, 0.5f, 1e-4f);

    check_true("sep(same) < 0.01",
               sky_sep_deg(sky_altaz_to_vec(10.0f, 0.0f), sky_altaz_to_vec(10.0f, 0.0f)) < 0.01f);
    check_near("sep(x,y) = 90", sky_sep_deg(sky_v3(1, 0, 0), sky_v3(0, 1, 0)), 90.0f, 1e-3f);

    check_near("ha_deg(2,23) = 45", sky_ha_deg(2.0f, 23.0f), 45.0f, 1e-3f);
    check_near("ha_deg(23,2) = -45", sky_ha_deg(23.0f, 2.0f), -45.0f, 1e-3f);
    check_near("ha_deg(12,0) = 180", sky_ha_deg(12.0f, 0.0f), 180.0f, 1e-3f);

    check_near("lst_hours(J2000, 0)", sky_lst_hours(946728000LL, 0.0f), 18.6974f, 1e-3f);
    check_near("lst_hours(2026-11-08 03:00Z, -87.6)", sky_lst_hours(1794106800LL, -87.6f), 0.3147f, 2e-3f);
    check_near("lst_advance(23.9, 360 s)", sky_lst_advance(23.9f, 360.0f), 0.000274f, 1e-4f);

    check_near("hours_until_ha(-100, -85.14)", sky_hours_until_ha(-100.0f, -85.14f), 0.98796f, 1e-3f);
    check_near("hours_until_ha(170, -170) wraps", sky_hours_until_ha(170.0f, -170.0f), 1.32971f, 1e-3f);
    check_near("hours_until_ha(10, 5) wraps the long way", sky_hours_until_ha(10.0f, 5.0f), 23.6021f, 2e-3f);
    check_near("ha_after_minutes(-10, 60)", sky_ha_after_minutes(-10.0f, 60.0f), 5.041f, 1e-3f);
    check_near("ha_after_minutes(178, 60) wraps", sky_ha_after_minutes(178.0f, 60.0f), -166.959f, 1e-3f);

    check_near("smoothstep(0)", sky_smoothstep(0.0f), 0.0f, 1e-6f);
    check_near("smoothstep(1)", sky_smoothstep(1.0f), 1.0f, 1e-6f);
    check_near("smoothstep(0.5)", sky_smoothstep(0.5f), 0.5f, 1e-6f);
    check_near("smoothstep(0.25)", sky_smoothstep(0.25f), 0.15625f, 1e-6f);
    check_near("smoothstep(-1)", sky_smoothstep(-1.0f), 0.0f, 1e-6f);
    check_near("smoothstep(2)", sky_smoothstep(2.0f), 1.0f, 1e-6f);

    sky_cam_pose_t a = { 350.0f, 0.0f, 300.0f }, b = { 10.0f, 20.0f, 500.0f };
    sky_cam_pose_t m = sky_cam_pose_lerp(a, b, 0.5f);
    check_near("pose_lerp yaw (wrap180)", sky_wrap180(m.yaw_deg), 0.0f, 1e-3f);
    check_near("pose_lerp pitch", m.pitch_deg, 10.0f, 1e-3f);
    check_near("pose_lerp focal", m.focal_px, 400.0f, 1e-3f);
}

static int8_t s_h1[SKY_HORIZON_N];

static void test_horizon(void)
{
    const float az[4] = { 0, 90, 180, 270 }, alt[4] = { 0, 10, 20, 10 };
    check_true("resample 4 points returns true", sky_horizon_resample(az, alt, 4, s_h1));
    check_true("out[0]=0", s_h1[0] == 0);
    check_true("out[1]=1", s_h1[1] == 1);
    check_true("out[9]=5", s_h1[9] == 5);
    check_true("out[18]=10", s_h1[18] == 10);
    check_true("out[36]=20", s_h1[36] == 20);
    check_true("out[54]=10", s_h1[54] == 10);
    check_true("out[63]=5", s_h1[63] == 5);
    check_true("out[71]=1", s_h1[71] == 1);

    int8_t h2[SKY_HORIZON_N];
    const float az2[4] = { 180, 0, 270, 90 }, alt2[4] = { 20, 0, 10, 10 };
    sky_horizon_resample(az2, alt2, 4, h2);
    check_true("shuffled input gives identical outline", memcmp(s_h1, h2, sizeof h2) == 0);

    const float az1 = 123.0f, alt1 = 7.0f;
    check_true("n=1 returns true", sky_horizon_resample(&az1, &alt1, 1, h2));
    int all7 = 1;
    for (int i = 0; i < SKY_HORIZON_N; i++) all7 &= (h2[i] == 7);
    check_true("n=1 all 72 outputs 7", all7);

    memset(h2, 5, sizeof h2);
    check_true("n=0 returns false", !sky_horizon_resample(az, alt, 0, h2));
    int all0 = 1;
    for (int i = 0; i < SKY_HORIZON_N; i++) all0 &= (h2[i] == 0);
    check_true("n=0 all outputs 0", all0);
    check_true("NULL array returns false", !sky_horizon_resample((const float *)0, alt, 4, h2));

    const float az_c[2] = { 0, 180 }, alt_c[2] = { 95, -30 };
    sky_horizon_resample(az_c, alt_c, 2, h2);
    check_true("alt 95 clamps to 89", h2[0] == 89);
    check_true("alt -30 clamps to -10", h2[36] == -10);

    /* Wrap at 0/360: inputs either side of north, one given as -10. */
    const float az_w[2] = { -10, 10 }, alt_w[2] = { 10, 20 };
    sky_horizon_resample(az_w, alt_w, 2, h2);
    check_true("resample across north: out[0]=15", h2[0] == 15);
    check_true("resample across north: out[71]=13 (az 355)", h2[71] == 13);
    check_true("resample across north: out[36]=15 (az 180, midway back)", h2[36] == 15);

    /* Duplicate azimuths: the first wins. */
    const float az_d[2] = { 90, 90 }, alt_d[2] = { 4, 30 };
    sky_horizon_resample(az_d, alt_d, 2, h2);
    check_true("duplicate azimuth: first wins", h2[18] == 4 && h2[0] == 4);

    check_near("horizon_alt(47.5)", sky_horizon_alt(s_h1, true, 47.5f), 5.5f, 1e-4f);
    check_near("horizon_alt invalid", sky_horizon_alt(s_h1, false, 47.5f), 0.0f, 1e-6f);
    check_near("horizon_alt(357.5) wraps 355->0", sky_horizon_alt(s_h1, true, 357.5f), 0.5f, 1e-4f);
    check_near("horizon_alt(-2.5) wraps", sky_horizon_alt(s_h1, true, -2.5f), 0.5f, 1e-4f);
    check_near("horizon_alt(360) = out[0]", sky_horizon_alt(s_h1, true, 360.0f), 0.0f, 1e-4f);

    /* Smooth outline over a real 18-point NINA profile (walls to 49, a gap to 3). */
    const float az_r[18] = { 0, 3.76f, 19.35f, 21.71f, 33.83f, 38.65f, 42.58f, 72.01f, 156.07f,
                             156.39f, 175.54f, 195.75f, 213.9f, 232.15f, 266.44f, 277.04f,
                             298.37f, 360 };
    const float alt_r[18] = { 22.54f, 22.54f, 5.88f, 21.46f, 13.52f, 22.19f, 2.71f, 48.71f,
                              46.06f, 23.61f, 8.54f, 9.06f, 28.11f, 36.89f, 35.07f, 42.87f,
                              48.76f, 22.54f };
    int8_t hr[SKY_HORIZON_N];
    sky_horizon_resample(az_r, alt_r, 18, hr);
    int exact = 1, bounded = 1, cont = 1;
    for (int i = 0; i < SKY_HORIZON_N; i++) {
        const float az0 = SKY_HORIZON_STEP_DEG * (float)i;
        if (fabsf(sky_horizon_alt_smooth(hr, true, az0) - (float)hr[i]) > 1e-4f) exact = 0;
        const float lo = (float)(hr[i] < hr[(i + 1) % SKY_HORIZON_N] ? hr[i] : hr[(i + 1) % SKY_HORIZON_N]);
        const float hi = (float)(hr[i] > hr[(i + 1) % SKY_HORIZON_N] ? hr[i] : hr[(i + 1) % SKY_HORIZON_N]);
        for (int k = 1; k < 20; k++) {
            const float v = sky_horizon_alt_smooth(hr, true, az0 + 0.25f * (float)k);
            if (v < lo - 1e-3f || v > hi + 1e-3f) bounded = 0;
        }
        if (fabsf(sky_horizon_alt_smooth(hr, true, az0 - 0.01f) -
                  sky_horizon_alt_smooth(hr, true, az0 + 0.01f)) > 0.2f) cont = 0;
    }
    check_true("smooth: exact at all 72 samples (real profile)", exact);
    check_true("smooth: never leaves the two neighbouring samples", bounded);
    check_true("smooth: continuous across every sample", cont);
    check_near("smooth invalid -> 0", sky_horizon_alt_smooth(hr, false, 40.0f), 0.0f, 1e-6f);
    check_near("smooth NULL -> 0", sky_horizon_alt_smooth((const int8_t *)0, true, 40.0f), 0.0f, 1e-6f);
    check_near("smooth wraps: -2.5 == 357.5", sky_horizon_alt_smooth(hr, true, -2.5f),
               sky_horizon_alt_smooth(hr, true, 357.5f), 1e-4f);

    int8_t ramp[SKY_HORIZON_N];
    for (int i = 0; i < SKY_HORIZON_N; i++) ramp[i] = (int8_t)(i <= 36 ? i : 72 - i);
    check_near("smooth keeps a straight run straight (az 52.5)",
               sky_horizon_alt_smooth(ramp, true, 52.5f), 10.5f, 1e-4f);
    check_true("smooth rounds the tent top (az 182.5 below 36, above 35)",
               sky_horizon_alt_smooth(ramp, true, 182.5f) < 36.0f &&
               sky_horizon_alt_smooth(ramp, true, 182.5f) > 35.0f);
    memset(ramp, 7, sizeof ramp);
    check_near("smooth flat stays flat", sky_horizon_alt_smooth(ramp, true, 123.4f), 7.0f, 1e-4f);
}

static sky_path_t s_path;

static void test_path(void)
{
    sky_v3_t dirs[SKY_PATH_N];

    sky_path_build(&s_path, -5.39f, 41.9f, 0.0f, (const int8_t *)0, false);
    check_true("flat dec -5.39: has_rise && has_set", s_path.has_rise && s_path.has_set);
    check_true("flat dec -5.39: not always/never up", !s_path.always_up && !s_path.never_up);
    check_near("flat rise", s_path.rise_ha_deg, -85.14f, 0.1f);
    check_near("flat set", s_path.set_ha_deg, 85.14f, 0.1f);
    check_near("flat mid", s_path.mid_ha_deg, 0.0f, 0.1f);
    check_true("up[36]=1 up[0]=0 up[18]=0 up[19]=1",
               s_path.up[36] == 1 && s_path.up[0] == 0 && s_path.up[18] == 0 && s_path.up[19] == 1);
    check_true("run 19..53", s_path.run_first == 19 && s_path.run_last == 53);
    check_true("work_dirs 35", sky_path_work_dirs(&s_path, dirs, SKY_PATH_N) == 35);
    check_true("work_dirs max 4", sky_path_work_dirs(&s_path, dirs, 4) == 4);

    sky_path_build(&s_path, -5.39f, 41.9f, 120.0f, (const int8_t *)0, false);
    check_near("ha_now 120: next run rise", s_path.rise_ha_deg, -85.14f, 0.1f);
    check_near("ha_now 120: next run set", s_path.set_ha_deg, 85.14f, 0.1f);
    check_near("ha_now 120: mid", s_path.mid_ha_deg, 0.0f, 0.1f);

    int8_t h10[SKY_HORIZON_N];
    memset(h10, 10, sizeof h10);
    sky_path_build(&s_path, -5.39f, 41.9f, 0.0f, h10, true);
    check_near("10 deg horizon rise", s_path.rise_ha_deg, -71.44f, 0.1f);
    check_near("10 deg horizon set", s_path.set_ha_deg, 71.44f, 0.1f);
    check_true("10 deg horizon run 22..50", s_path.run_first == 22 && s_path.run_last == 50);

    /* Circumpolar. */
    sky_path_build(&s_path, 70.0f, 41.9f, 33.0f, (const int8_t *)0, false);
    check_true("dec 70 always_up, no crossings",
               s_path.always_up && !s_path.has_rise && !s_path.has_set);
    check_near("dec 70 mid = upper culmination", s_path.mid_ha_deg, 0.0f, 1e-6f);
    check_true("dec 70 run 0..72", s_path.run_first == 0 && s_path.run_last == 72);
    check_true("dec 70 work_dirs 73", sky_path_work_dirs(&s_path, dirs, SKY_PATH_N) == 73);

    sky_path_build(&s_path, -60.0f, 41.9f, 0.0f, (const int8_t *)0, false);
    check_true("dec -60 never_up, run -1",
               s_path.never_up && s_path.run_first == -1 && s_path.run_last == -1);
    check_true("dec -60 work_dirs 0", sky_path_work_dirs(&s_path, dirs, SKY_PATH_N) == 0);

    /* A run that wraps past HA 180: dec 60 at lat 41.9 culminates twice at
     * az 0 (alt 71.9 and 11.9) and swings out to about az +-42 between. An
     * outline open only within 20 degrees of north splits the circle into two
     * up runs, one around each culmination; at ha_now 180 the working run is
     * the lower one, which crosses HA 180. */
    int8_t hn[SKY_HORIZON_N];
    for (int i = 0; i < SKY_HORIZON_N; i++) {
        float a = sky_wrap180(5.0f * (float)i);
        hn[i] = (a > -20.0f && a < 20.0f) ? 0 : 89;
    }
    sky_path_build(&s_path, 60.0f, 41.9f, 180.0f, hn, true);
    check_true("wrapped run: crossings exist", s_path.has_rise && s_path.has_set);
    check_true("wrapped run: run_last < run_first", s_path.run_last < s_path.run_first);
    check_near("wrapped run: mid at lower culmination", fabsf(s_path.mid_ha_deg), 180.0f, 1.0f);
    {
        int cnt = sky_path_work_dirs(&s_path, dirs, SKY_PATH_N);
        int expect = (SKY_PATH_N - 1 - s_path.run_first) + s_path.run_last + 1;
        check_true("wrapped run: work_dirs count", cnt == expect && cnt > 0);
    }

    /* Near-overhead target (dec = lat): passes 0.1 degrees from the zenith. */
    sky_path_build(&s_path, 41.8f, 41.9f, 0.0f, (const int8_t *)0, false);
    {
        float alt, az;
        sky_vec_to_altaz(s_path.dir[36], &alt, &az);
        check_near("near-overhead target alt at HA 0", alt, 89.9f, 0.02f);
        check_true("near-overhead target rises and sets", s_path.has_rise && s_path.has_set);
        check_near("near-overhead target mid", s_path.mid_ha_deg, 0.0f, 0.1f);
    }
}

/* M 81 from the user's localhost NINA profile (2026-11-09 18:30 CST): a
 * jagged north outline splits the low pass into a 4-sample sliver and the
 * main run. The working run is the sliver, so the camera must frame every up
 * sample instead, and look at the highest one. */
static void test_path_sliver(void)
{
    const float az[] = {0, 4, 12, 13, 16, 27, 34, 39, 41, 46, 80, 101, 125, 148, 175, 176, 194, 199,
                        210, 229, 234, 263, 284, 294, 326, 343, 358, 360};
    const float al[] = {27, 8, 25, 14, 22, 4, 7, 14, 26, 34, 50, 58, 61, 54, 42, 19, 14, 9, 9, 24,
                        33, 32, 45, 50, 41, 27, 27, 27};
    int8_t h[SKY_HORIZON_N];
    check_true("sliver: horizon resampled", sky_horizon_resample(az, al, 28, h));
    const float lat = 38.7582721f, dec = 69.0653f;
    const float ha_now = sky_ha_deg(21.692f, 9.92588f);   /* 176.5: just before lower culmination */
    sky_path_build(&s_path, dec, lat, ha_now, h, true);
    /* Raw pattern 0011110111...1110000: the single dip at index 6 is bridged,
     * so the run is 2..54 and the sliver 2..5 is part of it. */
    check_true("sliver: up[6] bridged", s_path.up[6] == 1 && s_path.up[1] == 0 && s_path.up[55] == 0);
    check_true("sliver: working run is 2..54", s_path.run_first == 2 && s_path.run_last == 54);
    check_near("sliver: rise 19:09 kept", s_path.rise_ha_deg, -172.9f, 0.2f);
    sky_v3_t dirs[SKY_PATH_N + 2];
    check_true("sliver: work_dirs 53", sky_path_work_dirs(&s_path, dirs, SKY_PATH_N) == 53);
    const int n = sky_path_up_dirs(&s_path, dirs, SKY_PATH_N);
    check_true("sliver: up_dirs 53", n == 53);
    check_near("sliver: up yaw = upper culmination (north)",
               sky_wrap180(sky_cam_up_yaw(&s_path, dec, lat)), 0.0f, 0.5f);

    /* The fit over the up samples sees the whole arc; over the sliver it zooms. */
    sky_frame_t fr = {0};
    fr.x0 = 24.0f; fr.y0 = 24.0f; fr.x1 = 696.0f; fr.y1 = 696.0f; fr.px = 360.0f; fr.py = 360.0f;
    const sky_v3_t tgt = sky_hadec_to_vec(ha_now, dec, lat);
    sky_pose_t pose = sky_mount_pose(lat, tgt, SKY_PIER_EAST, ha_now);
    sky_v3_t pts[SKY_RIG_FIT_N];
    const int np = sky_rig_fit_points(&pose, pts);
    sky_cam_pose_t cam;
    dirs[n] = tgt;
    sky_cam_fit(&cam, sky_cam_up_yaw(&s_path, dec, lat), &fr, dirs, n + 1, pts, np, 75.0f, 0.0f);
    check_true("sliver: up fit is wide (focal < 320)", cam.focal_px < 320.0f);
    check_true("sliver: up fit pitch 10..18", cam.pitch_deg > 10.0f && cam.pitch_deg < 18.0f);
    int m = sky_path_work_dirs(&s_path, dirs, SKY_PATH_N);
    dirs[m] = tgt;
    sky_cam_fit(&cam, sky_cam_work_yaw(&s_path, dec, lat), &fr, dirs, m + 1, pts, np, 75.0f, 0.0f);
    check_true("sliver: run fit is wide too (focal < 320)", cam.focal_px < 320.0f);

    /* Run selection on synthetic patterns (n = 12). */
    {
        int f = -1, l = -1;
        /* now at 3 (down): the 2-sample sliver at 1..2 is behind, the next
         * start forward is the 5-run at 7..11 -> that one. */
        const uint8_t a[12] = {0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1};
        sky_path_pick_run_(a, 12, 3, &f, &l);
        check_true("pick: long run ahead", f == 7 && l == 11);
        /* now at 0 (down): the sliver 1..2 comes first but is short, the
         * 5-run further on qualifies -> 7..11, not 1..2. */
        sky_path_pick_run_(a, 12, 0, &f, &l);
        check_true("pick: skip the sliver", f == 7 && l == 11);
        /* only slivers: the longest (3) wins over the nearer (2). */
        const uint8_t b[12] = {0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0};
        sky_path_pick_run_(b, 12, 0, &f, &l);
        check_true("pick: longest sliver", f == 5 && l == 7);
        /* up now: the containing run, wrapping past the end. */
        const uint8_t c[12] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1};
        sky_path_pick_run_(c, 12, 11, &f, &l);
        check_true("pick: containing run wraps", f == 10 && l == 1);
        /* bridge: one dip closes, two do not. */
        uint8_t d[12] = {1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1};
        sky_path_bridge_(d, 12);
        check_true("bridge: single dip closed, double kept",
                   d[2] == 1 && d[5] == 0 && d[6] == 0);
    }

    /* Nothing up: up_dirs 0 and the yaw falls back to the run rule. */
    sky_path_build(&s_path, -60.0f, 41.9f, 0.0f, (const int8_t *)0, false);
    check_true("never up: up_dirs 0", sky_path_up_dirs(&s_path, dirs, SKY_PATH_N) == 0);
    check_near("never up: up yaw = work yaw", sky_cam_up_yaw(&s_path, -60.0f, 41.9f),
               sky_cam_work_yaw(&s_path, -60.0f, 41.9f), 1e-6f);
}

static void test_mount(void)
{
    const float lat = 41.9f;
    sky_pose_t r = sky_mount_rest_pose(lat);
    check_v3("rest polar", r.polar, 0.0f, 0.74431f, 0.66783f, 1e-4f);
    check_v3("rest tube", r.tube, 0.0f, 0.74431f, 0.66783f, 1e-4f);
    check_v3("rest decl", r.decl, 0.0f, -0.66783f, 0.74431f, 1e-4f);
    check_v3("rest weight", r.weight, 0.0f, 0.66783f, -0.74431f, 1e-4f);

    sky_pose_t p = sky_mount_pose(lat, sky_hadec_to_vec(90.0f, 0.0f, lat), SKY_PIER_EAST, 90.0f);
    check_v3("HA +90 EAST weight", p.weight, 0.0f, 0.6678f, -0.7443f, 1e-3f);
    p = sky_mount_pose(lat, sky_hadec_to_vec(-90.0f, 0.0f, lat), SKY_PIER_WEST, -90.0f);
    check_v3("HA -90 WEST weight", p.weight, 0.0f, 0.6678f, -0.7443f, 1e-3f);
    p = sky_mount_pose(lat, sky_hadec_to_vec(1.0f, -5.39f, lat), SKY_PIER_EAST, 1.0f);
    check_v3("HA 1 EAST decl", p.decl, 0.9998f, -0.0117f, 0.0130f, 2e-3f);

    {
        sky_v3_t t = sky_hadec_to_vec(30.0f, 20.0f, lat);
        sky_pose_t u = sky_mount_pose(lat, t, SKY_PIER_UNKNOWN, 30.0f);
        sky_pose_t e = sky_mount_pose(lat, t, SKY_PIER_EAST, 30.0f);
        check_true("UNKNOWN ha +30 == EAST", memcmp(&u, &e, sizeof u) == 0);
        t = sky_hadec_to_vec(-30.0f, 20.0f, lat);
        u = sky_mount_pose(lat, t, SKY_PIER_UNKNOWN, -30.0f);
        e = sky_mount_pose(lat, t, SKY_PIER_WEST, -30.0f);
        check_true("UNKNOWN ha -30 == WEST", memcmp(&u, &e, sizeof u) == 0);
    }

    /* Sweep, both hemispheres, both sides. */
    const float lats[2] = { 41.9f, -30.0f };
    for (int li = 0; li < 2; li++) {
        const float la = lats[li];
        const sky_v3_t north = sky_v3(0.0f, cosf(la * SKY_DEG2RAD), sinf(la * SKY_DEG2RAD));
        int ok_perp = 1, ok_tube = 1, ok_unit = 1, ok_neg = 1, ok_same = 1, ok_opp = 1, ok_down = 1;
        int ok_polar = 1;
        for (int ha = -170; ha <= 170; ha += 10) {
            for (int dec = -30; dec <= 80; dec += 10) {
                for (int side = 1; side <= 2; side++) {
                    sky_v3_t t = sky_hadec_to_vec((float)ha, (float)dec, la);
                    sky_pose_t q = sky_mount_pose(la, t, (sky_pier_t)side, (float)ha);
                    if (fabsf(sky_v3_dot(q.weight, north)) > 1e-4f) ok_perp = 0;
                    if (fabsf(sky_v3_dot(q.weight, q.tube)) > 1e-4f) ok_tube = 0;
                    if (fabsf(sky_v3_len(q.weight) - 1.0f) > 1e-4f) ok_unit = 0;
                    if (sky_v3_len(sky_v3_add(q.weight, q.decl)) > 1e-6f) ok_neg = 0;
                    if (sky_v3_len(sky_v3_sub(q.tube, t)) > 1e-6f) ok_same = 0;
                    if (fabsf(sky_v3_dot(q.weight, q.decl) + 1.0f) > 1e-4f) ok_opp = 0;
                    if (sky_v3_len(sky_v3_sub(q.polar, sky_pole(la))) > 1e-6f) ok_polar = 0;
                    /* Normal state: EAST west of the meridian, WEST east of it. */
                    const int normal = (side == SKY_PIER_EAST) ? (ha > 0) : (ha < 0);
                    if (normal && q.weight.z >= 0.0f) ok_down = 0;
                }
            }
        }
        char buf[96];
        snprintf(buf, sizeof buf, "lat %d sweep: weight at right angles to first axis", (int)la);
        check_true(buf, ok_perp);
        snprintf(buf, sizeof buf, "lat %d sweep: weight at right angles to tube", (int)la);
        check_true(buf, ok_tube);
        snprintf(buf, sizeof buf, "lat %d sweep: |weight| = 1", (int)la);
        check_true(buf, ok_unit);
        snprintf(buf, sizeof buf, "lat %d sweep: weight = -decl", (int)la);
        check_true(buf, ok_neg);
        snprintf(buf, sizeof buf, "lat %d sweep: tube and weight on opposite sides", (int)la);
        check_true(buf, ok_opp);
        snprintf(buf, sizeof buf, "lat %d sweep: tube = pointing direction", (int)la);
        check_true(buf, ok_same);
        snprintf(buf, sizeof buf, "lat %d sweep: polar = elevated pole", (int)la);
        check_true(buf, ok_polar);
        snprintf(buf, sizeof buf, "lat %d sweep: normal state weight hangs down", (int)la);
        check_true(buf, ok_down);
    }

    /* Tube on the pole (NINA home: Dec 90, Alt = lat, Az 0) is the rest pose,
     * counterweight straight down, on either side of pier. */
    {
        const sky_pose_t rest = sky_mount_rest_pose(lat);
        p = sky_mount_pose(lat, sky_pole(lat), SKY_PIER_EAST, 0.0f);
        check_v3("degenerate tube = pole, EAST decl", p.decl, rest.decl.x, rest.decl.y, rest.decl.z, 1e-6f);
        check_true("degenerate tube = pole, EAST weight down", p.weight.z < 0.0f);
        p = sky_mount_pose(lat, sky_pole(lat), SKY_PIER_WEST, 0.0f);
        check_v3("degenerate tube = pole, WEST decl", p.decl, rest.decl.x, rest.decl.y, rest.decl.z, 1e-6f);
        /* NINA's own numbers at home: Alt = lat, Az 360. */
        p = sky_mount_pose(38.758f, sky_altaz_to_vec(38.758f, 360.0f), SKY_PIER_EAST, 90.0f);
        check_true("NINA home alt/az: weight down", p.weight.z < -0.7f && fabsf(p.weight.x) < 1e-3f);
    }

    p = sky_mount_pose(-30.0f, sky_hadec_to_vec(90.0f, 0.0f, -30.0f), SKY_PIER_EAST, 90.0f);
    check_true("south HA +90 EAST weight.z < -0.5", p.weight.z < -0.5f);
    check_v3("south polar", p.polar, 0.0f, -0.86603f, 0.5f, 1e-4f);
    p = sky_mount_pose(-30.0f, sky_hadec_to_vec(-90.0f, -50.0f, -30.0f), SKY_PIER_WEST, -90.0f);
    check_true("south HA -90 WEST weight.z < -0.5", p.weight.z < -0.5f);
}

static void test_rig(void)
{
    sky_pose_t r = sky_mount_rest_pose(41.9f);
    sky_capsule_t c[SKY_RIG_CAPSULES];
    check_true("rig_capsules returns 8", sky_rig_capsules(&r, c) == SKY_RIG_CAPSULES);
    check_v3("pier foot at the origin", c[0].a, 0.0f, 0.0f, 0.0f, 1e-6f);
    check_v3("pier top under the polar housing", c[0].b, 0.0f, 0.0f, 1.00f, 1e-6f);
    const uint8_t parts[SKY_RIG_CAPSULES] = {
        SKY_PART_PIER, SKY_PART_MOUNT, SKY_PART_MOUNT,
        SKY_PART_SHAFT, SKY_PART_WEIGHT, SKY_PART_TUBE, SKY_PART_FRONT, SKY_PART_CAMERA,
    };
    int ok = 1;
    for (int i = 0; i < SKY_RIG_CAPSULES; i++) ok &= (c[i].part == parts[i]);
    check_true("part tags in documented order", ok);
    check_v3("tube capsule front = tube_front", c[5].b, 0.0f, 0.3894f, 2.0337f, 1e-3f);

    check_v3("tube_front(rest)", sky_rig_tube_front(&r), 0.0f, 0.3894f, 2.0337f, 1e-3f);
    sky_v3_t fp[SKY_RIG_FIT_N];
    check_true("fit_points returns 5", sky_rig_fit_points(&r, fp) == SKY_RIG_FIT_N);
    check_v3("fit point 4 (weight end)", fp[4], 0.0f, 0.5343f, 0.5246f, 1e-3f);
    check_v3("fit point 1 (axis crossing)", fp[1], 0.0f, 0.0f, 1.12f, 1e-6f);
    check_v3("fit point 0 = pier foot", fp[0], 0.0f, 0.0f, 0.0f, 1e-6f);
    check_v3("fit point 3 = camera capsule back", fp[3], c[7].b.x, c[7].b.y, c[7].b.z, 1e-5f);
}

static void test_camera(void)
{
    const sky_frame_t fr = rect_frame(24, 24, 696, 696, 360, 360);
    float sx, sy;

    check_v3("cam_pos(0)", sky_cam_pos(0.0f), 0.0f, -2.6f, 1.6f, 1e-5f);

    sky_cam_t c = make_cam(0.0f, 0.0f, 300.0f, &fr);
    check_true("y0p0: north projects", sky_cam_screen(&c, sky_cam_dir(&c, sky_v3(0, 1, 0)), &sx, &sy));
    check_near("y0p0: north sx", sx, 360.0f, 1e-3f);
    check_near("y0p0: north sy", sy, 360.0f, 1e-3f);
    sky_cam_screen(&c, sky_cam_dir(&c, sky_v3_norm(sky_v3(0.5f, 1, 0))), &sx, &sy);
    check_near("y0p0: norm(0.5,1,0) sx", sx, 510.0f, 1e-2f);
    check_near("y0p0: norm(0.5,1,0) sy", sy, 360.0f, 1e-2f);
    sky_cam_screen(&c, sky_cam_dir(&c, sky_v3_norm(sky_v3(0, 1, 1))), &sx, &sy);
    check_near("y0p0: norm(0,1,1) sx", sx, 360.0f, 1e-2f);
    check_near("y0p0: norm(0,1,1) sy", sy, 60.0f, 1e-2f);
    check_true("y0p0: east does not project", !sky_cam_screen(&c, sky_cam_dir(&c, sky_v3(1, 0, 0)), &sx, &sy));
    sky_cam_screen(&c, sky_cam_point(&c, sky_v3(0, 0, 1.12f)), &sx, &sy);
    check_near("y0p0: point O sx", sx, 360.0f, 0.05f);
    check_near("y0p0: point O sy", sy, 415.38f, 0.05f);

    c = make_cam(0.0f, 30.0f, 300.0f, &fr);
    sky_cam_screen(&c, sky_cam_dir(&c, sky_altaz_to_vec(30.0f, 0.0f)), &sx, &sy);
    check_near("y0p30: altaz(30,0) sx", sx, 360.0f, 1e-3f);
    check_near("y0p30: altaz(30,0) sy", sy, 360.0f, 1e-3f);

    c = make_cam(90.0f, 0.0f, 300.0f, &fr);
    sky_cam_screen(&c, sky_cam_dir(&c, sky_v3(1, 0, 0)), &sx, &sy);
    check_near("y90p0: east sx", sx, 360.0f, 1e-3f);
    check_near("y90p0: east sy", sy, 360.0f, 1e-3f);
    sky_cam_screen(&c, sky_cam_dir(&c, sky_v3_norm(sky_v3(1, -1, 0))), &sx, &sy);
    check_near("y90p0: south-east sx", sx, 660.0f, 1e-2f);
    check_near("y90p0: south-east sy", sy, 360.0f, 1e-2f);

    /* Focal limits. */
    check_near("focal_limit rect (1,0,1)", sky_focal_limit(&fr, sky_v3(1, 0, 1), 0.0f), 336.0f, 1e-2f);
    check_near("focal_limit rect (0,-0.5,1)", sky_focal_limit(&fr, sky_v3(0, -0.5f, 1), 0.0f), 672.0f, 1e-2f);
    check_true("focal_limit rect (0,0,1) = 1e9", sky_focal_limit(&fr, sky_v3(0, 0, 1), 0.0f) >= 1e9f);
    check_true("focal_limit rect (0,0,-1) = 1e9", sky_focal_limit(&fr, sky_v3(0, 0, -1), 0.0f) >= 1e9f);
    const sky_frame_t disc = disc_frame(360, 360, 340, 360, 360);
    check_near("focal_limit disc (1,0,1)", sky_focal_limit(&disc, sky_v3(1, 0, 1), 0.0f), 340.0f, 1e-2f);
    check_near("focal_limit disc u,v 0.6,0.8",
               sky_focal_limit(&disc, sky_v3(1.2f, 1.6f, 2.0f), 0.0f), 340.0f, 1e-2f);
    const sky_frame_t disc_out = disc_frame(360, 360, 200, 360, 600);
    check_near("focal_limit disc principal outside = 0",
               sky_focal_limit(&disc_out, sky_v3(1, 0, 1), 0.0f), 0.0f, 1e-6f);
    {
        sky_frame_t cut = disc_frame(360, 360, 340, 360, 360);
        cut.y1 = 500.0f;
        check_near("focal_limit disc cut at y1 500, (0,-1,1)",
                   sky_focal_limit(&cut, sky_v3(0, -1, 1), 0.0f), 140.0f, 1e-2f);
    }

    const float f_min = 336.0f / tanf(55.0f * SKY_DEG2RAD);
    const float f_max = 336.0f / tanf(15.0f * SKY_DEG2RAD);
    check_near("frame_half rect", sky_frame_half(&fr), 336.0f, 1e-4f);
    check_near("frame_half disc", sky_frame_half(&disc), 340.0f, 1e-4f);
    check_near("f_min rect", f_min, 235.27f, 0.05f);
    check_near("f_max rect", f_max, 1253.97f, 0.5f);

    /* Home fit: pole plus the rest pose's fit points. */
    check_near("home yaw north", sky_cam_home_yaw(41.9f), 0.0f, 1e-6f);
    check_near("home yaw south", sky_cam_home_yaw(-30.0f), 180.0f, 1e-6f);
    {
        sky_pose_t r = sky_mount_rest_pose(41.9f);
        sky_v3_t pts[SKY_RIG_FIT_N];
        sky_v3_t pole = sky_pole(41.9f);
        sky_cam_pose_t out;
        sky_rig_fit_points(&r, pts);
        sky_cam_fit(&out, sky_cam_home_yaw(41.9f), &fr, &pole, 1, pts, SKY_RIG_FIT_N, 75.0f, 0.0f);
        check_near("home fit yaw", out.yaw_deg, 0.0f, 1e-4f);
        /* Midway between the pole (41.9) and the pier foot (atan(-1.6/2.6) = -31.61): 5.145. */
        check_near("home fit pitch (mid of min and max elevation)", out.pitch_deg, 5.145f, 0.05f);
        check_true("home fit focal within limits", out.focal_px >= f_min - 0.01f && out.focal_px <= f_max + 0.01f);
        check_true("home fit: every input inside the frame less 27.5 px",
                   all_inside(&out, &fr, &pole, 1, pts, SKY_RIG_FIT_N, 27.5f));

        /* Southern site: home faces south, the elevated pole is south. */
        sky_pose_t rs = sky_mount_rest_pose(-30.0f);
        sky_v3_t ps = sky_pole(-30.0f);
        sky_rig_fit_points(&rs, pts);
        sky_cam_fit(&out, sky_cam_home_yaw(-30.0f), &fr, &ps, 1, pts, SKY_RIG_FIT_N, 75.0f, 0.0f);
        check_near("south home fit yaw", out.yaw_deg, 180.0f, 1e-4f);
        check_true("south home fit: pole and rig inside",
                   all_inside(&out, &fr, &ps, 1, pts, SKY_RIG_FIT_N, 27.5f));
    }

    /* Tight limit: one direction on the axis. */
    {
        sky_v3_t d = sky_altaz_to_vec(20.0f, 0.0f);
        sky_cam_pose_t out;
        sky_cam_fit(&out, 0.0f, &fr, &d, 1, (const sky_v3_t *)0, 0, 75.0f, 0.0f);
        check_near("single dir: pitch 20", out.pitch_deg, 20.0f, 1e-3f);
        check_near("single dir: focal = f_max", out.focal_px, f_max, 0.5f);
        check_true("single dir: inside", all_inside(&out, &fr, &d, 1, (const sky_v3_t *)0, 0, 27.5f));
    }

    /* Wide limit: two directions 50 degrees either side still fit near f_min. */
    {
        sky_v3_t d[2] = { sky_altaz_to_vec(0.0f, 310.0f), sky_altaz_to_vec(0.0f, 50.0f) };
        sky_cam_pose_t out;
        sky_cam_fit(&out, 0.0f, &fr, d, 2, (const sky_v3_t *)0, 0, 75.0f, 0.0f);
        check_near("wide pair: focal = 308 / tan 50", out.focal_px, 308.0f / tanf(50.0f * SKY_DEG2RAD), 0.1f);
        check_true("wide pair: both inside", all_inside(&out, &fr, d, 2, (const sky_v3_t *)0, 0, 27.5f));
        sky_v3_t e[2] = { sky_altaz_to_vec(0.0f, 285.0f), sky_altaz_to_vec(0.0f, 75.0f) };
        sky_cam_fit(&out, 0.0f, &fr, e, 2, (const sky_v3_t *)0, 0, 75.0f, 0.0f);
        check_near("too wide pair: focal clamps to f_min", out.focal_px, f_min, 0.05f);
        sky_v3_t g = sky_altaz_to_vec(0.0f, 100.0f);
        sky_cam_fit(&out, 0.0f, &fr, &g, 1, (const sky_v3_t *)0, 0, 75.0f, 0.0f);
        check_near("input past 80 deg off yaw skipped: default pitch", out.pitch_deg, SKY_PITCH_DEFAULT, 1e-6f);
    }

    /* Near-overhead target: pitch is held at the maximum and it still fits. */
    {
        sky_v3_t d = sky_altaz_to_vec(85.0f, 0.0f);
        sky_cam_pose_t out;
        sky_cam_fit(&out, 0.0f, &fr, &d, 1, (const sky_v3_t *)0, 0, 75.0f, 0.0f);
        check_near("near overhead: pitch clamps to 75", out.pitch_deg, 75.0f, 1e-4f);
        check_true("near overhead: inside", all_inside(&out, &fr, &d, 1, (const sky_v3_t *)0, 0, 27.5f));
    }

    /* Work yaw. */
    {
        sky_path_build(&s_path, -5.39f, 41.9f, 0.0f, (const int8_t *)0, false);
        check_near("work yaw dec -5.39", sky_cam_work_yaw(&s_path, -5.39f, 41.9f), 180.0f, 0.5f);
        sky_path_build(&s_path, 70.0f, 41.9f, 0.0f, (const int8_t *)0, false);
        check_near("work yaw dec 70 (wrap180)", sky_wrap180(sky_cam_work_yaw(&s_path, 70.0f, 41.9f)), 0.0f, 0.5f);
    }

    /* Horizon limit. */
    {
        const sky_frame_t hf = rect_frame(24, 24, 696, 456, 360, 240);
        sky_v3_t d[2] = { sky_altaz_to_vec(60.0f, 180.0f), sky_altaz_to_vec(20.0f, 150.0f) };
        sky_cam_pose_t out;
        sky_cam_fit(&out, 180.0f, &hf, d, 2, (const sky_v3_t *)0, 0, 75.0f, 456.0f);
        check_true("horizon limit: flat horizon at or above row 456.5",
                   hf.py + out.focal_px * tanf(out.pitch_deg * SKY_DEG2RAD) <= 456.5f);
    }

    /* Edge arrows. */
    {
        float x = 0, y = 0, ang = 0;
        c = make_cam(0.0f, 0.0f, 300.0f, &fr);
        check_true("arrow (2,0,1) needed", sky_edge_arrow(&fr, &c, sky_v3(2, 0, 1), 28.0f, &x, &y, &ang));
        check_near("arrow (2,0,1) x", x, 668.0f, 1e-2f);
        check_near("arrow (2,0,1) y", y, 360.0f, 1e-2f);
        check_near("arrow (2,0,1) angle", ang, 0.0f, 1e-3f);
        check_true("arrow (0,0,-1) needed", sky_edge_arrow(&fr, &c, sky_v3(0, 0, -1), 28.0f, &x, &y, &ang));
        check_near("arrow (0,0,-1) x", x, 360.0f, 1e-2f);
        check_near("arrow (0,0,-1) y", y, 668.0f, 1e-2f);
        check_near("arrow (0,0,-1) angle", ang, 90.0f, 1e-3f);
        check_true("arrow (1,0,-1) needed", sky_edge_arrow(&fr, &c, sky_v3(1, 0, -1), 28.0f, &x, &y, &ang));
        check_near("arrow (1,0,-1) x", x, 668.0f, 1e-2f);
        check_near("arrow (1,0,-1) y", y, 360.0f, 1e-2f);
        check_near("arrow (1,0,-1) angle", ang, 0.0f, 1e-3f);
        x = -1.0f;
        check_true("arrow (0.1,0,1) inside: none", !sky_edge_arrow(&fr, &c, sky_v3(0.1f, 0, 1), 28.0f, &x, &y, &ang));
        check_true("arrow inside: outputs untouched", x == -1.0f);
        sky_cam_t cd = make_cam(0.0f, 0.0f, 300.0f, &disc);
        check_true("disc arrow (2,0,1) needed", sky_edge_arrow(&disc, &cd, sky_v3(2, 0, 1), 28.0f, &x, &y, &ang));
        check_near("disc arrow (2,0,1) x", x, 672.0f, 1e-2f);
        check_near("disc arrow (2,0,1) y", y, 360.0f, 1e-2f);
    }
}

static void test_words(void)
{
    nina_plan_t plan;
    int done = -1, total = -1;
    memset(&plan, 0, sizeof plan);
    plan.n_items = 2;
    plan.rounds = 3;
    plan.round_done = 1;
    plan.items[0].iterations = 10;
    plan.items[0].completed = 3;
    plan.items[0].enabled = true;
    plan.items[1].iterations = 5;
    plan.items[1].completed = 0;
    plan.items[1].enabled = true;
    sky_plan_counts(&plan, 0, 0, &done, &total);
    check_true("plan counts 18 of 45", done == 18 && total == 45);
    plan.n_items = 3;
    plan.items[2].iterations = 4;
    plan.items[2].completed = 2;
    plan.items[2].enabled = false;
    sky_plan_counts(&plan, 0, 0, &done, &total);
    check_true("disabled item ignored: 18 of 45", done == 18 && total == 45);
    sky_plan_counts((const nina_plan_t *)0, 7, 20, &done, &total);
    check_true("NULL plan: fallback 7 of 20", done == 7 && total == 20);
    plan.n_items = 0;
    sky_plan_counts(&plan, 7, 20, &done, &total);
    check_true("n_items 0: fallback 7 of 20", done == 7 && total == 20);

    check_true("act flipping+exposing -> FLIPPING",
               sky_activity(true, true, false, false, false, false, false, 0, 0) == SKY_ACT_FLIPPING);
    check_true("act exposing+at_park -> EXPOSING",
               sky_activity(false, true, true, false, false, false, false, 0, 0) == SKY_ACT_EXPOSING);
    check_true("act at_park+slewing -> RESTING (park before slewing)",
               sky_activity(false, false, true, true, true, true, false, 0, 0) == SKY_ACT_RESTING);
    check_true("act slewing -> MOVING",
               sky_activity(false, false, false, true, false, false, false, 0, 0) == SKY_ACT_MOVING);
    check_true("act done 75 of 75 -> DONE",
               sky_activity(false, false, false, false, true, true, true, 75, 75) == SKY_ACT_DONE);
    check_true("act done 5 total 0 -> not DONE",
               sky_activity(false, false, false, false, true, true, true, 5, 0) == SKY_ACT_WAITING);
    check_true("act target known and down -> BELOW",
               sky_activity(false, false, false, false, true, true, false, 0, 0) == SKY_ACT_BELOW);
    check_true("act tracking -> WAITING",
               sky_activity(false, false, false, false, true, false, false, 0, 0) == SKY_ACT_WAITING);
    check_true("act all false -> RESTING",
               sky_activity(false, false, false, false, false, false, false, 0, 0) == SKY_ACT_RESTING);
}

static void test_compass(void)
{
    char buf[16];
    check_true("compass step wide (5.2 px/deg) = 30", sky_compass_step(5.2f, 96.0f) == 30);
    check_true("compass step tight (12.2 px/deg) = 10", sky_compass_step(12.2f, 96.0f) == 10);
    check_true("compass step very tight (25 px/deg) = 5", sky_compass_step(25.0f, 96.0f) == 5);
    check_true("compass step widest (1 px/deg) = 90", sky_compass_step(1.0f, 96.0f) == 90);
    check_true("compass text 0", strcmp(sky_compass_text(0, buf, sizeof buf), "0\xC2\xB0") == 0);
    check_true("compass text 270", strcmp(sky_compass_text(270, buf, sizeof buf), "270\xC2\xB0") == 0);
    check_true("compass text 30 deg", strcmp(sky_compass_text(30, buf, sizeof buf), "30\xC2\xB0") == 0);
    check_true("compass text wraps 360", strcmp(sky_compass_text(360, buf, sizeof buf), "0\xC2\xB0") == 0);
}

int main(void)
{
    test_directions_time();
    test_compass();
    test_horizon();
    test_path();
    test_path_sliver();
    test_mount();
    test_rig();
    test_camera();
    test_words();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
