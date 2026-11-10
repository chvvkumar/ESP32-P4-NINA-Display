#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui/info_overlay_types.h"
#include "nina_plan_types.h"   /* nina_plan_t: the running container's Smart Exposure plan */

#define MAX_FILTERS 10
#define HFR_RING_SIZE 500  // Matches GRAPH_MAX_POINTS for HFR graph overlay
#define NINA_HORIZON_N 72  // horizon_alt[] samples, 5 degrees of azimuth each; equals SKY_HORIZON_N

// Filter information
typedef struct {
    char name[32];      // Filter name (e.g., "Ha", "L", "R", "G", "B")
    int id;             // Filter position/ID
} nina_filter_t;

// NINA client data structure
typedef struct {
    bool connected;
    char status[32];
    char target_name[64];
    char prev_target_container[64];   // last RUNNING target container Name (stripped) — for new-target detection
    char profile_name[64];
    char profile_id[64];      // NINA profile Id (falls back to Name); keys the per-profile colours
    char telescope_name[64];
    char camera_name[64];

    struct {
        float temp;
        float cooler_power;
    } camera;
    
    struct {
        float rms_total;
        float rms_ra;
        float rms_dec;
    } guider;
    
    struct {
        int position;
    } focuser;
    
    struct {
        float illumination;
    } moon;
    
    // Sequence info
    int exposure_count;         // Completed exposures for current filter (CompletedIterations, flattened through enclosing "Loop For Iterations" conditions)
    int exposure_iterations;    // Planned exposures for current filter (Iterations x enclosing loop Iterations)
    int exposure_total_count;   // Cumulative exposures done for current filter (ExposureCount)
    float exposure_current;     // Elapsed time in current exposure (seconds)
    float exposure_total;       // Total duration of current exposure (seconds, from ExposureTime)
    int64_t exposure_end_epoch; // Absolute end time (Unix epoch seconds) for client-side interpolation
    bool is_exposing;           // True when camera is actively exposing (from IsExposing API field)
    char current_filter[32];    // Current filter name (e.g., "Ha", "Sii", "L")
    char container_name[64];    // Running container name (e.g., "LRGBSHO") - stripped of "_Container"
    char container_step[64];    // Currently running step/instruction name (e.g., "Smart Exposure", "Auto Focus")
    nina_plan_t plan;           // Parent container's Smart Exposure plan (sub bar/ring); n_items 0 = no plan
    char time_remaining[32];    // Time remaining for entire sequence (HH:MM:SS format)
    bool is_dithering;
    bool is_waiting;              // Sequence is in a wait state (TS-WAITSTART)
    int64_t wait_start_epoch;     // Wait start time (Unix epoch seconds)

    // NINA-PC clock domain anchor. NINA timestamps (ExposureEndTime, sequence
    // ExpectedDateTime, WaitStartTime) are stamped by the NINA PC's clock,
    // which can be several seconds skewed from the device's SNTP clock. The
    // camera-info fetchers capture NINA's own clock from the HTTP Date
    // response header; all NINA-timestamp math should use this pair (via
    // nina_client_now_epoch()) instead of time(NULL).
    int64_t nina_clock_epoch;    /* NINA-domain UTC epoch from HTTP Date header, 0 = unknown */
    int64_t nina_clock_mono_us;  /* esp_timer_get_time() at capture */

    // Image stats
    float hfr;
    int stars;
    char target_time_remaining[16]; // Earliest remaining time across all conditions (H:MM format)
    char target_time_reason[16];    // Binding constraint label: "TIME LIMIT", "SETS IN", "DAWN IN"
    int  target_condition_count;    // Number of active loop conditions (for "+" indicator)

    // Mount
    char meridian_flip[16];
    float rotator_angle;
    bool rotator_connected;

    // Mount pointing — filled every cycle from the bundled /equipment/info
    // "Mount" object, or from /equipment/mount/info on the legacy slow tier.
    // mount_pointing_valid means: mount Connected, not AtPark, and both angles
    // present in that response. Cleared when the instance goes offline.
    float mount_alt_deg;        // MountInfo Altitude (degrees)
    float mount_az_deg;         // MountInfo Azimuth (degrees)
    float site_elev_m;          // MountInfo SiteElevation (metres)
    bool  mount_pointing_valid;

    // Sky scene inputs (NINA page layouts 8 and 9). Filled per poll by
    // parse_mount_pointing() in nina_api_fetchers.c, from the same MountInfo
    // object the pointing above comes from (bundled /equipment/info "Mount"
    // every foreground cycle, or /equipment/mount/info on the legacy tier).
    // Recomputed from scratch on every parse; a NULL mount object clears every
    // flag below to false. demo_data.c fills all of them in demo mode.
    bool    mount_connected;    // MountInfo Connected
    bool    mount_at_park;      // AtPark. Gate on this BEFORE mount_slewing: a real
                                // mount at park has reported Slewing true with AtPark true
    bool    mount_slewing;      // Slewing
    bool    mount_tracking;     // TrackingEnabled
    uint8_t mount_pier_side;    // SideOfPier: 0 unknown/absent, 1 "pierEast", 2 "pierWest"
                                // (values equal sky_pier_t in sky_scene_math.h)
    bool    site_valid;         // SiteLatitude AND SiteLongitude both numbers this parse
    float   site_lat_deg;       // SiteLatitude, degrees north
    float   site_lon_deg;       // SiteLongitude, degrees east
    bool    lst_valid;          // SiderealTime was a number this parse
    float   lst_hours;          // SiderealTime, hours [0,24), at lst_mono_us
    int64_t lst_mono_us;        // esp_timer_get_time() when lst_hours was parsed
    bool    flip_valid;         // TimeToMeridianFlip usable: connected, !at_park, tracking,
                                // and 0 <= value < 23.99 h (NINA reports 24 as "none")
    float   flip_minutes;       // TimeToMeridianFlip * 60, minutes until the flip

    // Target coordinates from sequence/json (nina_sequence.c), the active
    // target container's first item carrying Coordinates {RA (hours), Dec
    // (degrees)}: "Slew and center" / "Slew, center and rotate", else the
    // "Center After Drift" trigger's Coordinates.Coordinates. J2000 as NINA
    // stores them. Rewritten on every sequence poll; valid=false when the
    // container has no such item.
    bool    target_coord_valid;
    float   target_ra_h;        // hours [0,24)
    float   target_dec_deg;     // degrees [-90,90]
    // Primary source: the container's own Target.InputCoordinates from
    // sequence/state, fetched once per container name and remembered here
    // (nina_sequence.c). A non-empty container name with valid=false means
    // that container was looked up and has no Target of its own.
    bool    state_coord_valid;
    char    state_coord_container[64];  // full name, "_Container" suffix included
    float   state_coord_ra_h;
    float   state_coord_dec_deg;

    // Horizon outline of the active profile (profile/horizon), fetched once
    // per connect in the static pass and resampled to NINA_HORIZON_N
    // altitudes, one per 5 degrees of azimuth (index i = azimuth 5*i), whole
    // degrees. horizon_valid=false (draw a flat horizon) when the endpoint
    // fails, is missing on an old ninaAPI, or returns no points.
    bool    horizon_valid;
    int8_t  horizon_alt[NINA_HORIZON_N];

    // Camera sensor geometry (camera info XSize/YSize/PixelSize) — FOV math.
    int   cam_x_size;
    int   cam_y_size;
    float cam_pixel_size_um;

    // Active profile TelescopeSettings.FocalLength (mm), 0 = unknown.
    float focal_length_mm;

    // Safety monitor state (updated via SAFETY-CHANGED WebSocket event)
    bool safety_is_safe;
    bool safety_connected;

    // Power/Switch info (from /equipment/switch/info)
    struct {
        float input_voltage;
        float total_amps;
        float total_watts;
        char  amps_name[32];       // API name for current (e.g., "Total Current", "Amp")
        char  watts_name[32];      // API name for power (e.g., "Total Power", "Watt")
        float pwm[4];              // PWM/dew heater outputs (%)
        char  pwm_names[4][32];    // Names for each PWM output
        int   pwm_count;
        bool  switch_connected;
    } power;

    // Available filters from filter wheel
    nina_filter_t filters[MAX_FILTERS];
    int filter_count;

    // WebSocket state
    bool websocket_connected;

    // Set true when a new image is saved (IMAGE-SAVE event or image-history change)
    // Consumer should clear after handling
    _Atomic bool new_image_available;

    // Set true by WebSocket event handlers to request immediate UI refresh.
    // Data task checks this and refreshes UI without waiting for next poll cycle.
    _Atomic bool ui_refresh_needed;

    // Set by WebSocket event handlers when a sequence-relevant event occurs
    // (IMAGE-SAVE, TS-NEWTARGETSTART, SEQUENCE-STARTING). Causes immediate
    // sequence poll on next cycle.
    _Atomic bool sequence_poll_needed;

    // Set by PROFILE-CHANGED WebSocket event to force re-fetch of cached
    // static data (profile name, filters, telescope) on the next poll cycle.
    _Atomic bool profile_refresh_needed;

    // Set on every static (re)fetch and by the PROFILE-REMOVED WebSocket event.
    // The instance poll task consumes it after the profile switch and drops
    // remembered per-profile colours whose profile no longer exists in NINA.
    _Atomic bool profile_prune_needed;

    // Set when a profile change may have been missed (the WebSocket just
    // (re)connected, or the static pass got no profile id). The poll re-reads
    // the active profile, at most once per NINA_PROFILE_CHECK_MIN_MS, and asks
    // for a full static pass only when the id really changed.
    _Atomic bool profile_check_needed;

    // Timestamp (ms from esp_timer_get_time/1000) of last successful poll.
    // Used by the UI to display a stale-data indicator.  0 = never polled.
    int64_t last_successful_poll_ms;

    // IMAGE-SAVE detailed stats (captured from WebSocket events)
    imagestats_detail_data_t last_image_stats;

    // Autofocus V-curve data (captured from WebSocket events)
    autofocus_data_t autofocus;

    // Local HFR ring buffer — populated by IMAGE-SAVE WebSocket events.
    // Used to serve HFR graph auto-refreshes without re-fetching /image-history?all=true.
    // Allocated in PSRAM by the data task (pointer set after allocation).
    struct {
        float *hfr;       // HFR values [HFR_RING_SIZE], PSRAM-allocated
        int   *stars;     // Star counts [HFR_RING_SIZE], PSRAM-allocated
        int    count;     // Total entries written (may exceed HFR_RING_SIZE)
        int    write_idx; // Next write position (wraps at HFR_RING_SIZE)
    } hfr_ring;

    // Mutex for synchronizing access between WebSocket event handler and data task.
    // Must be created with nina_client_init_mutex() before use.
    SemaphoreHandle_t mutex;
} nina_client_t;

/* Snapshot of one online rig's sky pointing, for consumers that draw where the
 * telescopes look (ADS-B page). fov_deg is the diagonal field of view when the
 * sensor and focal length are all known, else 1.0. */
typedef struct {
    bool    valid;
    float   alt_deg;
    float   az_deg;
    float   fov_deg;
    float   site_elev_m;
    uint8_t instance;
} nina_pointing_t;

/**
 * Copy the pointing of every CONNECTED instance that has a valid mount pointing
 * into @p out (at most @p max entries). Takes each client lock briefly; holds no
 * lock on return. Safe from any task; does not touch LVGL.
 * @return number of entries written.
 */
int nina_client_get_pointings(nina_pointing_t *out, int max);

// One-time module init (creates DNS cache mutex). Call from app_main() before tasks.
void nina_client_init(void);

// Initialize the mutex for a nina_client_t instance. Call once after struct init.
void nina_client_init_mutex(nina_client_t *client);

// Lock/unlock helpers with short timeouts suitable for real-time use.
// nina_client_lock() returns true if the lock was acquired.
bool nina_client_lock(nina_client_t *client, uint32_t timeout_ms);
void nina_client_unlock(nina_client_t *client);

// Current time in the NINA-PC clock domain (Unix epoch seconds).
// Returns nina_clock_epoch advanced by the device's monotonic esp_timer since
// capture, or falls back to (int64_t)time(NULL) while the pair is unknown.
// The pair is written under the client mutex; callers should hold it or
// tolerate a rare torn read (int64 on RV32) — lock-free UI paths use a cached
// pair instead (see dashboard_page_t.cached_nina_epoch).
int64_t nina_client_now_epoch(const nina_client_t *client);

// Polling intervals (ms)
#define NINA_POLL_SLOW_MS     30000   // Focuser, mount, switch
#define NINA_POLL_SEQUENCE_MS 15000   // Sequence counts (supplemented by event-driven sequence_poll_needed)
// Image-count change probe (WebSocket-down fallback only). Exposures are
// 30-600 s apart, so probing on every fast cycle just doubled the request rate
// exactly when the link was already degraded enough to drop the WebSocket.
#define NINA_POLL_IMAGE_COUNT_MS 8000
#define NINA_PROFILE_CHECK_MIN_MS 30000  // Floor between active-profile re-reads

// Polling state - tracks timers and cached static data between polls
typedef struct {
    // Timestamps (ms from esp_timer_get_time)
    int64_t last_slow_poll_ms;
    int64_t last_sequence_poll_ms;
    int64_t last_image_count_ms;   // Last /image-history count probe (WS-down path)
    int64_t last_profile_check_ms; // Last active-profile re-read (profile_check_needed)

    // Static data fetched once
    bool static_fetched;

    // Cached static data (survives across polls)
    char cached_profile[64];
    char cached_profile_id[64];
    char cached_telescope[64];
    nina_filter_t cached_filters[MAX_FILTERS];
    int cached_filter_count;

    // Persistent keep-alive slot for the shared HTTP fetcher (http_fetch_conn_t*,
    // see main/http_fetch.h). Opaque here to avoid a header dependency; owned
    // and destroyed via nina_poll_state_init()/http_fetch_conn_destroy().
    void *http_client;

    // Cached image count for change-detection gating of /image-history.
    // -1 = not yet fetched (forces initial full fetch).
    int cached_image_count;

    // Set true if /equipment/info returned 404 (old ninaAPI); disables bundled fetch
    bool bundle_not_available;
} nina_poll_state_t;

// Initialize polling state (call once before polling loop)
void nina_poll_state_init(nina_poll_state_t *state);

// Tiered polling - fetches data at different rates based on change frequency
void nina_client_poll(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance);

// Heartbeat-only polling for background (inactive) instances
// Only fetches camera info to maintain connection status
void nina_client_poll_heartbeat(const char *base_url, nina_client_t *data, int instance);

// Background polling for inactive instances — pre-fetches slow-changing data
// (profile, filters, focuser, mount, switch, sequence) so it's ready on page switch.
// Skips fast-changing data: guider RMS, HFR/stars, current filter position.
void nina_client_poll_background(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance);

// Fetch NINA's full profile list (profile/show without active=true) and drop
// remembered per-profile colours whose profile no longer exists. Skips the
// clean on a failed fetch, a non-array or empty list, or a list that does not
// contain the loaded profile. Call from the instance poll task.
void nina_client_prune_profiles(const char *base_url, int instance);

// DNS pre-check: resolve hostname from a NINA base URL.
// Returns true if hostname resolves (or is an IP address), false on DNS failure.
// Use before polling to avoid expensive HTTP client setup for unreachable hosts.
bool nina_client_dns_check(const char *base_url);

// Fetch prepared image as JPEG from NINA API
// The persistent PSRAM scratch buffer is allocated inside on first call.
// Returns heap-allocated JPEG bytes (caller must free), or NULL on error
// Uses: GET /prepared-image?resize=true&size=WxH&quality=Q&autoPrepare=true
uint8_t *nina_client_fetch_prepared_image(const char *base_url, int width, int height, int quality, size_t *out_size);
