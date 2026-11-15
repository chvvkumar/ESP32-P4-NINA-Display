/* Host test for main/sky_rig_model.h. */
#include "sky_rig_model.h"

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

#define LAT 41.9f

static bool feq(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static bool v3_near(sky_v3_t a, sky_v3_t b, float tol)
{
    return feq(a.x, b.x, tol) && feq(a.y, b.y, tol) && feq(a.z, b.z, tol);
}

static bool v3_finite(sky_v3_t a)
{
    return isfinite(a.x) && isfinite(a.y) && isfinite(a.z);
}

static bool pose_near(const sky_pose_t *a, const sky_pose_t *b, float tol)
{
    return v3_near(a->polar, b->polar, tol) && v3_near(a->decl, b->decl, tol) &&
           v3_near(a->tube, b->tube, tol) && v3_near(a->weight, b->weight, tol);
}

static void test_parts_table(void)
{
    static const uint8_t strips[SKY_RIG_PART_N] = { 3, 2, 2, 0, 2, 2, 1, 3, 1, 3, 2, 2, 2, 3, 2, 2, 2, 1, 1, 3, 2 };
    static const uint8_t over[SKY_RIG_PART_N] = {
        SKY_RIG_NO_WRAP, SKY_RIGP_PIER, SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP,
        SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP,
        SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIGP_TUBE, SKY_RIGP_TUBE, SKY_RIGP_TUBE, SKY_RIGP_TUBE,
        SKY_RIG_NO_WRAP, SKY_RIG_NO_WRAP, SKY_RIGP_SHIELD,
    };
    const sky_pose_t pose = sky_mount_rest_pose(LAT);
    sky_rig_part_t parts[SKY_RIG_PART_N];
    CHECK(sky_rig_parts(&pose, 1.0f, parts) == SKY_RIG_PART_N);
    CHECK(SKY_RIGP_LIP == SKY_RIG_PART_N - 1);
    for (int i = 0; i < SKY_RIG_PART_N; i++) {
        CHECK(parts[i].id == i);
        CHECK(parts[i].strips == strips[i]);
        CHECK(parts[i].over == over[i]);
        CHECK(parts[i].colour < SKY_RIGC_N);
        CHECK(parts[i].group <= SKY_RIGG_TUBE);
        CHECK(parts[i].r0 > 0.0f && parts[i].r1 > 0.0f);
        if (parts[i].over != SKY_RIG_NO_WRAP) {
            CHECK(parts[i].over < SKY_RIG_PART_N);
            const sky_rig_part_t *in = &parts[parts[i].over];
            CHECK(in->r0 < parts[i].r0 && in->r1 < parts[i].r1);
        }
    }
    CHECK(parts[SKY_RIGP_PIER].group == SKY_RIGG_PIER && parts[SKY_RIGP_PIER_BAND].group == SKY_RIGG_PIER);
    CHECK(parts[SKY_RIGP_BASE].group == SKY_RIGG_MOUNT && parts[SKY_RIGP_WEIGHT].group == SKY_RIGG_MOUNT);
    CHECK(parts[SKY_RIGP_DOVETAIL].group == SKY_RIGG_TUBE && parts[SKY_RIGP_LIP].group == SKY_RIGG_TUBE);
    CHECK(parts[SKY_RIGP_TUBE].colour == SKY_RIGC_WHITE && parts[SKY_RIGP_SHIELD].colour == SKY_RIGC_WHITE);
    CHECK(parts[SKY_RIGP_SHAFT].colour == SKY_RIGC_STEEL && parts[SKY_RIGP_PIER_BAND].colour == SKY_RIGC_BAND);
    CHECK(feq(parts[SKY_RIGP_FOCUSER].r0, 0.045f, 1e-6f) && feq(parts[SKY_RIGP_FOCUSER].r1, 0.06f, 1e-6f));
}

static void test_scale_one_is_the_real_rig(void)
{
    const sky_pose_t pose = sky_mount_rest_pose(LAT);
    sky_rig_part_t parts[SKY_RIG_PART_N];
    sky_v3_t fit[SKY_RIG_FIT_N];
    sky_rig_parts(&pose, 1.0f, parts);
    sky_rig_model_fit_points(&pose, 1.0f, fit);
    const sky_v3_t o = sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
    CHECK(v3_near(parts[SKY_RIGP_PIER].a, sky_v3(0.0f, 0.0f, 0.0f), 1e-6f));
    CHECK(v3_near(parts[SKY_RIGP_PIER].b, sky_v3(0.0f, 0.0f, 0.92f), 1e-5f));
    CHECK(feq(parts[SKY_RIGP_PIER_BAND].a.z, 0.79f, 1e-5f) && feq(parts[SKY_RIGP_PIER_BAND].b.z, 0.83f, 1e-5f));
    CHECK(feq(parts[SKY_RIGP_BASE].b.z, 1.04f, 1e-5f));
    CHECK(feq(sky_v3_len(sky_v3_sub(sky_rig_model_saddle(&pose, 1.0f), o)), 0.22f, 1e-5f));
    CHECK(feq(sky_v3_len(sky_v3_sub(fit[2], fit[3])), 1.25f, 1e-5f));
    CHECK(feq(sky_rig_model_mouth_r(1.0f), 0.09f, 1e-6f));
    CHECK(feq(parts[SKY_RIGP_SHIELD].r1, sky_rig_model_mouth_r(1.0f), 1e-6f));
    CHECK(v3_near(parts[SKY_RIGP_SHIELD].b, sky_rig_model_front(&pose, 1.0f), 1e-5f));
    CHECK(v3_near(parts[SKY_RIGP_LIP].b, sky_rig_model_front(&pose, 1.0f), 1e-5f));
    CHECK(v3_near(parts[SKY_RIGP_CAMERA].a, fit[3], 1e-5f));
    CHECK(v3_near(parts[SKY_RIGP_SHAFT].b, fit[4], 1e-5f));
    /* The coaxial chain has no gap: each part starts where the last ended. */
    CHECK(v3_near(parts[SKY_RIGP_CAMERA].b, parts[SKY_RIGP_WHEEL].a, 1e-5f));
    CHECK(v3_near(parts[SKY_RIGP_WHEEL].b, parts[SKY_RIGP_FOCUSER].a, 1e-5f));
    CHECK(v3_near(parts[SKY_RIGP_FOCUSER].b, parts[SKY_RIGP_COLLAR].a, 1e-5f));
    CHECK(v3_near(parts[SKY_RIGP_COLLAR].b, parts[SKY_RIGP_TUBE].a, 1e-5f));
    CHECK(v3_near(parts[SKY_RIGP_TUBE].b, parts[SKY_RIGP_SHIELD].a, 1e-5f));
    /* Rail toward the mount, handle away from it. */
    const sky_v3_t s = sky_rig_model_saddle(&pose, 1.0f);
    CHECK(sky_v3_dot(sky_v3_sub(parts[SKY_RIGP_DOVETAIL].a, s), pose.decl) < 0.0f);
    CHECK(sky_v3_dot(sky_v3_sub(parts[SKY_RIGP_HANDLE].a, s), pose.decl) > 0.0f);
}

static void test_scale_knob(void)
{
    static const float lats[2] = { LAT, -30.0f };
    static const sky_pier_t sides[2] = { SKY_PIER_EAST, SKY_PIER_WEST };
    const sky_v3_t o = sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
    sky_rig_part_t parts[SKY_RIG_PART_N], unit[SKY_RIG_PART_N];
    sky_v3_t fit[SKY_RIG_FIT_N];
    float low = 1e9f;
    for (int li = 0; li < 2; li++) {
        for (int si = 0; si < 2; si++) {
            for (int alt = 10; alt <= 90; alt += 10) {
                for (int az = 0; az <= 330; az += 30) {
                    const sky_v3_t dir = sky_altaz_to_vec((float)alt, (float)az);
                    const sky_pose_t pose = sky_mount_pose(lats[li], dir, sides[si], 0.0f);
                    sky_rig_parts(&pose, SKY_RIG_SCALE, parts);
                    sky_rig_parts(&pose, 1.0f, unit);
                    sky_rig_model_fit_points(&pose, SKY_RIG_SCALE, fit);
                    bool clear = true, lin = true, fin = true;
                    for (int i = 0; i < SKY_RIG_PART_N; i++) {
                        /* The pier foot is the one end that stands on the ground. */
                        if (i != SKY_RIGP_PIER && parts[i].a.z < 0.02f) clear = false;
                        if (parts[i].b.z < 0.02f) clear = false;
                        if (i != SKY_RIGP_PIER && parts[i].a.z < low) low = parts[i].a.z;
                        if (parts[i].b.z < low) low = parts[i].b.z;
                        if (!feq(parts[i].r0, unit[i].r0 * SKY_RIG_SCALE, 1e-6f)) lin = false;
                        if (!feq(parts[i].r1, unit[i].r1 * SKY_RIG_SCALE, 1e-6f)) lin = false;
                        if (!v3_finite(parts[i].a) || !v3_finite(parts[i].b)) fin = false;
                    }
                    CHECK(clear);
                    CHECK(lin);
                    CHECK(fin);
                    CHECK(v3_near(fit[1], o, 1e-6f));
                    /* The head is centred on the axis crossing at every scale. */
                    const sky_v3_t mid = sky_v3_scale(sky_v3_add(parts[SKY_RIGP_HEAD].a, parts[SKY_RIGP_HEAD].b), 0.5f);
                    CHECK(v3_near(mid, o, 1e-5f));
                    CHECK(v3_near(parts[SKY_RIGP_SHAFT].a, o, 1e-6f));
                }
            }
        }
    }
    printf("  lowest part end at SKY_RIG_SCALE: %.3f m\n", (double)low);
    /* Zenith: the camera end clears the ground by 0.07 m. */
    const sky_pose_t up = sky_mount_pose(LAT, sky_v3(0.0f, 0.0f, 1.0f), SKY_PIER_EAST, 0.0f);
    sky_rig_model_fit_points(&up, SKY_RIG_SCALE, fit);
    CHECK(feq(fit[3].z, 0.07f, 1e-4f));
    CHECK(feq(sky_rig_model_mouth_r(SKY_RIG_SCALE), 0.135f, 1e-6f));
}

static void test_fit_points(void)
{
    const sky_pose_t pose = sky_mount_pose(LAT, sky_altaz_to_vec(40.0f, 200.0f), SKY_PIER_EAST, 0.0f);
    const float k = SKY_RIG_SCALE;
    const sky_v3_t o = sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
    const sky_v3_t s = sky_v3_add(o, sky_v3_scale(pose.decl, 0.22f * k));
    sky_v3_t fit[SKY_RIG_FIT_N];
    CHECK(sky_rig_model_fit_points(&pose, k, fit) == SKY_RIG_FIT_N);
    CHECK(SKY_RIG_FIT_N == 5);
    CHECK(v3_near(fit[0], sky_v3(0.0f, 0.0f, 0.0f), 1e-6f));
    CHECK(v3_near(fit[1], o, 1e-6f));
    CHECK(v3_near(fit[2], sky_rig_model_front(&pose, k), 1e-6f));
    CHECK(v3_near(fit[2], sky_v3_add(s, sky_v3_scale(pose.tube, 0.55f * k)), 1e-5f));
    CHECK(v3_near(fit[3], sky_v3_sub(s, sky_v3_scale(pose.tube, 0.70f * k)), 1e-5f));
    CHECK(v3_near(fit[4], sky_v3_add(o, sky_v3_scale(pose.weight, 0.29f * k)), 1e-5f));
}

static void test_pier_is_east(void)
{
    CHECK(sky_pier_is_east(SKY_PIER_EAST, -50.0f));
    CHECK(!sky_pier_is_east(SKY_PIER_WEST, 50.0f));
    CHECK(sky_pier_is_east(SKY_PIER_UNKNOWN, 0.0f));
    CHECK(sky_pier_is_east(SKY_PIER_UNKNOWN, 30.0f));
    CHECK(!sky_pier_is_east(SKY_PIER_UNKNOWN, -0.5f));
    /* Agrees with sky_mount_pose() on which way decl points. */
    const sky_v3_t dir = sky_hadec_to_vec(-40.0f, 20.0f, LAT);
    const sky_pose_t unk = sky_mount_pose(LAT, dir, SKY_PIER_UNKNOWN, -40.0f);
    const sky_pose_t west = sky_mount_pose(LAT, dir, SKY_PIER_WEST, -40.0f);
    CHECK(v3_near(unk.decl, west.decl, 1e-6f));
}

static void test_axis_roundtrip(void)
{
    static const float lats[3] = { LAT, -30.0f, 5.0f };
    static const sky_pier_t sides[2] = { SKY_PIER_EAST, SKY_PIER_WEST };
    for (int li = 0; li < 3; li++) {
        const float lat = lats[li];
        for (int ha = -170; ha <= 180; ha += 10) {
            for (int dec = -80; dec <= 90; dec += 10) {
                for (int si = 0; si < 2; si++) {
                    const sky_v3_t dir = sky_hadec_to_vec((float)ha, (float)dec, lat);
                    const sky_pose_t p = sky_mount_pose(lat, dir, sides[si], (float)ha);
                    const sky_axis_t st = sky_axis_from_pose(&p, lat);
                    const sky_pose_t back = sky_axis_pose(st, lat);
                    CHECK(pose_near(&p, &back, 1e-4f));
                    CHECK(st.a_deg > -180.0f && st.a_deg <= 180.0f);
                    CHECK(st.dm_deg >= -90.0f && st.dm_deg <= 270.0f);
                }
            }
        }
        const sky_pose_t rest = sky_mount_rest_pose(lat);
        const sky_axis_t rs = sky_axis_from_pose(&rest, lat);
        const sky_pose_t rb = sky_axis_pose(rs, lat);
        CHECK(feq(rs.a_deg, 0.0f, 1e-3f));
        CHECK(feq(rs.dm_deg, 90.0f, 1e-3f));
        CHECK(pose_near(&rest, &rb, 1e-4f));
    }
}

static void test_axis_lerp_ends(void)
{
    const sky_axis_t a = { 37.5f, 12.0f }, b = { -142.5f, 168.0f };
    const sky_axis_t k0 = sky_axis_lerp(a, b, 0.0f), k1 = sky_axis_lerp(a, b, 1.0f);
    const sky_axis_t kh = sky_axis_lerp(a, b, 0.5f);
    CHECK(feq(k0.a_deg, a.a_deg, 1e-4f) && feq(k0.dm_deg, a.dm_deg, 1e-4f));
    CHECK(feq(k1.a_deg, b.a_deg, 1e-4f) && feq(k1.dm_deg, b.dm_deg, 1e-4f));
    CHECK(feq(kh.a_deg, -52.5f, 1e-4f) && feq(kh.dm_deg, 90.0f, 1e-4f));
}

/* East and west poses of one sky point, over a grid. */
static void flip_pairs(bool weight_test)
{
    static const float lats[2] = { LAT, -30.0f };
    for (int li = 0; li < 2; li++) {
        const float lat = lats[li];
        for (int ha = -165; ha <= 165; ha += 15) {
            for (int dec = -30; dec <= 75; dec += 15) {
                const sky_v3_t dir = sky_hadec_to_vec((float)ha, (float)dec, lat);
                const sky_pose_t pe = sky_mount_pose(lat, dir, SKY_PIER_EAST, (float)ha);
                const sky_pose_t pw = sky_mount_pose(lat, dir, SKY_PIER_WEST, (float)ha);
                const sky_axis_t ae = sky_axis_from_pose(&pe, lat), aw = sky_axis_from_pose(&pw, lat);
                if (!weight_test) {
                    const sky_pose_t mid = sky_axis_pose(sky_axis_lerp(ae, aw, 0.5f), lat);
                    CHECK(sky_sep_deg(mid.tube, sky_pole(lat)) < 0.01f);
                    CHECK(feq(fabsf(ae.a_deg - aw.a_deg), 180.0f, 1e-2f));
                    CHECK(feq(ae.dm_deg + aw.dm_deg, 180.0f, 1e-2f));
                    continue;
                }
                const float a_max = fmaxf(fabsf(ae.a_deg), fabsf(aw.a_deg));
                const float z_max = fmaxf(pe.weight.z, pw.weight.z);
                bool a_ok = true, z_ok = true, fin = true;
                for (int i = 0; i <= 20; i++) {
                    const sky_axis_t st = sky_axis_lerp(ae, aw, (float)i / 20.0f);
                    const sky_pose_t p = sky_axis_pose(st, lat);
                    if (fabsf(st.a_deg) > a_max + 1e-4f) a_ok = false;
                    if (p.weight.z > z_max + 1e-3f) z_ok = false;
                    if (!v3_finite(p.tube) || !v3_finite(p.decl) || !v3_finite(p.weight)) fin = false;
                    if (!feq(sky_v3_len(p.tube), 1.0f, 1e-4f) || !feq(sky_v3_dot(p.tube, p.decl), 0.0f, 1e-4f)) fin = false;
                }
                CHECK(a_ok);
                CHECK(z_ok);
                CHECK(fin);
            }
        }
    }
}

static void test_flip_passes_the_pole(void)
{
    flip_pairs(false);
}

static void test_weight_never_over_the_top(void)
{
    flip_pairs(true);
}

int main(void)
{
    test_parts_table();
    test_scale_one_is_the_real_rig();
    test_scale_knob();
    test_fit_points();
    test_pier_is_east();
    test_axis_roundtrip();
    test_axis_lerp_ends();
    test_flip_passes_the_pole();
    test_weight_never_over_the_top();
    printf("test_sky_rig_model: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
