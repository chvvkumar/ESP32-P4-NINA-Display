/* Host test for main/sky_rig_draw.h. */
#include "sky_rig_draw.h"

#include <stdio.h>

static int s_checks, s_fails;

#define CHECK(cond) \
    do { \
        s_checks++; \
        if (!(cond)) { \
            s_fails++; \
            if (s_fails <= 20) printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
        } \
    } while (0)

#define LAT      41.9f
#define BIG_MAX  1024
#define GUARD    4

static sky_rig_prim_t s_prims[BIG_MAX + GUARD];

typedef struct {
    sky_pose_t pose;
    bool       parked;
} rig_case_t;

static sky_frame_t frame720(void)
{
    sky_frame_t fr = { false, 360.0f, 360.0f, 360.0f, 0.0f, 0.0f, 720.0f, 720.0f, 360.0f, 360.0f };
    return fr;
}

static sky_cam_t cam_of(float yaw, float pitch, float focal)
{
    const sky_frame_t fr = frame720();
    const sky_cam_pose_t cp = { yaw, pitch, focal };
    sky_cam_t cam;
    sky_cam_setup(&cam, &cp, &fr);
    return cam;
}

static rig_case_t aim(float alt, float az, sky_pier_t side)
{
    rig_case_t c;
    c.pose = sky_mount_pose(LAT, sky_altaz_to_vec(alt, az), side, 0.0f);
    c.parked = false;
    return c;
}

/* The mockup's six poses. */
static int six_poses(rig_case_t out[6])
{
    out[0] = aim(60.0f, 170.0f, SKY_PIER_EAST);   /* high */
    out[1] = aim(20.0f, 135.0f, SKY_PIER_WEST);   /* left */
    out[2] = aim(20.0f, 225.0f, SKY_PIER_EAST);   /* right */
    out[3] = aim(50.0f, 315.0f, SKY_PIER_EAST);   /* back */
    out[4] = aim(60.0f, 190.0f, SKY_PIER_WEST);   /* flipped */
    out[5].pose = sky_mount_rest_pose(LAT);       /* parked */
    out[5].parked = true;
    return 6;
}

static void guard_arm(int max)
{
    for (int i = 0; i < GUARD; i++) {
        s_prims[max + i].kind = 0xA5;
        s_prims[max + i].tone = 0x5A;
        s_prims[max + i].w = -777.0f;
    }
}

static bool guard_ok(int max)
{
    for (int i = 0; i < GUARD; i++) {
        if (s_prims[max + i].kind != 0xA5 || s_prims[max + i].tone != 0x5A || s_prims[max + i].w != -777.0f) return false;
    }
    return true;
}

static int points_of(const sky_rig_prim_t *p)
{
    if (p->kind == SKY_PRIM_TRI) return 3;
    return (p->kind == SKY_PRIM_LINE) ? 2 : 1;
}

/* Finite, positive widths where a width is read, known kind and tone, inside the box. */
static bool list_ok(const sky_rig_prim_t *p, int n, const float *box)
{
    for (int i = 0; i < n; i++) {
        if (p[i].kind > SKY_PRIM_DOT || p[i].tone >= SKY_TONE_N || p[i].round > 1 || p[i].pad != 0) return false;
        if (!isfinite(p[i].w)) return false;
        if (p[i].kind != SKY_PRIM_TRI && !(p[i].w > 0.0f)) return false;
        for (int k = 0; k < 3; k++) {
            if (!isfinite(p[i].x[k]) || !isfinite(p[i].y[k])) return false;
            if (fabsf(p[i].x[k]) > SKY_RIG_XY_LIMIT || fabsf(p[i].y[k]) > SKY_RIG_XY_LIMIT) return false;
        }
        const float grow = (p[i].kind == SKY_PRIM_LINE) ? 0.5f * p[i].w : p[i].w;
        for (int k = 0; k < points_of(&p[i]) && box; k++) {
            if (p[i].x[k] - grow < box[0] - 1e-3f || p[i].x[k] + grow > box[2] + 1e-3f) return false;
            if (p[i].y[k] - grow < box[1] - 1e-3f || p[i].y[k] + grow > box[3] + 1e-3f) return false;
        }
    }
    return true;
}

static int tess(const rig_case_t *c, const sky_cam_t *cam, int max, float box[4])
{
    sky_rig_part_t parts[SKY_RIG_PART_N];
    const int n = sky_rig_parts(&c->pose, SKY_RIG_SCALE, parts);
    guard_arm(max);
    return sky_rig_tessellate(parts, n, &c->pose, SKY_RIG_SCALE, cam, c->parked, s_prims, max, box);
}

static int count_tone(int n, int tone)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (s_prims[i].tone == tone) k++;
    }
    return k;
}

static void test_circle_face_on(void)
{
    const sky_cam_t cam = cam_of(0.0f, 0.0f, 600.0f);
    const sky_v3_t c = sky_v3_add(cam.pos, sky_v3_scale(cam.fwd, 3.0f));
    float xs[16], ys[16];
    sky_ellipse_t e;
    CHECK(sky_circle_project(&cam, c, sky_v3_scale(cam.fwd, -1.0f), 0.2f, 16, xs, ys, &e));
    const float want = 0.2f * 600.0f / 3.0f;
    CHECK(e.vis);
    CHECK(fabsf(e.rx - want) < 0.02f * want);
    CHECK(fabsf(e.ry - want) < 0.02f * want);
    CHECK(e.facing > 0.99f);
    CHECK(fabsf(e.x - 360.0f) < 1e-2f && fabsf(e.y - 360.0f) < 1e-2f);
    for (int i = 0; i < 16; i++) {
        const float d = sqrtf((xs[i] - e.x) * (xs[i] - e.x) + (ys[i] - e.y) * (ys[i] - e.y));
        CHECK(fabsf(d - want) < 0.02f * want);
    }
    CHECK(sky_circle_project(&cam, c, cam.right, 0.2f, 16, xs, ys, &e));
    CHECK(e.ry < 0.02f * e.rx);
    CHECK(fabsf(e.rx - want) < 0.02f * want);
    /* NULL ellipse, too few steps, behind the camera. */
    CHECK(sky_circle_project(&cam, c, cam.right, 0.2f, 12, xs, ys, NULL));
    CHECK(!sky_circle_project(&cam, c, cam.right, 0.2f, 2, xs, ys, &e));
    CHECK(!e.vis);
    CHECK(!sky_circle_project(&cam, sky_v3_sub(cam.pos, cam.fwd), cam.fwd, 0.2f, 12, xs, ys, &e));
    CHECK(!e.vis && isfinite(e.rx) && isfinite(e.ry) && isfinite(e.rot_rad) && isfinite(e.facing));
}

static void test_circle_points(void)
{
    const sky_cam_t cam = cam_of(30.0f, 10.0f, 600.0f);
    static const float tilt[4] = { 0.0f, 25.0f, 50.0f, 75.0f };
    for (int t = 0; t < 4; t++) {
        for (int roll = 0; roll < 360; roll += 45) {
            /* A normal tilted from the line of sight, turned round it. */
            const float a = tilt[t] * SKY_DEG2RAD, b = (float)roll * SKY_DEG2RAD;
            const sky_v3_t across = sky_v3_add(sky_v3_scale(cam.right, cosf(b)), sky_v3_scale(cam.up, sinf(b)));
            const sky_v3_t nrm = sky_v3_add(sky_v3_scale(cam.fwd, -cosf(a)), sky_v3_scale(across, sinf(a)));
            const sky_v3_t c = sky_v3_add(cam.pos, sky_v3_scale(cam.fwd, 3.0f));
            float xs[16], ys[16];
            sky_ellipse_t e;
            CHECK(sky_circle_project(&cam, c, nrm, 0.09f, 16, xs, ys, &e));
            float worst = 0.0f;
            for (int i = 0; i < 16; i++) {
                const float dx = xs[i] - e.x, dy = ys[i] - e.y;
                const float lx = dx * cosf(e.rot_rad) + dy * sinf(e.rot_rad);
                const float ly = -dx * sinf(e.rot_rad) + dy * cosf(e.rot_rad);
                const float q = sqrtf((lx / e.rx) * (lx / e.rx) + (ly / e.ry) * (ly / e.ry));
                const float err = sqrtf(dx * dx + dy * dy) * fabsf(1.0f - 1.0f / q);
                if (!(err <= worst)) worst = err;
            }
            CHECK(worst < 0.5f);
        }
    }
}

static float tri_side(const sky_rig_prim_t *t, int k, float px, float py)
{
    const int j = (k + 1) % 3;
    return (t->x[j] - t->x[k]) * (py - t->y[k]) - (t->y[j] - t->y[k]) * (px - t->x[k]);
}

static bool tri_has(const sky_rig_prim_t *t, float px, float py)
{
    const float a = tri_side(t, 0, px, py), b = tri_side(t, 1, px, py), c = tri_side(t, 2, px, py);
    return (a >= 0.0f && b >= 0.0f && c >= 0.0f) || (a <= 0.0f && b <= 0.0f && c <= 0.0f);
}

static bool same_pt(const sky_rig_prim_t *a, int i, const sky_rig_prim_t *b, int j)
{
    return fabsf(a->x[i] - b->x[j]) < 1e-3f && fabsf(a->y[i] - b->y[j]) < 1e-3f;
}

static void test_fan_covers(void)
{
    float xs[16], ys[16];
    sky_rig_prim_t out[16];
    for (int i = 0; i < 16; i++) {
        const float a = 6.2831853f * (float)i / 16.0f;
        xs[i] = 100.0f + 30.0f * cosf(a);
        ys[i] = 100.0f + 30.0f * sinf(a);
    }
    CHECK(sky_rig_fan(xs, ys, 16, 100.0f, 100.0f, 7, out, 16) == 16);
    bool covered = true, tones = true;
    for (int gy = 70; gy <= 130; gy++) {
        for (int gx = 70; gx <= 130; gx++) {
            const float dx = (float)gx - 100.0f, dy = (float)gy - 100.0f;
            if (dx * dx + dy * dy >= 28.0f * 28.0f) continue;
            bool in = false;
            for (int t = 0; t < 16 && !in; t++) in = tri_has(&out[t], (float)gx, (float)gy);
            if (!in) covered = false;
        }
    }
    CHECK(covered);
    int shared = 0;
    for (int a = 0; a < 16; a++) {
        if (out[a].kind != SKY_PRIM_TRI || out[a].tone != 7) tones = false;
        for (int b = a + 1; b < 16; b++) {
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    const int i2 = (i + 1) % 3, j2 = (j + 1) % 3;
                    if ((same_pt(&out[a], i, &out[b], j) && same_pt(&out[a], i2, &out[b], j2)) ||
                        (same_pt(&out[a], i, &out[b], j2) && same_pt(&out[a], i2, &out[b], j))) shared++;
                }
            }
        }
    }
    CHECK(tones);
    CHECK(shared == 0);
    /* Capacity and degenerate input. */
    CHECK(sky_rig_fan(xs, ys, 16, 100.0f, 100.0f, 7, out, 5) == 5);
    CHECK(sky_rig_fan(xs, ys, 2, 100.0f, 100.0f, 7, out, 16) == 0);
    CHECK(sky_rig_fan(xs, ys, 16, 100.0f, 100.0f, 7, out, 0) == 0);
}

static void test_tone_of(void)
{
    const sky_v3_t l = sky_v3(0.0f, 0.0f, 1.0f);
    CHECK(sky_rig_tone_of(sky_v3(0.0f, 0.0f, 1.0f), l) == 0);
    CHECK(sky_rig_tone_of(sky_v3(1.0f, 0.0f, 0.0f), l) == 1);
    CHECK(sky_rig_tone_of(sky_v3(0.0f, 0.0f, -1.0f), l) == 2);
    CHECK(sky_rig_tone_of(sky_altaz_to_vec(30.0f, 0.0f), l) == 0);
    CHECK(sky_rig_tone_of(sky_altaz_to_vec(20.0f, 0.0f), l) == 1);
    CHECK(sky_rig_tone_of(sky_altaz_to_vec(-5.0f, 0.0f), l) == 2);
    CHECK(SKY_TONE(SKY_RIGC_BAND, 2) == SKY_TONE_EDGE - 1);
    CHECK(SKY_TONE_N == SKY_RIGC_N * 3 + 7);
}

static void test_budget(void)
{
    static const float yaw[3] = { 0.0f, 180.0f, 90.0f }, pitch[3] = { 10.0f, 35.0f, 60.0f };
    static const float focal[3] = { 300.0f, 600.0f, 900.0f };
    rig_case_t pose[6];
    const int np = six_poses(pose);
    int lo = BIG_MAX, hi = 0;
    for (int p = 0; p < np; p++) {
        for (int c = 0; c < 3; c++) {
            for (int f = 0; f < 3; f++) {
                const sky_cam_t cam = cam_of(yaw[c], pitch[c], focal[f]);
                float box[4];
                const int n = tess(&pose[p], &cam, SKY_RIG_PRIM_MAX, box);
                CHECK(n >= 40 && n <= SKY_RIG_PRIM_MAX);
                CHECK(list_ok(s_prims, n, box));
                CHECK(guard_ok(SKY_RIG_PRIM_MAX));
                if (n < lo) lo = n;
                if (n > hi) hi = n;
            }
        }
    }
    printf("  six poses, three cameras: %d to %d primitives\n", lo, hi);
}

static void test_painter_order(void)
{
    static const float yaw[2] = { 0.0f, 180.0f }, pitch[2] = { 10.0f, 35.0f };
    rig_case_t pose[6];
    const int np = six_poses(pose);
    for (int p = 0; p < np; p++) {
        for (int c = 0; c < 2; c++) {
            const sky_cam_t cam = cam_of(yaw[c], pitch[c], 600.0f);
            const int n = tess(&pose[p], &cam, BIG_MAX, NULL);
            int first = 0;
            while (first < n && s_prims[first].tone == SKY_TONE_EDGE) first++;
            CHECK(first > 0 && first <= SKY_RIG_PART_N && first < n);
            bool late_edge = false;
            for (int i = first; i < n; i++) {
                if (s_prims[i].tone == SKY_TONE_EDGE) late_edge = true;
            }
            CHECK(!late_edge);
            const int t = s_prims[first].tone;
            CHECK(t / 3 == SKY_RIGC_BLACK || t / 3 == SKY_RIGC_BAND);
            /* The pale pier band is drawn after the first pier strip (it wraps the pier). */
            int band = -1;
            for (int i = first; i < n && band < 0; i++) {
                if (s_prims[i].tone / 3 == SKY_RIGC_BAND) band = i;
            }
            CHECK(band > first);
        }
    }
}

static void test_mouth(void)
{
    const sky_cam_t cam = cam_of(0.0f, 10.0f, 600.0f);   /* stands south, looks north */
    rig_case_t toward = aim(10.0f, 180.0f, SKY_PIER_EAST);
    int n = tess(&toward, &cam, BIG_MAX, NULL);
    CHECK(count_tone(n, SKY_TONE_MOUTH) == 16);
    CHECK(count_tone(n, SKY_TONE_ACCENT) == 16);
    CHECK(count_tone(n, SKY_TONE_CAP) == 0 && count_tone(n, SKY_TONE_CAP_RIM) == 0);
    toward.parked = true;
    n = tess(&toward, &cam, BIG_MAX, NULL);
    CHECK(count_tone(n, SKY_TONE_CAP) == 16);
    CHECK(count_tone(n, SKY_TONE_CAP_RIM) == 16);
    CHECK(count_tone(n, SKY_TONE_MOUTH) == 0 && count_tone(n, SKY_TONE_ACCENT) == 0);
    CHECK(count_tone(n, SKY_TONE_LENS) == 0 && count_tone(n, SKY_TONE_GLINT) == 0);
    const rig_case_t away = aim(30.0f, 0.0f, SKY_PIER_EAST);
    n = tess(&away, &cam, BIG_MAX, NULL);
    CHECK(count_tone(n, SKY_TONE_ACCENT) == 10);
    CHECK(count_tone(n, SKY_TONE_MOUTH) == 0 && count_tone(n, SKY_TONE_LENS) == 0);
    for (int i = 0; i < n; i++) {
        if (s_prims[i].tone == SKY_TONE_ACCENT) CHECK(s_prims[i].kind == SKY_PRIM_LINE && s_prims[i].w == 2.0f);
    }
    rig_case_t rest;
    rest.pose = sky_mount_rest_pose(LAT);
    rest.parked = true;
    n = tess(&rest, &cam, BIG_MAX, NULL);
    CHECK(count_tone(n, SKY_TONE_CAP_RIM) == 10 && count_tone(n, SKY_TONE_ACCENT) == 0);

    /* Looking straight down the tube from 3 m: the whole lens shows, with its glint. */
    const rig_case_t level = aim(20.0f, 180.0f, SKY_PIER_EAST);
    sky_cam_t eye = cam;
    eye.pos = sky_v3_add(sky_rig_model_front(&level.pose, SKY_RIG_SCALE), sky_v3_scale(level.pose.tube, 3.0f));
    eye.fwd = sky_v3_scale(level.pose.tube, -1.0f);
    eye.right = sky_v3_norm(sky_v3_cross(eye.fwd, sky_v3(0.0f, 0.0f, 1.0f)));
    eye.up = sky_v3_cross(eye.right, eye.fwd);
    n = tess(&level, &eye, BIG_MAX, NULL);
    CHECK(count_tone(n, SKY_TONE_LENS) == 12);
    CHECK(count_tone(n, SKY_TONE_GLINT) == 1);
    int last_mouth = -1, first_lens = n, glint = -1;
    for (int i = 0; i < n; i++) {
        if (s_prims[i].tone == SKY_TONE_MOUTH) last_mouth = i;
        if (s_prims[i].tone == SKY_TONE_LENS && i < first_lens) first_lens = i;
        if (s_prims[i].tone == SKY_TONE_GLINT) glint = i;
    }
    CHECK(last_mouth < first_lens && first_lens < glint);
    CHECK(glint >= 0 && s_prims[glint].kind == SKY_PRIM_DOT);
}

static void test_box(void)
{
    rig_case_t pose[6];
    const int np = six_poses(pose);
    for (int p = 0; p < np; p++) {
        const sky_cam_t cam = cam_of(200.0f, 20.0f, 500.0f);
        float box[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        const int n = tess(&pose[p], &cam, BIG_MAX, box);
        CHECK(n > 0 && box[2] > box[0] && box[3] > box[1]);
        CHECK(list_ok(s_prims, n, box));
        /* The box is tight: some primitive touches each side. */
        bool touch[4] = { false, false, false, false };
        for (int i = 0; i < n; i++) {
            const float grow = (s_prims[i].kind == SKY_PRIM_LINE) ? 0.5f * s_prims[i].w : s_prims[i].w;
            for (int k = 0; k < points_of(&s_prims[i]); k++) {
                if (s_prims[i].x[k] - grow <= box[0] + 1e-3f) touch[0] = true;
                if (s_prims[i].y[k] - grow <= box[1] + 1e-3f) touch[1] = true;
                if (s_prims[i].x[k] + grow >= box[2] - 1e-3f) touch[2] = true;
                if (s_prims[i].y[k] + grow >= box[3] - 1e-3f) touch[3] = true;
            }
        }
        CHECK(touch[0] && touch[1] && touch[2] && touch[3]);
    }
    /* Nothing emitted: the box is all zero. */
    const sky_cam_t cam = cam_of(0.0f, 10.0f, 600.0f);
    float box[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    CHECK(tess(&pose[0], &cam, 0, box) == 0);
    CHECK(box[0] == 0.0f && box[1] == 0.0f && box[2] == 0.0f && box[3] == 0.0f);
    CHECK(guard_ok(0));
    sky_rig_part_t parts[SKY_RIG_PART_N];
    sky_rig_parts(&pose[0].pose, SKY_RIG_SCALE, parts);
    CHECK(sky_rig_tessellate(parts, 0, &pose[0].pose, SKY_RIG_SCALE, &cam, false, s_prims, BIG_MAX, box) == 0);
    CHECK(sky_rig_tessellate(parts, SKY_RIG_PART_N, &pose[0].pose, SKY_RIG_SCALE, &cam, false, NULL, BIG_MAX, box) == 0);
    CHECK(sky_rig_tessellate(parts, 99, &pose[0].pose, SKY_RIG_SCALE, &cam, false, s_prims, BIG_MAX, NULL) > 40);
}

static void test_behind_camera(void)
{
    rig_case_t pose[6];
    const int np = six_poses(pose);
    static const float along[5] = { -0.9f, -0.3f, 0.0f, 0.4f, 0.8f };
    for (int p = 0; p < np; p++) {
        for (int k = 0; k < 5; k++) {
            for (int yaw = 0; yaw < 360; yaw += 60) {
                sky_cam_t cam = cam_of((float)yaw, 20.0f, 700.0f);
                /* The eye inside the tube. */
                cam.pos = sky_v3_add(sky_rig_model_saddle(&pose[p].pose, SKY_RIG_SCALE),
                                     sky_v3_scale(pose[p].pose.tube, along[k]));
                float box[4];
                const int n = tess(&pose[p], &cam, 64, box);
                CHECK(n >= 0 && n <= 64);
                CHECK(guard_ok(64));
                CHECK(list_ok(s_prims, n, box));
                CHECK(isfinite(box[0]) && isfinite(box[1]) && isfinite(box[2]) && isfinite(box[3]));
            }
        }
    }
    /* The eye exactly on the head centre and on a part end. */
    sky_cam_t cam = cam_of(0.0f, 0.0f, 700.0f);
    float box[4];
    cam.pos = sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
    int n = tess(&pose[0], &cam, 64, box);
    CHECK(n <= 64 && guard_ok(64) && list_ok(s_prims, n, box));
    cam.pos = sky_rig_model_front(&pose[0].pose, SKY_RIG_SCALE);
    n = tess(&pose[0], &cam, 64, box);
    CHECK(n <= 64 && guard_ok(64) && list_ok(s_prims, n, box));
}

/* Every pointing, both pier sides, parked, mid-ease, round the whole camera
 * orbit at the focal limits: finite output, no overrun, and the renderer's
 * capacity is enough: a list that fills it exactly is cut again with one more
 * slot, and must come out the same (nothing was waiting to be dropped). */
static void test_sweep(void)
{
    static const float pitch[3] = { -10.0f, 30.0f, 75.0f };
    static const float focal[2] = { 250.0f, 1500.0f };
    static const float ease[3] = { 0.25f, 0.5f, 0.75f };
    int hi = 0, raw_hi = 0, filled = 0, reduced = 0;
    long total = 0, runs = 0;
    for (int alt = -10; alt <= 90; alt += 10) {
        for (int az = 0; az < 360; az += 15) {
            const sky_v3_t dir = sky_altaz_to_vec((float)alt, (float)az);
            rig_case_t rc[6];
            rc[0].pose = sky_mount_pose(LAT, dir, SKY_PIER_EAST, 0.0f);
            rc[1].pose = sky_mount_pose(LAT, dir, SKY_PIER_WEST, 0.0f);
            const sky_axis_t ae = sky_axis_from_pose(&rc[0].pose, LAT), aw = sky_axis_from_pose(&rc[1].pose, LAT);
            for (int e = 0; e < 3; e++) rc[2 + e].pose = sky_axis_pose(sky_axis_lerp(ae, aw, ease[e]), LAT);
            rc[5].pose = sky_mount_rest_pose(LAT);
            for (int i = 0; i < 6; i++) rc[i].parked = (i == 5);
            for (int i = 0; i < 6; i++) {
                bool ok = true;
                for (int yaw = 0; yaw < 360; yaw += 45) {
                    for (int pi = 0; pi < 3; pi++) {
                        for (int fi = 0; fi < 2; fi++) {
                            const sky_cam_t cam = cam_of((float)yaw, pitch[pi], focal[fi]);
                            float box[4];
                            const int raw = tess(&rc[i], &cam, BIG_MAX, box);
                            if (raw >= BIG_MAX || !guard_ok(BIG_MAX) || !list_ok(s_prims, raw, box)) ok = false;
                            const int n = tess(&rc[i], &cam, SKY_RIG_PRIM_MAX, box);
                            if (n > SKY_RIG_PRIM_MAX || n > raw || !guard_ok(SKY_RIG_PRIM_MAX)) ok = false;
                            if (!list_ok(s_prims, n, box)) ok = false;
                            if (n >= SKY_RIG_PRIM_MAX && tess(&rc[i], &cam, SKY_RIG_PRIM_MAX + 1, box) != n) filled++;
                            if (n < raw) reduced++;
                            if (raw > raw_hi) raw_hi = raw;
                            if (n > hi) hi = n;
                            total += n;
                            runs++;
                            /* A small list: never overrun. */
                            const int m = tess(&rc[i], &cam, 101, box);
                            if (m > 101 || !guard_ok(101) || !list_ok(s_prims, m, box)) ok = false;
                        }
                    }
                }
                CHECK(ok);
            }
        }
    }
    CHECK(hi <= SKY_RIG_PRIM_MAX);
    CHECK(filled == 0);
    printf("  sweep: %ld lists, mean %ld, most %d of %d primitives (%d at full detail, %d lists reduced)\n",
           runs, total / runs, hi, SKY_RIG_PRIM_MAX, raw_hi, reduced);
}

int main(void)
{
    test_circle_face_on();
    test_circle_points();
    test_fan_covers();
    test_tone_of();
    test_budget();
    test_painter_order();
    test_mouth();
    test_box();
    test_behind_camera();
    test_sweep();
    printf("test_sky_rig_draw: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
