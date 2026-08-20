#pragma once

/**
 * @file adsb_client.h
 * @brief ADS-B client for the Flights page: polls a local tar1090/readsb
 *        receiver and publishes a ranked snapshot of positioned contacts.
 *
 * Source: `<flights_url>/data/aircraft.json` (readsb/tar1090 schema, with the
 * older dump1090 field names accepted as fallbacks) polled every
 * `flights_update_interval_s`, plus `<flights_url>/data/receiver.json` fetched
 * once per poller start for the receiver lat/lon (falls back to
 * `weather_lat`/`weather_lon` from config).
 *
 * The published `adsb_data_t` lives in PSRAM behind a mutex; the UI copies a
 * whole snapshot out with adsb_client_snapshot() and never holds the lock while
 * drawing. Geometry (elevation, bearing, separation, projections) is pure math
 * in adsb_geom.h so it is host-testable.
 *
 * Ranking happens here, not in the UI, so all three page modes agree on the
 * order: emergency first, then nearest by dist_nm (see the `rank` field).
 *
 * With `flights_route_lookup` on, callsigns are resolved to an origin-destination
 * IATA pair via adsb.im's /api/0/routeset, cached in PSRAM and re-applied to each
 * published snapshot.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/** Hard cap on published contacts. More positioned contacts than this: the
 *  best ADSB_MAX_AC by rank are kept and the rest only counted. */
#define ADSB_MAX_AC 64

/** sep_deg sentinel when no rig pointing is available. */
#define ADSB_SEP_NONE 999.0f

/** Position history ("trail") kept per contact: how long, and how many samples. */
#define ADSB_TRAIL_S   120
#define ADSB_TRAIL_MAX 48

/**
 * One trail sample, already reduced to what the two disc projections need.
 *
 * The client stores range/bearing/elevation rather than lat/lon so the UI only
 * rotates and projects: a haversine per point per update (64 x 48) would be
 * ~3000 transcendental calls every poll for numbers that never change after
 * the sample was taken.
 */
typedef struct {
    float dist_nm;      /**< ground range from the receiver */
    float bearing_deg;  /**< true bearing from the receiver [0,360) */
    float el_deg;       /**< elevation over the receiver horizon */
} adsb_trail_pt_t;      /* 12 B */

typedef struct {
    char     hex[8];
    char     flight[10];        /**< trimmed callsign, "" if none */
    char     type[6];           /**< readsb `t` */
    char     cat[4];            /**< readsb `category` (A0-A7, B*, C*), "" if absent */
    float    lat, lon;          /**< valid iff has_pos */
    float    alt_ft;            /**< alt_geom else alt_baro; ground = 0 */
    bool     alt_is_geom;
    bool     on_ground;
    float    gs_kt;             /**< gs else speed */
    float    track_deg;         /**< 0-359, -1 unknown */
    float    vrate_fpm;         /**< baro_rate else vert_rate else geom_rate */
    float    dist_nm;           /**< r_dst else haversine from receiver */
    float    bearing_deg;       /**< r_dir else computed */
    float    el_deg;            /**< elevation from receiver, curved earth */
    float    az_deg;            /**< == bearing_deg */
    float    seen_pos_s;
    uint16_t squawk;            /**< 0 if none (parsed from the octal string) */
    uint8_t  db_flags;          /**< readsb dbFlags (1 mil, 2 interesting, 4 PIA, 8 LADD) */
    bool     has_pos;
    bool     emergency;         /**< squawk 7500/7600/7700 or "emergency" != "none" */
    float    sep_deg;           /**< angular separation to nearest online FOV centre,
                                  *  ADSB_SEP_NONE if no rig is pointing. Informational
                                  *  only -- ranking no longer uses it. */
    int8_t   rank;              /**< 0 = nearest, 1, 2, ...; -1 = no position.
                                  *  Every positioned contact gets a rank (Board lists
                                  *  the first 8; Sky/Scope tag ranks 0-2). */
    char     route[12];         /**< "LAX-STL" IATA pair, "" unknown */
    char     reg[10];           /**< readsb `r` registration */
    char     op[24];            /**< readsb `ownOp` owner/operator, trimmed */
    char     desc[24];          /**< readsb `desc` type description, trimmed */
    bool     route_pending;     /**< route lookup requested, not yet answered */
    uint8_t  trail_n;           /**< samples in adsb_data_t.trail[i] for THIS contact,
                                  *  oldest first, newest == the current position.
                                  *  The trail row index IS the contact index: row i
                                  *  belongs to ac[i]. 0/1 means nothing to draw. */
} adsb_ac_t;

typedef struct {
    adsb_ac_t ac[ADSB_MAX_AC];
    /** Position history, row i belonging to ac[i] (see adsb_ac_t.trail_n).
     *  36 KB: kept inside the struct so adsb_client_snapshot() stays one memcpy. */
    adsb_trail_pt_t trail[ADSB_MAX_AC][ADSB_TRAIL_MAX];
    int       count;            /**< published contacts (<= ADSB_MAX_AC) */
    int       count_total;      /**< aircraft[] length in the JSON (may exceed count) */
    int       count_pos;        /**< with position */
    int       count_above_gate; /**< has_pos && el_deg >= flights_min_el */
    float     rx_lat, rx_lon, rx_elev_m;
    bool      rx_valid;
    int64_t   now_ms;           /**< esp_timer ms at parse */
    bool      ever_ok;          /**< page_conn inputs */
    int64_t   last_ok_ms;
    int       fail_count;
    bool      route_lookup_active; /**< mirrors flights_route_lookup so the UI can
                                     *  label "route unknown" vs "lookup off" */
    float     msg_rate;         /**< receiver messages per second, from the delta
                                  *  of aircraft.json's top-level `messages` counter
                                  *  between consecutive successful polls. 0 until
                                  *  two polls have been seen; resets on poller start
                                  *  or when the counter goes backwards (receiver
                                  *  restart). Unrounded: the UI formats it. */
} adsb_data_t;

/** Create the mutex and the PSRAM data block. Idempotent. */
void adsb_client_init(void);

/** Copy the published data under the mutex. False if never polled successfully
 *  (or the lock could not be taken); *out is untouched in that case. */
bool adsb_client_snapshot(adsb_data_t *out);

/** URL / range / gate changed: drop the cached receiver position and the
 *  contact list so the next poll starts clean. */
void adsb_client_note_config_change(void);

/** Poll gate, written by the dashboard on page enter/leave (same pattern as
 *  json_page_active). */
extern _Atomic bool adsb_page_active;

/** Spawn the poll task on the poll_task spine. Idempotent; safe to call from
 *  boot and from a runtime enable. */
void adsb_ensure_task_running(void);
