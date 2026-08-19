#pragma once

/**
 * @file adsb_geom.h
 * @brief Pure geometry for the ADS-B page: elevation over a curved earth,
 *        great-circle distance/bearing, angular separation on the sky, and the
 *        two screen projections (Sky Dome, Radar Scope).
 *
 * Header-only, `float`-only (the P4 FPU is single precision -- `double` in a
 * per-contact loop drops to soft-float), nothing beyond <math.h>. Compiles
 * unchanged on the device and on a host compiler; host-tested by
 * test/host/test_adsb_geom.c.
 *
 * Conventions: azimuth/bearing 0 = true north, increasing clockwise (east =
 * 90). Elevation 0 = horizon, 90 = zenith. Screen y grows DOWNWARD, which is
 * why both projections subtract the cos term from cy.
 */

#include <math.h>

/** Mean earth radius, metres (no refraction correction -- see adsb_elevation_deg). */
#define ADSB_EARTH_R_M   6371000.0f
#define ADSB_M_PER_NM    1852.0f
#define ADSB_M_PER_FT    0.3048f

#define ADSB_DEG2RAD     0.017453292519943295f
#define ADSB_RAD2DEG     57.29577951308232f

/** Wrap to [0,360). */
static inline float adsb_wrap360(float deg)
{
    deg = fmodf(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

/** Wrap to (-180,180]. */
static inline float adsb_wrap180(float deg)
{
    deg = adsb_wrap360(deg + 180.0f) - 180.0f;
    return deg;
}

/**
 * Elevation angle of a contact above the receiver's horizon, degrees.
 *
 * Geometric only: no atmospheric refraction (which would bias every contact
 * up by a few hundredths of a degree at these elevations -- below the panel's
 * resolution and below the position error of the ADS-B fix itself).
 *
 * `d` is the ground distance, `h` the height above the receiver; the
 * `d*d/(2R)` term is the earth's drop over that distance, which is what makes
 * a distant aircraft sit LOWER than flat-earth trigonometry says (5.9 km of
 * drop at 250 nm).
 *
 * @param dist_nm    ground distance receiver -> contact, nautical miles
 * @param alt_ft     contact altitude above MSL, feet
 * @param rx_elev_m  receiver elevation above MSL, metres
 */
static inline float adsb_elevation_deg(float dist_nm, float alt_ft, float rx_elev_m)
{
    float d = dist_nm * ADSB_M_PER_NM;
    float h = alt_ft * ADSB_M_PER_FT - rx_elev_m;
    if (d <= 0.0f) return (h >= 0.0f) ? 90.0f : -90.0f;
    return atan2f(h - (d * d) / (2.0f * ADSB_EARTH_R_M), d) * ADSB_RAD2DEG;
}

/** Great-circle distance between two lat/lon pairs, nautical miles. */
static inline float adsb_haversine_nm(float lat1, float lon1, float lat2, float lon2)
{
    float p1 = lat1 * ADSB_DEG2RAD;
    float p2 = lat2 * ADSB_DEG2RAD;
    float dp = (lat2 - lat1) * ADSB_DEG2RAD;
    float dl = (lon2 - lon1) * ADSB_DEG2RAD;
    float sp = sinf(dp * 0.5f);
    float sl = sinf(dl * 0.5f);
    float a = sp * sp + cosf(p1) * cosf(p2) * sl * sl;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    return (c * ADSB_EARTH_R_M) / ADSB_M_PER_NM;
}

/** Initial great-circle bearing from point 1 to point 2, degrees true [0,360). */
static inline float adsb_bearing_deg(float lat1, float lon1, float lat2, float lon2)
{
    float p1 = lat1 * ADSB_DEG2RAD;
    float p2 = lat2 * ADSB_DEG2RAD;
    float dl = (lon2 - lon1) * ADSB_DEG2RAD;
    float y = sinf(dl) * cosf(p2);
    float x = cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl);
    return adsb_wrap360(atan2f(y, x) * ADSB_RAD2DEG);
}

/** Angular separation between two sky directions (az/el, degrees), degrees. */
static inline float adsb_sep_deg(float az1, float el1, float az2, float el2)
{
    float e1 = el1 * ADSB_DEG2RAD;
    float e2 = el2 * ADSB_DEG2RAD;
    float da = (az1 - az2) * ADSB_DEG2RAD;
    float c = sinf(e1) * sinf(e2) + cosf(e1) * cosf(e2) * cosf(da);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * ADSB_RAD2DEG;
}

/**
 * Sky Dome projection: truncated stereographic, so the whole visible dome from
 * @p min_el up to the zenith fills the disc of radius @p radius.
 *
 * `r = radius * tan((90-el)/2) / tan((90-min_el)/2)`: zenith at the centre,
 * the gate elevation exactly on the rim. Stereographic (rather than the
 * cheaper `r = radius*(90-el)/(90-min_el)`) keeps small circles round, which
 * is what makes the per-rig FOV circle honest.
 *
 * @p up_az is rotated to the top of the screen; contacts below @p min_el
 * project outside the rim (r > radius) and the caller is expected to drop them.
 */
static inline void adsb_sky_project(float az_deg, float el_deg, float min_el_deg,
                                    float up_az_deg, float cx, float cy, float radius,
                                    float *out_x, float *out_y)
{
    float denom = tanf((90.0f - min_el_deg) * 0.5f * ADSB_DEG2RAD);
    if (denom < 1e-6f) denom = 1e-6f;          /* min_el -> 90: degenerate disc */
    float r = radius * tanf((90.0f - el_deg) * 0.5f * ADSB_DEG2RAD) / denom;
    float t = (az_deg - up_az_deg) * ADSB_DEG2RAD;
    *out_x = cx + r * sinf(t);
    *out_y = cy - r * cosf(t);
}

/**
 * Radar Scope projection: linear in ground distance, same rotation as the Sky
 * Dome so a contact keeps its screen bearing when the mode is cycled.
 * `dist == range` lands exactly on the rim.
 */
static inline void adsb_scope_project(float bearing_deg, float dist_nm, float range_nm,
                                      float up_az_deg, float cx, float cy, float radius,
                                      float *out_x, float *out_y)
{
    if (range_nm < 1e-3f) range_nm = 1e-3f;
    float r = radius * (dist_nm / range_nm);
    float t = (bearing_deg - up_az_deg) * ADSB_DEG2RAD;
    *out_x = cx + r * sinf(t);
    *out_y = cy - r * cosf(t);
}
