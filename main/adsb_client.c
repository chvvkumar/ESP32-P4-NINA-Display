/**
 * @file adsb_client.c
 * @brief ADS-B client: polls tar1090/readsb aircraft.json, ranks contacts and
 *        publishes a snapshot for the Flights page.
 *
 * Shape follows json_client.c / octoprint_client.c: one poll task on the
 * poll_task spine, page-gated, keep-alive http_fetch_conn_t owned by that task
 * alone. Because the gate teardown happens in the spine's on_park callback
 * (which runs ON the poll task at a point where no fetch is in flight), this
 * module needs no cross-task conn mutex -- unlike json/octoprint, which tear
 * their slot down from data_update_task.
 *
 * Publication is a pointer swap: the poll task parses into s_work (PSRAM) with
 * no lock held, then swaps s_work/s_pub under the mutex. The UI's snapshot
 * memcpy is therefore the only thing that ever blocks, and never behind a
 * 130 KB JSON parse.
 */

#include "adsb_client.h"
#include "adsb_geom.h"
#include "app_config.h"
#include "http_fetch.h"
#include "nina_client.h"
#include "poll_task.h"
#include "tasks.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "adsb_client";

/* 34 contacts measured 18.7 KB; tar1090 has no server-side filter, so leave
 * generous room for a busy sky. Over the cap the body is truncated, the parse
 * fails and the page shows "Cannot reach", which beats an OOM. */
#define ADSB_RESPONSE_MAX     131072
#define ADSB_RECEIVER_MAX     4096
#define ADSB_HTTP_TIMEOUT_MS  6000

/* Route lookup (adsb.im). Verified response element shape:
 *   { "callsign":"SWA776", "_airport_codes_iata":"LAX-STL",
 *     "airline_code":"SWA", "airport_codes":"KLAX-KSTL", "number":"776",
 *     "_airports":[...], "plausible":true }
 * An unresolvable callsign comes back with "unknown" in every code field and
 * NO "plausible" member; a resolved-but-geographically-implausible route comes
 * back with "plausible":false. Both count as unknown here. */
#define ADSB_ROUTE_URL         "https://adsb.im/api/0/routeset"
#define ADSB_ROUTE_CACHE_N     128
#define ADSB_ROUTE_BATCH_MAX   32
#define ADSB_ROUTE_MIN_GAP_MS  15000            /* never more than one call per 15 s */
#define ADSB_ROUTE_RETRY_MS    (10 * 60 * 1000) /* re-ask about a miss after 10 min */
#define ADSB_ROUTE_MAX_TRIES   3
#define ADSB_ROUTE_REQ_MAX     3072             /* 32 planes x ~68 B + wrapper */
#define ADSB_ROUTE_RESP_MAX    65536            /* ~800 B per element with _airports */
#define ADSB_ROUTE_TIMEOUT_MS  8000


_Atomic bool adsb_page_active = false;

/* Published snapshot and the poll task's working copy; swapped under s_mux. */
static adsb_data_t      *s_pub  = NULL;
static adsb_data_t      *s_work = NULL;
static SemaphoreHandle_t s_mux  = NULL;

/* Poll-task-only state. */
static http_fetch_conn_t *s_conn = NULL;
static TaskHandle_t       s_task = NULL;
static bool  s_rx_fetched = false;   /* receiver.json resolved this session */
static float s_rx_lat = 0.0f, s_rx_lon = 0.0f;
static bool  s_rx_valid = false;

/* Set by adsb_client_note_config_change(), consumed at the top of a poll. */
static _Atomic bool s_cfg_dirty = false;

/* Receiver message-rate state: aircraft.json's top-level `messages` counter
 * from the previous successful poll and when it was read. Only the poll task
 * touches these. The counter is read as a double (it outgrows int32 in days)
 * and kept as uint64_t; a value below the previous one means the receiver
 * restarted, so the delta is dropped and the baseline re-armed. */
static bool     s_msg_have_prev = false;
static uint64_t s_msg_prev = 0;
static int64_t  s_msg_prev_ms = 0;

/* Route cache, poll-task-only (PSRAM). fetched_ms == 0 marks a free slot. */
typedef struct {
    int64_t fetched_ms;      /**< when this callsign was last asked about */
    char    callsign[10];
    char    route[12];       /**< "" = not resolved (yet or ever) */
    uint8_t tries;           /**< consecutive unresolved answers */
} adsb_route_entry_t;

static adsb_route_entry_t *s_route_cache = NULL;
static int64_t             s_route_last_ms = 0;

/* Trail store, poll-task-only (PSRAM). Keyed by ICAO hex and therefore
 * INDEPENDENT of the published rank order: a contact that overtakes another
 * between polls keeps its own history. hex[0] == '\0' marks a free slot.
 *
 * Kept private rather than accumulated inside adsb_data_t because s_work and
 * s_pub are swapped on every publication -- s_work always holds the poll before
 * last, so an in-struct history would drop every other sample. */
typedef struct {
    char            hex[8];
    int64_t         seen_ms;               /**< last poll this hex was present */
    float           last_lat, last_lon;    /**< position of the newest sample */
    int32_t         t_s[ADSB_TRAIL_MAX];   /**< sample times, parallel to pt[] */
    adsb_trail_pt_t pt[ADSB_TRAIL_MAX];    /**< oldest first */
    uint8_t         n;
} adsb_track_t;

static adsb_track_t *s_track = NULL;   /* ADSB_MAX_AC entries */

/* Forward declarations (prototype-before-use under -Werror). */
static void  base_url_copy(const char *src, char *dst, size_t dst_len);
static bool  fetch_receiver(const char *base);
static void  parse_one(const cJSON *item, adsb_ac_t *a, float min_el,
                       const nina_pointing_t *pts, int npts);
static bool  better_than(const adsb_ac_t *a, const adsb_ac_t *b);
static void  rank_contacts(adsb_data_t *d);
static adsb_track_t *track_for(const char *hex, int64_t now_ms);
static void  track_sample(adsb_track_t *tr, const adsb_ac_t *a, int32_t now_s, int32_t window_s);
static void  track_expire(adsb_track_t *tr, int32_t now_s, int32_t window_s);
static void  trail_update(adsb_data_t *d);
static adsb_route_entry_t *route_cache_find(const char *cs);
static adsb_route_entry_t *route_cache_slot(const char *cs, int64_t now);
static bool  route_needs_lookup(const adsb_route_entry_t *e, int64_t now);
static void  route_apply(adsb_data_t *d, bool lookup_on);
static bool  route_step(const adsb_data_t *d);
static bool  adsb_poll_once(void *arg);
static uint32_t adsb_interval_ms(void *arg);
static void  adsb_on_park(void *arg);
static void  adsb_poll_task(void *arg);

// =============================================================================
// Lifecycle / publication
// =============================================================================

void adsb_client_init(void)
{
    if (s_mux) return;
    s_pub  = heap_caps_calloc(1, sizeof(adsb_data_t), MALLOC_CAP_SPIRAM);
    s_work = heap_caps_calloc(1, sizeof(adsb_data_t), MALLOC_CAP_SPIRAM);
    s_route_cache = heap_caps_calloc(ADSB_ROUTE_CACHE_N,
                                     sizeof(adsb_route_entry_t), MALLOC_CAP_SPIRAM);
    s_track = heap_caps_calloc(ADSB_MAX_AC, sizeof(adsb_track_t), MALLOC_CAP_SPIRAM);
    s_mux  = xSemaphoreCreateMutex();
    if (!s_pub || !s_work || !s_mux) {
        ESP_LOGE(TAG, "init failed (%u bytes x2 PSRAM)", (unsigned)sizeof(adsb_data_t));
    }
}

bool adsb_client_snapshot(adsb_data_t *out)
{
    if (!out || !s_mux || !s_pub) return false;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool ok = s_pub->ever_ok;
    if (ok) memcpy(out, s_pub, sizeof(adsb_data_t));
    xSemaphoreGive(s_mux);
    return ok;
}

void adsb_client_note_config_change(void)
{
    atomic_store(&s_cfg_dirty, true);
}

// =============================================================================
// Receiver position
// =============================================================================

/* Copy flights_url minus any trailing '/' so "<base>/data/x.json" is well
 * formed whichever way the user typed it. */
static void base_url_copy(const char *src, char *dst, size_t dst_len)
{
    size_t n = strlen(src);
    while (n > 0 && src[n - 1] == '/') n--;
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool fetch_receiver(const char *base)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/data/receiver.json", base);

    http_fetch_opts_t opts = {
        .timeout_ms = ADSB_HTTP_TIMEOUT_MS,
        .max_attempts = 1,
        .max_response_bytes = ADSB_RECEIVER_MAX,
    };
    char *body = NULL;
    size_t len = 0;
    if (http_fetch_text(url, &opts, &body, &len) != ESP_OK) return false;

    bool ok = false;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        const cJSON *lat = cJSON_GetObjectItem(root, "lat");
        const cJSON *lon = cJSON_GetObjectItem(root, "lon");
        if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            s_rx_lat = (float)lat->valuedouble;
            s_rx_lon = (float)lon->valuedouble;
            ok = true;
        }
        cJSON_Delete(root);
    }
    heap_caps_free(body);
    return ok;
}

// =============================================================================
// Parser
// =============================================================================

/* Number out of the first present member of a NULL-terminated name list.
 * Returns false when none of them is a number (a string "ground" altitude is
 * not a number, and is handled separately by the caller). */
static bool num_of(const cJSON *item, const char *const *names, float *out,
                   const char **which)
{
    for (int i = 0; names[i]; i++) {
        const cJSON *n = cJSON_GetObjectItem(item, names[i]);
        if (cJSON_IsNumber(n)) {
            *out = (float)n->valuedouble;
            if (which) *which = names[i];
            return true;
        }
    }
    return false;
}

/* True if any of @p names is the readsb string "ground". */
static bool is_ground(const cJSON *item, const char *const *names)
{
    for (int i = 0; names[i]; i++) {
        const cJSON *n = cJSON_GetObjectItem(item, names[i]);
        if (cJSON_IsString(n) && n->valuestring &&
            strcmp(n->valuestring, "ground") == 0) {
            return true;
        }
    }
    return false;
}

/* Copy a JSON string member into a fixed buffer, trimming leading/trailing
 * spaces (readsb space-pads `flight` to 8 chars) and dropping anything outside
 * printable ASCII. The aircraft database carries accented operator names in
 * UTF-8, and the LVGL fonts on this device have no glyphs for them, so a
 * multi-byte sequence would render as tofu; skipping the bytes keeps the rest
 * of the name readable. */
static void str_of(const cJSON *item, const char *name, char *dst, size_t dst_len)
{
    dst[0] = '\0';
    const cJSON *n = cJSON_GetObjectItem(item, name);
    if (!cJSON_IsString(n) || !n->valuestring) return;

    const char *s = n->valuestring;
    while (*s == ' ') s++;
    size_t n_len = strlen(s);
    while (n_len > 0 && s[n_len - 1] == ' ') n_len--;

    size_t w = 0;
    for (size_t i = 0; i < n_len && w + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c < 0x7f) dst[w++] = (char)c;
    }
    while (w > 0 && dst[w - 1] == ' ') w--;   /* trim again after dropping bytes */
    dst[w] = '\0';
}

static const char *const ALT_NAMES[]   = { "alt_geom", "alt_baro", "altitude", NULL };
static const char *const GS_NAMES[]    = { "gs", "speed", NULL };
static const char *const VRATE_NAMES[] = { "baro_rate", "vert_rate", "geom_rate", NULL };

/* Decode one aircraft[] element. Caller has already established that lat/lon
 * are present (has_pos). @p pts / @p npts are the online rig pointings used for
 * sep_deg; @p min_el is unused here but kept so the gate stays one concept. */
static void parse_one(const cJSON *item, adsb_ac_t *a, float min_el,
                      const nina_pointing_t *pts, int npts)
{
    (void)min_el;
    memset(a, 0, sizeof(*a));
    a->rank = -1;
    a->track_deg = -1.0f;
    a->sep_deg = ADSB_SEP_NONE;

    str_of(item, "hex", a->hex, sizeof(a->hex));
    str_of(item, "flight", a->flight, sizeof(a->flight));
    str_of(item, "t", a->type, sizeof(a->type));
    /* Aircraft-database extras (readsb serves these when it has a db loaded;
     * all three are simply absent on a bare dump1090 feed). */
    str_of(item, "r", a->reg, sizeof(a->reg));
    str_of(item, "ownOp", a->op, sizeof(a->op));
    str_of(item, "desc", a->desc, sizeof(a->desc));

    const cJSON *lat = cJSON_GetObjectItem(item, "lat");
    const cJSON *lon = cJSON_GetObjectItem(item, "lon");
    a->lat = (float)lat->valuedouble;
    a->lon = (float)lon->valuedouble;
    a->has_pos = true;

    /* Altitude: alt_geom > alt_baro > altitude; any of them as the string
     * "ground" means exactly that and pins the altitude to 0. */
    const char *alt_src = NULL;
    if (is_ground(item, ALT_NAMES)) {
        a->on_ground = true;
        a->alt_ft = 0.0f;
    } else if (num_of(item, ALT_NAMES, &a->alt_ft, &alt_src)) {
        a->alt_is_geom = (alt_src && strcmp(alt_src, "alt_geom") == 0);
    }

    num_of(item, GS_NAMES, &a->gs_kt, NULL);
    num_of(item, VRATE_NAMES, &a->vrate_fpm, NULL);

    const cJSON *trk = cJSON_GetObjectItem(item, "track");
    if (cJSON_IsNumber(trk)) a->track_deg = adsb_wrap360((float)trk->valuedouble);

    const cJSON *sp = cJSON_GetObjectItem(item, "seen_pos");
    if (cJSON_IsNumber(sp)) a->seen_pos_s = (float)sp->valuedouble;

    const cJSON *db = cJSON_GetObjectItem(item, "dbFlags");
    if (cJSON_IsNumber(db)) a->db_flags = (uint8_t)db->valueint;

    /* Squawk arrives as a 4-digit octal-notation string ("7700"). Stored as the
     * literal digits so the UI can print it back verbatim and the emergency
     * comparison reads like the code a pilot dials. */
    const cJSON *sq = cJSON_GetObjectItem(item, "squawk");
    if (cJSON_IsString(sq) && sq->valuestring) {
        a->squawk = (uint16_t)strtol(sq->valuestring, NULL, 10);
    }
    a->emergency = (a->squawk == 7500 || a->squawk == 7600 || a->squawk == 7700);
    const cJSON *em = cJSON_GetObjectItem(item, "emergency");
    if (cJSON_IsString(em) && em->valuestring && strcmp(em->valuestring, "none") != 0) {
        a->emergency = true;
    }

    /* Range/bearing: readsb's own r_dst/r_dir when present (it already knows
     * the receiver position), else computed from the receiver fix. */
    const cJSON *rdst = cJSON_GetObjectItem(item, "r_dst");
    const cJSON *rdir = cJSON_GetObjectItem(item, "r_dir");
    if (cJSON_IsNumber(rdst) && cJSON_IsNumber(rdir)) {
        a->dist_nm    = (float)rdst->valuedouble;
        a->bearing_deg = adsb_wrap360((float)rdir->valuedouble);
    } else if (s_rx_valid) {
        a->dist_nm    = adsb_haversine_nm(s_rx_lat, s_rx_lon, a->lat, a->lon);
        a->bearing_deg = adsb_bearing_deg(s_rx_lat, s_rx_lon, a->lat, a->lon);
    } else {
        /* No receiver reference anywhere (receiver.json unreachable AND no
         * weather location set) and the feed carries no r_dst/r_dir: range and
         * bearing are unknowable, so leaving dist_nm at 0 would put EVERY
         * contact at the zenith. Mark it unplaceable; the caller skips it. */
        a->has_pos = false;
        return;
    }
    a->az_deg = a->bearing_deg;

    float rx_elev = (npts > 0) ? pts[0].site_elev_m : 0.0f;
    a->el_deg = adsb_elevation_deg(a->dist_nm, a->alt_ft, rx_elev);

    for (int i = 0; i < npts; i++) {
        float s = adsb_sep_deg(a->az_deg, a->el_deg, pts[i].az_deg, pts[i].alt_deg);
        if (s < a->sep_deg) a->sep_deg = s;
    }
}

// =============================================================================
// Ranking -- done here so Sky, Scope and Board agree on one order
// =============================================================================

/* Emergencies first, then nearest by slant-ground range. Deliberately NOT
 * elevation- or sep_deg-based: the page is about flight awareness ("what is
 * that overhead"), where the honest answer is the closest aircraft, and a
 * pointing-dependent order made the same sky look different on three pages. */
static bool better_than(const adsb_ac_t *a, const adsb_ac_t *b)
{
    if (a->emergency != b->emergency) return a->emergency;
    return a->dist_nm < b->dist_nm;
}

/* Ranks EVERY positioned contact 0..count-1 in that order; the elevation gate
 * is not applied here. The Board lists the first N, the Sky and Scope modes tag
 * ranks 0-2, and Sky does its own el_deg filtering. */
static void rank_contacts(adsb_data_t *d)
{
    /* Insertion sort: n <= ADSB_MAX_AC (64) and the array is nearly sorted
     * across polls, so this beats anything with a call overhead.
     * ponytail: O(n^2) worst case at n=64; only matters if ADSB_MAX_AC grows. */
    for (int i = 1; i < d->count; i++) {
        adsb_ac_t tmp = d->ac[i];
        int j = i - 1;
        while (j >= 0 && better_than(&tmp, &d->ac[j])) {
            d->ac[j + 1] = d->ac[j];
            j--;
        }
        d->ac[j + 1] = tmp;
    }

    for (int i = 0; i < d->count; i++) {
        d->ac[i].rank = (i < 127) ? (int8_t)i : (int8_t)127;
    }
}

// =============================================================================
// Position trails
// =============================================================================

/* Truncating copy. Used instead of snprintf("%s") for the small fixed fields
 * here and in the route cache: gcc cannot bound the source length through a
 * 2-D array element and fails the build on -Werror=format-truncation. */
static void copy_fixed(char *dst, size_t dst_len, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Slot for @p hex: the existing track, else a free slot, else the track that
 * has gone unseen the longest. Recycling by seen_ms means a busy sky evicts the
 * aircraft that already left, never one still on screen.
 * ponytail: linear scan over 64 slots per contact; nothing next to the parse. */
static adsb_track_t *track_for(const char *hex, int64_t now_ms)
{
    for (int i = 0; i < ADSB_MAX_AC; i++) {
        if (s_track[i].hex[0] != '\0' && strcmp(s_track[i].hex, hex) == 0) {
            s_track[i].seen_ms = now_ms;
            return &s_track[i];
        }
    }
    adsb_track_t *pick = NULL;
    for (int i = 0; i < ADSB_MAX_AC; i++) {
        adsb_track_t *t = &s_track[i];
        if (t->hex[0] == '\0') { pick = t; break; }
        if (!pick || t->seen_ms < pick->seen_ms) pick = t;
    }
    if (!pick) return NULL;
    memset(pick, 0, sizeof(*pick));
    copy_fixed(pick->hex, sizeof(pick->hex), hex);
    pick->seen_ms = now_ms;
    return pick;
}

/* Append one sample. A parked or slow-updating contact would otherwise fill the
 * ring with 48 copies of the same point and push its real history out, so a
 * sample is only taken when the position actually moved -- or once every 10 s
 * regardless, which keeps a stationary contact's trail alive rather than
 * letting it age out and reappear as a jump. */
static void track_sample(adsb_track_t *tr, const adsb_ac_t *a, int32_t now_s,
                         int32_t window_s)
{
    if (tr->n > 0) {
        bool moved = (a->lat != tr->last_lat) || (a->lon != tr->last_lon);
        /* Space samples so the ring's slots cover the whole window (every
         * poll at the 120 s baseline, every ~5 s at a 240 s window). */
        int32_t min_gap = window_s / ADSB_TRAIL_MAX;
        int32_t since   = now_s - tr->t_s[tr->n - 1];
        if (since < min_gap) return;
        if (!moved && since < 10) return;
    }
    if (tr->n >= ADSB_TRAIL_MAX) {
        memmove(&tr->pt[0], &tr->pt[1], (ADSB_TRAIL_MAX - 1) * sizeof(tr->pt[0]));
        memmove(&tr->t_s[0], &tr->t_s[1], (ADSB_TRAIL_MAX - 1) * sizeof(tr->t_s[0]));
        tr->n = ADSB_TRAIL_MAX - 1;
    }
    tr->pt[tr->n].dist_nm     = a->dist_nm;
    tr->pt[tr->n].bearing_deg = a->bearing_deg;
    tr->pt[tr->n].el_deg      = a->el_deg;
    tr->t_s[tr->n] = now_s;
    tr->n++;
    tr->last_lat = a->lat;
    tr->last_lon = a->lon;
}

/* Drop samples older than the window. They are always at the head, so this is
 * one memmove and never a scan of the whole ring. */
static void track_expire(adsb_track_t *tr, int32_t now_s, int32_t window_s)
{
    int drop = 0;
    while (drop < tr->n && (now_s - tr->t_s[drop]) > window_s) drop++;
    if (drop == 0) return;
    tr->n = (uint8_t)(tr->n - drop);
    if (tr->n > 0) {
        memmove(tr->pt, &tr->pt[drop], tr->n * sizeof(tr->pt[0]));
        memmove(tr->t_s, &tr->t_s[drop], tr->n * sizeof(tr->t_s[0]));
    }
}

/* Sample every published contact, then copy its history into the row the UI
 * will read. Runs AFTER rank_contacts() so trail row i lines up with ac[i]. */
static void trail_update(adsb_data_t *d)
{
    if (!s_track) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    int32_t now_s  = (int32_t)(now_ms / 1000);

    /* The trail window scales with the configured range so a loop across the
     * picture is about the same on-screen length at every range: 120 s at the
     * 50 nm baseline, 240 s at 100 nm, clamped to [60, 600]. Sample spacing
     * scales with it in track_sample so ADSB_TRAIL_MAX covers the window. */
    int range = app_config_get()->flights_range_nm;
    if (range < 10) range = 50;
    int32_t window_s = (int32_t)ADSB_TRAIL_S * range / 50;
    if (window_s < 60)  window_s = 60;
    if (window_s > 600) window_s = 600;

    /* Recycle tracks whose aircraft has been gone longer than the trail window:
     * anything they still hold would have expired sample by sample anyway. */
    for (int i = 0; i < ADSB_MAX_AC; i++) {
        if (s_track[i].hex[0] != '\0' &&
            (now_ms - s_track[i].seen_ms) > (int64_t)window_s * 1000) {
            s_track[i].hex[0] = '\0';
            s_track[i].n = 0;
        }
    }

    for (int i = 0; i < d->count; i++) {
        adsb_ac_t *a = &d->ac[i];
        a->trail_n = 0;
        if (!a->has_pos || a->hex[0] == '\0') continue;

        adsb_track_t *tr = track_for(a->hex, now_ms);
        if (!tr) continue;
        track_sample(tr, a, now_s, window_s);
        track_expire(tr, now_s, window_s);

        if (tr->n > 0) {
            memcpy(d->trail[i], tr->pt, (size_t)tr->n * sizeof(adsb_trail_pt_t));
        }
        a->trail_n = tr->n;
    }
}

// =============================================================================
// Route lookup (adsb.im /api/0/routeset)
// =============================================================================

static adsb_route_entry_t *route_cache_find(const char *cs)
{
    if (!s_route_cache) return NULL;
    for (int i = 0; i < ADSB_ROUTE_CACHE_N; i++) {
        if (s_route_cache[i].fetched_ms != 0 &&
            strcmp(s_route_cache[i].callsign, cs) == 0) {
            return &s_route_cache[i];
        }
    }
    return NULL;
}

/* Existing entry, else a free slot, else the least-recently-asked one recycled.
 * ponytail: linear scan over 128 entries, twice per batch; nothing next to a
 * 130 KB JSON parse. */
static adsb_route_entry_t *route_cache_slot(const char *cs, int64_t now)
{
    if (!s_route_cache) return NULL;
    adsb_route_entry_t *found = route_cache_find(cs);
    if (found) return found;

    adsb_route_entry_t *pick = NULL;
    for (int i = 0; i < ADSB_ROUTE_CACHE_N; i++) {
        adsb_route_entry_t *e = &s_route_cache[i];
        if (e->fetched_ms == 0) { pick = e; break; }
        if (!pick || e->fetched_ms < pick->fetched_ms) pick = e;
    }
    if (!pick) return NULL;
    memset(pick, 0, sizeof(*pick));
    copy_fixed(pick->callsign, sizeof(pick->callsign), cs);
    pick->fetched_ms = now;
    return pick;
}

/* A resolved route is kept for the life of the cache entry. A miss is retried
 * at most ADSB_ROUTE_MAX_TRIES times, no sooner than 10 minutes apart, so a
 * general-aviation tail number with no airline route never becomes a poll-rate
 * request loop against a third-party service. */
static bool route_needs_lookup(const adsb_route_entry_t *e, int64_t now)
{
    return e->route[0] == '\0' && e->tries < ADSB_ROUTE_MAX_TRIES &&
           (now - e->fetched_ms) >= ADSB_ROUTE_RETRY_MS;
}

/* Stamp each contact with its cached route, or mark it pending. Called once on
 * the private working copy before publication, and again on the published copy
 * (under the mutex) right after a lookup lands, so a route shows up in the same
 * cycle it was resolved. */
static void route_apply(adsb_data_t *d, bool lookup_on)
{
    d->route_lookup_active = lookup_on;
    for (int i = 0; i < d->count; i++) {
        adsb_ac_t *a = &d->ac[i];
        a->route[0] = '\0';
        a->route_pending = false;
        if (!lookup_on || a->flight[0] == '\0') continue;

        const adsb_route_entry_t *e = route_cache_find(a->flight);
        if (e && e->route[0]) {
            copy_fixed(a->route, sizeof(a->route), e->route);
        } else {
            /* "..." only until the FIRST answer arrives; a miss then shows the
             * operator/type fallback while the silent 10-minute retries continue. */
            a->route_pending = (!e || e->tries == 0);
        }
    }
}

/* One route batch. Returns true if a request was actually issued (the caller
 * then re-applies the cache to the published snapshot).
 *
 * Runs on the poll task AFTER the aircraft snapshot has been published and
 * OUTSIDE the data mutex, so an 8 s call to a third-party host can never delay
 * the traffic update or block the UI's snapshot copy. */
static bool route_step(const adsb_data_t *d)
{
    if (!s_route_cache) return false;
    int64_t now = esp_timer_get_time() / 1000;
    if (s_route_last_ms != 0 && (now - s_route_last_ms) < ADSB_ROUTE_MIN_GAP_MS) {
        return false;
    }

    char *req = heap_caps_malloc(ADSB_ROUTE_REQ_MAX, MALLOC_CAP_SPIRAM);
    if (!req) return false;

    char pend[ADSB_ROUTE_BATCH_MAX][10];
    int  npend = 0;
    int  off = snprintf(req, ADSB_ROUTE_REQ_MAX, "{\"planes\":[");

    for (int i = 0; i < d->count && npend < ADSB_ROUTE_BATCH_MAX; i++) {
        const adsb_ac_t *a = &d->ac[i];
        if (a->flight[0] == '\0') continue;

        const adsb_route_entry_t *e = route_cache_find(a->flight);
        if (e && !route_needs_lookup(e, now)) continue;

        bool dup = false;
        for (int k = 0; k < npend; k++) {
            if (strcmp(pend[k], a->flight) == 0) { dup = true; break; }
        }
        if (dup) continue;

        int n = snprintf(req + off, (size_t)(ADSB_ROUTE_REQ_MAX - off),
                         "%s{\"callsign\":\"%s\",\"lat\":%.4f,\"lng\":%.4f}",
                         npend ? "," : "", a->flight, (double)a->lat, (double)a->lon);
        if (n < 0 || off + n > ADSB_ROUTE_REQ_MAX - 4) break;
        off += n;
        copy_fixed(pend[npend], sizeof(pend[npend]), a->flight);
        npend++;
    }

    if (npend == 0) {
        heap_caps_free(req);
        return false;
    }
    snprintf(req + off, (size_t)(ADSB_ROUTE_REQ_MAX - off), "]}");
    s_route_last_ms = now;

    /* Burn a try on every callsign in the batch up front, so a request that
     * fails outright (or comes back missing an element) still counts against
     * ADSB_ROUTE_MAX_TRIES instead of retrying forever. */
    for (int k = 0; k < npend; k++) {
        adsb_route_entry_t *e = route_cache_slot(pend[k], now);
        if (!e) continue;
        e->fetched_ms = now;
        if (e->tries < 255) e->tries++;
    }

    http_fetch_opts_t opts = {
        .timeout_ms = ADSB_ROUTE_TIMEOUT_MS,
        .use_tls_bundle = true,
        .max_attempts = 1,
        .max_response_bytes = ADSB_ROUTE_RESP_MAX,
        .post_body = req,
        .content_type = "application/json",
    };
    char  *body = NULL;
    size_t len = 0;
    esp_err_t err = http_fetch_text(ADSB_ROUTE_URL, &opts, &body, &len);
    heap_caps_free(req);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "route lookup failed for %d callsign(s)", npend);
        return true;   /* tries were consumed; re-apply so pending flags settle */
    }

    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return true;
    }

    int resolved = 0;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, root) {
        char cs[10];
        str_of(el, "callsign", cs, sizeof(cs));
        if (cs[0] == '\0') continue;
        adsb_route_entry_t *e = route_cache_find(cs);
        if (!e) continue;

        char r[12];
        str_of(el, "_airport_codes_iata", r, sizeof(r));
        /* "plausible" is absent when nothing was found and false when the pair
         * does not fit the aircraft's reported position; both mean unknown. */
        const cJSON *pl = cJSON_GetObjectItem(el, "plausible");
        bool plausible = cJSON_IsTrue(pl) ||
                         (cJSON_IsNumber(pl) && pl->valueint != 0);
        if (r[0] != '\0' && strcmp(r, "unknown") != 0 && plausible) {
            copy_fixed(e->route, sizeof(e->route), r);
            e->tries = 0;
            resolved++;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "route lookup: %d asked, %d resolved", npend, resolved);
    return true;
}

// =============================================================================
// Poll
// =============================================================================

static bool adsb_poll_once(void *arg)
{
    (void)arg;
    const app_config_t *cfg = app_config_get();
    if (cfg->flights_url[0] == '\0') return true;   /* nothing to poll, no backoff */

    char base[128];
    base_url_copy(cfg->flights_url, base, sizeof(base));

    if (atomic_exchange(&s_cfg_dirty, false)) {
        s_rx_fetched = false;
        s_rx_valid = false;
        memset(s_work, 0, sizeof(*s_work));
        /* The receiver reference may have moved, so every stored range/bearing
         * is now wrong: start the histories over rather than draw a kink. */
        if (s_track) memset(s_track, 0, ADSB_MAX_AC * sizeof(adsb_track_t));
        s_msg_have_prev = false;   /* a different receiver has a different counter */
    }

    /* Receiver position: receiver.json, retried on every poll until it
     * succeeds (a transient failure must not latch the fallback for the
     * whole session); meanwhile use the weather location the user already
     * configured for the clock page. */
    if (!s_rx_fetched) {
        s_rx_fetched = fetch_receiver(base);
        s_rx_valid = s_rx_fetched;
        if (!s_rx_valid && (cfg->weather_lat != 0.0f || cfg->weather_lon != 0.0f)) {
            s_rx_lat = cfg->weather_lat;
            s_rx_lon = cfg->weather_lon;
            s_rx_valid = true;
            ESP_LOGW(TAG, "receiver.json unavailable, using weather location");
        }
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/data/aircraft.json", base);

    if (!s_conn) s_conn = http_fetch_conn_create();
    http_fetch_opts_t opts = {
        .timeout_ms = ADSB_HTTP_TIMEOUT_MS,
        .max_attempts = 1,
        .max_response_bytes = ADSB_RESPONSE_MAX,
        .conn = s_conn,
    };
    char *body = NULL;
    size_t len = 0;
    if (http_fetch_text(url, &opts, &body, &len) != ESP_OK) goto fail;

    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (!root) goto fail;

    const cJSON *arr = cJSON_GetObjectItem(root, "aircraft");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        goto fail;
    }
    /* Top-level `messages`: readsb/tar1090 and dump1090 both publish it. */
    const cJSON *msgs_j = cJSON_GetObjectItem(root, "messages");
    bool have_msgs = cJSON_IsNumber(msgs_j) && msgs_j->valuedouble >= 0.0;
    uint64_t msgs = have_msgs ? (uint64_t)msgs_j->valuedouble : 0;

    nina_pointing_t pts[MAX_NINA_INSTANCES];
    int npts = nina_client_get_pointings(pts, MAX_NINA_INSTANCES);
    if (npts < 0) npts = 0;
    bool have_pointing = (npts > 0);
    float min_el = (float)cfg->flights_min_el;
    bool lookup_on = cfg->flights_route_lookup;

    adsb_data_t *d = s_work;
    d->count = 0;
    d->count_total = cJSON_GetArraySize(arr);
    d->count_pos = 0;
    d->count_above_gate = 0;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const cJSON *lat = cJSON_GetObjectItem(item, "lat");
        const cJSON *lon = cJSON_GetObjectItem(item, "lon");
        if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) continue;  /* counted only */
        d->count_pos++;

        adsb_ac_t a;
        parse_one(item, &a, min_el, pts, npts);
        if (!a.has_pos) continue;   /* no receiver reference: counted, not placed */
        if (a.el_deg >= min_el) d->count_above_gate++;

        if (d->count < ADSB_MAX_AC) {
            d->ac[d->count++] = a;
            continue;
        }
        /* Full: displace the worst entry if this one outranks it.
         * ponytail: linear worst-scan per overflow contact; n=64, negligible
         * next to the JSON parse. Swap for a heap only if ADSB_MAX_AC grows. */
        int worst = 0;
        for (int i = 1; i < d->count; i++) {
            if (better_than(&d->ac[worst], &d->ac[i])) worst = i;
        }
        if (better_than(&a, &d->ac[worst])) d->ac[worst] = a;
    }
    cJSON_Delete(root);

    rank_contacts(d);
    trail_update(d);            /* after ranking: trail row i belongs to ac[i] */
    route_apply(d, lookup_on);

    d->rx_lat = s_rx_lat;
    d->rx_lon = s_rx_lon;
    d->rx_elev_m = have_pointing ? pts[0].site_elev_m : 0.0f;
    d->rx_valid = s_rx_valid;
    d->now_ms = esp_timer_get_time() / 1000;
    d->ever_ok = true;
    d->last_ok_ms = d->now_ms;
    d->fail_count = 0;

    /* Message rate: delta of the receiver's counter over the elapsed time.
     * 0 until a baseline exists; a counter that went backwards (receiver
     * restart) or a missing key re-arms the baseline instead of publishing
     * a bogus burst. */
    d->msg_rate = 0.0f;
    if (have_msgs) {
        int64_t dt_ms = d->now_ms - s_msg_prev_ms;
        if (s_msg_have_prev && msgs >= s_msg_prev && dt_ms > 0) {
            d->msg_rate = (float)(msgs - s_msg_prev) * 1000.0f / (float)dt_ms;
        }
        s_msg_prev = msgs;
        s_msg_prev_ms = d->now_ms;
        s_msg_have_prev = true;
    } else {
        s_msg_have_prev = false;
    }

    /* Publish: swap the buffers, so the UI is never blocked behind a parse. */
    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        adsb_data_t *tmp = s_pub;
        s_pub = s_work;
        s_work = tmp;
        xSemaphoreGive(s_mux);
    }

    /* Routes come AFTER publication and outside the mutex: `d` is now the
     * published buffer, and reading its callsigns races with nothing (the UI
     * only ever reads it too). One batch per cycle at most; on a hit the cache
     * is re-applied to the same buffer under the mutex so the routes land in
     * this cycle rather than the next. */
    if (lookup_on && atomic_load(&adsb_page_active) && route_step(d)) {
        if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
            route_apply(s_pub, true);
            xSemaphoreGive(s_mux);
        }
    }
    return true;

fail:
    /* Leave the published contacts in place so the page can render them
     * dimmed; page_conn_eval() turns fail_count into the STALE/DOWN tiers. */
    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        if (s_pub->fail_count < 1000) s_pub->fail_count++;
        xSemaphoreGive(s_mux);
    }
    return false;
}

static uint32_t adsb_interval_ms(void *arg)
{
    (void)arg;
    uint32_t ms = (uint32_t)app_config_get()->flights_update_interval_s * 1000u;
    return (ms < 2000u) ? 2000u : ms;
}

/* Runs on the poll task at its parked point, nothing in flight: the safe place
 * to close the keep-alive socket. A drained-parked slot holds an OPEN socket
 * and the page-gated loop stops running, so it would otherwise sit against the
 * ~9-connection ceiling for as long as the page is hidden. */
static void adsb_on_park(void *arg)
{
    (void)arg;
    if (s_conn) {
        http_fetch_conn_destroy(s_conn);
        s_conn = NULL;
    }
}

static void adsb_poll_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ADS-B poll task started");
    s_msg_have_prev = false;   /* message-rate baseline starts fresh */
    poll_loop_spec_t spec = {
        .name = "adsb",
        .wifi_group = s_wifi_event_group,
        .wifi_bits = WIFI_CONNECTED_BIT,
        .page_active = &adsb_page_active,
        .poll_once = adsb_poll_once,
        .interval_ms = adsb_interval_ms,
        .backoff_initial_ms = 5000,
        .backoff_max_ms = 60000,
        .on_park = adsb_on_park,
    };
    poll_loop_run(&spec, NULL);
}

void adsb_ensure_task_running(void)
{
    if (!app_config_get()->flights_enabled) return;
    adsb_client_init();
    if (!s_pub || !s_work || !s_mux) return;

    static portMUX_TYPE adsb_spawn_mux = portMUX_INITIALIZER_UNLOCKED;
    psram_task_ensure(&s_task, &adsb_spawn_mux,
                      adsb_poll_task, "adsb", 12288, NULL, 3, 0);
}
