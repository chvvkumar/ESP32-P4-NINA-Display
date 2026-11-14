/* The real rig as a part list, its fit points and the two-angle mount state for the sky scene layouts. */
#pragma once

/**
 * @file sky_rig_model.h
 * @brief The owner's telescope as 21 rods (a 140 mm refractor on a small
 *        harmonic mount on a round pier), the points the camera fit holds in
 *        view, and the two-angle mount state used to ease through the pole on
 *        a pier-side change.
 *
 * Header-only, C99, `static inline`, float only, over sky_scene_math.h. No
 * ESP-IDF, FreeRTOS or LVGL include. Host-tested by
 * test/host/test_sky_rig_model.c.
 *
 * SCALE
 *   Every offset and radius below is metres at scale 1 and is multiplied by
 *   the `scale` argument about the head centre O = (0, 0, SKY_RIG_PIVOT_Z), so
 *   the axis crossing never moves. The pier foot alone stays on the ground.
 *
 * AXIS STATE (sky_axis_t)
 *   a_deg   rotation of the declination axis about the elevated pole, 0 with
 *           the counterweight straight down (the rest pose), (-180, 180].
 *   dm_deg  angle of the tube from the equator toward the elevated pole, in
 *           the plane across the declination axis. 90 is the pole. One side of
 *           the pier is [-90, 90], the other (90, 270]. For one sky point the
 *           two sides differ by 180 in a_deg and mirror dm_deg about 90.
 */

#include "sky_scene_math.h"

#define SKY_RIG_SCALE     1.5f    /* drawn larger than life; 1.0f is the real rig */
#define SKY_RIG_PART_N    21      /* sky_rig_parts() always fills this many */
#define SKY_POLE_EASE_MS  1500    /* ease through the pole on a pier-side change */

#define SKY_RIG_SADDLE    0.22f   /* head centre to the tube axis, along decl */
#define SKY_RIG_MOUTH     0.55f   /* saddle to the mouth, along the tube */
#define SKY_RIG_CAM_END   0.70f   /* saddle to the camera end, against the tube */
#define SKY_RIG_SHAFT_END 0.29f   /* head centre to the shaft end, along weight */
#define SKY_RIG_MOUTH_R   0.09f   /* dew shield radius */

typedef enum {
    SKY_RIGC_WHITE = 0,
    SKY_RIGC_RED,
    SKY_RIGC_BLACK,
    SKY_RIGC_STEEL,
    SKY_RIGC_BAND,
    SKY_RIGC_N
} sky_rig_colour_t;

typedef enum {
    SKY_RIGG_PIER = 0,
    SKY_RIGG_MOUNT,
    SKY_RIGG_TUBE
} sky_rig_group_t;

typedef enum {
    SKY_RIGP_PIER = 0,
    SKY_RIGP_PIER_BAND,
    SKY_RIGP_BASE,
    SKY_RIGP_HEAD,
    SKY_RIGP_DEC,
    SKY_RIGP_CLAMP,
    SKY_RIGP_SHAFT,
    SKY_RIGP_WEIGHT,
    SKY_RIGP_DOVETAIL,
    SKY_RIGP_CAMERA,
    SKY_RIGP_WHEEL,
    SKY_RIGP_FOCUSER,
    SKY_RIGP_COLLAR,
    SKY_RIGP_TUBE,
    SKY_RIGP_RING_REAR,
    SKY_RIGP_RING_MID,
    SKY_RIGP_BAND,
    SKY_RIGP_BAND_RING,
    SKY_RIGP_HANDLE,
    SKY_RIGP_SHIELD,
    SKY_RIGP_LIP
} sky_rig_part_id_t;

#define SKY_RIG_NO_WRAP 0xff

/** A rod from @p a to @p b, radius @p r0 at a and @p r1 at b, metres, world
 *  frame. `strips` is how many flat tones the visible half is cut into (0 =
 *  the head, drawn as a box). `over` is the id of the part this one is wrapped
 *  round, or SKY_RIG_NO_WRAP. */
typedef struct {
    sky_v3_t a, b;
    float    r0, r1;
    uint8_t  id, group, colour, strips, over, pad[3];
} sky_rig_part_t;

/* Which line a part lies on. */
enum {
    SKY_RIGF_VERT_ = 0,   /* the vertical through O, offsets up from O */
    SKY_RIGF_POLAR_,      /* O + polar * d */
    SKY_RIGF_DECL_,       /* O + decl * d */
    SKY_RIGF_WEIGHT_,     /* O + weight * d */
    SKY_RIGF_TUBE_        /* saddle + tube * d + decl * side */
};

typedef struct {
    uint8_t group, colour, strips, over, frame;
    float   from, to, side, r0, r1;
} sky_rig_row_t_;

/* The table of feature (18), from base.js modelReal(). Row index = part id. */
static const sky_rig_row_t_ SKY_RIG_ROWS_[SKY_RIG_PART_N] = {
    /* pier: `from` is replaced by the ground */
    { SKY_RIGG_PIER,  SKY_RIGC_BLACK, 3, SKY_RIG_NO_WRAP,  SKY_RIGF_VERT_,   -0.20f,  -0.20f,  0.0f,    0.075f, 0.075f },
    { SKY_RIGG_PIER,  SKY_RIGC_BAND,  2, SKY_RIGP_PIER,    SKY_RIGF_VERT_,   -0.33f,  -0.29f,  0.0f,    0.077f, 0.077f },
    { SKY_RIGG_MOUNT, SKY_RIGC_RED,   2, SKY_RIG_NO_WRAP,  SKY_RIGF_VERT_,   -0.20f,  -0.08f,  0.0f,    0.05f,  0.05f  },
    { SKY_RIGG_MOUNT, SKY_RIGC_RED,   0, SKY_RIG_NO_WRAP,  SKY_RIGF_POLAR_,  -0.09f,  0.09f,   0.0f,    0.065f, 0.065f },
    { SKY_RIGG_MOUNT, SKY_RIGC_BLACK, 2, SKY_RIG_NO_WRAP,  SKY_RIGF_DECL_,   -0.04f,  0.10f,   0.0f,    0.06f,  0.06f  },
    { SKY_RIGG_MOUNT, SKY_RIGC_RED,   2, SKY_RIG_NO_WRAP,  SKY_RIGF_DECL_,   0.10f,   0.125f,  0.0f,    0.05f,  0.05f  },
    { SKY_RIGG_MOUNT, SKY_RIGC_STEEL, 1, SKY_RIG_NO_WRAP,  SKY_RIGF_WEIGHT_, 0.0f,    0.29f,   0.0f,    0.009f, 0.009f },
    { SKY_RIGG_MOUNT, SKY_RIGC_BLACK, 3, SKY_RIG_NO_WRAP,  SKY_RIGF_WEIGHT_, 0.21f,   0.29f,   0.0f,    0.065f, 0.065f },
    { SKY_RIGG_TUBE,  SKY_RIGC_RED,   1, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   -0.28f,  0.06f,   -0.085f, 0.02f,  0.02f  },
    { SKY_RIGG_TUBE,  SKY_RIGC_RED,   3, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   -0.70f,  -0.60f,  0.0f,    0.045f, 0.045f },
    { SKY_RIGG_TUBE,  SKY_RIGC_BLACK, 2, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   -0.60f,  -0.565f, 0.0f,    0.075f, 0.075f },
    { SKY_RIGG_TUBE,  SKY_RIGC_BLACK, 2, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   -0.565f, -0.29f,  0.0f,    0.045f, 0.06f  },
    { SKY_RIGG_TUBE,  SKY_RIGC_RED,   2, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   -0.29f,  -0.25f,  0.0f,    0.07f,  0.07f  },
    { SKY_RIGG_TUBE,  SKY_RIGC_WHITE, 3, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   -0.25f,  0.35f,   0.0f,    0.075f, 0.075f },
    { SKY_RIGG_TUBE,  SKY_RIGC_BLACK, 2, SKY_RIGP_TUBE,    SKY_RIGF_TUBE_,   -0.22f,  -0.19f,  0.0f,    0.088f, 0.088f },
    { SKY_RIGG_TUBE,  SKY_RIGC_BLACK, 2, SKY_RIGP_TUBE,    SKY_RIGF_TUBE_,   -0.02f,  0.01f,   0.0f,    0.088f, 0.088f },
    { SKY_RIGG_TUBE,  SKY_RIGC_BLACK, 2, SKY_RIGP_TUBE,    SKY_RIGF_TUBE_,   0.28f,   0.34f,   0.0f,    0.078f, 0.078f },
    { SKY_RIGG_TUBE,  SKY_RIGC_RED,   1, SKY_RIGP_TUBE,    SKY_RIGF_TUBE_,   0.34f,   0.35f,   0.0f,    0.08f,  0.08f  },
    { SKY_RIGG_TUBE,  SKY_RIGC_RED,   1, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   -0.22f,  0.01f,   0.105f,  0.012f, 0.012f },
    { SKY_RIGG_TUBE,  SKY_RIGC_WHITE, 3, SKY_RIG_NO_WRAP,  SKY_RIGF_TUBE_,   0.35f,   0.55f,   0.0f,    0.09f,  0.09f  },
    { SKY_RIGG_TUBE,  SKY_RIGC_RED,   2, SKY_RIGP_SHIELD,  SKY_RIGF_TUBE_,   0.53f,   0.55f,   0.0f,    0.093f, 0.093f },
};

/** Head centre: the axis crossing, the same point at every scale. */
static inline sky_v3_t sky_rig_model_head(void)
{
    return sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
}

/** Where the tube axis meets the declination axis: O + decl * 0.22 * scale. */
static inline sky_v3_t sky_rig_model_saddle(const sky_pose_t *pose, float scale)
{
    return sky_v3_add(sky_rig_model_head(), sky_v3_scale(pose->decl, SKY_RIG_SADDLE * scale));
}

/** Mouth centre (front of the dew shield), where the beam starts. */
static inline sky_v3_t sky_rig_model_front(const sky_pose_t *pose, float scale)
{
    return sky_v3_add(sky_rig_model_saddle(pose, scale), sky_v3_scale(pose->tube, SKY_RIG_MOUTH * scale));
}

/** Dew shield radius at @p scale. */
static inline float sky_rig_model_mouth_r(float scale)
{
    return SKY_RIG_MOUTH_R * scale;
}

/** The rig as SKY_RIG_PART_N parts, out[i].id == i. Returns SKY_RIG_PART_N. */
static inline int sky_rig_parts(const sky_pose_t *pose, float scale, sky_rig_part_t out[SKY_RIG_PART_N])
{
    const sky_v3_t o = sky_rig_model_head();
    const sky_v3_t s = sky_rig_model_saddle(pose, scale);
    const sky_v3_t up = sky_v3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < SKY_RIG_PART_N; i++) {
        const sky_rig_row_t_ *r = &SKY_RIG_ROWS_[i];
        sky_rig_part_t *p = &out[i];
        sky_v3_t org = o, dir = up;
        switch (r->frame) {
        case SKY_RIGF_POLAR_:
            dir = pose->polar;
            break;
        case SKY_RIGF_DECL_:
            dir = pose->decl;
            break;
        case SKY_RIGF_WEIGHT_:
            dir = pose->weight;
            break;
        case SKY_RIGF_TUBE_:
            org = sky_v3_add(s, sky_v3_scale(pose->decl, r->side * scale));
            dir = pose->tube;
            break;
        default:
            break;
        }
        p->a = sky_v3_add(org, sky_v3_scale(dir, r->from * scale));
        p->b = sky_v3_add(org, sky_v3_scale(dir, r->to * scale));
        p->r0 = r->r0 * scale;
        p->r1 = r->r1 * scale;
        p->id = (uint8_t)i;
        p->group = r->group;
        p->colour = r->colour;
        p->strips = r->strips;
        p->over = r->over;
        p->pad[0] = 0;
        p->pad[1] = 0;
        p->pad[2] = 0;
    }
    out[SKY_RIGP_PIER].a = sky_v3(0.0f, 0.0f, 0.0f);   /* the pier stands on the ground */
    return SKY_RIG_PART_N;
}

/**
 * The SKY_RIG_FIT_N points the camera fit holds in view: the pier foot (the
 * origin), the head centre, the mouth centre, the camera end and the
 * counterweight shaft end, in that order. Returns SKY_RIG_FIT_N.
 */
static inline int sky_rig_model_fit_points(const sky_pose_t *pose, float scale, sky_v3_t out[SKY_RIG_FIT_N])
{
    const sky_v3_t o = sky_rig_model_head();
    const sky_v3_t s = sky_rig_model_saddle(pose, scale);
    out[0] = sky_v3(0.0f, 0.0f, 0.0f);
    out[1] = o;
    out[2] = sky_rig_model_front(pose, scale);
    out[3] = sky_v3_sub(s, sky_v3_scale(pose->tube, SKY_RIG_CAM_END * scale));
    out[4] = sky_v3_add(o, sky_v3_scale(pose->weight, SKY_RIG_SHAFT_END * scale));
    return SKY_RIG_FIT_N;
}

/* ---- the two-angle mount state ---------------------------------------------- */

typedef struct {
    float a_deg, dm_deg;
} sky_axis_t;

/** The pier side sky_mount_pose() resolves: EAST and WEST as given, UNKNOWN
 *  is EAST when @p ha_deg >= 0. */
static inline bool sky_pier_is_east(sky_pier_t side, float ha_deg)
{
    if (side == SKY_PIER_EAST) return true;
    if (side == SKY_PIER_WEST) return false;
    return ha_deg >= 0.0f;
}

/* The rest frame at a latitude: P the elevated pole, D0 the declination axis
 * with the counterweight straight down, E = P x D0 (due east or west). */
static inline void sky_axis_frame_(float lat_deg, sky_v3_t *p, sky_v3_t *d0, sky_v3_t *e)
{
    const sky_pose_t rest = sky_mount_rest_pose(lat_deg);
    *p = rest.polar;
    *d0 = rest.decl;
    *e = sky_v3_cross(rest.polar, rest.decl);
}

/** Axis state of a pose made by sky_mount_pose() or sky_mount_rest_pose() at
 *  @p lat_deg. The rest pose gives a_deg 0, dm_deg 90. */
static inline sky_axis_t sky_axis_from_pose(const sky_pose_t *p, float lat_deg)
{
    sky_v3_t pole, d0, e;
    sky_axis_frame_(lat_deg, &pole, &d0, &e);
    const sky_v3_t q = sky_v3_cross(p->decl, pole);   /* on the equator, across decl */
    sky_axis_t st;
    st.a_deg = sky_wrap180(atan2f(sky_v3_dot(p->decl, e), sky_v3_dot(p->decl, d0)) * SKY_RAD2DEG);
    st.dm_deg = atan2f(sky_v3_dot(p->tube, pole), sky_v3_dot(p->tube, q)) * SKY_RAD2DEG;
    if (st.dm_deg < -90.0f) st.dm_deg += 360.0f;
    return st;
}

/** The pose of an axis state: the inverse of sky_axis_from_pose(). */
static inline sky_pose_t sky_axis_pose(sky_axis_t st, float lat_deg)
{
    sky_v3_t pole, d0, e;
    sky_axis_frame_(lat_deg, &pole, &d0, &e);
    const float a = st.a_deg * SKY_DEG2RAD, dm = st.dm_deg * SKY_DEG2RAD;
    sky_pose_t p;
    p.polar = pole;
    p.decl = sky_v3_norm(sky_v3_add(sky_v3_scale(d0, cosf(a)), sky_v3_scale(e, sinf(a))));
    const sky_v3_t q = sky_v3_cross(p.decl, pole);
    p.tube = sky_v3_norm(sky_v3_add(sky_v3_scale(pole, sinf(dm)), sky_v3_scale(q, cosf(dm))));
    p.weight = sky_v3_scale(p.decl, -1.0f);
    return p;
}

/** Both angles linearly, @p k not eased here and a_deg NOT wrapped: both ends
 *  lie in (-180, 180], so the counterweight swings through "down" and never
 *  over the top. Between the two pier sides of one sky point the midpoint has
 *  dm_deg 90: the tube on the pole. */
static inline sky_axis_t sky_axis_lerp(sky_axis_t from, sky_axis_t to, float k)
{
    sky_axis_t o;
    o.a_deg = from.a_deg + (to.a_deg - from.a_deg) * k;
    o.dm_deg = from.dm_deg + (to.dm_deg - from.dm_deg) * k;
    return o;
}
