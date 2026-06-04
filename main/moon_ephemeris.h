#pragma once

#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float       illum;        /* illuminated fraction 0..1 */
    float       cycle;        /* synodic cycle fraction 0..1 (0=new, 0.5=full) */
    bool        waxing;       /* true = waxing (Moon east of Sun) */
    uint8_t     phase_index;  /* 0..7: New, WaxCres, FirstQ, WaxGib, Full, WanGib, LastQ, WanCres */
    const char *phase_name;   /* human-readable phase name */
    float       orient_rad;   /* rotation to apply to a north-up disc so the lit limb
                                 points to the correct sky direction (radians, CCW+).
                                 0 = north-up convention (lit-on-right for waxing). */
    float       lib_lon;      /* optical libration in longitude l' (radians, +E) */
    float       lib_lat;      /* optical libration in latitude  b' (radians, +N) */
    float       sun_lon;      /* sub-solar selenographic longitude (radians) */
    float       sun_lat;      /* sub-solar selenographic latitude  (radians) */
    float       roll;         /* disc roll for renderer (radians, CCW+): P-q located, P north-up */
    float       axis_P;       /* position angle of lunar axis P (radians), pre-parallactic */
    bool        have_location;/* false => lat/lon were unset; orient_rad is the
                                 north-up fallback */
} moon_state_t;

/* Compute moon phase + orientation for a UTC time and observer location.
 * lat/lon in degrees. If lat==0 && lon==0 they are treated as unset and a
 * north-up convention orientation is returned (have_location=false). */
void moon_compute(time_t utc, double lat, double lon, moon_state_t *out);

/* Build a moon_state_t from a synodic cycle fraction (0=new, 0.5=full) and a
 * supplied orientation, without any ephemeris computation. Used for animating
 * through the moon cycle. */
void moon_state_from_cycle(double cycle, float orient_rad, moon_state_t *out);

/* Compute the current/next moon up-period relative to `now`.
 * lat/lon are observer coordinates in decimal degrees.
 * If lat==0.0 && lon==0.0 (location unset) both events are marked invalid.
 *
 * *set  = first downward horizon crossing AFTER now (end of current or next
 *         up-period); *set_valid = false if none found in the search window.
 * *rise:
 *   - moon UP   at now -> UTC time of the upward crossing that STARTED the
 *                         current up-period (the most recent rise <= now).
 *   - moon DOWN at now -> UTC time of the NEXT upward crossing after now.
 *   *rise_valid = false if no such crossing is found in the search window.
 *
 * h0 = +0.125 deg (refraction + parallax approximation).
 * Search window: [now - 26 h, now + 50 h] at 10-minute steps (76 h total). */
void moon_rise_set(time_t now, double lat, double lon,
                   time_t *rise, bool *rise_valid,
                   time_t *set,  bool *set_valid);

#ifdef __cplusplus
}
#endif
