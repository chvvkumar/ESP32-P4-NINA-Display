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

    if (!out->have_location) {
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
    out->have_location = true;
}
