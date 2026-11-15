/* Pure night sky helpers (twilight, Moon, star look, guiding ring, hills,
 * compass and time-left parsing) for the sky scene layouts 8 and 9.
 *
 * Header-only, C99, static inline, single-precision float only. No ESP-IDF,
 * FreeRTOS or LVGL include. Every function clamps or guards its inputs so it
 * never returns a non-finite value (a NaN or infinite input reads as 0 or the
 * documented fallback). Host-tested by test/host/test_sky_night_math.c.
 *
 * Colours are raw 0xRRGGBB. The caller maps them (Red Night, brightness) later.
 */
#pragma once

#include "sky_scene_math.h"
#include <string.h>

#define SKY_PULSE_S          0.9f
#define SKY_ARRIVE_S         0.6f
#define SKY_DRAIN_S          0.45f
#define SKY_TRAIL_S          1.2f
#define SKY_SWEEP_FADE_S     1.0f
#define SKY_STAR_MAG_LIMIT   4.6f

/* ---- small helpers ---------------------------------------------------------- */

/* Clamp that maps NaN to lo. */
static inline float sky_nm_clampf(float v, float lo, float hi)
{
    if (!(v > lo)) return lo;
    if (v > hi) return hi;
    return v;
}

/* Linear mix of two raw 0xRRGGBB colours, k clamped to [0,1], rounded per channel. */
static inline uint32_t sky_rgb_mix(uint32_t a, uint32_t b, float k)
{
    k = sky_nm_clampf(k, 0.0f, 1.0f);
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const float ca = (float)((a >> sh) & 0xFFu);
        const float cb = (float)((b >> sh) & 0xFFu);
        int v = (int)(ca + (cb - ca) * k + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out |= ((uint32_t)v) << sh;
    }
    return out;
}

/* ---- twilight and Moon light ------------------------------------------------ */

/* 1 with the Sun at -5 degrees or higher, 0 at -18 or lower, linear between. */
static inline float sky_twilight(float sun_alt_deg)
{
    return sky_nm_clampf((sun_alt_deg + 18.0f) / 13.0f, 0.0f, 1.0f);
}

/* Moon light on the sky: illum * clamp(alt / 30, 0, 1); 0 at or below the horizon. */
static inline float sky_moon_light(float illum, float moon_alt_deg)
{
    if (!(moon_alt_deg > 0.0f)) return 0.0f;
    return sky_nm_clampf(illum, 0.0f, 1.0f) * sky_nm_clampf(moon_alt_deg / 30.0f, 0.0f, 1.0f);
}

/* Star-washing Moon light: illum * clamp((alt + 1) / 20, 0, 1); 0 at or below -1. */
static inline float sky_moon_wash(float illum, float moon_alt_deg)
{
    if (!(moon_alt_deg > -1.0f)) return 0.0f;
    return sky_nm_clampf(illum, 0.0f, 1.0f) * sky_nm_clampf((moon_alt_deg + 1.0f) / 20.0f, 0.0f, 1.0f);
}

typedef struct { uint32_t top, mid, low; } sky_grad_t;

/* Three sky gradient stops (raw colours in, raw out). Moon light blends toward
 * the Moon pair, then twilight moves 90 percent toward the dusk pair. */
static inline sky_grad_t sky_colours(float tw, float ml, uint32_t top, uint32_t glow,
                                     uint32_t top_dusk, uint32_t glow_dusk,
                                     uint32_t top_moon, uint32_t glow_moon, uint32_t haze)
{
    sky_grad_t g;
    tw = sky_nm_clampf(tw, 0.0f, 1.0f);
    ml = sky_nm_clampf(ml, 0.0f, 1.0f);
    uint32_t t = sky_rgb_mix(top, top_moon, ml);
    uint32_t gl = sky_rgb_mix(glow, glow_moon, ml);
    t = sky_rgb_mix(t, top_dusk, 0.9f * tw);
    gl = sky_rgb_mix(gl, glow_dusk, 0.9f * tw);
    g.top = t;
    g.mid = sky_rgb_mix(t, gl, 0.4f);
    g.low = sky_rgb_mix(gl, haze, 0.15f);
    return g;
}

/* Horizon haze: mix(haze, dusk, 0.5 tw). */
static inline uint32_t sky_haze_colour(float tw, uint32_t haze, uint32_t dusk)
{
    return sky_rgb_mix(haze, dusk, 0.5f * sky_nm_clampf(tw, 0.0f, 1.0f));
}

/* Sun-side glow at normalised distance d (0 centre, 1 edge): colour near->far
 * over 0..0.45 then far; alpha 0.36 tw at 0, 0.15 tw at 0.45, 0 at 1 (linear). */
static inline void sky_sun_glow(float d, float tw, uint32_t near_rgb, uint32_t far_rgb,
                                uint32_t *rgb, float *alpha)
{
    float a;
    uint32_t c;
    tw = sky_nm_clampf(tw, 0.0f, 1.0f);
    d = sky_nm_clampf(d, 0.0f, 1.0f);
    if (d < 0.45f) {
        const float u = d / 0.45f;
        a = 0.36f + (0.15f - 0.36f) * u;
        c = sky_rgb_mix(near_rgb, far_rgb, u);
    } else {
        const float u = (d - 0.45f) / 0.55f;
        a = 0.15f * (1.0f - u);
        c = far_rgb;
    }
    if (rgb) *rgb = c;
    if (alpha) *alpha = a * tw;
}

/* Screen column of the Sun glow centre. yaw_off_deg is the Sun azimuth minus
 * the camera yaw (positive = to the right). A visible projection within 85
 * degrees is clamped to -0.1..1.1 of size; otherwise the sign picks the edge. */
static inline float sky_sun_side_x(bool vis, float proj_x, float yaw_off_deg, float size)
{
    const float lo = -0.1f * size, hi = 1.1f * size;
    if (!(size > 0.0f) || size > 1.0e6f) return 0.0f;
    if (!(yaw_off_deg == yaw_off_deg)) yaw_off_deg = 0.0f;
    if (vis && fabsf(yaw_off_deg) <= 85.0f && proj_x == proj_x) {
        if (proj_x < lo) return lo;
        if (proj_x > hi) return hi;
        return proj_x;
    }
    return (yaw_off_deg < 0.0f) ? lo : hi;
}

/* ---- stars ------------------------------------------------------------------ */

/* Faintest magnitude drawn: 4.6 at night, minus 2.4 tw and 1.4 wash. */
static inline float sky_star_limit(float tw, float wash)
{
    return SKY_STAR_MAG_LIMIT - 2.4f * sky_nm_clampf(tw, 0.0f, 1.0f)
                              - 1.4f * sky_nm_clampf(wash, 0.0f, 1.0f);
}

static inline float sky_star_alpha(float mag, float limit)
{
    if (!(mag == mag) || !(limit == limit) || mag > limit) return 0.0f;
    return sky_nm_clampf((limit - mag) / 2.2f, 0.18f, 1.0f) * 0.72f;
}

static inline float sky_star_radius(float mag)
{
    if (!(mag == mag)) return 0.65f;
    return sky_nm_clampf(2.5f - 0.45f * mag, 0.65f, 3.1f);
}

/* Fade of a star near the Moon: 1 beyond 26 degrees, 1 - wash inside 4. */
static inline float sky_star_moon_fade(float wash, float sep_deg)
{
    wash = sky_nm_clampf(wash, 0.0f, 1.0f);
    return 1.0f - wash * (1.0f - sky_nm_clampf((sep_deg - 4.0f) / 22.0f, 0.0f, 1.0f));
}

/* Distance from point p to segment ab (screen pixels). */
static inline float sky_seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    const float dx = bx - ax, dy = by - ay;
    const float l2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (l2 > 1e-12f) t = sky_nm_clampf(((px - ax) * dx + (py - ay) * dy) / l2, 0.0f, 1.0f);
    const float qx = ax + t * dx - px, qy = ay + t * dy - py;
    const float d = sqrtf(qx * qx + qy * qy);
    return (d == d && d < 1e18f) ? d : 0.0f;
}

/* ---- Moon ------------------------------------------------------------------- */

/* The lit part of the Moon as nine point pairs: limb[i] on the lit limb and
 * term[i] on the terminator, i = 0..8 from the top pole to the bottom pole,
 * centred at (cx, cy), radius r, then rotated by ang_rad so the lit side
 * (local +x) faces direction ang_rad (screen radians, y down). */
static inline void sky_moon_lit_strip(float illum, float r, float ang_rad, float cx, float cy,
                                      float limb[9][2], float term[9][2])
{
    illum = sky_nm_clampf(illum, 0.0f, 1.0f);
    r = sky_nm_clampf(r, 0.0f, 1.0e6f);
    if (!(ang_rad == ang_rad)) ang_rad = 0.0f;
    if (!(cx == cx)) cx = 0.0f;
    if (!(cy == cy)) cy = 0.0f;
    const float ca = cosf(ang_rad), sa = sinf(ang_rad);
    const float k = 2.0f * illum - 1.0f;
    for (int i = 0; i < 9; i++) {
        const float t = (-90.0f + 22.5f * (float)i) * SKY_DEG2RAD;
        const float c = (i == 0 || i == 8) ? 0.0f : cosf(t);
        const float y = r * sinf(t);
        const float lx = r * c;
        const float tx = -k * r * c;
        limb[i][0] = cx + lx * ca - y * sa;
        limb[i][1] = cy + lx * sa + y * ca;
        term[i][0] = cx + tx * ca - y * sa;
        term[i][1] = cy + tx * sa + y * ca;
    }
}

/* One pixel of the Moon glow mask (size x size A8 picture, centred). Alpha is
 * peak (0..1) out to inner_r, falling linearly to 0 at size/2, plus a 4x4
 * ordered dither of +-dither/2 (8-bit units). 0 outside the radius. */
static inline uint8_t sky_glow_a8(int x, int y, int size, float inner_r, float peak, float dither)
{
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    if (size < 2) return 0;
    const float R = (float)size * 0.5f;
    const float c = ((float)size - 1.0f) * 0.5f;
    const float dx = (float)x - c, dy = (float)y - c;
    const float d = sqrtf(dx * dx + dy * dy);
    if (!(d < R)) return 0;
    inner_r = sky_nm_clampf(inner_r, 0.0f, R - 0.001f);
    peak = sky_nm_clampf(peak, 0.0f, 1.0f);
    dither = sky_nm_clampf(dither, 0.0f, 64.0f);
    float f = 1.0f;
    if (d > inner_r) f = 1.0f - (d - inner_r) / (R - inner_r);
    const float b = ((float)bayer[((unsigned)y & 3u) * 4u + ((unsigned)x & 3u)] + 0.5f) / 16.0f - 0.5f;
    float v = peak * 255.0f * f + b * dither;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)(v + 0.5f);
}

/* ---- guiding ring, HFR, light gathering -------------------------------------- */

typedef struct { float amp_px, poor; } sky_guide_t;

/* Wobble of the ring outline. poor = clamp((rms - good_max) / (ok_max - good_max)).
 * amp = clamp(stddev(last 8) * 12 + poor * 2.2, 0, 3.5) rounded to 0.5 px.
 * Fallback thresholds 0.55 and 1.15 when good_max <= 0 or ok_max <= good_max. */
static inline sky_guide_t sky_guide_shape(const float *hist, int n, float rms, float good_max, float ok_max)
{
    sky_guide_t g;
    float m = 0.0f, v = 0.0f;
    int k = 0;
    if (!(good_max > 0.0f) || !(ok_max > good_max) || !(ok_max < 1.0e9f)) {
        good_max = 0.55f;
        ok_max = 1.15f;
    }
    if (hist && n > 0) {
        const int from = (n > 8) ? n - 8 : 0;
        for (int i = from; i < n; i++) {
            if (hist[i] == hist[i] && fabsf(hist[i]) < 1.0e6f) {
                m += hist[i];
                k++;
            }
        }
        if (k > 0) {
            m /= (float)k;
            for (int i = from; i < n; i++) {
                if (hist[i] == hist[i] && fabsf(hist[i]) < 1.0e6f) v += (hist[i] - m) * (hist[i] - m);
            }
            v /= (float)k;
        }
    }
    g.poor = sky_nm_clampf((rms - good_max) / (ok_max - good_max), 0.0f, 1.0f);
    float amp = sky_nm_clampf(sqrtf(v) * 12.0f + g.poor * 2.2f, 0.0f, 3.5f);
    amp = floorf(amp * 2.0f + 0.5f) * 0.5f;
    g.amp_px = (amp > 3.5f) ? 3.5f : amp;
    return g;
}

/* Ring radius at screen angle ang_rad with the two-harmonic wobble. */
static inline float sky_ring_radius(float base_r, float amp_px, float ang_rad, float ph1, float ph2)
{
    if (!(ang_rad == ang_rad)) ang_rad = 0.0f;
    if (!(ph1 == ph1)) ph1 = 0.0f;
    if (!(ph2 == ph2)) ph2 = 0.0f;
    amp_px = sky_nm_clampf(amp_px, 0.0f, 1.0e3f);
    return base_r + amp_px * (0.6f * sinf(3.0f * ang_rad + ph1) + 0.4f * sinf(5.0f * ang_rad + ph2));
}

/* 0 sharp .. 1 soft from the node's HFR thresholds (fallback 2.6 and 4.2). */
static inline float sky_hfr_soft(float hfr, float good_max, float ok_max)
{
    if (!(hfr > 0.0f)) return 0.0f;
    if (!(good_max > 0.0f) || !(ok_max > good_max) || !(ok_max < 1.0e9f)) {
        good_max = 2.6f;
        ok_max = 4.2f;
    }
    return sky_nm_clampf((hfr - good_max) / (ok_max - good_max), 0.0f, 1.0f);
}

/* Light gathered by the target: the exposure fraction, or after a finish
 * 1 - age / SKY_DRAIN_S while that is larger. pulse_age_s < 0 = no pulse. */
static inline float sky_gathered(bool exposing, float frac, float pulse_age_s)
{
    float g = exposing ? sky_nm_clampf(frac, 0.0f, 1.0f) : 0.0f;
    if (pulse_age_s == pulse_age_s && pulse_age_s >= 0.0f) {
        const float dr = 1.0f - pulse_age_s / SKY_DRAIN_S;
        if (dr > g) g = dr;
    }
    return sky_nm_clampf(g, 0.0f, 1.0f);
}

/* ---- text ------------------------------------------------------------------- */

/* Minutes from "2h 05m" (NINA) or "2:05" (demo); -1 for anything else.
 * ponytail: parses a display string; add a numeric field to nina_client_t if a third format ever appears. */
static inline int sky_parse_minutes(const char *s)
{
    int h = 0, m = 0, nd = 0;
    if (!s) return -1;
    while (*s >= '0' && *s <= '9' && nd < 4) {
        h = h * 10 + (*s - '0');
        s++;
        nd++;
    }
    if (nd == 0) return -1;
    if (*s == ':') {
        s++;
        if (!(s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') || s[2] != '\0') return -1;
        m = (s[0] - '0') * 10 + (s[1] - '0');
    } else if (*s == 'h') {
        s++;
        if (*s != ' ') return -1;
        s++;
        if (!(s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') || s[2] != 'm' || s[3] != '\0') return -1;
        m = (s[0] - '0') * 10 + (s[1] - '0');
    } else {
        return -1;
    }
    if (m > 59) return -1;
    return h * 60 + m;
}

/* Compass text for an azimuth: rank 2 N/E/S/W, rank 1 NE/SE/SW/NW, rank 0 the
 * degrees plus a UTF-8 degree sign written into buf. Returns "" if buf is unusable. */
static inline const char *sky_compass_label(int az_deg, char *buf, size_t n, int *rank)
{
    int a = az_deg % 360;
    if (a < 0) a += 360;
    if (rank) *rank = 2;
    switch (a) {
    case 0:   return "N";
    case 90:  return "E";
    case 180: return "S";
    case 270: return "W";
    default:  break;
    }
    if (rank) *rank = 1;
    switch (a) {
    case 45:  return "NE";
    case 135: return "SE";
    case 225: return "SW";
    case 315: return "NW";
    default:  break;
    }
    if (rank) *rank = 0;
    if (!buf || n < 6) return "";
    snprintf(buf, n, "%d\xC2\xB0", a);
    return buf;
}

/* ---- hills ------------------------------------------------------------------ */

/* Integer mix to [0, 1). */
static inline float sky_hash01(uint32_t n)
{
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    n ^= n >> 16;
    return (float)(n >> 8) * (1.0f / 16777216.0f);
}

/* Value noise with cells lattice points round the circle; wraps cleanly at 360. */
static inline float sky_vnoise_wrap(float az_deg, int cells, uint32_t seed)
{
    if (cells < 1) cells = 1;
    if (cells > 4096) cells = 4096;
    if (!(az_deg == az_deg) || fabsf(az_deg) > 1.0e7f) az_deg = 0.0f;
    const float x = sky_wrap360(az_deg) / 360.0f * (float)cells;
    int i0 = (int)floorf(x);
    if (i0 < 0) i0 = 0;
    if (i0 >= cells) i0 = cells - 1;
    const float f = sky_smoothstep(x - (float)i0);
    const int i1 = (i0 + 1) % cells;
    const float a = sky_hash01(seed + (uint32_t)i0 * 0x9e3779b1U);
    const float b = sky_hash01(seed + (uint32_t)i1 * 0x9e3779b1U);
    return a + (b - a) * f;
}

/* Hill height fraction (of the floor-to-outline height) at an azimuth.
 * layer 1 = middle hills, 2 = near hills; result 0.05..0.9. */
static inline float sky_hill_frac(float az_deg, int layer)
{
    const float n144 = sky_vnoise_wrap(az_deg, 144, 144u);
    if (layer == 2) {
        const float n45 = sky_vnoise_wrap(az_deg, 45, 45u);
        return sky_nm_clampf(0.22f + 0.20f * (n45 - 0.5f) + 0.06f * (n144 - 0.5f), 0.05f, 0.9f);
    }
    const float n19 = sky_vnoise_wrap(az_deg, 19, 19u);
    return sky_nm_clampf(0.55f + 0.44f * (n19 - 0.5f) + 0.06f * (n144 - 0.5f), 0.05f, 0.9f);
}
