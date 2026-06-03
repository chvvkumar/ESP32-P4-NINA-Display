/* Host test: cc -o tmoon test/moon/test_moon_compute.c main/moon_ephemeris.c -lm && ./tmoon
 * (on Windows MSVC: cl /Fe:tmoon.exe test\moon\test_moon_compute.c main\moon_ephemeris.c) */
#include "../../main/moon_ephemeris.h"
#include <stdio.h>
#include <stdlib.h>
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <time.h>
#include <string.h>

/* Build a UTC time_t from calendar fields without local-tz contamination. */
static time_t mkutc(int y,int mo,int d,int h,int mi){
#if defined(_WIN32)
    struct tm t={0}; t.tm_year=y-1900;t.tm_mon=mo-1;t.tm_mday=d;t.tm_hour=h;t.tm_min=mi;
    return _mkgmtime(&t);
#else
    struct tm t={0}; t.tm_year=y-1900;t.tm_mon=mo-1;t.tm_mday=d;t.tm_hour=h;t.tm_min=mi;
    return timegm(&t);
#endif
}

static int fails=0;
static void check_illum(const char*label,time_t t,double expect,double tol){
    moon_state_t s; moon_compute(t,40.0,-74.0,&s);
    double diff=fabs(s.illum-expect);
    printf("%-22s illum=%.3f (expect %.2f, dphase=%s) %s\n",
        label,s.illum,expect,s.phase_name, diff<=tol?"OK":"FAIL");
    if(diff>tol) fails++;
}
static void check_wax(const char*label,time_t t,bool expect){
    moon_state_t s; moon_compute(t,40.0,-74.0,&s);
    printf("%-22s waxing=%d (expect %d) %s\n",label,s.waxing,expect,
        s.waxing==expect?"OK":"FAIL");
    if(s.waxing!=expect) fails++;
}

int main(void){
    /* Reference instants (UTC) from published almanac (timeanddate.com). */
    /* 2024-01-11 11:57 UTC: New Moon -> illum ~0 */
    check_illum("2024 New Moon",      mkutc(2024,1,11,11,57), 0.00, 0.04);
    /* 2024-01-25 17:54 UTC: Full Moon -> illum ~1 */
    check_illum("2024 Full Moon",     mkutc(2024,1,25,17,54), 1.00, 0.04);
    /* 2024-01-18 03:53 UTC: First Quarter -> illum ~0.5, waxing */
    check_illum("2024 First Quarter", mkutc(2024,1,18,3,53),  0.50, 0.06);
    check_wax  ("2024 First Quarter", mkutc(2024,1,18,3,53),  true);
    /* 2024-02-02 23:18 UTC: Last Quarter -> illum ~0.5, waning */
    check_wax  ("2024 Last Quarter",  mkutc(2024,2,2,23,18),  false);

    /* Meeus ch.53 worked example: 1992-04-12 00:00 TD (TD~UTC within ~1 min here). */
    {
        moon_state_t s; moon_compute(mkutc(1992,4,12,0,0), 40.0, -74.0, &s);
        double l_deg = s.lib_lon * 180.0/M_PI;
        double b_deg = s.lib_lat * 180.0/M_PI;
        double P_deg = s.axis_P  * 180.0/M_PI;
        printf("1992-04-12 lib l'=%.2f(exp -1.23) b'=%.2f(exp +4.20) P=%.2f(exp +15.08)\n",
               l_deg, b_deg, P_deg);
        if (fabs(l_deg-(-1.23))>0.15){ printf("  FAIL l'\n"); fails++; }
        if (fabs(b_deg-( 4.20))>0.15){ printf("  FAIL b'\n"); fails++; }
        if (fabs(P_deg-( 15.08))>0.2){ printf("  FAIL P\n");  fails++; }
    }
    /* Informational tonight cross-check (non-failing). */
    {
        moon_state_t s; moon_compute(mkutc(2026,6,2,2,0), 40.0, -74.0, &s);
        printf("2026-06-02 02:00Z: illum=%.0f%% %s lib_lon=%.1f lib_lat=%.1f deg\n",
               s.illum*100.0f, s.phase_name, s.lib_lon*180.0/M_PI, s.lib_lat*180.0/M_PI);
    }

    printf("\n%s (%d failures)\n", fails?"TESTS FAILED":"ALL TESTS PASSED", fails);
    return fails?1:0;
}
