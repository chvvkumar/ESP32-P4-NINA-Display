/*
 * radar_sites.h - NWS WSR-88D radar site lookup table.
 *
 * Baked table of every WSR-88D (NEXRAD) site, used to auto-pick the radar
 * nearest a configured latitude/longitude. The picked id is substituted into
 * https://radar.weather.gov/ridge/standard/{TOKEN}_0.gif
 *
 * Generated: 2026-08-16
 * Source:    https://api.weather.gov/radar/stations?stationType=WSR-88D
 *            (api.weather.gov requires a descriptive User-Agent header)
 * Note:      GeoJSON coordinates are [longitude, latitude] in that order.
 * Excluded:  RKJK, RKSG, RODN - overseas DoD sites listed by the API whose
 *            image URL 404s on radar.weather.gov. The other 156 all return
 *            200 image/gif.
 *
 * Regenerate by re-querying the Source URL above and re-emitting the table in
 * main/radar_sites.c; the data is verified, do not hand-edit individual rows.
 */

#ifndef RADAR_SITES_H
#define RADAR_SITES_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  id[5];   /* 4-char ICAO id, NUL terminated */
    float lat;     /* degrees north, positive = N */
    float lon;     /* degrees east, positive = E */
} radar_site_t;

#define RADAR_SITE_COUNT 156

/* Nearest site to the given position. Returns a pointer to a static string
 * that is never NULL and never freed. */
const char *radar_site_nearest(float lat, float lon);

/* Coordinates of a WSR-88D site by uppercase id. false when unknown. */
bool radar_site_coords(const char *id, float *lat, float *lon);

#ifdef __cplusplus
}
#endif

#endif /* RADAR_SITES_H */
