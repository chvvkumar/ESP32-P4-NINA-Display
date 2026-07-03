#include "moon_ephemeris.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG (M_PI/180.0)
#define RAD (180.0/M_PI)

static const char *PHASE_NAMES[8] = {
    "New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
    "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent"
};

static double rev(double x){ x = fmod(x, 360.0); return x < 0 ? x + 360.0 : x; }

/* Single-precision degree-argument trig for the moon rise/set sample loop: the
 * P4 FPU is single precision, so reduce the angle to [0,360) in double then
 * evaluate the transcendental in float. */
static inline float sind_f(double deg){ return sinf((float)(rev(deg) * DEG)); }
static inline float cosd_f(double deg){ return cosf((float)(rev(deg) * DEG)); }

static uint8_t phase_index_from_cycle(double cyc)
{
    if      (cyc < 0.0335 || cyc >= 0.9665) return 0;
    else if (cyc < 0.2165) return 1;
    else if (cyc < 0.2835) return 2;
    else if (cyc < 0.4665) return 3;
    else if (cyc < 0.5335) return 4;
    else if (cyc < 0.7165) return 5;
    else if (cyc < 0.7835) return 6;
    else                   return 7;
}

void moon_compute(time_t utc, double lat, double lon, moon_state_t *out)
{
    out->have_location = !(lat == 0.0 && lon == 0.0);

    /* Days since 2000 Jan 0.0 (JD 2451543.5), as a double. */
    double d = (double)utc / 86400.0 + 2440587.5 - 2451543.5;
    double ut_hours = fmod((double)utc / 3600.0, 24.0);
    if (ut_hours < 0) ut_hours += 24.0;

    double oblecl = (23.4393 - 3.563e-7 * d) * DEG;

    /* ---- Sun ---- */
    double ws = 282.9404 + 4.70935e-5 * d;
    double es = 0.016709 - 1.151e-9 * d;
    double Ms = rev(356.0470 + 0.9856002585 * d);
    double Es = Ms + RAD * es * sin(Ms*DEG) * (1 + es*cos(Ms*DEG));
    double xvs = cos(Es*DEG) - es;
    double yvs = sqrt(1 - es*es) * sin(Es*DEG);
    double vs  = atan2(yvs, xvs) * RAD;
    double rs  = sqrt(xvs*xvs + yvs*yvs);
    double slon = rev(vs + ws);                 /* true solar ecliptic longitude (deg) */
    double Ls  = rev(ws + Ms);                   /* sun mean longitude */
    /* Sun equatorial */
    double xss = rs * cos(slon*DEG);
    double yss = rs * sin(slon*DEG);
    double xes = xss;
    double yes = yss * cos(oblecl);
    double zes = yss * sin(oblecl);
    double RA_s  = atan2(yes, xes) * RAD;
    double Dec_s = atan2(zes, sqrt(xes*xes + yes*yes)) * RAD;

    /* ---- Moon ---- */
    double Nm = 125.1228 - 0.0529538083 * d;
    double im = 5.1454;
    double wm = 318.0634 + 0.1643573223 * d;
    double am = 60.2666;
    double em = 0.054900;
    double Mm = rev(115.3654 + 13.0649929509 * d);
    double Em = Mm + RAD * em * sin(Mm*DEG) * (1 + em*cos(Mm*DEG));
    Em = Em - (Em - RAD*em*sin(Em*DEG) - Mm) / (1 - em*cos(Em*DEG)); /* one refine */
    double xv = am * (cos(Em*DEG) - em);
    double yv = am * (sqrt(1 - em*em) * sin(Em*DEG));
    double vm = atan2(yv, xv) * RAD;
    double rm = sqrt(xv*xv + yv*yv);
    double xh = rm * (cos(Nm*DEG)*cos((vm+wm)*DEG) - sin(Nm*DEG)*sin((vm+wm)*DEG)*cos(im*DEG));
    double yh = rm * (sin(Nm*DEG)*cos((vm+wm)*DEG) + cos(Nm*DEG)*sin((vm+wm)*DEG)*cos(im*DEG));
    double zh = rm * (sin((vm+wm)*DEG)*sin(im*DEG));
    double mlon = atan2(yh, xh) * RAD;
    double mlat = atan2(zh, sqrt(xh*xh + yh*yh)) * RAD;

    /* Perturbations (Schlyter) */
    double Lm = rev(Nm + wm + Mm);   /* moon mean longitude */
    double D  = rev(Lm - Ls);        /* mean elongation */
    double F  = rev(Lm - Nm);        /* argument of latitude */
    mlon += -1.274 * sin((Mm - 2*D)*DEG)
          +  0.658 * sin((2*D)*DEG)
          -  0.186 * sin(Ms*DEG)
          -  0.059 * sin((2*Mm - 2*D)*DEG)
          -  0.057 * sin((Mm - 2*D + Ms)*DEG)
          +  0.053 * sin((Mm + 2*D)*DEG)
          +  0.046 * sin((2*D - Ms)*DEG)
          +  0.041 * sin((Mm - Ms)*DEG)
          -  0.035 * sin(D*DEG)
          -  0.031 * sin((Mm + Ms)*DEG)
          -  0.015 * sin((2*F - 2*D)*DEG)
          +  0.011 * sin((Mm - 4*D)*DEG);
    mlat += -0.173 * sin((F - 2*D)*DEG)
          -  0.055 * sin((Mm - F - 2*D)*DEG)
          -  0.046 * sin((Mm + F - 2*D)*DEG)
          +  0.033 * sin((F + 2*D)*DEG)
          +  0.017 * sin((2*Mm + F)*DEG);
    mlon = rev(mlon);

    /* Moon equatorial (geocentric) */
    double xg = cos(mlon*DEG) * cos(mlat*DEG);
    double yg = sin(mlon*DEG) * cos(mlat*DEG);
    double zg = sin(mlat*DEG);
    double xe = xg;
    double ye = yg*cos(oblecl) - zg*sin(oblecl);
    double ze = yg*sin(oblecl) + zg*cos(oblecl);
    double RA_m  = rev(atan2(ye, xe) * RAD);
    double Dec_m = atan2(ze, sqrt(xe*xe + ye*ye)) * RAD;

    /* ---- Phase ---- */
    double elong = acos(cos((slon - mlon)*DEG) * cos(mlat*DEG)) * RAD; /* 0..180 */
    double FV = 180.0 - elong;                 /* phase angle */
    double k = (1.0 + cos(FV*DEG)) / 2.0;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    out->illum = (float)k;

    /* Waxing if Moon is east of Sun (0 < mlon-slon < 180). */
    double dlon = rev(mlon - slon);
    out->waxing = (dlon < 180.0);

    /* Phase index from elongation fraction of the synodic cycle. */
    double cyc = dlon / 360.0;  /* 0=new .. 0.5=full .. 1=new */
    out->cycle = (float)cyc;
    uint8_t pi = phase_index_from_cycle(cyc);
    out->phase_index = pi;
    out->phase_name  = PHASE_NAMES[pi];

    /* ---- Orientation ---- */
    /* Bright-limb position angle chi (Meeus 48.5), from celestial north, eastward. */
    double dRA = (RA_s - RA_m) * DEG;
    double chi = atan2(cos(Dec_s*DEG) * sin(dRA),
                       sin(Dec_s*DEG) * cos(Dec_m*DEG)
                       - cos(Dec_s*DEG) * sin(Dec_m*DEG) * cos(dRA)); /* radians */

    /* ---- Libration (Meeus ch.53, optical) ---- */
    const double I = 1.54242 * DEG;
    double Wl   = (mlon - Nm) * DEG;
    double beta = mlat * DEG;
    double sb = sin(beta), cb = cos(beta);
    double bprime = asin(-sin(Wl)*cb*sin(I) - sb*cos(I));
    double Acol   = atan2(sin(Wl)*cb*cos(I) - sb*sin(I), cos(Wl)*cb);
    double lprime = Acol - F*DEG;
    while (lprime >  M_PI) lprime -= 2*M_PI;
    while (lprime < -M_PI) lprime += 2*M_PI;
    out->lib_lon = (float)lprime;
    out->lib_lat = (float)bprime;
    /* Position angle of the axis P (Meeus 53). */
    double Vp = Nm * DEG;
    double xP = sin(I)*sin(Vp);
    double yP = sin(I)*cos(Vp)*cos(oblecl) - cos(I)*sin(oblecl);
    double wP = atan2(xP, yP);
    double Pang = asin( sqrt(xP*xP + yP*yP) * cos(RA_m*DEG - wP) / cos(bprime) );
    out->axis_P = (float)Pang;

    /* ---- Sub-solar point (Sun's selenographic coords), SIMPLIFIED ----
     * Treats the Sun as anti-solar from the Moon (lambdaH = slon + 180) and the
     * Sun's ecliptic latitude as 0. This omits the Moon's parallax (~0.95 deg) and
     * the Moon/Sun ecliptic-latitude contribution, so sun_lon carries up to ~1 deg
     * error -> the lit-fraction terminator longitude inherits that error. Adequate
     * for Phase 1; to be refined with the Meeus ch.53 colongitude term in Phase 2. */
    double lambdaH = slon*DEG + M_PI;   /* Sun direction from the Moon, approx */
    double WlS = lambdaH - Nm*DEG;
    double bS  = asin(-sin(WlS)*sin(I));
    double AS  = atan2(sin(WlS)*cos(I), cos(WlS));
    double lS  = AS - F*DEG;
    while (lS >  M_PI) lS -= 2*M_PI;
    while (lS < -M_PI) lS += 2*M_PI;
    out->sun_lon = (float)lS;
    out->sun_lat = (float)bS;

    if (!out->have_location) {
        out->roll = out->axis_P;
        /* North-up convention: lit-on-right (waxing) means bright limb at +90deg.
         * Render expects orient_rad=0 for that convention, so collapse to 0. */
        out->orient_rad = 0.0f;
        return;
    }

    /* Local sidereal time -> hour angle of the Moon. */
    double gmst0 = rev(Ls + 180.0);                 /* deg */
    double lst   = rev(gmst0 + 15.0 * ut_hours + lon); /* deg */
    double H     = rev(lst - RA_m) * DEG;           /* hour angle, radians */
    /* Parallactic angle q. */
    double q = atan2(sin(H),
                     tan(lat*DEG) * cos(Dec_m*DEG) - sin(Dec_m*DEG) * cos(H)); /* radians */

    /* Position angle of bright limb relative to the viewer's zenith.
     * In the north-up convention the renderer puts the bright limb at +90deg (right).
     * The sky-accurate bright-limb zenith angle is (chi - q). Convert to the
     * renderer's rotation: rotate the north-up disc by (chi - q - 90deg). */
    double zenith_pa = chi - q;                     /* radians, from zenith, eastward */
    out->orient_rad = (float)(zenith_pa - M_PI/2.0);
    out->roll = (float)(Pang - q);
}

void moon_state_from_cycle(double cycle, float orient_rad, moon_state_t *out)
{
    cycle = fmod(cycle, 1.0);
    if (cycle < 0.0) cycle += 1.0;
    double k = (1.0 - cos(2.0 * M_PI * cycle)) / 2.0;
    if (k < 0.0) k = 0.0;
    if (k > 1.0) k = 1.0;
    out->illum         = (float)k;
    out->waxing        = (cycle < 0.5);
    out->cycle         = (float)cycle;
    uint8_t pi         = phase_index_from_cycle(cycle);
    out->phase_index   = pi;
    out->phase_name    = PHASE_NAMES[pi];
    out->orient_rad    = orient_rad;
    out->lib_lon       = 0.0f;
    out->lib_lat       = 0.0f;
    out->sun_lon       = 0.0f;
    out->sun_lat       = 0.0f;
    out->roll          = orient_rad;
    out->axis_P        = 0.0f;
    out->have_location = true;
}

/* Internal: compute geocentric RA (degrees, 0..360) and Dec (degrees) for a
 * given UTC time_t.  Mirrors the moon block from moon_compute() without the
 * orientation / libration work so it can be called cheaply in a sample loop. */
static void moon_ra_dec_at(time_t utc, double *ra_out, double *dec_out)
{
    double d = (double)utc / 86400.0 + 2440587.5 - 2451543.5;

    double oblecl_deg = 23.4393 - 3.563e-7 * d;   /* obliquity, degrees */

    /* Sun mean longitude for GMST0 and perturbation terms. */
    double ws = 282.9404 + 4.70935e-5 * d;
    double es = 0.016709 - 1.151e-9 * d;
    double Ms = rev(356.0470 + 0.9856002585 * d);
    double Es = Ms + RAD * es * sind_f(Ms) * (1.0 + es * cosd_f(Ms));
    float  xvs = cosd_f(Es) - (float)es;
    float  yvs = sqrtf((float)(1.0 - es*es)) * sind_f(Es);
    float  vs  = atan2f(yvs, xvs) * (float)RAD;
    double slon = rev(vs + ws);
    double Ls   = rev(ws + Ms);

    /* Moon orbital elements. */
    double Nm = 125.1228 - 0.0529538083 * d;
    double im = 5.1454;
    double wm = 318.0634 + 0.1643573223 * d;
    double am = 60.2666;
    double em = 0.054900;
    double Mm = rev(115.3654 + 13.0649929509 * d);
    double Em = Mm + RAD * em * sind_f(Mm) * (1.0 + em * cosd_f(Mm));
    Em = Em - (Em - RAD * em * sind_f(Em) - Mm) / (1.0 - em * cosd_f(Em));
    float  xv = (float)am * (cosd_f(Em) - (float)em);
    float  yv = (float)am * (sqrtf((float)(1.0 - em*em)) * sind_f(Em));
    float  vm = atan2f(yv, xv) * (float)RAD;
    float  rm = sqrtf(xv*xv + yv*yv);
    float  xh = rm * (cosd_f(Nm)*cosd_f(vm+wm) - sind_f(Nm)*sind_f(vm+wm)*cosd_f(im));
    float  yh = rm * (sind_f(Nm)*cosd_f(vm+wm) + cosd_f(Nm)*sind_f(vm+wm)*cosd_f(im));
    float  zh = rm * (sind_f(vm+wm)*sind_f(im));
    float  mlon = atan2f(yh, xh) * (float)RAD;
    float  mlat = atan2f(zh, sqrtf(xh*xh + yh*yh)) * (float)RAD;

    /* Perturbations. */
    double Lm = rev(Nm + wm + Mm);
    double D  = rev(Lm - Ls);
    double F  = rev(Lm - Nm);
    mlon += -1.274f * sind_f(Mm - 2*D)
          +  0.658f * sind_f(2*D)
          -  0.186f * sind_f(Ms)
          -  0.059f * sind_f(2*Mm - 2*D)
          -  0.057f * sind_f(Mm - 2*D + Ms)
          +  0.053f * sind_f(Mm + 2*D)
          +  0.046f * sind_f(2*D - Ms)
          +  0.041f * sind_f(Mm - Ms)
          -  0.035f * sind_f(D)
          -  0.031f * sind_f(Mm + Ms)
          -  0.015f * sind_f(2*F - 2*D)
          +  0.011f * sind_f(Mm - 4*D);
    mlat += -0.173f * sind_f(F - 2*D)
          -  0.055f * sind_f(Mm - F - 2*D)
          -  0.046f * sind_f(Mm + F - 2*D)
          +  0.033f * sind_f(F + 2*D)
          +  0.017f * sind_f(2*Mm + F);
    mlon = (float)rev(mlon);

    /* Ecliptic -> equatorial. */
    float xg = cosd_f(mlon) * cosd_f(mlat);
    float yg = sind_f(mlon) * cosd_f(mlat);
    float zg = sind_f(mlat);
    float xe = xg;
    float ye = yg*cosd_f(oblecl_deg) - zg*sind_f(oblecl_deg);
    float ze = yg*sind_f(oblecl_deg) + zg*cosd_f(oblecl_deg);
    *ra_out  = rev(atan2f(ye, xe) * (float)RAD);
    *dec_out = atan2f(ze, sqrtf(xe*xe + ye*ye)) * (float)RAD;

    (void)slon; /* true solar longitude not needed here; suppress unused-variable warning */
}

/* Compute the moon's topocentric-ish altitude (radians) at time t for an
 * observer at (lon degrees, precomputed sin_lat/cos_lat).  The JD/sidereal
 * setup uses double; the trig for dec/H/altitude uses float per the P4
 * single-precision FPU constraint. */
static float moon_alt_at(time_t t, double lon, float sin_lat, float cos_lat)
{
    double ra_deg, dec_deg;
    moon_ra_dec_at(t, &ra_deg, &dec_deg);

    /* GMST0 via Schlyter: GMST0 = Ls + 180 (degrees), where Ls = ws + Ms. */
    double d_t     = (double)t / 86400.0 + 2440587.5 - 2451543.5;
    double ws_t    = 282.9404 + 4.70935e-5 * d_t;
    double Ms_t    = rev(356.0470 + 0.9856002585 * d_t);
    double Ls_t    = rev(ws_t + Ms_t);
    double gmst0_t = rev(Ls_t + 180.0);                  /* degrees */

    double ut_hours_t = fmod((double)t / 3600.0, 24.0);
    if (ut_hours_t < 0.0) ut_hours_t += 24.0;

    double lst_t  = rev(gmst0_t + 15.0 * ut_hours_t + lon); /* degrees */
    double H_deg  = lst_t - ra_deg;                          /* hour angle, degrees */

    /* Altitude trig in float (single-precision FPU). */
    float dec_rad = (float)(dec_deg * (M_PI / 180.0));
    float H_rad   = (float)(H_deg   * (M_PI / 180.0));
    float sin_dec = sinf(dec_rad);
    float cos_dec = cosf(dec_rad);
    float cos_H   = cosf(H_rad);

    float sin_alt = sin_lat * sin_dec + cos_lat * cos_dec * cos_H;
    if (sin_alt >  1.0f) sin_alt =  1.0f;
    if (sin_alt < -1.0f) sin_alt = -1.0f;
    return asinf(sin_alt);   /* radians */
}

/* Compute the current/next moon up-period relative to `now`.
 *
 * Semantics (relative to the moment `now` is called):
 *   *set  = first downward horizon crossing AFTER now (end of current/next up-period).
 *   *rise:
 *     - moon UP   at now -> most recent upward crossing at or before now
 *                           (the rise that started the current up-period);
 *     - moon DOWN at now -> first upward crossing after now (the next rise).
 *
 * Search window: [now - 26 h, now + 50 h] at 10-minute steps (456 samples, 76 h).
 * This window guarantees that at least one full up-period is captured regardless
 * of where `now` falls in the ~24.8 h lunar day.
 *
 * If lat==0.0 && lon==0.0 (location unset), both events are marked invalid.
 * If no crossing is found in the window, *_valid is false and the time is 0. */
void moon_rise_set(time_t now, double lat, double lon,
                   time_t *rise, bool *rise_valid,
                   time_t *set,  bool *set_valid)
{
    *rise_valid = false;
    *set_valid  = false;
    *rise       = 0;
    *set        = 0;

    if (lat == 0.0 && lon == 0.0) {
        return;  /* location unset */
    }

    /* Standard moonrise/set horizon: refraction + semidiameter - parallax net. */
    const float h0_rad = 0.125f * (float)(M_PI / 180.0);

    /* Precompute observer latitude trig once (doubles -> float for altitude loop). */
    float sin_lat = sinf((float)(lat * (M_PI / 180.0)));
    float cos_lat = cosf((float)(lat * (M_PI / 180.0)));

    /* Search window: 26 h before now .. 50 h after now (76 h total).
     * Step = 10 minutes (600 s); 456 + 1 = 457 samples. */
    const int   STEP_S   = 600;
    const int   BACK_S   = 26 * 3600;           /* look-back to find current rise */
    const int   FWRD_S   = 50 * 3600;           /* look-ahead to find next set    */
    const int   WIN_S    = BACK_S + FWRD_S;
    const int   N_STEPS  = WIN_S / STEP_S + 1;  /* 457 samples */

    time_t win_start = now - (time_t)BACK_S;

    /* Determine whether the moon is currently above the horizon. */
    bool up_now = (moon_alt_at(now, lon, sin_lat, cos_lat) >= h0_rad);

    /* Scan crossings across the window, collecting:
     *   rise_before - latest upward crossing with interpolated_time <= now
     *   rise_after  - earliest upward crossing with interpolated_time  > now
     *   set_after   - earliest downward crossing with interpolated_time > now */
    time_t rise_before = 0, rise_after = 0, set_after = 0;
    bool   have_rise_before = false, have_rise_after = false, have_set_after = false;

    float prev_alt  = moon_alt_at(win_start, lon, sin_lat, cos_lat);

    for (int i = 1; i < N_STEPS; i++) {
        time_t t   = win_start + (time_t)(i * STEP_S);
        float  alt = moon_alt_at(t, lon, sin_lat, cos_lat);

        float prev_v = prev_alt - h0_rad;
        float cur_v  = alt      - h0_rad;

        if (prev_v < 0.0f && cur_v >= 0.0f) {
            /* Upward crossing — interpolate. */
            float  frac    = prev_v / (prev_v - cur_v);   /* 0..1 within step */
            time_t cross_t = win_start + (time_t)((i - 1) * STEP_S)
                             + (time_t)(frac * STEP_S + 0.5f);

            if (cross_t <= now) {
                /* Keep the latest one before/at now. */
                if (!have_rise_before || cross_t > rise_before) {
                    rise_before     = cross_t;
                    have_rise_before = true;
                }
            } else {
                /* Keep only the earliest one after now. */
                if (!have_rise_after) {
                    rise_after     = cross_t;
                    have_rise_after = true;
                }
            }
        } else if (prev_v >= 0.0f && cur_v < 0.0f) {
            /* Downward crossing — interpolate. */
            float  frac    = prev_v / (prev_v - cur_v);
            time_t cross_t = win_start + (time_t)((i - 1) * STEP_S)
                             + (time_t)(frac * STEP_S + 0.5f);

            /* Keep only the earliest downward crossing after now. */
            if (cross_t > now && !have_set_after) {
                set_after     = cross_t;
                have_set_after = true;
            }
        }

        prev_alt = alt;
    }

    /* Assign rise: if moon is currently up, report the rise that started this
     * up-period; otherwise report the next upcoming rise. */
    if (up_now) {
        if (have_rise_before) {
            *rise       = rise_before;
            *rise_valid = true;
        } else if (have_rise_after) {   /* fallback: window too short to find prior */
            *rise       = rise_after;
            *rise_valid = true;
        }
    } else {
        if (have_rise_after) {
            *rise       = rise_after;
            *rise_valid = true;
        } else if (have_rise_before) {  /* fallback: window too short to find next */
            *rise       = rise_before;
            *rise_valid = true;
        }
    }

    /* Assign set: always the first downward crossing after now. */
    if (have_set_after) {
        *set       = set_after;
        *set_valid = true;
    }
}
