#pragma once

#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float       illum;        /* illuminated fraction 0..1 */
    bool        waxing;       /* true = waxing (Moon east of Sun) */
    uint8_t     phase_index;  /* 0..7: New, WaxCres, FirstQ, WaxGib, Full, WanGib, LastQ, WanCres */
    const char *phase_name;   /* human-readable phase name */
    float       orient_rad;   /* rotation to apply to a north-up disc so the lit limb
                                 points to the correct sky direction (radians, CCW+).
                                 0 = north-up convention (lit-on-right for waxing). */
    bool        have_location;/* false => lat/lon were unset; orient_rad is the
                                 north-up fallback */
} moon_state_t;

/* Compute moon phase + orientation for a UTC time and observer location.
 * lat/lon in degrees. If lat==0 && lon==0 they are treated as unset and a
 * north-up convention orientation is returned (have_location=false). */
void moon_compute(time_t utc, double lat, double lon, moon_state_t *out);

#ifdef __cplusplus
}
#endif
