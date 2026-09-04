#pragma once

/*
 * sun_pos.h - solar elevation for a lat/lon at a UTC instant, single precision.
 *
 * The Clouds page needs exactly one decision from the sun: is it high enough at
 * the weather location, at THIS frame's timestamp, for a visible-light satellite
 * picture to show anything (clouds_wms.h, CLOUDS_SUN_MIN_EL_DEG). Nothing here
 * needs rise/set times or refraction, so this is the NOAA low-accuracy solar
 * position (error well under 1 degree over 1970-2105), not an ephemeris.
 *
 * Header-only and free of ESP-IDF, FreeRTOS and LVGL, so the host test suite
 * (test/host/test_sun_pos.c) compiles it directly, and so clouds_wms.h -- which
 * is itself host-tested -- can include it.
 *
 * FLOAT ONLY. The P4 FPU is single precision; a double here is a soft-float
 * call in a path that runs once per animation frame per poll. main/moon_ephemeris.c
 * is the project's double-precision ephemeris and is deliberately NOT reused:
 * it is a .c with a time_t/double API and cannot be included from a header.
 *
 * The day-of-year helper is a local copy of the civil-from-days arithmetic that
 * also lives in clouds_wms.h. The duplication is deliberate and one-directional:
 * clouds_wms.h includes THIS file, so this file cannot include it back.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUN_DEG2RAD 0.01745329252f
#define SUN_RAD2DEG 57.29577951f

/* Split @p unix_s into the UTC day of year (1..366) and the UTC hour of day
 * (0..24). Proleptic Gregorian, no localtime, no time_t. Either output pointer
 * may be NULL. */
static inline void sun_utc_doy_hour(uint32_t unix_s, int *doy, float *hour)
{
    /* civil-from-days (Howard Hinnant), shifted to a 1 March era. */
    int64_t z = (int64_t)(unix_s / 86400u) + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const unsigned dom = doe - (365u * yoe + yoe / 4u - yoe / 100u);   /* 0 = 1 March */
    const unsigned mp  = (5u * dom + 2u) / 153u;                       /* 0 = March */
    const unsigned dd  = dom - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm  = (mp < 10u) ? mp + 3u : mp - 9u;               /* 1..12 */
    const int y = (int)((int64_t)yoe + era * 400 + ((mm <= 2u) ? 1 : 0));

    /* Cumulative days before each month in a non-leap year; index 0 unused. */
    static const unsigned cum[13] = { 0u, 0u, 31u, 59u, 90u, 120u, 151u,
                                      181u, 212u, 243u, 273u, 304u, 334u };
    const bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    unsigned n = cum[mm] + dd;
    if (leap && mm > 2u) {
        n += 1u;
    }
    if (doy != NULL) {
        *doy = (int)n;
    }
    if (hour != NULL) {
        *hour = (float)(unix_s % 86400u) / 3600.0f;
    }
}

/* Solar elevation in degrees (-90..+90) at @p lat_deg / @p lon_deg (degrees
 * north / east) at UTC instant @p unix_s. Geometric centre of the disc, no
 * atmospheric refraction and no parallax: about 0.5 degrees optimistic at the
 * horizon, which is irrelevant against a 10 degree threshold. */
static inline float sun_elevation_deg(float lat_deg, float lon_deg, uint32_t unix_s)
{
    int doy = 1;
    float hour = 0.0f;
    sun_utc_doy_hour(unix_s, &doy, &hour);

    /* Fractional year, radians (NOAA). */
    const float g = (6.283185307f / 365.0f) *
                    ((float)(doy - 1) + (hour - 12.0f) / 24.0f);
    const float s1 = sinf(g),        c1 = cosf(g);
    const float s2 = sinf(2.0f * g), c2 = cosf(2.0f * g);
    const float s3 = sinf(3.0f * g), c3 = cosf(3.0f * g);

    /* Equation of time, minutes. */
    const float eqtime = 229.18f * (0.000075f + 0.001868f * c1 - 0.032077f * s1
                                    - 0.014615f * c2 - 0.040849f * s2);

    /* Solar declination, radians. */
    const float decl = 0.006918f - 0.399912f * c1 + 0.070257f * s1
                     - 0.006758f * c2 + 0.000907f * s2
                     - 0.002697f * c3 + 0.001480f * s3;

    /* True solar time in minutes. The clock is UTC, so there is no timezone
     * term: the whole longitude correction is the 4 min/degree offset. */
    float ha = (hour * 60.0f + eqtime + 4.0f * lon_deg) / 4.0f - 180.0f;   /* degrees */
    /* Wrap into (-180, 180] in one step: fmodf is total, so no input (a wild
     * longitude, a NaN-free huge hour) can spin here. */
    ha = fmodf(ha + 540.0f, 360.0f) - 180.0f;

    const float lat = lat_deg * SUN_DEG2RAD;
    float cosz = sinf(lat) * sinf(decl) +
                 cosf(lat) * cosf(decl) * cosf(ha * SUN_DEG2RAD);
    if (cosz > 1.0f) {
        cosz = 1.0f;
    }
    if (cosz < -1.0f) {
        cosz = -1.0f;
    }
    /* cos(zenith) is sin(elevation), so asinf of it IS the elevation. */
    return asinf(cosz) * SUN_RAD2DEG;
}
