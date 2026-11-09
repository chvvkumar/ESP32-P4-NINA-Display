#pragma once

/**
 * @file sky_scene_math.h
 * @brief Pure geometry and camera logic for the sky scene (NINA page layouts
 *        8 "Readout on the ground" and 9 "In the sky").
 *
 * Header-only, C99, `static inline`, float only. No ESP-IDF, FreeRTOS or LVGL
 * include: compiles unchanged on the device and on a host compiler, and is
 * host-tested by test/host/test_sky_scene_math.c.
 *
 * The P4 FPU is single precision. Every per-point routine uses float and
 * sinf/cosf/sqrtf/atan2f. The one exception is sky_lst_hours() (sidereal time
 * from a Unix clock), which runs at most once per update and uses double.
 *
 * FRAMES AND UNITS
 *   World:   x east, y north, z up. Sky directions are unit vectors in this
 *            frame (the sky is at infinity: only the camera ROTATION applies
 *            to them). Telescope model points are metres in the same frame,
 *            the tripod standing on the ground at the origin.
 *   Angles:  degrees in every signature. Azimuth 0 = north, 90 = east,
 *            clockwise, range [0, 360). Altitude 0 = horizon, 90 = zenith.
 *            Hour angle (HA) is positive WEST of the meridian, range
 *            (-180, 180]. Declination positive north.
 *   Time:    hours for RA and sidereal time, range [0, 24).
 *   Camera:  camera space x right, y up, z forward (pinhole). Screen x grows
 *            right, screen y grows DOWN: sx = px + f*x/z, sy = py - f*y/z.
 *
 * STATUS: bodies marked "WP-MATH STUB" return a safe default and are
 * implemented by work package WP-MATH (see
 * docs/superpowers/plans/2026-11-08-sky-scene-layouts.md). Every signature,
 * type, constant and documented behaviour in this file is the contract the
 * other work packages compile against: do not change one without updating the
 * plan.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "nina_plan_types.h"   /* nina_plan_t, pure (stdbool only) */

/* ---- constants ------------------------------------------------------------ */

#define SKY_DEG2RAD          0.017453292519943295f
#define SKY_RAD2DEG          57.29577951308232f
#define SKY_SIDEREAL_RATE    1.0027379f   /* sidereal seconds per solar second */

/* Horizon outline: one altitude per 5 degrees of azimuth, index i = az 5*i. */
#define SKY_HORIZON_N        72
#define SKY_HORIZON_STEP_DEG 5.0f
#define SKY_HORIZON_IN_MAX   360          /* inputs past this many are ignored */

/* Target path: one sample per 5 degrees of hour angle, index i = HA -180+5*i,
 * so index 36 is the meridian (HA 0) and index 72 repeats index 0. */
#define SKY_PATH_N           73
#define SKY_PATH_STEP_DEG    5.0f

/* Telescope model, metres. The axes cross at SKY_RIG_PIVOT_Z above the
 * ground on the vertical through the origin. */
#define SKY_RIG_PIVOT_Z      1.12f
#define SKY_RIG_CAPSULES     11           /* sky_rig_capsules() always fills this many */
#define SKY_RIG_FIT_N        7            /* sky_rig_fit_points() always fills this many */

/* Camera rig: the viewer stands SKY_CAM_DIST metres behind the telescope
 * (opposite the look direction) with the eye SKY_CAM_HEIGHT above the ground. */
#define SKY_CAM_DIST         2.6f
#define SKY_CAM_HEIGHT       1.6f
#define SKY_NEAR_Z           0.02f        /* camera-space z below this is not projected */

/* Camera fit. */
#define SKY_FOV_WIDE_DEG     110.0f       /* widest view across the frame (f_min) */
#define SKY_FOV_TIGHT_DEG    30.0f        /* tightest view across the frame (f_max) */
#define SKY_FIT_MARGIN_PX    28.0f        /* inset from the frame edge for fitted points */
#define SKY_FIT_SIDE_DEG     80.0f        /* inputs further than this off the yaw are not fitted */
#define SKY_PITCH_MIN_DEG    (-10.0f)
#define SKY_PITCH_MAX_DEG    75.0f
#define SKY_PITCH_DEFAULT    15.0f        /* used when nothing could be fitted */

/* Camera animation and pointing tolerance (consumed by nina_sky_scene.c). */
#define SKY_FLY_MS           2400         /* one fly-around, eased by sky_smoothstep() */
#define SKY_ON_TARGET_DEG    1.0f         /* mount within this of the target = on target */

/* ---- vectors ---------------------------------------------------------------- */

typedef struct {
    float x, y, z;
} sky_v3_t;

static inline sky_v3_t sky_v3(float x, float y, float z)
{
    sky_v3_t v = { x, y, z };
    return v;
}

static inline sky_v3_t sky_v3_add(sky_v3_t a, sky_v3_t b) { return sky_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline sky_v3_t sky_v3_sub(sky_v3_t a, sky_v3_t b) { return sky_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline sky_v3_t sky_v3_scale(sky_v3_t a, float k)  { return sky_v3(a.x * k, a.y * k, a.z * k); }
static inline float    sky_v3_dot(sky_v3_t a, sky_v3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float    sky_v3_len(sky_v3_t a)             { return sqrtf(sky_v3_dot(a, a)); }

static inline sky_v3_t sky_v3_cross(sky_v3_t a, sky_v3_t b)
{
    return sky_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/** Unit vector along @p a; (0,0,0) when |a| < 1e-9. */
static inline sky_v3_t sky_v3_norm(sky_v3_t a)
{
    float n = sky_v3_len(a);
    if (n < 1e-9f) return sky_v3(0.0f, 0.0f, 0.0f);
    return sky_v3_scale(a, 1.0f / n);
}

/* ---- angles ----------------------------------------------------------------- */

/** Wrap to [0, 360). */
static inline float sky_wrap360(float deg)
{
    deg = fmodf(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    if (deg >= 360.0f) deg -= 360.0f;
    return deg;
}

/** Wrap to (-180, 180]. */
static inline float sky_wrap180(float deg)
{
    deg = sky_wrap360(deg);
    return (deg > 180.0f) ? deg - 360.0f : deg;
}

/** Wrap hours to [0, 24). */
static inline float sky_wrap24(float h)
{
    h = fmodf(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    if (h >= 24.0f) h -= 24.0f;
    return h;
}

/** Hermite ease 3k^2 - 2k^3 of @p k clamped to [0,1]. 0.25 -> 0.15625. */
static inline float sky_smoothstep(float k)
{
    if (k <= 0.0f) return 0.0f;
    if (k >= 1.0f) return 1.0f;
    return k * k * (3.0f - 2.0f * k);
}

/* ---- sky directions --------------------------------------------------------- */

/** Unit direction for altitude/azimuth (degrees). */
static inline sky_v3_t sky_altaz_to_vec(float alt_deg, float az_deg)
{
    const float a = alt_deg * SKY_DEG2RAD, z = az_deg * SKY_DEG2RAD;
    const float ca = cosf(a);
    return sky_v3(ca * sinf(z), ca * cosf(z), sinf(a));
}

/** Altitude and azimuth (degrees, az in [0,360)) of a direction; @p v need not
 *  be unit length. A zero vector gives alt 0, az 0. */
static inline void sky_vec_to_altaz(sky_v3_t v, float *alt_deg, float *az_deg)
{
    sky_v3_t u = sky_v3_norm(v);
    float z = u.z;
    if (z > 1.0f) z = 1.0f;
    if (z < -1.0f) z = -1.0f;
    if (alt_deg) *alt_deg = asinf(z) * SKY_RAD2DEG;
    if (az_deg)  *az_deg  = sky_wrap360(atan2f(u.x, u.y) * SKY_RAD2DEG);
}

/**
 * Unit direction of a star at hour angle @p ha_deg (positive west) and
 * declination @p dec_deg, seen from latitude @p lat_deg:
 *   x = -cos(d) sin(H)
 *   y =  sin(d) cos(lat) - cos(d) cos(H) sin(lat)
 *   z =  sin(lat) sin(d) + cos(lat) cos(d) cos(H)
 * Example: lat 41.9, dec -5.39, HA 0 -> alt 42.71, az 180.
 */
static inline sky_v3_t sky_hadec_to_vec(float ha_deg, float dec_deg, float lat_deg)
{
    const float h = ha_deg * SKY_DEG2RAD, d = dec_deg * SKY_DEG2RAD, p = lat_deg * SKY_DEG2RAD;
    const float cd = cosf(d), sd = sinf(d), cp = cosf(p), sp = sinf(p), ch = cosf(h);
    return sky_v3(-cd * sinf(h), sd * cp - cd * ch * sp, sp * sd + cp * cd * ch);
}

/** Angle between two directions, degrees, in [0, 180]. Uses atan2 of the
 *  cross and dot products, so it stays accurate near 0. */
static inline float sky_sep_deg(sky_v3_t a, sky_v3_t b)
{
    return atan2f(sky_v3_len(sky_v3_cross(a, b)), sky_v3_dot(a, b)) * SKY_RAD2DEG;
}

/** The ELEVATED celestial pole at latitude @p lat_deg, which is where the
 *  mount's first axis points: (0, cos lat, sin lat) for lat >= 0, and
 *  (0, -cos lat, -sin lat) for lat < 0 (south pole, up by |lat|). */
static inline sky_v3_t sky_pole(float lat_deg)
{
    const float p = lat_deg * SKY_DEG2RAD;
    sky_v3_t n = sky_v3(0.0f, cosf(p), sinf(p));
    return (lat_deg >= 0.0f) ? n : sky_v3_scale(n, -1.0f);
}

/* ---- time ------------------------------------------------------------------- */

/** Hour angle in degrees, (-180, 180], from local sidereal time and RA (hours). */
static inline float sky_ha_deg(float lst_h, float ra_h)
{
    return sky_wrap180((lst_h - ra_h) * 15.0f);
}

/** Sidereal time @p dt_s solar seconds after @p lst_h, wrapped to [0,24). */
static inline float sky_lst_advance(float lst_h, float dt_s)
{
    return sky_wrap24(lst_h + dt_s * SKY_SIDEREAL_RATE / 3600.0f);
}

/**
 * Local sidereal time (hours, [0,24)) at Unix time @p unix_s and east-positive
 * longitude @p lon_deg. GMST = 18.697374558 + 24.06570982441908 * D hours with
 * D = days since 2000-01-01 12:00 UT (Unix 946728000). Double precision: call
 * once per update, never per point. Fallback only: the mount's own
 * SiderealTime is preferred whenever the mount reports it.
 */
static inline float sky_lst_hours(int64_t unix_s, float lon_deg)
{
    const double d = (double)unix_s / 86400.0 - 10957.5;
    double g = fmod(18.697374558 + 24.06570982441908 * d + (double)lon_deg / 15.0, 24.0);
    if (g < 0.0) g += 24.0;
    return (float)g;
}

/** Solar hours until the target's hour angle goes from @p ha_now_deg to
 *  @p ha_then_deg moving forward (west), in [0, 24/1.0027379). */
static inline float sky_hours_until_ha(float ha_now_deg, float ha_then_deg)
{
    return sky_wrap360(ha_then_deg - ha_now_deg) / (15.0f * SKY_SIDEREAL_RATE);
}

/** Hour angle @p minutes solar minutes after @p ha_now_deg, wrapped (-180,180]. */
static inline float sky_ha_after_minutes(float ha_now_deg, float minutes)
{
    return sky_wrap180(ha_now_deg + minutes * 0.25f * SKY_SIDEREAL_RATE);
}

/* ---- horizon outline -------------------------------------------------------- */

/**
 * Resample NINA's profile/horizon (parallel Azimuths[] and Altitudes[] arrays,
 * degrees, any order, any spacing) to SKY_HORIZON_N altitudes at azimuth
 * 5*i. Each output is the linear interpolation, along the wrapped azimuth
 * circle, between the nearest input at or below and the nearest input at or
 * above that azimuth (O(72*n), no sort, no allocation). Input azimuths are
 * wrapped to [0,360) first. Outputs are rounded to the nearest degree and
 * clamped to [-10, 89].
 *
 * Edge cases: n <= 0 or a NULL array -> out[] all 0 and returns false. n == 1
 * -> every output is that one altitude. n > SKY_HORIZON_IN_MAX -> only the
 * first SKY_HORIZON_IN_MAX pairs are used. Duplicate azimuths: the first wins.
 *
 * @return true when out[] holds a real outline.
 */
static inline bool sky_horizon_resample(const float *az_deg, const float *alt_deg, int n,
                                        int8_t out[SKY_HORIZON_N])
{
    for (int i = 0; i < SKY_HORIZON_N; i++) out[i] = 0;
    if (!az_deg || !alt_deg || n <= 0) return false;
    if (n > SKY_HORIZON_IN_MAX) n = SKY_HORIZON_IN_MAX;
    for (int k = 0; k < SKY_HORIZON_N; k++) {
        const float azk = SKY_HORIZON_STEP_DEG * (float)k;
        int lo = 0, hi = 0;
        float dlo = 1e9f, dhi = 1e9f;
        for (int i = 0; i < n; i++) {
            const float a = sky_wrap360(az_deg[i]);
            const float db = sky_wrap360(azk - a);   /* going backwards to the input */
            const float da = sky_wrap360(a - azk);   /* going forwards to the input */
            if (db < dlo) {
                dlo = db;
                lo = i;
            }
            if (da < dhi) {
                dhi = da;
                hi = i;
            }
        }
        float v;
        if (lo == hi || dlo + dhi <= 0.0f) {
            v = alt_deg[lo];
        } else {
            v = alt_deg[lo] + (alt_deg[hi] - alt_deg[lo]) * dlo / (dlo + dhi);
        }
        if (v > 89.0f) v = 89.0f;
        if (v < -10.0f) v = -10.0f;
        out[k] = (int8_t)lroundf(v);
    }
    return true;
}

/**
 * Horizon altitude (degrees) at azimuth @p az_deg: linear interpolation
 * between the two neighbouring 5 degree samples, wrapping 355 -> 0.
 * Returns 0 when @p alt72 is NULL or @p valid is false (a flat horizon).
 */
static inline float sky_horizon_alt(const int8_t *alt72, bool valid, float az_deg)
{
    if (!alt72 || !valid) return 0.0f;
    const float a = sky_wrap360(az_deg) / SKY_HORIZON_STEP_DEG;
    int i0 = (int)a;
    if (i0 >= SKY_HORIZON_N) i0 = SKY_HORIZON_N - 1;
    const int i1 = (i0 + 1) % SKY_HORIZON_N;
    const float k = a - (float)i0;
    return (float)alt72[i0] + ((float)alt72[i1] - (float)alt72[i0]) * k;
}

/**
 * Smooth horizon altitude (degrees) at azimuth @p az_deg, for drawing the
 * outline as a rounded silhouette. Monotone cubic Hermite through the 72
 * samples (uniform 5 degree spacing, wrapping 355 -> 0): the tangent at a
 * sample is the harmonic mean of the two neighbouring slopes when they share
 * a sign, else 0. Properties:
 *   - exact at every sample azimuth (5*i gives alt72[i]);
 *   - never overshoots: between two samples the value stays within them, so a
 *     wall top or a gap bottom is never raised or lowered past its data;
 *   - continuous slope, so corners are rounded over about one sample step.
 * Returns 0 when @p alt72 is NULL or @p valid is false (a flat horizon).
 */
static inline float sky_horizon_alt_smooth(const int8_t *alt72, bool valid, float az_deg)
{
    if (!alt72 || !valid) return 0.0f;
    const float a = sky_wrap360(az_deg) / SKY_HORIZON_STEP_DEG;
    int i1 = (int)a;
    if (i1 >= SKY_HORIZON_N) i1 = SKY_HORIZON_N - 1;
    const float t = a - (float)i1;
    const float p0 = (float)alt72[(i1 + SKY_HORIZON_N - 1) % SKY_HORIZON_N];
    const float p1 = (float)alt72[i1];
    const float p2 = (float)alt72[(i1 + 1) % SKY_HORIZON_N];
    const float p3 = (float)alt72[(i1 + 2) % SKY_HORIZON_N];
    const float d0 = p1 - p0, d1 = p2 - p1, d2 = p3 - p2;
    const float m1 = (d0 * d1 > 0.0f) ? 2.0f * d0 * d1 / (d0 + d1) : 0.0f;
    const float m2 = (d1 * d2 > 0.0f) ? 2.0f * d1 * d2 / (d1 + d2) : 0.0f;
    const float t2 = t * t, t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * p1 + (t3 - 2.0f * t2 + t) * m1 +
           (-2.0f * t3 + 3.0f * t2) * p2 + (t3 - t2) * m2;
}

/* ---- the target's path ------------------------------------------------------ */

/**
 * The target's diurnal circle sampled every 5 degrees of hour angle, with the
 * crossings of the horizon outline and the working arc. ~1 KB: keep it in
 * PSRAM inside the scene state, never on a task stack.
 */
typedef struct {
    sky_v3_t dir[SKY_PATH_N];   /* direction at HA = -180 + 5*i */
    uint8_t  up[SKY_PATH_N];    /* 1 when that sample is above the horizon outline at its az */
    float    ha_now_deg;        /* the HA the path was built for */
    bool     always_up;         /* every sample up: circumpolar over this outline */
    bool     never_up;          /* no sample up */
    bool     has_rise;          /* a below->above crossing exists */
    bool     has_set;           /* an above->below crossing exists */
    float    rise_ha_deg;       /* the crossing that starts the working run */
    float    set_ha_deg;        /* the crossing that ends the working run */
    float    mid_ha_deg;        /* middle of the working run, see sky_path_build() */
    int      run_first;         /* first and last sample index of the working run, */
    int      run_last;          /* run_last may be < run_first when it wraps past HA 180; -1 when never_up */
} sky_path_t;

/**
 * Build the path of a target at declination @p dec_deg from latitude
 * @p lat_deg, for the current hour angle @p ha_now_deg, against the horizon
 * outline (@p alt72 / @p horizon_valid as sky_horizon_alt()).
 *
 * up[i] = altitude of dir[i] > sky_horizon_alt(az of dir[i]).
 *
 * Crossings are found between consecutive samples i, i+1 (i = 0..71) where up
 * changes, and located by linear interpolation of (alt - horizon_alt) in HA
 * (accurate to about 0.05 degrees at 5 degree steps).
 *
 * WORKING RUN: the contiguous run of up samples (wrapping past HA 180) that
 * contains ha_now_deg when the target is up now; otherwise the next run that
 * starts after ha_now_deg going forward in HA. rise/set are that run's
 * crossings, mid_ha_deg = wrap180(rise + wrap360(set - rise) / 2).
 *
 * Special cases: always_up -> has_rise = has_set = false, run 0..72,
 * mid_ha_deg = 0 (upper culmination). never_up -> run_first = run_last = -1,
 * mid_ha_deg = 0, has_rise = has_set = false.
 *
 * Example (flat horizon, lat 41.9, dec -5.39, any ha_now in the up run):
 * rise -85.14, set +85.14, mid 0, up[36] = 1, up[0] = 0.
 */
/* Helper for sky_path_build(): altitude of @p d above the horizon outline. */
static inline float sky_path_margin_(sky_v3_t d, const int8_t *alt72, bool valid)
{
    float alt = 0.0f, az = 0.0f;
    sky_vec_to_altaz(d, &alt, &az);
    return alt - sky_horizon_alt(alt72, valid, az);
}

/* Helper for sky_path_build(): HA where the margin crosses zero between sample
 * i and sample i + 1 (i in 0..71). */
static inline float sky_path_cross_(const sky_path_t *p, int i, const int8_t *alt72, bool valid)
{
    const float g0 = sky_path_margin_(p->dir[i], alt72, valid);
    const float g1 = sky_path_margin_(p->dir[i + 1], alt72, valid);
    float k = 0.5f;
    if (g0 != g1) k = g0 / (g0 - g1);
    return sky_wrap180(-180.0f + SKY_PATH_STEP_DEG * ((float)i + k));
}

static inline void sky_path_build(sky_path_t *out, float dec_deg, float lat_deg, float ha_now_deg,
                                  const int8_t *alt72, bool horizon_valid)
{
    const int n = SKY_PATH_N - 1;   /* 72 distinct samples; index 72 repeats 0 */
    int n_up = 0;
    for (int i = 0; i < SKY_PATH_N; i++) {
        out->dir[i] = sky_hadec_to_vec(-180.0f + SKY_PATH_STEP_DEG * (float)i, dec_deg, lat_deg);
        out->up[i] = (sky_path_margin_(out->dir[i], alt72, horizon_valid) > 0.0f) ? 1 : 0;
    }
    out->up[n] = out->up[0];
    for (int i = 0; i < n; i++) n_up += out->up[i];

    out->ha_now_deg = ha_now_deg;
    out->always_up = (n_up == n);
    out->never_up = (n_up == 0);
    out->has_rise = false;
    out->has_set = false;
    out->rise_ha_deg = 0.0f;
    out->set_ha_deg = 0.0f;
    out->mid_ha_deg = 0.0f;
    out->run_first = -1;
    out->run_last = -1;
    if (out->always_up) {
        out->run_first = 0;
        out->run_last = n;
        return;
    }
    if (out->never_up) return;

    int j = (int)floorf((sky_wrap180(ha_now_deg) + 180.0f) / SKY_PATH_STEP_DEG);
    if (j < 0) j = 0;
    if (j >= n) j = 0;
    while (!out->up[j]) j = (j + 1) % n;   /* terminates: at least one sample is up */
    int first = j, last = j;
    while (out->up[(first + n - 1) % n]) first = (first + n - 1) % n;
    while (out->up[(last + 1) % n]) last = (last + 1) % n;

    out->run_first = first;
    out->run_last = last;
    out->has_rise = true;
    out->has_set = true;
    out->rise_ha_deg = sky_path_cross_(out, (first + n - 1) % n, alt72, horizon_valid);
    out->set_ha_deg = sky_path_cross_(out, last, alt72, horizon_valid);
    out->mid_ha_deg = sky_wrap180(out->rise_ha_deg +
                                  sky_wrap360(out->set_ha_deg - out->rise_ha_deg) * 0.5f);
}

/** Copy the directions of the working run's samples into @p out (at most
 *  @p max), in increasing HA order. Returns the count, 0 when never_up. */
static inline int sky_path_work_dirs(const sky_path_t *p, sky_v3_t *out, int max)
{
    if (!p || !out || max <= 0 || p->never_up || p->run_first < 0) return 0;
    int cnt = 0;
    int i = p->run_first;
    for (;;) {
        if (cnt >= max) break;
        out[cnt++] = p->dir[i];
        if (i == p->run_last) break;
        i++;
        if (i >= SKY_PATH_N - 1 && p->run_last < p->run_first) i = 0;   /* wrap past HA 180 */
        if (i >= SKY_PATH_N) break;
    }
    return cnt;
}

/* ---- the mount -------------------------------------------------------------- */

/** NINA SideOfPier. Values are stored as-is in nina_client_t.mount_pier_side. */
typedef enum {
    SKY_PIER_UNKNOWN = 0,   /* "pierUnknown" or absent */
    SKY_PIER_EAST    = 1,   /* "pierEast": tube on the east side, the normal state west of the meridian */
    SKY_PIER_WEST    = 2,   /* "pierWest": tube on the west side, the normal state east of the meridian */
} sky_pier_t;

/** The equatorial mount's true pose. All four are unit vectors, world frame. */
typedef struct {
    sky_v3_t polar;    /* first axis, sky_pole(lat) */
    sky_v3_t decl;     /* second axis, from the axis crossing toward the TUBE side */
    sky_v3_t tube;     /* where the tube points */
    sky_v3_t weight;   /* counterweight shaft direction, always -decl */
} sky_pose_t;

/**
 * Pose for a mount pointing at @p tube_dir (unit) on side @p side.
 *
 * decl is perpendicular to the first axis and to the tube:
 *   N    = (0, cos lat, sin lat)    (always the north pole vector, BOTH hemispheres)
 *   decl = +norm(N x tube) for SKY_PIER_EAST, -norm(N x tube) for SKY_PIER_WEST
 *   SKY_PIER_UNKNOWN uses EAST when @p ha_deg >= 0, else WEST.
 * Degenerate (|N x tube| < 1e-4, tube along the pole): decl = (+1,0,0) for
 * EAST, (-1,0,0) for WEST. weight = -decl. polar = sky_pole(lat).
 *
 * Physics check (lat 41.9, dec 0): HA +90 on EAST and HA -90 on WEST both give
 * weight = (0, 0.6678, -0.7443), the shaft hanging down. Southern sites
 * (lat -30, HA +90, EAST) also give weight.z < 0.
 */
static inline sky_pose_t sky_mount_pose(float lat_deg, sky_v3_t tube_dir, sky_pier_t side, float ha_deg)
{
    const float lat = lat_deg * SKY_DEG2RAD;
    const sky_v3_t north = sky_v3(0.0f, cosf(lat), sinf(lat));
    const sky_v3_t c = sky_v3_cross(north, tube_dir);
    bool east;
    switch (side) {
    case SKY_PIER_EAST:
        east = true;
        break;
    case SKY_PIER_WEST:
        east = false;
        break;
    default:
        east = (ha_deg >= 0.0f);
        break;
    }
    sky_pose_t p;
    p.polar = sky_pole(lat_deg);
    p.tube = tube_dir;
    if (sky_v3_len(c) < 1e-4f) {
        p.decl = sky_v3(1.0f, 0.0f, 0.0f);
    } else {
        p.decl = sky_v3_norm(c);
    }
    if (!east) p.decl = sky_v3_scale(p.decl, -1.0f);
    p.weight = sky_v3_scale(p.decl, -1.0f);
    return p;
}

/**
 * Rest (park) pose: tube along the elevated pole, counterweight straight down:
 * polar = tube = sky_pole(lat), decl = (0, -sin lat, cos lat),
 * weight = (0, sin lat, -cos lat). lat 41.9 -> weight (0, 0.6678, -0.7443).
 */
static inline sky_pose_t sky_mount_rest_pose(float lat_deg)
{
    const float p = lat_deg * SKY_DEG2RAD;
    sky_pose_t r;
    r.polar = sky_pole(lat_deg);
    r.tube = r.polar;
    r.decl = sky_v3(0.0f, -sinf(p), cosf(p));
    r.weight = sky_v3_scale(r.decl, -1.0f);
    return r;
}

/* ---- the telescope model ---------------------------------------------------- */

/** What a capsule is, so the renderer can colour it. */
typedef enum {
    SKY_PART_LEG = 0,
    SKY_PART_MOUNT,    /* pier, polar housing, declination housing */
    SKY_PART_SHAFT,
    SKY_PART_WEIGHT,
    SKY_PART_TUBE,
    SKY_PART_FRONT,    /* ring at the front of the tube (cyan accent) */
    SKY_PART_CAMERA,
} sky_part_t;

/** A rod from @p a to @p b of radius @p r metres. */
typedef struct {
    sky_v3_t a, b;
    float    r;
    uint8_t  part;     /* sky_part_t */
} sky_capsule_t;

/**
 * The telescope as SKY_RIG_CAPSULES capsules, in this order (O = (0,0,
 * SKY_RIG_PIVOT_Z), S = O + decl*0.42, P/D/T/W = polar/decl/tube/weight):
 *   0..2  legs     (0,0,0.84) -> feet at radius 0.55, azimuth 0/120/240, z 0, r 0.04
 *   3     pier     (0,0,0.84) -> (0,0,1.00), r 0.09                MOUNT
 *   4     polar    O - P*0.30 -> O + P*0.13, r 0.085               MOUNT
 *   5     decl     O - D*0.06 -> O + D*0.25, r 0.075               MOUNT
 *   6     shaft    O -> O + W*0.80, r 0.022                        SHAFT
 *   7     weight   O + W*0.50 -> O + W*0.65, r 0.12                WEIGHT
 *   8     tube     S - T*0.50 -> S + T*0.90, r 0.13                TUBE
 *   9     front    S + T*0.85 -> S + T*0.90, r 0.14                FRONT
 *   10    camera   S - T*0.50 -> S - T*0.66, r 0.07                CAMERA
 * Returns SKY_RIG_CAPSULES.
 */
static inline int sky_rig_capsules(const sky_pose_t *pose, sky_capsule_t out[SKY_RIG_CAPSULES])
{
    const sky_v3_t o = sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
    const sky_v3_t s = sky_v3_add(o, sky_v3_scale(pose->decl, 0.42f));
    const sky_v3_t hip = sky_v3(0.0f, 0.0f, 0.84f);
    for (int i = 0; i < 3; i++) {
        const float az = 120.0f * (float)i * SKY_DEG2RAD;
        out[i].a = hip;
        out[i].b = sky_v3(0.55f * sinf(az), 0.55f * cosf(az), 0.0f);
        out[i].r = 0.04f;
        out[i].part = SKY_PART_LEG;
    }
    out[3].a = hip;
    out[3].b = sky_v3(0.0f, 0.0f, 1.00f);
    out[3].r = 0.09f;
    out[3].part = SKY_PART_MOUNT;
    out[4].a = sky_v3_sub(o, sky_v3_scale(pose->polar, 0.30f));
    out[4].b = sky_v3_add(o, sky_v3_scale(pose->polar, 0.13f));
    out[4].r = 0.085f;
    out[4].part = SKY_PART_MOUNT;
    out[5].a = sky_v3_sub(o, sky_v3_scale(pose->decl, 0.06f));
    out[5].b = sky_v3_add(o, sky_v3_scale(pose->decl, 0.25f));
    out[5].r = 0.075f;
    out[5].part = SKY_PART_MOUNT;
    out[6].a = o;
    out[6].b = sky_v3_add(o, sky_v3_scale(pose->weight, 0.80f));
    out[6].r = 0.022f;
    out[6].part = SKY_PART_SHAFT;
    out[7].a = sky_v3_add(o, sky_v3_scale(pose->weight, 0.50f));
    out[7].b = sky_v3_add(o, sky_v3_scale(pose->weight, 0.65f));
    out[7].r = 0.12f;
    out[7].part = SKY_PART_WEIGHT;
    out[8].a = sky_v3_sub(s, sky_v3_scale(pose->tube, 0.50f));
    out[8].b = sky_v3_add(s, sky_v3_scale(pose->tube, 0.90f));
    out[8].r = 0.13f;
    out[8].part = SKY_PART_TUBE;
    out[9].a = sky_v3_add(s, sky_v3_scale(pose->tube, 0.85f));
    out[9].b = sky_v3_add(s, sky_v3_scale(pose->tube, 0.90f));
    out[9].r = 0.14f;
    out[9].part = SKY_PART_FRONT;
    out[10].a = sky_v3_sub(s, sky_v3_scale(pose->tube, 0.50f));
    out[10].b = sky_v3_sub(s, sky_v3_scale(pose->tube, 0.66f));
    out[10].r = 0.07f;
    out[10].part = SKY_PART_CAMERA;
    return SKY_RIG_CAPSULES;
}

/** The front of the tube, S + T*0.90, where the beam starts.
 *  Rest pose at lat 41.9 -> (0, 0.3894, 2.0337). */
static inline sky_v3_t sky_rig_tube_front(const sky_pose_t *pose)
{
    const sky_v3_t o = sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
    const sky_v3_t s = sky_v3_add(o, sky_v3_scale(pose->decl, 0.42f));
    return sky_v3_add(s, sky_v3_scale(pose->tube, 0.90f));
}

/**
 * The SKY_RIG_FIT_N points the camera fit holds in view: the three feet, O,
 * the tube front (S + T*0.90), the camera back (S - T*0.66) and the weight
 * end (O + W*0.80), in that order. Returns SKY_RIG_FIT_N.
 */
static inline int sky_rig_fit_points(const sky_pose_t *pose, sky_v3_t out[SKY_RIG_FIT_N])
{
    const sky_v3_t o = sky_v3(0.0f, 0.0f, SKY_RIG_PIVOT_Z);
    const sky_v3_t s = sky_v3_add(o, sky_v3_scale(pose->decl, 0.42f));
    for (int i = 0; i < 3; i++) {
        const float az = 120.0f * (float)i * SKY_DEG2RAD;
        out[i] = sky_v3(0.55f * sinf(az), 0.55f * cosf(az), 0.0f);
    }
    out[3] = o;
    out[4] = sky_rig_tube_front(pose);
    out[5] = sky_v3_sub(s, sky_v3_scale(pose->tube, 0.66f));
    out[6] = sky_v3_add(o, sky_v3_scale(pose->weight, 0.80f));
    return SKY_RIG_FIT_N;
}

/* ---- the screen frame ------------------------------------------------------- */

/**
 * Where scene content must fit, in panel pixels.
 * round == false: the rectangle x0..x1, y0..y1.
 * round == true:  the disc of radius r about (cx, cy), additionally cut at
 *                 y1 when y1 > 0 (layout 8's readout band); x0, y0, x1 unused
 *                 otherwise.
 * (px, py) is the principal point of the pinhole camera.
 */
typedef struct {
    bool  round;
    float cx, cy, r;
    float x0, y0, x1, y1;
    float px, py;
} sky_frame_t;

/** True when (x, y) lies inside the frame shrunk by @p margin px. */
static inline bool sky_frame_contains(const sky_frame_t *fr, float x, float y, float margin)
{
    if (fr->round) {
        const float dx = x - fr->cx, dy = y - fr->cy, rr = fr->r - margin;
        if (fr->y1 > 0.0f && y > fr->y1 - margin) return false;
        return rr > 0.0f && dx * dx + dy * dy <= rr * rr;
    }
    return x >= fr->x0 + margin && x <= fr->x1 - margin &&
           y >= fr->y0 + margin && y <= fr->y1 - margin;
}

/** Half the frame's width: r for a disc, min(x1-x0, y1-y0)/2 for a rect.
 *  The focal limits are f_min = half / tan(SKY_FOV_WIDE_DEG/2) and
 *  f_max = half / tan(SKY_FOV_TIGHT_DEG/2). */
static inline float sky_frame_half(const sky_frame_t *fr)
{
    if (fr->round) return fr->r;
    const float w = fr->x1 - fr->x0, h = fr->y1 - fr->y0;
    return 0.5f * ((w < h) ? w : h);
}

/* ---- the camera ------------------------------------------------------------- */

/** What the camera state machine animates. */
typedef struct {
    float yaw_deg;     /* azimuth faced, [0,360) */
    float pitch_deg;   /* up from horizontal */
    float focal_px;    /* pinhole focal length */
} sky_cam_pose_t;

/** A ready-to-project camera. */
typedef struct {
    sky_v3_t pos;            /* eye, world metres */
    sky_v3_t fwd, right, up; /* world unit basis */
    float    focal_px, px, py;
} sky_cam_t;

/** Eye position for yaw @p yaw_deg: SKY_CAM_DIST behind the axis crossing,
 *  opposite the yaw, at SKY_CAM_HEIGHT: (-sin y * DIST, -cos y * DIST, HEIGHT).
 *  Independent of pitch. Yaw 0 -> (0, -2.6, 1.6). */
static inline sky_v3_t sky_cam_pos(float yaw_deg)
{
    const float y = yaw_deg * SKY_DEG2RAD;
    return sky_v3(-sinf(y) * SKY_CAM_DIST, -cosf(y) * SKY_CAM_DIST, SKY_CAM_HEIGHT);
}

/**
 * Build the camera: fwd = (cos p sin y, cos p cos y, sin p),
 * right = (cos y, -sin y, 0), up = right x fwd, pos = sky_cam_pos(yaw),
 * focal from @p pose, principal point from @p fr. No roll ever.
 */
static inline void sky_cam_setup(sky_cam_t *c, const sky_cam_pose_t *pose, const sky_frame_t *fr)
{
    const float y = pose->yaw_deg * SKY_DEG2RAD, p = pose->pitch_deg * SKY_DEG2RAD;
    c->pos = sky_cam_pos(pose->yaw_deg);
    c->fwd = sky_v3(cosf(p) * sinf(y), cosf(p) * cosf(y), sinf(p));
    c->right = sky_v3(cosf(y), -sinf(y), 0.0f);
    c->up = sky_v3_cross(c->right, c->fwd);
    c->focal_px = pose->focal_px;
    c->px = fr->px;
    c->py = fr->py;
}

/** Camera-space vector of a world DIRECTION (rotation only: the sky is at infinity). */
static inline sky_v3_t sky_cam_dir(const sky_cam_t *c, sky_v3_t w)
{
    return sky_v3(sky_v3_dot(w, c->right), sky_v3_dot(w, c->up), sky_v3_dot(w, c->fwd));
}

/** Camera-space vector of a world POINT (metres): rotation of (p - pos). */
static inline sky_v3_t sky_cam_point(const sky_cam_t *c, sky_v3_t p)
{
    return sky_cam_dir(c, sky_v3_sub(p, c->pos));
}

/** Screen position of camera-space @p v. Returns false (outputs untouched)
 *  when v.z < SKY_NEAR_Z. */
static inline bool sky_cam_screen(const sky_cam_t *c, sky_v3_t v, float *sx, float *sy)
{
    if (v.z < SKY_NEAR_Z) return false;
    *sx = c->px + c->focal_px * v.x / v.z;
    *sy = c->py - c->focal_px * v.y / v.z;
    return true;
}

/** Shortest-way interpolation between two poses: yaw by wrap180, pitch and
 *  focal linearly; @p k is NOT eased here (the caller applies sky_smoothstep).
 *  yaw 350 -> 10 at k 0.5 gives yaw 0. */
static inline sky_cam_pose_t sky_cam_pose_lerp(sky_cam_pose_t a, sky_cam_pose_t b, float k)
{
    sky_cam_pose_t o;
    o.yaw_deg = sky_wrap360(a.yaw_deg + sky_wrap180(b.yaw_deg - a.yaw_deg) * k);
    o.pitch_deg = a.pitch_deg + (b.pitch_deg - a.pitch_deg) * k;
    o.focal_px = a.focal_px + (b.focal_px - a.focal_px) * k;
    return o;
}

/**
 * Largest focal length (px) for which camera-space @p c, seen through a
 * camera whose principal point is (fr->px, fr->py), stays inside @p fr shrunk
 * by @p margin. With u = c.x/c.z, v = c.y/c.z:
 *   rect: x = px + f*u in [x0+m, x1-m] and y = py - f*v in [y0+m, y1-m]
 *   disc: |(px + f*u - cx, py - f*v - cy)| <= r - m (largest root of the
 *         quadratic), and y <= y1 - m when y1 > 0.
 * Returns 1e9 when the point imposes no limit or c.z < SKY_NEAR_Z (it cannot
 * be fitted and is left for an edge arrow), and 0 when the principal point
 * itself is outside the shrunk frame.
 * Examples (rect 24..696, principal 360,360, margin 0): (1,0,1) -> 336,
 * (0,-0.5,1) -> 672. Disc (360,360) r 340, margin 0: (1,0,1) -> 340.
 */
static inline float sky_focal_limit(const sky_frame_t *fr, sky_v3_t c, float margin)
{
    if (c.z < SKY_NEAR_Z) return 1e9f;
    if (!sky_frame_contains(fr, fr->px, fr->py, margin)) return 0.0f;
    const float u = c.x / c.z, v = c.y / c.z;
    float f = 1e9f, lim;
    if (fr->round) {
        const float a = fr->px - fr->cx, b = fr->py - fr->cy, rr = fr->r - margin;
        const float qa = u * u + v * v;
        if (qa > 1e-12f) {
            const float qb = a * u - b * v;
            const float qc = a * a + b * b - rr * rr;
            float disc = qb * qb - qa * qc;
            if (disc < 0.0f) disc = 0.0f;
            lim = (-qb + sqrtf(disc)) / qa;
            if (lim < f) f = lim;
        }
        if (fr->y1 > 0.0f && v < 0.0f) {
            lim = (fr->y1 - margin - fr->py) / -v;
            if (lim < f) f = lim;
        }
    } else {
        if (u > 0.0f) {
            lim = (fr->x1 - margin - fr->px) / u;
            if (lim < f) f = lim;
        } else if (u < 0.0f) {
            lim = (fr->x0 + margin - fr->px) / u;
            if (lim < f) f = lim;
        }
        if (v > 0.0f) {
            lim = (fr->py - fr->y0 - margin) / v;
            if (lim < f) f = lim;
        } else if (v < 0.0f) {
            lim = (fr->py - fr->y1 + margin) / v;
            if (lim < f) f = lim;
        }
    }
    if (f < 0.0f) f = 0.0f;
    return f;
}

/* Helper for sky_cam_fit(): input @p i (dirs first, then points) as a world
 * direction from @p pos. Returns false when it is too far off the yaw. */
static inline bool sky_fit_input_(int i, sky_v3_t pos, float yaw_deg,
                                  const sky_v3_t *dirs, int n_dirs, const sky_v3_t *pts,
                                  sky_v3_t *w, float *elev)
{
    float az = 0.0f;
    *w = (i < n_dirs) ? dirs[i] : sky_v3_norm(sky_v3_sub(pts[i - n_dirs], pos));
    sky_vec_to_altaz(*w, elev, &az);
    return fabsf(sky_wrap180(az - yaw_deg)) <= SKY_FIT_SIDE_DEG;
}

/* Helper for sky_cam_fit(): step 3, the clamped focal length at this pitch. */
static inline float sky_fit_focal_(float yaw_deg, float pitch_deg, const sky_frame_t *fr,
                                   const sky_v3_t *dirs, int n_dirs,
                                   const sky_v3_t *pts, int n_pts)
{
    const float half = sky_frame_half(fr);
    const float f_min = half / tanf(0.5f * SKY_FOV_WIDE_DEG * SKY_DEG2RAD);
    const float f_max = half / tanf(0.5f * SKY_FOV_TIGHT_DEG * SKY_DEG2RAD);
    sky_cam_pose_t cp;
    sky_cam_t cam;
    cp.yaw_deg = yaw_deg;
    cp.pitch_deg = pitch_deg;
    cp.focal_px = 1.0f;
    sky_cam_setup(&cam, &cp, fr);
    float f = 1e9f;
    int kept = 0;
    for (int i = 0; i < n_dirs + n_pts; i++) {
        sky_v3_t w;
        float elev;
        if (!sky_fit_input_(i, cam.pos, yaw_deg, dirs, n_dirs, pts, &w, &elev)) continue;
        const float lim = sky_focal_limit(fr, sky_cam_dir(&cam, w), SKY_FIT_MARGIN_PX);
        if (lim < f) f = lim;
        kept++;
    }
    if (kept == 0) return f_min;   /* nothing to frame: the widest view */
    if (f < f_min) f = f_min;
    if (f > f_max) f = f_max;
    return f;
}

/**
 * Fit a camera for yaw @p yaw_deg so the given sky directions and telescope
 * points sit inside @p fr (shrunk by SKY_FIT_MARGIN_PX), as tightly as
 * allowed. Deterministic, no allocation:
 *   1. pos = sky_cam_pos(yaw). Each input becomes a world direction from pos
 *      (dirs as given, points as norm(pt - pos)). An input whose azimuth is
 *      more than SKY_FIT_SIDE_DEG off the yaw is skipped.
 *   2. pitch = clamp((min elev + max elev) / 2, SKY_PITCH_MIN_DEG,
 *      @p pitch_max_deg) over the kept inputs; SKY_PITCH_DEFAULT when none.
 *   3. f = clamp(min over kept inputs of sky_focal_limit(), f_min, f_max)
 *      (see sky_frame_half()).
 *   4. When @p horizon_max_y > 0 and py + f*tan(pitch) > horizon_max_y (the
 *      flat horizon at the yaw would fall below that row), pitch =
 *      atan((horizon_max_y - py) / f), step 3 is repeated once, and pitch is
 *      clamped by the same rule against the new f.
 * Inputs that still do not fit are the caller's edge arrows.
 */
static inline void sky_cam_fit(sky_cam_pose_t *out, float yaw_deg, const sky_frame_t *fr,
                               const sky_v3_t *dirs, int n_dirs,
                               const sky_v3_t *pts, int n_pts,
                               float pitch_max_deg, float horizon_max_y)
{
    const float yaw = sky_wrap360(yaw_deg);
    const sky_v3_t pos = sky_cam_pos(yaw);
    if (!dirs) n_dirs = 0;
    if (!pts) n_pts = 0;
    if (n_dirs < 0) n_dirs = 0;
    if (n_pts < 0) n_pts = 0;

    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n_dirs + n_pts; i++) {
        sky_v3_t w;
        float elev;
        if (!sky_fit_input_(i, pos, yaw, dirs, n_dirs, pts, &w, &elev)) continue;
        if (elev < lo) lo = elev;
        if (elev > hi) hi = elev;
    }
    float pitch = (hi >= lo) ? 0.5f * (lo + hi) : SKY_PITCH_DEFAULT;
    if (pitch < SKY_PITCH_MIN_DEG) pitch = SKY_PITCH_MIN_DEG;
    if (pitch > pitch_max_deg) pitch = pitch_max_deg;

    float f = sky_fit_focal_(yaw, pitch, fr, dirs, n_dirs, pts, n_pts);
    if (horizon_max_y > 0.0f && fr->py + f * tanf(pitch * SKY_DEG2RAD) > horizon_max_y) {
        pitch = atanf((horizon_max_y - fr->py) / f) * SKY_RAD2DEG;
        f = sky_fit_focal_(yaw, pitch, fr, dirs, n_dirs, pts, n_pts);
        if (fr->py + f * tanf(pitch * SKY_DEG2RAD) > horizon_max_y) {
            pitch = atanf((horizon_max_y - fr->py) / f) * SKY_RAD2DEG;
        }
    }
    out->yaw_deg = yaw;
    out->pitch_deg = pitch;
    out->focal_px = f;
}

/** Home yaw: 0 (facing north) for lat >= 0, 180 for lat < 0. */
static inline float sky_cam_home_yaw(float lat_deg)
{
    return (lat_deg >= 0.0f) ? 0.0f : 180.0f;
}

/** Work yaw: the azimuth of the working run's middle,
 *  sky_hadec_to_vec(p->mid_ha_deg, dec, lat). Flat horizon, lat 41.9: dec
 *  -5.39 -> 180, dec 70 (always up) -> 0. */
static inline float sky_cam_work_yaw(const sky_path_t *p, float dec_deg, float lat_deg)
{
    float az = 0.0f;
    sky_vec_to_altaz(sky_hadec_to_vec(p->mid_ha_deg, dec_deg, lat_deg), (float *)0, &az);
    return az;
}

/**
 * Edge arrow for camera-space @p c when it does NOT project inside @p fr
 * shrunk by @p margin. Screen direction d = norm(c.x, -c.y) (works behind the
 * camera too; (0, 1) straight down when both are ~0). The arrow sits where
 * the ray from (px, py) along d meets the frame shrunk by @p margin;
 * *angle_deg = atan2(d.y, d.x) in degrees (0 = pointing right, 90 = down).
 * Returns false (outputs untouched) when the point projects inside, so no
 * arrow is needed.
 * Example (rect 24..696, principal 360,360, focal 300, margin 28):
 * (2,0,1) -> (668, 360), angle 0; (0,0,-1) -> (360, 668), angle 90.
 */
static inline bool sky_edge_arrow(const sky_frame_t *fr, const sky_cam_t *cam, sky_v3_t c,
                                  float margin, float *x, float *y, float *angle_deg)
{
    float sx = 0.0f, sy = 0.0f;
    if (sky_cam_screen(cam, c, &sx, &sy) && sky_frame_contains(fr, sx, sy, margin)) return false;
    float dx = c.x, dy = -c.y;
    const float n = sqrtf(dx * dx + dy * dy);
    if (n < 1e-6f) {
        dx = 0.0f;
        dy = 1.0f;
    } else {
        dx /= n;
        dy /= n;
    }
    const float px = fr->px, py = fr->py;
    float t = 0.0f;
    if (fr->round) {
        const float a = px - fr->cx, b = py - fr->cy, rr = fr->r - margin;
        const float qb = a * dx + b * dy;
        float disc = qb * qb - (a * a + b * b - rr * rr);
        if (disc < 0.0f) disc = 0.0f;
        t = -qb + sqrtf(disc);
        if (fr->y1 > 0.0f && dy > 0.0f && py + t * dy > fr->y1 - margin) {
            t = (fr->y1 - margin - py) / dy;
        }
    } else {
        float best = 1e9f, tt;
        if (dx > 0.0f) {
            tt = (fr->x1 - margin - px) / dx;
            if (tt < best) best = tt;
        } else if (dx < 0.0f) {
            tt = (fr->x0 + margin - px) / dx;
            if (tt < best) best = tt;
        }
        if (dy > 0.0f) {
            tt = (fr->y1 - margin - py) / dy;
            if (tt < best) best = tt;
        } else if (dy < 0.0f) {
            tt = (fr->y0 + margin - py) / dy;
            if (tt < best) best = tt;
        }
        t = (best < 1e9f) ? best : 0.0f;
    }
    if (t < 0.0f) t = 0.0f;
    *x = px + t * dx;
    *y = py + t * dy;
    *angle_deg = atan2f(dy, dx) * SKY_RAD2DEG;
    return true;
}

/* ---- words ------------------------------------------------------------------ */

/**
 * Pictures done and planned, for "17 of 75".
 * With a plan (plan && plan->n_items > 0): over ENABLED items only,
 *   per_round = sum(iterations), total = rounds * per_round,
 *   done = round_done * per_round + sum(completed), clamped to [0, total].
 * Without one: done = @p fallback_done, total = @p fallback_total (the
 * per-filter exposure_count / exposure_iterations); total <= 0 means unknown
 * and the caller shows the done count alone.
 * Example: L 10 iter 3 done, R 5 iter 0 done, rounds 3, round_done 1 -> 18 of 45.
 */
static inline void sky_plan_counts(const nina_plan_t *plan, int fallback_done, int fallback_total,
                                   int *done, int *total)
{
    if (!plan || plan->n_items <= 0) {
        *done = fallback_done;
        *total = fallback_total;
        return;
    }
    int n = plan->n_items;
    if (n > NINA_PLAN_MAX_ITEMS) n = NINA_PLAN_MAX_ITEMS;
    int per_round = 0, completed = 0;
    for (int i = 0; i < n; i++) {
        if (!plan->items[i].enabled) continue;
        per_round += plan->items[i].iterations;
        completed += plan->items[i].completed;
    }
    const int t = plan->rounds * per_round;
    int d = plan->round_done * per_round + completed;
    if (d > t) d = t;
    if (d < 0) d = 0;
    *done = d;
    *total = (t > 0) ? t : 0;
}

/** What the rig is doing, which decides the plain word shown. */
typedef enum {
    SKY_ACT_RESTING = 0,   /* parked, or idle with nothing better known: "resting" */
    SKY_ACT_EXPOSING,      /* a picture is running: seconds, no word */
    SKY_ACT_MOVING,        /* slewing: "moving" */
    SKY_ACT_FLIPPING,      /* meridian flip running: "flipping" */
    SKY_ACT_DONE,          /* every planned picture taken: "done" */
    SKY_ACT_BELOW,         /* target known but under the horizon outline: "up at HH:MM" */
    SKY_ACT_WAITING,       /* tracking on target between pictures: dim "0" */
} sky_activity_t;

/**
 * First match wins, in this order:
 *   flipping                   -> FLIPPING
 *   exposing                   -> EXPOSING   (darks taken at park still show seconds)
 *   at_park                    -> RESTING    (BEFORE slewing: a parked mount has
 *                                             reported Slewing true with AtPark)
 *   slewing                    -> MOVING
 *   total > 0 && done >= total -> DONE
 *   target_known && !target_up -> BELOW
 *   tracking                   -> WAITING
 *   otherwise                  -> RESTING
 */
static inline sky_activity_t sky_activity(bool flipping, bool exposing, bool at_park, bool slewing,
                                          bool tracking, bool target_known, bool target_up,
                                          int done, int total)
{
    if (flipping) return SKY_ACT_FLIPPING;
    if (exposing) return SKY_ACT_EXPOSING;
    if (at_park) return SKY_ACT_RESTING;
    if (slewing) return SKY_ACT_MOVING;
    if (total > 0 && done >= total) return SKY_ACT_DONE;
    if (target_known && !target_up) return SKY_ACT_BELOW;
    if (tracking) return SKY_ACT_WAITING;
    return SKY_ACT_RESTING;
}
