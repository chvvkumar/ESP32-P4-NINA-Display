/*
 * radar_sites.c - NWS WSR-88D radar site table + nearest-site lookup.
 *
 * Generated: 2026-08-16
 * Source:    https://api.weather.gov/radar/stations?stationType=WSR-88D
 * Sites:     156
 *
 * Distance uses an equirectangular approximation (longitude scaled by
 * cosf(latitude)) and compares SQUARED distances, so there is no sqrt in the
 * loop. That is exact enough for a nearest-neighbour pick and much cheaper
 * than haversine on a single-precision FPU. All math is float: this target's
 * FPU has no double support and doubles fall back to soft-float.
 *
 * Antimeridian: the longitude delta is wrapped into [-180, 180], so Alaskan
 * and Pacific sites either side of 180 degrees compare correctly.
 *
 * Limitation: the equirectangular approximation degrades near the poles. No
 * WSR-88D site is above 71 degrees north, so this does not matter in practice.
 */

#include <math.h>
#include "radar_sites.h"

/* Degrees to radians. Spelled out rather than using M_PI, which is not
 * guaranteed to be visible under a strict-ANSI compile. */
#define RADAR_DEG_TO_RAD 0.017453292f

/* id, latitude, longitude - sorted by id */
static const radar_site_t s_radar_sites[RADAR_SITE_COUNT] = {
    { "KABR",   45.45583f,   -98.41305f },  /* Aberdeen */
    { "KABX",   35.14972f,  -106.82388f },  /* Albuquerque */
    { "KAKQ",   36.98388f,   -77.00750f },  /* Norfolk */
    { "KAMA",   35.23333f,  -101.70927f },  /* Amarillo */
    { "KAMX",   25.61055f,   -80.41305f },  /* Miami */
    { "KAPX",   44.90634f,   -84.71953f },  /* NCL Michigan */
    { "KARX",   43.82277f,   -91.19110f },  /* La Crosse */
    { "KATX",   48.19461f,  -122.49568f },  /* Seattle */
    { "KBBX",   39.49611f,  -121.63165f },  /* Beale AFB */
    { "KBGM",   42.19969f,   -75.98472f },  /* Binghamton */
    { "KBHX",   40.49833f,  -124.29215f },  /* Eureka (Bunker Hill) */
    { "KBIS",   46.77083f,  -100.76027f },  /* Bismarck */
    { "KBLX",   45.85377f,  -108.60679f },  /* Billings */
    { "KBMX",   33.17194f,   -86.76972f },  /* Birmingham */
    { "KBOX",   41.95577f,   -71.13686f },  /* Boston */
    { "KBRO",   25.91555f,   -97.41860f },  /* Brownsville */
    { "KBUF",   42.94860f,   -78.73694f },  /* Buffalo */
    { "KBYX",   24.59694f,   -81.70333f },  /* Key West */
    { "KCAE",   33.94860f,   -81.11861f },  /* Columbia */
    { "KCBW",   46.03916f,   -67.80642f },  /* Caribou */
    { "KCBX",   43.49021f,  -116.23602f },  /* Boise */
    { "KCCX",   40.92305f,   -78.00389f },  /* State College */
    { "KCLE",   41.41305f,   -81.86000f },  /* Cleveland */
    { "KCLX",   32.65552f,   -81.04219f },  /* Charleston,SC */
    { "KCRP",   27.78388f,   -97.51083f },  /* Corpus Christi */
    { "KCXX",   44.51110f,   -73.16639f },  /* Burlington */
    { "KCYS",   41.15194f,  -104.80610f },  /* Cheyenne */
    { "KDAX",   38.50111f,  -121.67782f },  /* Sacramento */
    { "KDDC",   37.76083f,   -99.96888f },  /* Dodge City */
    { "KDFX",   29.27250f,  -100.28027f },  /* Laughlin AFB */
    { "KDGX",   32.27999f,   -89.98444f },  /* Jackson/Brandon,MS */
    { "KDIX",   39.94694f,   -74.41072f },  /* Philadelphia */
    { "KDLH",   46.83694f,   -92.20971f },  /* Duluth */
    { "KDMX",   41.73110f,   -93.72285f },  /* Des Moines */
    { "KDOX",   38.82555f,   -75.44000f },  /* Dover AFB */
    { "KDTX",   42.69999f,   -83.47166f },  /* Detroit */
    { "KDVN",   41.61166f,   -90.58083f },  /* Quad Cities */
    { "KDYX",   32.53833f,   -99.25416f },  /* Dyess AFB */
    { "KEAX",   38.81024f,   -94.26446f },  /* Pleasant Hill */
    { "KEMX",   31.89361f,  -110.63027f },  /* Tucson */
    { "KENX",   42.58655f,   -74.06408f },  /* Albany */
    { "KEOX",   31.46055f,   -85.45938f },  /* Ft Rucker */
    { "KEPZ",   31.87305f,  -106.69799f },  /* El Paso */
    { "KESX",   35.70111f,  -114.89138f },  /* Las Vegas */
    { "KEVX",   30.56503f,   -85.92166f },  /* Eglin Afb */
    { "KEWX",   29.70405f,   -98.02860f },  /* Austin/San Antonio */
    { "KEYX",   35.09777f,  -117.56074f },  /* Edwards AFB */
    { "KFCX",   37.02416f,   -80.27416f },  /* Roanoke */
    { "KFDR",   34.36219f,   -98.97666f },  /* Altus AFB */
    { "KFDX",   34.63416f,  -103.61888f },  /* Cannon AFB */
    { "KFFC",   33.36333f,   -84.56583f },  /* Atlanta */
    { "KFSD",   43.58777f,   -96.72888f },  /* Sioux Falls */
    { "KFSX",   34.57433f,  -111.19843f },  /* Flagstaff */
    { "KFTG",   39.78663f,  -104.54580f },  /* Denver */
    { "KFWS",   32.57277f,   -97.30313f },  /* Dallas/Ft Worth */
    { "KGGW",   48.20635f,  -106.62468f },  /* Glasgow */
    { "KGJX",   39.06222f,  -108.21375f },  /* Grand Junction */
    { "KGLD",   39.36694f,  -101.70027f },  /* Goodland */
    { "KGRB",   44.49862f,   -88.11110f },  /* Green Bay */
    { "KGRK",   30.72166f,   -97.38277f },  /* Ft Hood */
    { "KGRR",   42.89388f,   -85.54488f },  /* Grand Rapids */
    { "KGSP",   34.88330f,   -82.21983f },  /* Greer */
    { "KGWX",   33.89691f,   -88.32919f },  /* Columbus AFB */
    { "KGYX",   43.89130f,   -70.25636f },  /* Portland,ME */
    { "KHDC",   30.51960f,   -90.40740f },  /* Hammond */
    { "KHDX",   33.07699f,  -106.12002f },  /* Holloman AFB */
    { "KHGX",   29.47194f,   -95.07888f },  /* Houston */
    { "KHNX",   36.31416f,  -119.63213f },  /* San Joaquin Valley */
    { "KHPX",   36.73666f,   -87.28499f },  /* Ft Campbell */
    { "KHTX",   34.93055f,   -86.08361f },  /* Northeast Alabama */
    { "KICT",   37.65444f,   -97.44305f },  /* Wichita */
    { "KICX",   37.59104f,  -112.86221f },  /* Cedar City */
    { "KILN",   39.42027f,   -83.82166f },  /* Cincinnati */
    { "KILX",   40.15049f,   -89.33679f },  /* Lincoln */
    { "KIND",   39.70749f,   -86.28027f },  /* Indianapolis */
    { "KINX",   36.17499f,   -95.56413f },  /* Tulsa */
    { "KIWA",   33.28916f,  -111.66999f },  /* Phoenix */
    { "KIWX",   41.35860f,   -85.70000f },  /* Northern Indiana */
    { "KJAX",   30.48463f,   -81.70190f },  /* Jacksonville */
    { "KJGX",   32.67499f,   -83.35111f },  /* Robins AFB */
    { "KJKL",   37.59083f,   -83.31305f },  /* Jackson,KY */
    { "KLBB",   33.65413f,  -101.81416f },  /* Lubbock */
    { "KLCH",   30.12530f,   -93.21588f },  /* Lake Charles */
    { "KLGX",   47.11689f,  -124.10663f },  /* Langley Hill */
    { "KLNX",   41.95794f,  -100.57621f },  /* North Platte */
    { "KLOT",   41.60444f,   -88.08444f },  /* Chicago */
    { "KLRX",   40.73972f,  -116.80277f },  /* Elko */
    { "KLSX",   38.69888f,   -90.68277f },  /* St Louis */
    { "KLTX",   33.98916f,   -78.42916f },  /* Wilmington */
    { "KLVX",   37.97527f,   -85.94388f },  /* Louisville */
    { "KLWX",   38.97610f,   -77.48750f },  /* Sterling */
    { "KLZK",   34.83638f,   -92.26194f },  /* Little Rock */
    { "KMAF",   31.94346f,  -102.18924f },  /* Midland/Odessa */
    { "KMAX",   42.08111f,  -122.71735f },  /* Medford */
    { "KMBX",   48.39249f,  -100.86443f },  /* Minot AFB */
    { "KMHX",   34.77583f,   -76.87639f },  /* Morehead City */
    { "KMKX",   42.96777f,   -88.55055f },  /* Milwaukee */
    { "KMLB",   28.11305f,   -80.65444f },  /* Melbourne */
    { "KMOB",   30.67944f,   -88.23972f },  /* Mobile */
    { "KMPX",   44.84888f,   -93.56552f },  /* Minneapolis */
    { "KMQT",   46.53110f,   -87.54833f },  /* Marquette */
    { "KMRX",   36.16833f,   -83.40194f },  /* Knoxville */
    { "KMSX",   47.04110f,  -113.98610f },  /* Missoula */
    { "KMTX",   41.26277f,  -112.44777f },  /* Salt Lake City */
    { "KMUX",   37.15522f,  -121.89843f },  /* San Francisco */
    { "KMVX",   47.52805f,   -97.32499f },  /* Fargo/Grand Forks */
    { "KMXX",   32.53664f,   -85.78975f },  /* Maxwell AFB */
    { "KNKX",   32.91888f,  -117.04193f },  /* San Diego */
    { "KNQA",   35.34472f,   -89.87333f },  /* Memphis */
    { "KOAX",   41.32027f,   -96.36680f },  /* Omaha */
    { "KOHX",   36.24722f,   -86.56250f },  /* Nashville */
    { "KOKX",   40.86552f,   -72.86392f },  /* Brookhaven */
    { "KOTX",   47.68055f,  -117.62582f },  /* Spokane */
    { "KPAH",   37.06833f,   -88.77194f },  /* Paducah */
    { "KPBZ",   40.53166f,   -80.21794f },  /* Pittsburgh */
    { "KPDT",   45.69055f,  -118.85290f },  /* Pendleton */
    { "KPOE",   31.15527f,   -92.97583f },  /* Ft Polk */
    { "KPUX",   38.45944f,  -104.18138f },  /* Pueblo */
    { "KRAX",   35.66527f,   -78.49000f },  /* Raleigh/Durham */
    { "KRGX",   39.75405f,  -119.46202f },  /* Reno */
    { "KRIW",   43.06610f,  -108.47729f },  /* Riverton/Lander */
    { "KRLX",   38.31110f,   -81.72277f },  /* Charleston,WV */
    { "KRTX",   45.71499f,  -122.96499f },  /* Portland,OR */
    { "KSFX",   43.10559f,  -112.68612f },  /* Pocatello */
    { "KSGF",   37.23527f,   -93.40027f },  /* Springfield */
    { "KSHV",   32.45083f,   -93.84124f },  /* Shreveport */
    { "KSJT",   31.37111f,  -100.49221f },  /* San Angelo */
    { "KSOX",   33.81773f,  -117.63599f },  /* Santa Ana Mts */
    { "KSRX",   35.29041f,   -94.36188f },  /* Western Arkansas */
    { "KTBW",   27.70527f,   -82.40194f },  /* Tampa */
    { "KTFX",   47.45972f,  -111.38527f },  /* Great Falls */
    { "KTLH",   30.39749f,   -84.32889f },  /* Tallahassee */
    { "KTLX",   35.33305f,   -97.27775f },  /* Norman */
    { "KTWX",   38.99694f,   -96.23249f },  /* Topeka */
    { "KTYX",   43.75582f,   -75.68000f },  /* Ft Drum */
    { "KUDX",   44.12471f,  -102.82999f },  /* Rapid City */
    { "KUEX",   40.32083f,   -98.44194f },  /* Grand Island */
    { "KVAX",   30.89027f,   -83.00180f },  /* Moody AFB */
    { "KVBX",   34.83855f,  -120.39790f },  /* Vandenberg AFB */
    { "KVNX",   36.74083f,   -98.12749f },  /* Vance AFB */
    { "KVTX",   34.41166f,  -119.17860f },  /* Los Angeles */
    { "KVWX",   38.26024f,   -87.72452f },  /* Evansville,IN */
    { "KYUX",   32.49527f,  -114.65668f },  /* Yuma */
    { "PABC",   60.79194f,  -161.87637f },  /* Bethel */
    { "PACG",   56.85277f,  -135.52915f },  /* Sitka */
    { "PAEC",   64.51139f,  -165.29498f },  /* Nome */
    { "PAHG",   60.72591f,  -151.35144f },  /* Kenai */
    { "PAIH",   59.46194f,  -146.30109f },  /* Middleton Island */
    { "PAKC",   58.67944f,  -156.62942f },  /* King Salmon */
    { "PAPD",   65.03511f,  -147.50140f },  /* Fairbanks Faa */
    { "PGUA",   13.45583f,   144.81112f },  /* Andersen AFB */
    { "PHKI",   21.89389f,  -159.55249f },  /* South Kauai */
    { "PHKM",   20.12527f,  -155.77776f },  /* Kamuela/Kohala Apt */
    { "PHMO",   21.13277f,  -157.18026f },  /* Molokai */
    { "PHWA",   19.09500f,  -155.56887f },  /* South Shore */
    { "TJUA",   18.11566f,   -66.07817f },  /* San Juan */
};

const char *radar_site_nearest(float lat, float lon)
{
    /* Longitude degrees shrink towards the poles; scale them so the
     * comparison is in roughly equal units on both axes. */
    float lon_scale = cosf(lat * RADAR_DEG_TO_RAD);

    const radar_site_t *best = &s_radar_sites[0];
    float best_d2 = 1.0e30f;

    for (int i = 0; i < RADAR_SITE_COUNT; i++) {
        const radar_site_t *s = &s_radar_sites[i];

        float dlat = s->lat - lat;
        float dlon = s->lon - lon;

        /* Wrap across the antimeridian. */
        if (dlon > 180.0f) {
            dlon -= 360.0f;
        } else if (dlon < -180.0f) {
            dlon += 360.0f;
        }

        dlon *= lon_scale;

        float d2 = dlat * dlat + dlon * dlon;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = s;
        }
    }

    return best->id;
}
