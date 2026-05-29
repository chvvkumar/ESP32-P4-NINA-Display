/* Host test: cc -o tmoon test/moon/test_moon_compute.c main/moon_ephemeris.c -lm && ./tmoon
 * (on Windows MSVC: cl /Fe:tmoon.exe test\moon\test_moon_compute.c main\moon_ephemeris.c) */
#include "../../main/moon_ephemeris.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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

    printf("\n%s (%d failures)\n", fails?"TESTS FAILED":"ALL TESTS PASSED", fails);
    return fails?1:0;
}
