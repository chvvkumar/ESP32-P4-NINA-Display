#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "display_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of NINA instances supported
#define MAX_NINA_INSTANCES 3

/* Auto-rotate slideshow stop indices (auto_rotate_order2[] entries).
 * Each value names a distinct slideshow stop. Indices 0-7 match the legacy
 * auto_rotate bitmask bit positions exactly; indices 8-11 split the former
 * single "Image Display" stop (old bit-index 8) into one stop per image source.
 * (The absolute page index each stop resolves to lives in
 * ui/nina_dashboard_internal.h and shifts whenever a page is added; it is
 * deliberately not repeated here.)
 *   0  ARP_IDX_SUMMARY    -> PAGE_IDX_SUMMARY
 *   1  ARP_IDX_NINA1      -> NINA_PAGE_OFFSET+0
 *   2  ARP_IDX_NINA2      -> NINA_PAGE_OFFSET+1
 *   3  ARP_IDX_NINA3      -> NINA_PAGE_OFFSET+2
 *   4  ARP_IDX_SYSINFO    -> SYSINFO_PAGE_IDX
 *   5  ARP_IDX_ALLSKY     -> PAGE_IDX_ALLSKY
 *   6  ARP_IDX_SPOTIFY    -> PAGE_IDX_SPOTIFY
 *   7  ARP_IDX_CLOCK      -> PAGE_IDX_CLOCK
 *   8  ARP_IDX_IMG_GOES   -> PAGE_IDX_IMG_GOES
 *   9  ARP_IDX_IMG_MOON   -> PAGE_IDX_IMG_MOON
 *  10  ARP_IDX_IMG_SOLAR  -> PAGE_IDX_IMG_SOLAR
 *  11  ARP_IDX_IMG_CUSTOM -> PAGE_IDX_IMG_CUSTOM
 *
 * Stop values are page_ref_t ids (ids 0..11 are frozen identical to the
 * ARP_IDX_* constants above). Pages added to the registry after the 0..11
 * anchored block keep their frozen registry id as their slideshow stop value,
 * so the range is no longer contiguous: ids 12..23 name non-slideshow entries
 * (image-default sentinel, Settings, overlays) and are NOT valid stops.
 *  24  ARP_IDX_JSON       -> PAGE_IDX_JSON      (== PAGE_REF_JSON)
 *  25  ARP_IDX_HA         -> PAGE_IDX_HA        (== PAGE_REF_HA)
 *  26  ARP_IDX_OCTOPRINT  -> PAGE_IDX_OCTOPRINT (== PAGE_REF_OCTOPRINT)
 *  27  ARP_IDX_RADAR      -> the Weather Radar page   (== PAGE_REF_RADAR)
 *  28  ARP_IDX_CLOUDS     -> the Clouds page          (== PAGE_REF_CLOUDS)
 *  29  ARP_IDX_ADSB       -> the ADS-B page           (== PAGE_REF_ADSB)
 * Use ARP_STOP_IS_VALID() for validation, never `< ARP_IDX_MAX` alone.
 */
#define ARP_IDX_SUMMARY     0
#define ARP_IDX_NINA1       1
#define ARP_IDX_NINA2       2
#define ARP_IDX_NINA3       3
#define ARP_IDX_SYSINFO     4
#define ARP_IDX_ALLSKY      5
#define ARP_IDX_SPOTIFY     6
#define ARP_IDX_CLOCK       7
#define ARP_IDX_IMG_GOES    8
#define ARP_IDX_IMG_MOON    9
#define ARP_IDX_IMG_SOLAR  10
#define ARP_IDX_IMG_CUSTOM 11
#define ARP_IDX_MAX        12   /* exclusive bound of the contiguous 0..11 block only */
#define ARP_IDX_JSON       24   /* == PAGE_REF_JSON (frozen registry id) */
#define ARP_IDX_HA         25   /* == PAGE_REF_HA (frozen registry id) */
#define ARP_IDX_OCTOPRINT  26   /* == PAGE_REF_OCTOPRINT (frozen registry id) */
#define ARP_IDX_RADAR      27   /* == PAGE_REF_RADAR (frozen registry id) */
#define ARP_IDX_CLOUDS     28   /* == PAGE_REF_CLOUDS (frozen registry id) */
#define ARP_IDX_ADSB       29   /* == PAGE_REF_ADSB (frozen registry id) */
/* True if @p b names a valid slideshow stop. */
#define ARP_STOP_IS_VALID(b) ((b) < ARP_IDX_MAX || (b) == ARP_IDX_JSON || (b) == ARP_IDX_HA || \
                              (b) == ARP_IDX_OCTOPRINT || (b) == ARP_IDX_RADAR || \
                              (b) == ARP_IDX_CLOUDS || (b) == ARP_IDX_ADSB)
#define ARP_ORDER_CAPACITY 24   /* size of the LIVE auto_rotate_order2[] (v63) */
/* Size of the RETIRED auto_rotate_order2_retired[] that still sits mid-struct at
 * its original offset. Used ONLY by the v63 migration lift in app_config.c.
 * v63 needed a bigger stop list. Growing the array in place would have moved
 * every field after it, which breaks two things at once: every bulk-memcpy
 * migration (they copy a stored blob as a prefix) and the forward-tolerant
 * downgrade path, which relies on app_config_t being strictly append-only.
 * So the old array was RETIRED in place (reserved bytes, nothing shifts) and a
 * new 24-entry array was appended at the end of the struct, the same
 * retire-and-append idiom already used for auto_rotate_order[8],
 * auto_rotate_pages and the image_display_* fields. */
#define ARP_ORDER_CAPACITY_RETIRED 16

// Current config struct version — bump on every layout change.
#define APP_CONFIG_VERSION 81

/* Tiles-config blobs no longer live inside app_config_t (v52 split them out to
 * dedicated NVS string keys "json_tiles"/"ha_tiles"). These bound the value
 * length; the web length-guards use them since they can no longer sizeof() a
 * removed struct field. Value length includes the terminating NUL. */
#define JSON_TILES_CONFIG_MAX 6144   /* max bytes (incl NUL) for the "json_tiles" NVS value */
#define HA_TILES_CONFIG_MAX   6144   /* max bytes (incl NUL) for the "ha_tiles"  NVS value */

#define WIDGET_STYLE_COUNT 13

/* alert_voice_types bitmask (v54). Bit set = that alert kind is spoken. */
#define ALERT_VOICE_TYPE_RMS    0x01
#define ALERT_VOICE_TYPE_HFR    0x02
#define ALERT_VOICE_TYPE_SAFETY 0x04
#define ALERT_VOICE_TYPE_ALL    (ALERT_VOICE_TYPE_RMS | ALERT_VOICE_TYPE_HFR | ALERT_VOICE_TYPE_SAFETY)

/* RETIRED v47: idle_page_override_target now stores a page_ref_t registry id
 * (see page_registry.h). Enum kept temporarily for migration reference. */
typedef enum {
    IDLE_TARGET_SUMMARY        = -1,
    IDLE_TARGET_CLOCK          =  0,
    IDLE_TARGET_ALLSKY         =  1,
    IDLE_TARGET_SPOTIFY        =  2,
    IDLE_TARGET_IMAGE_DISPLAY  =  3,
    IDLE_TARGET_SYSINFO        =  4,
    IDLE_TARGET_NINA1          =  5,
    IDLE_TARGET_NINA2          =  6,
    IDLE_TARGET_NINA3          =  7,
} idle_target_t;

typedef struct {
    char ssid[32];
    char password[64];
} wifi_network_t;

typedef struct {
    uint32_t config_version;        // Must be first field — used to detect legacy blobs
    char api_url[3][128];           // API base URLs per instance
    char ntp_server[64];
    char tz_string[64];             // POSIX TZ string (e.g. "EST5EDT,M3.2.0,M11.1.0")
    char filter_colors[3][512];     // JSON filter color map per instance: {"L":"#787878","R":"#991b1b",...}
    char rms_thresholds[3][256];    // JSON RMS threshold config per instance
    char hfr_thresholds[3][256];    // JSON HFR threshold config per instance
    int theme_index;                // Index of the selected theme
    int brightness;                 // Display brightness 0-100 (default 50)
    int color_brightness;           // Global color brightness for dynamic elements 0-100 (default 100)
    bool mqtt_enabled;              // Enable MQTT Home Assistant integration
    char mqtt_broker_url[128];      // MQTT broker URL (e.g. "mqtt://192.168.1.100")
    char mqtt_username[64];         // MQTT broker username
    char mqtt_password[64];         // MQTT broker password
    char mqtt_topic_prefix[64];     // MQTT topic prefix (default "ninadisplay")
    uint16_t mqtt_port;             // MQTT broker port (default 1883)
    int8_t   active_page_override;          // page_ref_t registry id (see ui/page_registry.h)
    bool     auto_rotate_enabled;            // enable automatic page rotation
    uint16_t auto_rotate_interval_s;        // seconds between automatic page rotations
    uint8_t  auto_rotate_effect;            // 0 = instant, 1 = fade, 2 = slide-left, 3 = slide-right
    bool     auto_rotate_skip_disconnected; // skip pages where NINA is not connected during auto-rotate
    uint8_t  auto_rotate_pages;            // rotation page bitmask, low byte (bits 0-7). High bits (8+) live in
                                           // auto_rotate_pages_hi at end of struct. Full mask layout:
                                           //   bit0=Summary, bit1-3=NINA 1-3, bit4=System Info, bit5=AllSky,
                                           //   bit6=Spotify, bit7=Clock, bit8=Image Display
                                           // RETIRED in v44 as the slideshow source — reconciled into the single ordered list; reserved for binary stability.
    uint8_t  update_rate_s;                // UI/data update interval in seconds (1-10, default 2)
    uint8_t  graph_update_interval_s;     // Graph overlay auto-refresh interval in seconds (2-30, default 5)
    uint8_t  connection_timeout_s;        // Seconds without successful poll before marking offline (2-30, default 6)
    uint8_t  toast_duration_s;           // Toast notification display duration in seconds (3-30, default 8)
    bool     debug_mode;                // Runtime debug/perf profiling toggle (default false)
    bool     instance_enabled[3];       // Per-instance enable flag (disabled = skip polling/WS)
    bool     screen_sleep_enabled;     // Turn off display when no NINA instances connected
    uint16_t screen_sleep_timeout_s;   // Seconds with 0 connections before screen off (default 60)
    bool     alert_flash_enabled;     // Enable border flash alerts for RMS/HFR/safety events (default true)
    uint8_t  idle_poll_interval_s;   // Heartbeat poll interval while screen sleeping (5-120, default 30)
    bool     wifi_power_save;        // Enable WiFi modem sleep for power savings (default false)
    uint8_t  widget_style;           // Widget panel style index (0-12, default 0)
    uint8_t  auto_update_check;     // 0=disabled, 1=enabled (check GitHub for firmware updates on boot)
    uint8_t  update_channel;        // 0=stable releases only, 1=include pre-releases

    // Deep sleep / power management
    bool     deep_sleep_enabled;        // Enable long-press BOOT button to enter deep sleep
    uint32_t deep_sleep_wake_timer_s;   // Timer wake duration in seconds (0 = no timer wake)
    bool     deep_sleep_on_idle;        // Auto-enter deep sleep after screen sleep timeout
    uint8_t  screen_rotation;           // Display rotation: 0=0°, 1=90°, 2=180°, 3=270°
    char     hostname[32];             // Device hostname for DHCP and MQTT HA (default "NINA-DISPLAY")

    // AllSky integration
    char     allsky_hostname[128];          // AllSky API host:port (e.g., "allskypi5.lan:8080")
    uint16_t allsky_update_interval_s;      // Poll interval 1-300s (default 5)
    float    allsky_dew_offset;             // °C above ambient for dew alert (default 5.0)
    char     allsky_field_config[1536];     // JSON key mappings per quadrant
    char     allsky_thresholds[1024];       // JSON threshold configs per field
    bool     allsky_enabled;                // Enable AllSky feature (page + poll task); default true
    bool     demo_mode;                    // Generate simulated astrophotography data (default false)

    // Spotify integration
    bool     spotify_enabled;              // Enable Spotify player page (default false)
    char     spotify_client_id[64];        // Spotify app client ID for PKCE auth
    uint16_t spotify_poll_interval_ms;     // Spotify API poll interval in ms (default 3000)
    bool     spotify_show_progress_bar;    // Show playback progress bar (default true)
    uint8_t  spotify_overlay_timeout_s;   // Seconds before overlay auto-hides (0 = never, default 5)

    // Added after v21 — must stay at end to preserve NVS binary compatibility
    bool     spotify_minimal_mode;        // Minimal mode: centered text, no controls (default false)
    bool     spotify_scroll_text;         // Scroll long text on Spotify overlay; false = wrap (default true)

    // Added after v23 — must stay at end to preserve NVS binary compatibility
    wifi_network_t wifi_networks[3];      // Priority-ordered WiFi networks ([0] = highest)

    // Added after v25 — must stay at end to preserve NVS binary compatibility
    bool     spotify_overlay_visible;   // Force minimal overlay visible from web UI (default false)

    // Added after v26 — must stay at end to preserve NVS binary compatibility
    uint8_t  auto_rotate_order[8];      // custom rotation order: array of bitmask-bit indices (0-7)
                                        // RETIRED in v44 as the slideshow source — reconciled into the single ordered list; reserved for binary stability.

    // Added after v27 — must stay at end to preserve NVS binary compatibility
    uint8_t  toast_aggregation_window_s;   // 0-15, default 5. 0 = disabled
    uint32_t toast_notify_mask;            // bitmask, default 0xFFFFFFFF (all on)
    bool     toast_instance_muted[3];      // per-instance mute, default all false

    // Weather
    uint8_t  weather_provider;          // 0=OWM, 1=Open-Meteo, 2=Wunderground
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;   // 900-3600, default 900
    uint8_t  weather_units;             // 0=imperial (°F), 1=metric (°C)
    uint8_t  weather_time_format;       // 0=12h, 1=24h

    // Idle page override
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target; // page_ref_t registry id (see ui/page_registry.h)
    bool     idle_page_persistent;      // Return to idle page after manual navigation
                                        // RETIRED in v44 — reserved; no longer read. Job moved to nav_grace_s.
    bool     idle_indicator_enabled;    // Show idle indicator on display (default true)

    // Added after v30 — must stay at end to preserve NVS binary compatibility
    char     admin_password[33];        // HTTP Basic auth password for web UI (default "changeme123!")

    // Added after v31 — must stay at end to preserve NVS binary compatibility
    bool     auth_enabled;              // When false, all endpoints are open; secrets still redacted (default true)

    // Added after v32 — must stay at end to preserve NVS binary compatibility
    // Image Display page. image_display_enabled / image_display_show_overlay are
    // RETIRED in v61 (image pages split): kept for binary layout, no longer read
    // by the firmware; the per-page goes_/moon_/solar_/custom_ fields appended
    // after v60 replace them.
    bool     image_display_enabled;          // RETIRED v61 (reserved)
    bool     image_display_show_overlay;     // RETIRED v61 (reserved)
    char     goes_region[16];                // NESDIS sector code, e.g. "umv" (default "umv")
    uint16_t goes_update_interval_s;         // GOES poll interval in seconds, 300-7200 (default 600)

    // Added after v33 — must stay at end to preserve NVS binary compatibility
    uint8_t  image_display_source;   // RETIRED v61 (reserved): read only by legacy migrations
    uint8_t  moon_bg_style;          // 0=black, 1=stars, 2=glow, 3=stars+glow
    float    moon_lat;               // observer latitude (deg), 0=unset
    float    moon_lon;               // observer longitude (deg), 0=unset

    // Added after v34 — must stay at end to preserve NVS binary compatibility
    uint8_t  solar_band;             // SDO/AIA band index 0..9 (default 0)

    // Added after v35 — must stay at end to preserve NVS binary compatibility
    bool     image_display_crop;     // RETIRED v61 (reserved); per-page goes_crop/solar_crop/custom_crop replace it

    // Added after v36 — must stay at end to preserve NVS binary compatibility
    uint8_t  moon_drag_light_mode;   // 0=true phase, 1=explore, 2=locked to surface (moon drag-to-rotate lighting)

    // Added after v37 — must stay at end to preserve NVS binary compatibility
    // Moon sphere orientation tuning
    uint8_t  moon_flip_u;            // 0/1, mirror texture longitude E<->W (default 0)
    uint8_t  moon_flip_v;            // 0/1, flip texture latitude N<->S (default 0)
    float    moon_roll_offset;       // degrees, clamp [-180,180] (default 0)
    float    moon_yaw_offset;        // degrees, clamp [-180,180] (default 0)
    float    moon_pitch_offset;      // degrees, clamp [-90,90] (default 0)

    // Added after v38 — must stay at end to preserve NVS binary compatibility
    uint8_t  moon_north_up;          // 0=true sky tilt, 1=always upright/north-up (default 1)

    // Added after v39 — must stay at end to preserve NVS binary compatibility
    // Moon touch-spin return behavior
    uint8_t  moon_spin_mode;         // 0=rubber band snap-back (default), 1=free spin (hold then return)
    uint8_t  moon_spin_return_s;     // free-spin hold before auto-return, clamp [3,60] (default 3)

    // Added after v40 — must stay at end to preserve NVS binary compatibility
    uint8_t  crash_log_retention_days; // Auto-purge crash records older than N days (0 = never, default 30)

    // Added after v41 — must stay at end to preserve NVS binary compatibility
    // Rotation bitmask high byte + 9th rotation-order slot. The in-place
    // auto_rotate_pages / auto_rotate_order[8] fields could not be widened
    // without shifting every trailing field's NVS offset, so the extra bits
    // are appended here instead. Effective mask = auto_rotate_pages |
    // (auto_rotate_pages_hi << 8); effective order has 9 slots where the last
    // is auto_rotate_order_ext.
    uint8_t  auto_rotate_pages_hi;     // rotation bitmask bits 8-15 (bit8=Image Display)
                                       // RETIRED in v44 as the slideshow source — reconciled into the single ordered list; reserved for binary stability.
    uint8_t  auto_rotate_order_ext;    // 9th rotation-order slot (0xFF = unused/terminator)
                                       // RETIRED in v44 as the slideshow source — reconciled into the single ordered list; reserved for binary stability.

    // Added after v42 — must stay at end to preserve NVS binary compatibility
    // Per-source Image Display render rotation: 0=0°,1=90°,2=180°,3=270° clockwise. GOES=source 0, Solar=source 2. Default 0.
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;

    // Added after v43 — must stay at end to preserve NVS binary compatibility
    uint16_t nav_grace_s;   // USER manual-nav grace window in seconds (10-300, default 10).
                            // Replaces idle_page_persistent: the grace window now
                            // performs the "honor the user's page" job in both modes.

    // Added after v44 — must stay at end to preserve NVS binary compatibility
    // Image Display "Custom Image URL" source (source index 3).
    char     custom_image_url[256];      // user-supplied JPEG image URL
    uint8_t  custom_orientation;         // render rotation: 0=0°,1=90°,2=180°,3=270° CW (mirrors solar_orientation)
    uint16_t custom_update_interval_s;   // poll interval in seconds (10-7200, default 60)

    // Added after v45 — must stay at end to preserve NVS binary compatibility
    // RETIRED v63: reserved bytes, no longer read outside the migration lift.
    // The live slideshow stop list is auto_rotate_order2[ARP_ORDER_CAPACITY] at
    // the END of this struct. This 16-byte array keeps its offset so that every
    // bulk-memcpy migration still lands a v46..v62 blob's stored stops here;
    // order2_lift_retired() (app_config.c) copies them to the live array once,
    // from the dispatcher tail. Never read this field anywhere else.
    uint8_t  auto_rotate_order2_retired[16];

    // Added after v47 — must stay at end to preserve NVS binary compatibility
    // Per-source Image Display mirror flips. 0/1 each; 0 = no flip (current
    // behavior). vflip mirrors top<->bottom, hflip mirrors left<->right.
    uint8_t  goes_vflip;     // GOES source: mirror top<->bottom (default 0)
    uint8_t  goes_hflip;     // GOES source: mirror left<->right (default 0)
    uint8_t  solar_vflip;    // Solar source: mirror top<->bottom (default 0)
    uint8_t  solar_hflip;    // Solar source: mirror left<->right (default 0)
    uint8_t  custom_vflip;   // Custom source: mirror top<->bottom (default 0)
    uint8_t  custom_hflip;   // Custom source: mirror left<->right (default 0)

    // Added after v48 — must stay at end to preserve NVS binary compatibility
    bool     home_page_lock; /* v49: hold the Home Page regardless of connection state */

    // Added after v49 — must stay at end to preserve NVS binary compatibility
    // JSON Display page
    bool     json_enabled;             // enable JSON Display page + poll task (default false)
    char     json_url[256];            // JSON source URL (http/https)
    char     json_auth_header[256];    // optional "Name: value" header ("" = none)
    uint16_t json_update_interval_s;   // poll interval 5-300s (default 30)
    /* json_tiles_config REMOVED in v52 — now NVS key "json_tiles"
     * (see app_config_get_json_tiles / app_config_set_json_tiles). */

    // Added after v50 — must stay at end to preserve NVS binary compatibility
    // Home Assistant page
    bool     ha_enabled;               // enable HA page + poll task (default false)
    char     ha_base_url[256];         // scheme+host+port, no path (http/https)
    char     ha_token[256];            // RAW long-lived token (device wraps as Bearer)
    uint16_t ha_update_interval_s;     // poll interval 5-300s (default 30)
    /* ha_tiles_config REMOVED in v52 — now NVS key "ha_tiles"
     * (see app_config_get_ha_tiles / app_config_set_ha_tiles). */

    // Added after v52 — must stay at end to preserve NVS binary compatibility
    bool     setup_hint_dismissed;     // user pressed "Don't show again" on the first-boot
                                       // setup-hint overlay; cleared only by factory reset
                                       // (default false)

    // Added after v53 — must stay at end to preserve NVS binary compatibility
    // Spoken voice alerts over the onboard speaker. Gated independently of
    // alert_flash_enabled: either, both, or neither may be enabled.
    uint8_t  alert_voice_enabled;      // master enable, 0 = off / 1 = on (default 1)
    uint8_t  alert_voice_volume;       // speaker volume 0-100 (default 90)
    uint8_t  alert_voice_types;        // ALERT_VOICE_TYPE_* bitmask (default 7 = all three)
    uint8_t  alert_voice_repeat_min;   // re-announce period in minutes while still
                                       // breached, 0 = announce once only (default 5)

    // Added after v54 — must stay at end to preserve NVS binary compatibility
    bool     alert_voice_muted[3];     // per-instance voice mute (default all false);
                                       // parallels toast_instance_muted, applied in
                                       // audio_alert_speak (test endpoints bypass it)

    // Added after v55 — must stay at end to preserve NVS binary compatibility
    uint32_t voice_notify_mask;        // bits 0-11: per-category voice alert mask, same
                                       // bits as toast_notify_mask.
                                       // bits 12-26 (v76): per-event spoken-phrase gates,
                                       // one per voice_event_t (bit = VOICE_EVENT_BIT_BASE
                                       // + event id, see main/audio_alert.h). Checked
                                       // ALONE, not ANDed with the category bit.
                                       // bits 27-31 spare. Default 0x7FFFFFF = all on.
    uint8_t  boot_jingle_enabled;      // play the startup jingle once at boot,
                                       // 0 = off / 1 = on (default 1)

    // Added after v56 — must stay at end to preserve NVS binary compatibility
    uint8_t  alert_voice_brief;        // brief spoken breach alerts, 0 = detailed
                                       // with value, 1 = brief (default 0)

    // Added after v57 — must stay at end to preserve NVS binary compatibility
    uint8_t  alert_voice_conn;         // announce NINA link connect, 0 = off / 1 = on
                                       // (default 1)
    uint8_t  alert_voice_disc;         // announce NINA link disconnect, 0 = off / 1 = on
                                       // (default 1)

    // Added after v58 — must stay at end to preserve NVS binary compatibility
    uint8_t  wifi_max_tx_dbm;          // WiFi TX power cap in dBm; 0 = no cap (chip
                                       // default/maximum). Only {0,8,11,14,17,20} are
                                       // accepted (default 0). Capping the C6 radio
                                       // stops the rail sag that glitches the panel.

    // Added after v59 — must stay at end to preserve NVS binary compatibility
    // OctoPrint 3D-printer page. Field order is FROZEN; append only.
    uint8_t  octoprint_enabled;        // enable the 3D Printer page + poll task (default 0)
    char     octoprint_url[128];       // OctoPrint base URL, scheme+host+port (http/https)
    char     octoprint_api_key[64];    // OctoPrint application/API key (secret)
    uint16_t octoprint_update_interval_s; // poll interval 2-300s (default 10)
    uint8_t  octoprint_image_source;   // 0 = G-code preview, 1 = webcam snapshot (default 0)
    uint8_t  octoprint_layout;         // 0=bento 2=glass 5=overlay 6=letterbox
                                       // (default 0); 1, 3 and 4 are retired,
                                       // render Bento, stay reserved
    char     octoprint_snapshot_url[128]; // explicit webcam snapshot URL; "" = derive
                                          // from octoprint_url (default "")

    // Added after v60 — must stay at end to preserve NVS binary compatibility
    // Image pages split (v61): one enable / overlay / crop / interval per page.
    // Field order is FROZEN; append only.
    bool     goes_enabled;             // GOES Satellite page (default false; migrated = image_display_enabled)
    bool     moon_enabled;             // Moon page (default false; migrated = image_display_enabled)
    bool     solar_enabled;            // Solar page (default false; migrated = image_display_enabled)
    bool     custom_enabled;           // Custom Image page (default false; migrated = image_display_enabled);
                                       // availability additionally needs custom_image_url non-empty
    uint16_t solar_update_interval_s;  // Solar poll interval 300-7200 s (default 600; migrated = goes_update_interval_s)
    uint16_t moon_update_interval_s;   // Moon re-render cadence 10-3600 s (default 60)
    bool     goes_crop;                // fill/crop borders (default false; migrated = image_display_crop)
    bool     solar_crop;               // (default false; migrated = image_display_crop)
    bool     custom_crop;              // (default false; migrated = false: Custom is never cropped today)
    bool     goes_show_overlay;        // caption overlay (default true; migrated = image_display_show_overlay)
    bool     moon_show_overlay;        // (default true; migrated = image_display_show_overlay)
    bool     solar_show_overlay;       // (default true; migrated = image_display_show_overlay)
    bool     custom_show_overlay;      // (default true; migrated = image_display_show_overlay)

    // Added after v61 — must stay at end to preserve NVS binary compatibility
    bool     octoprint_overlay_visible; // OctoPrint page: show the readings laid
                                        // over the picture (default true). This is
                                        // the saved default the page starts from;
                                        // a tap on the page toggles it for the
                                        // screen only and is never persisted.
                                        // Ignored by the Grid (bento) layout,
                                        // which has no overlay layer.

    // Added after v62 — must stay at end to preserve NVS binary compatibility
    // Weather Radar page. Field order is FROZEN; append only.
    bool     radar_enabled;            // enable the Weather Radar page + poll task (default false)
    char     radar_token[16];          // WSR-88D site id, uppercase [A-Z0-9] (e.g. "KTLX"), or
                                       // "CONUS". EMPTY is valid and means "resolve at fetch
                                       // time": the radar module picks the nearest WSR-88D site
                                       // from weather_lat/weather_lon, else CONUS. The resolved
                                       // value is NEVER written back here — writing config from
                                       // a poll task is forbidden in this codebase.
    uint16_t radar_update_interval_s;  // poll interval 120-7200 s (default 900)
    bool     radar_show_overlay;       // caption drawn over the radar image (default false)
    uint8_t  radar_crop;               // how the radar picture fits the panel. TWO states:
                                       // 0 = off (whole image, bars top and bottom), 1 = crop
                                       // (NOAA header/legend dropped, then a centred SQUARE crop
                                       // that fills 720x720 with no bars). The retired middle
                                       // value 2 ("fill screen", when 1 meant an 88% trim) can
                                       // still sit in NVS; it clamps to 1, which is the same
                                       // geometry it always asked for. See radar_play.h.
                                       // Widened from bool at v64+ WITHOUT a version bump: bool
                                       // and uint8_t are both one byte, so no offset moved, and
                                       // the two values already stored (0/1) keep their meaning.
                                       // The frozen app_config_v63_t snapshot below still
                                       // declares it as bool and must stay that way.
    uint8_t  radar_frames;             // how many radar images the page keeps and animates,
                                       // newest first (1-10, default 10). 1 = a still image,
                                       // no animation. NOAA publishes a new frame roughly
                                       // every 2 minutes and offers 10, which sets the
                                       // ceiling. Each retained frame costs about 660 KB of
                                       // PSRAM, so 10 frames is about 6.6 MB — the largest
                                       // single memory consumer this page adds.

    // The LIVE ordered slideshow stop list: ARP_IDX_* values, 0xFF = empty slot.
    // v63 replaced the 16-entry array that still sits mid-struct (now
    // auto_rotate_order2_retired) with this 24-entry one, appended here so no
    // existing field moved. THE NAME IS DELIBERATELY REUSED: it is the JSON wire
    // key exchanged with config_ui.html, so renaming it would break scripted API
    // clients, and every reader outside app_config.* keeps working unchanged as
    // long as it loops to ARP_ORDER_CAPACITY.
    //
    // 3am trap: in the frozen app_config_vNN_t snapshots below, the field named
    // auto_rotate_order2 is the OLD 16-entry array at the OLD offset. Same name,
    // different array. A migration that reads old->auto_rotate_order2 is reading
    // the retired one, which is correct for a snapshot and wrong for the live
    // struct.
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];

    // Added after v63 — must stay at end to preserve NVS binary compatibility
    bool     radar_dark_mode;          // true  = invert the greyscale basemap so
                                       //         the radar page reads dark (default)
                                       // false = leave the NWS image as published
                                       //         (light background)

    // Added after v64 — must stay at end to preserve NVS binary compatibility
    uint8_t  radar_map_style;          // which map the radar echoes are drawn over:
                                       // 0 = standard NWS picture with roads and city
                                       //     names (the pre-v65 behaviour)
                                       // 1 = state lines only (default)
                                       // 2 = state and county lines
                                       // On 1 and 2 the picture comes from a different
                                       // NWS map service: no banner or legend, black
                                       // background, so radar_crop and radar_dark_mode
                                       // do not apply. Values above 2 fall back to 1.

    // Added after v65 (Clouds page) — must stay at end to preserve NVS binary compatibility
    bool     clouds_enabled;           // show the Clouds satellite page (default false)
    bool     clouds_show_overlay;      // draw the label/time strip over the picture (default true)
    uint16_t clouds_update_interval_s; // 300-7200 s, default 900 (GIBS publishes every 10 min)
    uint8_t  clouds_frames;            // 1-10 animation frames, default 6 (~1 MB PSRAM each)
    uint8_t  clouds_zoom;              // 5-9 Web-Mercator zoom around weather_lat/lon, default 7

    // Added after v66 — must stay at end to preserve NVS binary compatibility
    char     custom_image_header[256]; // optional raw "Name: value" header sent with the
                                       // Custom Image fetch ("" = none). Same shape and
                                       // size as json_auth_header; a stored SECRET, so it is
                                       // redacted on GET /api/config and preserved on a
                                       // "********" POST (see web_handlers_config.c).
    uint8_t  clouds_channel;           // Clouds page satellite channel:
                                       // 0 = GeoColor (default), 1 = Clean Infrared (Band 13),
                                       // 2 = Air Mass. Values above 2 fall back to 0.

    // Added after v67 (ADS-B page) — must stay at end to preserve NVS binary compatibility
    bool     flights_enabled;          // show the ADS-B page (default false)
    char     flights_url[128];         // tar1090/readsb base URL, e.g. "http://pi.lan:8080"
                                       // (no trailing path; the poller appends
                                       // /data/aircraft.json). "" = unset. Not a secret.
    uint16_t flights_update_interval_s;// 2-60 s, default 3
    uint16_t flights_range_nm;         // 10-250 nm, default 50 (Radar Scope outer ring)
    uint8_t  flights_min_el;           // 0-89 deg elevation gate, default 10
    uint16_t flights_up_azimuth;       // 0-359 deg, default 0 (the azimuth drawn "up")
    uint8_t  flights_mode;             // 0 = Sky Dome (default), 1 = Radar Scope, 2 = Board.
                                       // Values above 2 fall back to 0.

    // Added after v68 (ADS-B route lookup) — must stay at end to preserve NVS binary compatibility
    bool     flights_route_lookup;     // default true: look up flight routes
                                       // (origin-destination) online for the ADS-B page

    // Added after v69 (ADS-B Radar Scope labels) — must stay at end to preserve NVS binary compatibility
    uint8_t  flights_label_max;        // how many Radar Scope contacts get a text label:
                                       // 0 = none, 1..63 = at most N, nearest first,
                                       // 64 (= ADSB_MAX_AC) = all drawn contacts. Default 64.

    // Added after v70 (ADS-B Radar Scope icon style) — must stay at end to preserve NVS binary compatibility
    uint8_t  flights_icon_style;       // Radar Scope contact glyph: 0 = classic arrows
                                       // (default), 1 = aircraft silhouettes. Values
                                       // above 1 fall back to 0.

    // Added after v71 (Clock page layout) — must stay at end to preserve NVS binary compatibility
    uint8_t  clock_layout;             // Clock page layout: 0 = Classic (default),
                                       // 1 = Console 92, 2 = Broadside, 3 = Evensong,
                                       // 4 = Blueprint, 5 = Transit Line,
                                       // 6 = Night Network. Values above 6 fall
                                       // back to 0.

    // Added after v72 (Cloud Cover map overlay) — must stay at end to preserve NVS binary compatibility
    uint8_t  clouds_basemap;           // Cloud Cover page map overlay:
                                       // 0 = Borders and roads (default),
                                       // 1 = Coastlines only,
                                       // 2 = Borders, roads and grid, 3 = None.
                                       // Values above 3 fall back to 0.

    // Added after v73 (Cloud Cover location marker) — must stay at end to preserve NVS binary compatibility
    bool     clouds_show_location;     // Cloud Cover page: draw a small marker
                                       // at the user's location (weather_lat /
                                       // weather_lon), which is the frame
                                       // centre. Default true.

    // Added after v74 (per-instance NINA page layout) — must stay at end to preserve NVS binary compatibility
    uint8_t  nina_layout[MAX_NINA_INSTANCES];  // per-instance dashboard page layout:
                                       // 0 = Dashboard (arc, default),
                                       // 1 = Image-forward (square panels),
                                       // 2 = Halo, 4 = Orbit (round panels).
                                       // 3 is RETIRED and never reused.
                                       // A binary that cannot draw an id shows
                                       // the Dashboard without rewriting the
                                       // stored value, so a panel swap restores
                                       // the choice. 3 and above 4 reset to 0.

    // Added after v76 (global audio mute) — must stay at end to preserve NVS binary compatibility
    bool     audio_muted;              // v77: silence ALL sound (voice alerts,
                                       // event phrases, connection announcements,
                                       // boot jingle) at the audio_alert enqueue
                                       // gate. The web test/preview endpoints
                                       // bypass it so the speaker stays
                                       // testable while muted. Default false.

    // Added after v77 (anonymous telemetry) -- must stay at end to preserve NVS binary compatibility
    bool     telemetry_enabled;        // v78: send one anonymous health report a
                                       // day (firmware version, uptime, crash
                                       // counters, memory, feature bitmask;
                                       // never URLs, names, coordinates or
                                       // secrets). Fresh installs default true;
                                       // the v78 migration forces false for
                                       // every upgrader (opt in).

    // Added after v78 (round Moon disc size) -- must stay at end to preserve NVS binary compatibility
    uint8_t  moon_round_size_pct;      // v79: round panels only. How much of the
                                       // rim diameter the Moon disc fills when
                                       // the page text is hidden, 50..150
                                       // (default 100). With the text shown the
                                       // disc is capped so it clears the rim
                                       // rows. Square panels ignore it.

    // Added after v79 (NINA notification scope) -- must stay at end to preserve NVS binary compatibility
    uint8_t  nina_notify_scope;        // v80: which rigs may raise toasts,
                                       // border flashes and spoken alerts.
                                       // 0 = every online rig (default),
                                       // 1 = only the rig whose page is on
                                       // screen (Summary counts as every rig;
                                       // any other page fires none).

    // Added after v80 (worldwide radar) -- must stay at end to preserve NVS binary compatibility
    uint8_t  radar_zoom;               // v81: Web-Mercator zoom of the RainViewer
                                       // window, 4..7 (default 6, about 650 km
                                       // across at mid latitudes). Used only by
                                       // the worldwide radar source.
    uint8_t  radar_palette;            // v81: RainViewer colour scheme id:
                                       // 1 Original, 2 Universal Blue,
                                       // 4 The Weather Channel,
                                       // 6 NEXRAD Level III (default),
                                       // 8 Dark Sky. Any other value resets to 6.
} app_config_t;

/* ── Version 43 config struct — used only for NVS migration to v44 ────── */
/* Byte-identical to app_config_t minus the trailing nav_grace_s field.   */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
} app_config_v43_t;

_Static_assert(offsetof(app_config_t, nav_grace_s) == sizeof(app_config_v43_t),
               "app_config_v43_t snapshot drifted from app_config_t layout");

/* ── Version 44 config struct — used only for NVS migration to v45 ────── */
/* Byte-identical to app_config_t minus the trailing custom_* fields.     */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
} app_config_v44_t;

/* custom_image_url (char[], align 1) appends directly after nav_grace_s,
 * reclaiming app_config_v44_t's 2-byte tail padding, so offsetof != sizeof here.
 * Check against the end of the last v44 field instead: this still catches any
 * field inserted/reordered ahead of the new block, without tripping on padding. */
_Static_assert(offsetof(app_config_t, custom_image_url) ==
                   offsetof(app_config_v44_t, nav_grace_s) +
                       sizeof(((app_config_v44_t *)0)->nav_grace_s),
               "app_config_v44_t snapshot drifted from app_config_t layout");

/* ── Version 45 config struct — used only for NVS migration to v46 ────── */
/* Byte-identical to app_config_t minus the trailing auto_rotate_order2[].  */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
} app_config_v45_t;

/* auto_rotate_order2 (uint8[16], align 1) appends directly after the uint16
 * custom_update_interval_s with no padding, so offsetof == end-of-last-field.
 * Same technique as the v44 custom_image_url assert: compare against the end
 * of the last v45 field to catch any field inserted/reordered ahead of the
 * new block. */
_Static_assert(offsetof(app_config_t, auto_rotate_order2_retired) ==
                   offsetof(app_config_v45_t, custom_update_interval_s) +
                       sizeof(((app_config_v45_t *)0)->custom_update_interval_s),
               "app_config_v45_t snapshot drifted from app_config_t layout");

/* ── Version 46 config struct — used only for NVS migration to v47 ────── */
/* Layout-identical to app_config_t. v47 changes no field layout — it only    */
/* remaps the stored VALUES of active_page_override and                       */
/* idle_page_override_target onto page_registry.h ids — so this snapshot is a  */
/* verbatim copy of the current struct body.                                  */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
} app_config_v46_t;

/* v46_t's assert lives just below the v47_t definition (it compares against
 * sizeof(app_config_v47_t), which must be a complete type first). */

/* ── Version 47 config struct — used only for NVS migration to v48 ────── */
/* Byte-identical to app_config_t minus the trailing per-source flip bytes  */
/* (goes/solar/custom v/hflip). Verbatim copy of the v47 struct body.       */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
} app_config_v47_t;

/* The 6 new uint8 flips (align 1) append directly after auto_rotate_order2[16]
 * (also align 1) with no padding, so offsetof == end-of-last-v47-field. Compare
 * against the end of auto_rotate_order2 to catch any field inserted/reordered
 * ahead of the new block without tripping on tail padding. */
_Static_assert(offsetof(app_config_t, goes_vflip) ==
                   offsetof(app_config_v47_t, auto_rotate_order2) +
                       sizeof(((app_config_v47_t *)0)->auto_rotate_order2),
               "app_config_v47_t snapshot drifted from app_config_t layout");

/* v46_t and v47_t have byte-identical bodies (v48 only appends 6 trailing flip
 * bytes past the v47 layout), so v46 stays valid by matching v47_t's size
 * rather than app_config_t's — avoids any tail-padding ambiguity from the
 * appended uint8 block. */
_Static_assert(sizeof(app_config_v46_t) == sizeof(app_config_v47_t),
               "app_config_v46_t snapshot drifted from app_config_t layout");

/* ── Version 48 config struct — used only for NVS migration to v49 ────── */
/* Byte-identical to app_config_t minus the trailing home_page_lock flag.   */
/* Verbatim copy of the v48 struct body (v47 body + the 6 flip bytes).      */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
} app_config_v48_t;

/* home_page_lock (bool, align 1) appends directly after custom_hflip (uint8,
 * align 1) with no padding, so offsetof == end-of-last-v48-field. Compare
 * against the end of custom_hflip to catch any field inserted/reordered ahead
 * of the new flag without tripping on tail padding. */
_Static_assert(offsetof(app_config_t, home_page_lock) ==
                   offsetof(app_config_v48_t, custom_hflip) +
                       sizeof(((app_config_v48_t *)0)->custom_hflip),
               "app_config_v48_t snapshot drifted from app_config_t layout");

/* ── Version 49 config struct — used only for NVS migration to v50 ────── */
/* Byte-identical to app_config_t minus the trailing JSON Display fields.   */
/* Verbatim copy of the v48 struct body PLUS the trailing home_page_lock.   */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
} app_config_v49_t;

/* json_enabled (bool, align 1) appends directly after home_page_lock (bool,
 * align 1) with no padding; compare against end of home_page_lock. */
_Static_assert(offsetof(app_config_t, json_enabled) ==
                   offsetof(app_config_v49_t, home_page_lock) +
                       sizeof(((app_config_v49_t *)0)->home_page_lock),
               "app_config_v49_t snapshot drifted from app_config_t layout");

/* ── Version 50 config struct — used only for NVS migration to v51 ────── */
/* Byte-identical to app_config_t minus the trailing Home Assistant fields. */
/* Verbatim copy of the v49 struct body PLUS the trailing JSON Display     */
/* fields (json_enabled/json_url/json_auth_header/json_update_interval_s/   */
/* json_tiles_config).                                                      */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    char     json_tiles_config[6144];
} app_config_v50_t;

/* v52 split json_tiles_config out of app_config_t. The v50 blob still carries
 * it inline (as its last field), immediately after json_update_interval_s. The
 * shared prefix [config_version .. json_update_interval_s] is byte-identical
 * between the old v50 blob and the new v52 struct; migrate_from_v50 copies that
 * prefix, so the new struct's ha_enabled (first field past the prefix) must land
 * exactly where the v50 blob's json_tiles_config began. */
_Static_assert(offsetof(app_config_t, ha_enabled) ==
                   offsetof(app_config_v50_t, json_tiles_config),
               "v50 prefix drifted: config split boundary moved");

/* ── Version 51 config struct — used only for NVS migration to v52 ────── */
/* Verbatim copy of the OLD (pre-split) v51 app_config_t body, with BOTH 6144   */
/* tiles blobs still inline. Used by migrate_from_v51 to lift the inline tiles  */
/* out to the "json_tiles"/"ha_tiles" NVS keys. */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    char     json_tiles_config[6144];   /* KEPT here — this is the OLD layout */
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    char     ha_tiles_config[6144];     /* KEPT here — this is the OLD layout */
} app_config_v51_t;

/* The migration memcpy's the shared prefix [0 .. json_update_interval_s]. That
 * prefix is byte-identical between the old v51 blob and the new v52 struct, so
 * assert the boundary: the new struct's ha_enabled sits exactly where the old
 * blob's json_tiles_config began. Catches any accidental prefix reorder. */
_Static_assert(offsetof(app_config_t, ha_enabled) ==
                   offsetof(app_config_v51_t, json_tiles_config),
               "v51 prefix drifted: config split boundary moved");

/* ── Version 52 config struct — used only for NVS migration to v53 ────── */
/* Byte-identical to app_config_t minus the trailing setup_hint_dismissed   */
/* field. Post-split layout: both tiles blobs already live in their own NVS */
/* keys ("json_tiles"/"ha_tiles"), so they are absent here too. */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
} app_config_v52_t;

/* setup_hint_dismissed (bool, align 1) appends directly after
 * ha_update_interval_s (uint16, align 2) with no padding, so offsetof ==
 * end-of-last-v52-field. Compare against the end of ha_update_interval_s to
 * catch any field inserted/reordered ahead of the new flag without tripping on
 * tail padding. */
_Static_assert(offsetof(app_config_t, setup_hint_dismissed) ==
                   offsetof(app_config_v52_t, ha_update_interval_s) +
                       sizeof(((app_config_v52_t *)0)->ha_update_interval_s),
               "app_config_v52_t snapshot drifted from app_config_t layout");

/* migrate_from_v52 memcpy's sizeof(app_config_v52_t) bytes into app_config_t.
 * Tail padding can make the two sizes equal (the new bool may land inside the
 * v52 struct's trailing pad), which is why the migration re-assigns the flag
 * explicitly after the copy; it must never exceed the destination. */
_Static_assert(sizeof(app_config_v52_t) <= sizeof(app_config_t),
               "v52 snapshot must not exceed the current config struct");

/* ── Version 53 config struct — used only for NVS migration to v54 ────── */
/* Byte-identical to app_config_t minus the four trailing alert_voice_*      */
/* fields. Same post-split layout as v52 (both tiles blobs live in their own */
/* NVS keys), plus the setup_hint_dismissed flag v53 appended.               */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
} app_config_v53_t;

/* alert_voice_enabled (uint8, align 1) appends directly after
 * setup_hint_dismissed (bool, align 1) with no padding, so offsetof ==
 * end-of-last-v53-field. Compare against the end of setup_hint_dismissed to
 * catch any field inserted/reordered ahead of the new block without tripping
 * on tail padding. */
_Static_assert(offsetof(app_config_t, alert_voice_enabled) ==
                   offsetof(app_config_v53_t, setup_hint_dismissed) +
                       sizeof(((app_config_v53_t *)0)->setup_hint_dismissed),
               "app_config_v53_t snapshot drifted from app_config_t layout");

/* migrate_from_v53 memcpy's sizeof(app_config_v53_t) bytes into app_config_t.
 * Tail padding can make the two sizes equal (the new uint8 block may land
 * inside the v53 struct's trailing pad), which is why the migration re-assigns
 * the four fields explicitly after the copy; it must never exceed the dest. */
_Static_assert(sizeof(app_config_v53_t) <= sizeof(app_config_t),
               "v53 snapshot must not exceed the current config struct");

/* ── Version 54 config struct — used only for NVS migration to v55 ────── */
/* Byte-identical to app_config_t minus the trailing alert_voice_muted[3].   */
/* Same layout as the v53 snapshot plus the four alert_voice_* scalars v54   */
/* appended.                                                                 */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
} app_config_v54_t;

/* alert_voice_muted (bool[3], align 1) appends directly after
 * alert_voice_repeat_min (uint8, align 1) with no padding, so offsetof ==
 * end-of-last-v54-field. Compare against the end of alert_voice_repeat_min to
 * catch any field inserted/reordered ahead of the new block without tripping
 * on tail padding. */
_Static_assert(offsetof(app_config_t, alert_voice_muted) ==
                   offsetof(app_config_v54_t, alert_voice_repeat_min) +
                       sizeof(((app_config_v54_t *)0)->alert_voice_repeat_min),
               "app_config_v54_t snapshot drifted from app_config_t layout");

/* migrate_from_v54 memcpy's sizeof(app_config_v54_t) bytes into app_config_t.
 * Tail padding can make the two sizes equal (the new bool[3] may land inside
 * the v54 struct's trailing pad), which is why the migration re-assigns the
 * mute array explicitly after the copy; it must never exceed the dest. */
_Static_assert(sizeof(app_config_v54_t) <= sizeof(app_config_t),
               "v54 snapshot must not exceed the current config struct");

/* ── Version 55 config struct — used only for NVS migration to v56 ────── */
/* Byte-identical to app_config_t minus the trailing voice_notify_mask and   */
/* boot_jingle_enabled fields v56 appended.                                  */
/* Same layout as the v54 snapshot plus the alert_voice_muted[3] array v55   */
/* appended.                                                                 */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
} app_config_v55_t;

/* voice_notify_mask (uint32, align 4) appends after alert_voice_muted
 * (bool[3], align 1), so the compiler may insert alignment padding between
 * them; an exact "offsetof == end-of-last-v55-field" equality (the pattern the
 * v53/v54 asserts use) would be unreliable here. Assert >= instead: it still
 * catches any field inserted/reordered ahead of the new block. */
_Static_assert(offsetof(app_config_t, voice_notify_mask) >=
                   offsetof(app_config_v55_t, alert_voice_muted) +
                       sizeof(((app_config_v55_t *)0)->alert_voice_muted),
               "app_config_v55_t snapshot drifted from app_config_t layout");

/* migrate_from_v55 memcpy's sizeof(app_config_v55_t) bytes into app_config_t.
 * Tail padding can make the copy land inside the dest's padding before
 * voice_notify_mask, which is why the migration re-assigns the new fields
 * (voice_notify_mask, boot_jingle_enabled) explicitly after the copy; it
 * must never exceed the dest. */
_Static_assert(sizeof(app_config_v55_t) <= sizeof(app_config_t),
               "v55 snapshot must not exceed the current config struct");

/* ── Version 56 config struct — used only for NVS migration to v57 ────── */
/* Byte-identical to app_config_t minus the trailing alert_voice_brief      */
/* field v57 appended.                                                      */
/* Same layout as the v55 snapshot plus the voice_notify_mask and           */
/* boot_jingle_enabled fields v56 appended.                                 */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
} app_config_v56_t;

/* alert_voice_brief (uint8, align 1) appends directly after boot_jingle_enabled
 * (uint8, align 1), so no alignment padding can sit between them and the exact
 * equality the v53/v54 asserts use is reliable here. */
_Static_assert(offsetof(app_config_t, alert_voice_brief) ==
                   offsetof(app_config_v56_t, boot_jingle_enabled) +
                       sizeof(((app_config_v56_t *)0)->boot_jingle_enabled),
               "app_config_v56_t snapshot drifted from app_config_t layout");

/* migrate_from_v56 memcpy's sizeof(app_config_v56_t) bytes into app_config_t.
 * Tail padding can make the two sizes equal (the new uint8 may land inside
 * the v56 struct's trailing pad), which is why the migration re-assigns
 * alert_voice_brief explicitly after the copy; it must never exceed the
 * dest. */
_Static_assert(sizeof(app_config_v56_t) <= sizeof(app_config_t),
               "v56 snapshot must not exceed the current config struct");

/* ── Version 57 config struct — used only for NVS migration to v58 ────── */
/* Byte-identical to app_config_t minus the trailing alert_voice_conn and   */
/* alert_voice_disc fields v58 appended.                                    */
/* Same layout as the v56 snapshot plus the alert_voice_brief field v57     */
/* appended.                                                                */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
} app_config_v57_t;

/* alert_voice_conn (uint8, align 1) appends directly after alert_voice_brief
 * (uint8, align 1), so no alignment padding can sit between them and the exact
 * equality the v53/v54 asserts use is reliable here. */
_Static_assert(offsetof(app_config_t, alert_voice_conn) ==
                   offsetof(app_config_v57_t, alert_voice_brief) +
                       sizeof(((app_config_v57_t *)0)->alert_voice_brief),
               "app_config_v57_t snapshot drifted from app_config_t layout");

/* migrate_from_v57 memcpy's sizeof(app_config_v57_t) bytes into app_config_t.
 * Tail padding can make the two sizes equal (the new uint8s may land inside
 * the v57 struct's trailing pad), which is why the migration re-assigns
 * alert_voice_conn and alert_voice_disc explicitly after the copy; it must
 * never exceed the dest. */
_Static_assert(sizeof(app_config_v57_t) <= sizeof(app_config_t),
               "v57 snapshot must not exceed the current config struct");

/* ── Version 58 config struct — used only for NVS migration to v59 ────── */
/* Byte-identical to app_config_t minus the trailing wifi_max_tx_dbm field  */
/* v59 appended.                                                            */
/* Same layout as the v57 snapshot plus the alert_voice_conn and            */
/* alert_voice_disc fields v58 appended.                                    */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
} app_config_v58_t;

/* wifi_max_tx_dbm (uint8, align 1) appends directly after alert_voice_disc
 * (uint8, align 1), so no alignment padding can sit between them and the exact
 * equality the v53/v54 asserts use is reliable here. */
_Static_assert(offsetof(app_config_t, wifi_max_tx_dbm) ==
                   offsetof(app_config_v58_t, alert_voice_disc) +
                       sizeof(((app_config_v58_t *)0)->alert_voice_disc),
               "app_config_v58_t snapshot drifted from app_config_t layout");

/* migrate_from_v58 memcpy's sizeof(app_config_v58_t) bytes into app_config_t.
 * Tail padding can make the two sizes equal (the new uint8 may land inside the
 * v58 struct's trailing pad), which is why the migration re-assigns
 * wifi_max_tx_dbm explicitly after the copy; it must never exceed the dest. */
_Static_assert(sizeof(app_config_v58_t) <= sizeof(app_config_t),
               "v58 snapshot must not exceed the current config struct");

/* ── Version 59 config struct — used only for NVS migration to v60 ────── */
/* Byte-identical to app_config_t minus the trailing OctoPrint block v60    */
/* appended. Same layout as the v58 snapshot plus the wifi_max_tx_dbm field */
/* v59 appended.                                                            */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
} app_config_v59_t;

/* octoprint_enabled (uint8, align 1) appends directly after wifi_max_tx_dbm
 * (uint8, align 1), so no alignment padding can sit between them and the exact
 * equality the v53/v54/v58 asserts use is reliable here. (The uint16
 * octoprint_update_interval_s further down MAY pick up one pad byte inside
 * app_config_t; that is internal to the new block and does not affect this
 * boundary.) v59's own trailing padding is still copied, so migrate_from_v59
 * re-asserts the first two fields it can reach; the rest keep the values
 * set_defaults() applied from SETTINGS_TABLE. */
_Static_assert(offsetof(app_config_t, octoprint_enabled) ==
                   offsetof(app_config_v59_t, wifi_max_tx_dbm) +
                       sizeof(((app_config_v59_t *)0)->wifi_max_tx_dbm),
               "app_config_v59_t snapshot drifted from app_config_t layout");

/* migrate_from_v59 memcpy's sizeof(app_config_v59_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v59_t) <= sizeof(app_config_t),
               "v59 snapshot must not exceed the current config struct");

/* ── Version 60 config struct — used only for NVS migration to v61 ────── */
/* Byte-identical to app_config_t minus the trailing image-pages block v61  */
/* appended. Same layout as the v59 snapshot plus the seven OctoPrint       */
/* fields v60 appended.                                                     */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
} app_config_v60_t;

/* goes_enabled (bool, align 1) appends directly after octoprint_snapshot_url
 * (char[], align 1), so no alignment padding can sit between them. */
_Static_assert(offsetof(app_config_t, goes_enabled) ==
                   offsetof(app_config_v60_t, octoprint_snapshot_url) +
                       sizeof(((app_config_v60_t *)0)->octoprint_snapshot_url),
               "app_config_v60_t snapshot drifted from app_config_t layout");

/* migrate_from_v60 memcpy's sizeof(app_config_v60_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v60_t) <= sizeof(app_config_t),
               "v60 snapshot must not exceed the current config struct");

/* ── Version 61 config struct — used only for NVS migration to v62 ────── */
/* Byte-identical to app_config_t minus the trailing octoprint_overlay_visible */
/* field v62 appended. Same layout as the v60 snapshot plus the thirteen      */
/* image-page fields v61 appended.                                           */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
} app_config_v61_t;

/* octoprint_overlay_visible (bool, align 1) appends directly after
 * custom_show_overlay (bool, align 1), so no alignment padding can sit
 * between them. */
_Static_assert(offsetof(app_config_t, octoprint_overlay_visible) ==
                   offsetof(app_config_v61_t, custom_show_overlay) +
                       sizeof(((app_config_v61_t *)0)->custom_show_overlay),
               "app_config_v61_t snapshot drifted from app_config_t layout");

/* migrate_from_v61 memcpy's sizeof(app_config_v61_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v61_t) <= sizeof(app_config_t),
               "v61 snapshot must not exceed the current config struct");

/* ── Version 62 config struct — used only for NVS migration to v63 ────── */
/* Byte-identical to app_config_t minus the trailing Weather Radar block and    */
/* the appended 24-entry auto_rotate_order2[] v63 added. Same body as the v61   */
/* snapshot plus the octoprint_overlay_visible flag v62 appended.               */
/* NOTE: this snapshot's auto_rotate_order2[16] is the array the live struct    */
/* now calls auto_rotate_order2_retired — same offset, same 16 bytes, different */
/* name. The live struct's auto_rotate_order2[] is a DIFFERENT, larger array at */
/* the end of the struct. That asymmetry is deliberate: it keeps the JSON wire  */
/* key stable while letting the stop list grow without moving a single field.   */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2[16];   /* the RETIRED array — see the note
                                        * on the live struct's field of the
                                        * same name */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
} app_config_v62_t;

/* radar_enabled (bool, align 1) appends directly after octoprint_overlay_visible
 * (bool, align 1), so no alignment padding can sit between them. Nothing moved
 * in v63 — the old array was retired in place — so this is the plain
 * end-of-last-field equality the v53/v54 asserts use. */
_Static_assert(offsetof(app_config_t, radar_enabled) ==
                   offsetof(app_config_v62_t, octoprint_overlay_visible) +
                       sizeof(((app_config_v62_t *)0)->octoprint_overlay_visible),
               "app_config_v62_t snapshot drifted from app_config_t layout");

/* migrate_from_v62 memcpy's sizeof(app_config_v62_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v62_t) <= sizeof(app_config_t),
               "v62 snapshot must not exceed the current config struct");

/* ── Version 63 config struct — used only for NVS migration to v64 ────── */
/* Byte-identical to app_config_t minus the trailing radar_dark_mode flag v64  */
/* appended. Same body as the v62 snapshot plus the Weather Radar block and    */
/* the 24-entry auto_rotate_order2[] v63 appended.                             */
/* Both slideshow arrays are present here, under the same names the live       */
/* struct uses: the retired 16-entry auto_rotate_order2_retired[] mid-struct   */
/* and the live 24-entry auto_rotate_order2[] at the end.                      */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    bool     radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
} app_config_v63_t;

/* radar_dark_mode (bool, align 1) appends directly after auto_rotate_order2[]
 * (uint8[24], align 1), so no alignment padding can sit between them. Nothing
 * moved in v64 — the field is a pure append — so this is the plain
 * end-of-last-field equality the v53/v54/v62 asserts use. */
_Static_assert(offsetof(app_config_t, radar_dark_mode) ==
                   offsetof(app_config_v63_t, auto_rotate_order2) +
                       sizeof(((app_config_v63_t *)0)->auto_rotate_order2),
               "app_config_v63_t snapshot drifted from app_config_t layout");

/* radar_crop widened bool -> uint8_t (a fit mode, not a flag) with NO version bump.
 * These two are the proof that costs nothing: the field is still one byte, and
 * it still sits at the same offset as in the frozen v63 snapshot that declares
 * it `bool`. Since it is one byte wide at an unchanged offset, nothing after it
 * moved either — which the radar_dark_mode assert directly above re-checks. A
 * stored blob therefore needs no migration: 0 and 1 already meant off and crop. */
_Static_assert(sizeof(((app_config_t *)0)->radar_crop) == 1,
               "radar_crop must stay one byte or the NVS blob layout shifts");
_Static_assert(offsetof(app_config_t, radar_crop) == offsetof(app_config_v63_t, radar_crop),
               "radar_crop moved: widening bool -> uint8_t must not shift the layout");

/* migrate_from_v63 memcpy's sizeof(app_config_v63_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v63_t) <= sizeof(app_config_t),
               "v63 snapshot must not exceed the current config struct");

/* ── Version 64 config struct — used only for NVS migration to v65 ────── */
/* Byte-identical to app_config_t minus the trailing radar_map_style byte v65 */
/* appended. Same body as the v63 snapshot plus radar_dark_mode v64 at the    */
/* end. radar_crop is declared uint8_t here, as the live struct has had it    */
/* since the v64 widening (one byte either way, same offset).                 */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
} app_config_v64_t;

/* radar_map_style (uint8_t, align 1) appends directly after radar_dark_mode
 * (bool, align 1), so no alignment padding can sit between them. Nothing
 * moved in v65 — the field is a pure append — so this is the plain
 * end-of-last-field equality the v53/v54/v62/v63 asserts use. */
_Static_assert(offsetof(app_config_t, radar_map_style) ==
                   offsetof(app_config_v64_t, radar_dark_mode) +
                       sizeof(((app_config_v64_t *)0)->radar_dark_mode),
               "app_config_v64_t snapshot drifted from app_config_t layout");

/* migrate_from_v64 memcpy's sizeof(app_config_v64_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v64_t) <= sizeof(app_config_t),
               "v64 snapshot must not exceed the current config struct");

/* ── Version 65 config struct — used only for NVS migration to v66 ────── */
/* Byte-identical to app_config_t minus the five trailing clouds_* fields v66 */
/* appended. Same body as the v64 snapshot plus radar_map_style v65 at the    */
/* end.                                                                       */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
} app_config_v65_t;

/* clouds_enabled (bool, align 1) appends directly after radar_map_style
 * (uint8_t, align 1), so no alignment padding can sit between them. Nothing
 * moved in v66 — the fields are a pure append — so this is the plain
 * end-of-last-field equality the v62/v63/v64 asserts use. */
_Static_assert(offsetof(app_config_t, clouds_enabled) ==
                   offsetof(app_config_v65_t, radar_map_style) +
                       sizeof(((app_config_v65_t *)0)->radar_map_style),
               "app_config_v65_t snapshot drifted from app_config_t layout");

/* migrate_from_v65 memcpy's sizeof(app_config_v65_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v65_t) <= sizeof(app_config_t),
               "v65 snapshot must not exceed the current config struct");

/* ── Version 66 config struct — used only for NVS migration to v67 ────── */
/* Byte-identical to app_config_t minus the two trailing fields v67       */
/* appended (custom_image_header, clouds_channel). Same body as the v65   */
/* snapshot plus the five clouds_* fields v66 added at the end.           */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
} app_config_v66_t;

/* custom_image_header (char[], align 1) appends directly after clouds_zoom
 * (uint8_t, align 1), so no alignment padding can sit between them. Nothing
 * moved in v67 — the fields are a pure append — so this is the plain
 * end-of-last-field equality the v62/v63/v64/v65 asserts use. */
_Static_assert(offsetof(app_config_t, custom_image_header) ==
                   offsetof(app_config_v66_t, clouds_zoom) +
                       sizeof(((app_config_v66_t *)0)->clouds_zoom),
               "app_config_v66_t snapshot drifted from app_config_t layout");

/* migrate_from_v66 memcpy's sizeof(app_config_v66_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v66_t) <= sizeof(app_config_t),
               "v66 snapshot must not exceed the current config struct");

/* ── Version 67 config struct — used only for NVS migration to v68 ────── */
/* Byte-identical to app_config_t minus the seven trailing flights_* fields */
/* v68 appended. Same body as the v66 snapshot plus the two fields v67    */
/* added at the end (custom_image_header, clouds_channel).                */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
} app_config_v67_t;

/* flights_enabled (bool, align 1) appends directly after clouds_channel
 * (uint8_t, align 1), so no alignment padding can sit between them. Nothing
 * moved in v68 — the fields are a pure append — so this is the plain
 * end-of-last-field equality the v62..v66 asserts use. */
_Static_assert(offsetof(app_config_t, flights_enabled) ==
                   offsetof(app_config_v67_t, clouds_channel) +
                       sizeof(((app_config_v67_t *)0)->clouds_channel),
               "app_config_v67_t snapshot drifted from app_config_t layout");

/* migrate_from_v67 memcpy's sizeof(app_config_v67_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v67_t) <= sizeof(app_config_t),
               "v67 snapshot must not exceed the current config struct");

/* ── Version 68 config struct — used only for NVS migration to v69 ────── */
/* Byte-identical to app_config_t minus the trailing flights_route_lookup  */
/* field v69 appended. Same body as the v67 snapshot plus the seven        */
/* flights_* fields v68 added at the end.                                  */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
} app_config_v68_t;

/* flights_route_lookup (bool, align 1) appends directly after flights_mode
 * (uint8_t, align 1), so no alignment padding can sit between them. Nothing
 * moved in v69 — the field is a pure append — so this is the plain
 * end-of-last-field equality the v62..v67 asserts use. */
_Static_assert(offsetof(app_config_t, flights_route_lookup) ==
                   offsetof(app_config_v68_t, flights_mode) +
                       sizeof(((app_config_v68_t *)0)->flights_mode),
               "app_config_v68_t snapshot drifted from app_config_t layout");

/* migrate_from_v68 memcpy's sizeof(app_config_v68_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v68_t) <= sizeof(app_config_t),
               "v68 snapshot must not exceed the current config struct");

/* ── Version 69 config struct — used only for NVS migration to v70 ────── */
/* Byte-identical to app_config_t minus the trailing flights_label_max     */
/* field v70 appended. Same body as the v68 snapshot plus the              */
/* flights_route_lookup bool v69 added at the end.                         */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
    bool     flights_route_lookup;
} app_config_v69_t;

/* flights_label_max (uint8_t, align 1) appends directly after
 * flights_route_lookup (bool, align 1), so no alignment padding can sit
 * between them. Nothing moved in v70 — the field is a pure append — so this
 * is the plain end-of-last-field equality the v62..v68 asserts use. */
_Static_assert(offsetof(app_config_t, flights_label_max) ==
                   offsetof(app_config_v69_t, flights_route_lookup) +
                       sizeof(((app_config_v69_t *)0)->flights_route_lookup),
               "app_config_v69_t snapshot drifted from app_config_t layout");

/* migrate_from_v69 memcpy's sizeof(app_config_v69_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v69_t) <= sizeof(app_config_t),
               "v69 snapshot must not exceed the current config struct");

/* ── Version 70 config struct — used only for NVS migration to v71 ────── */
/* Byte-identical to app_config_t minus the trailing flights_icon_style    */
/* field v71 appended. Same body as the v69 snapshot plus the              */
/* flights_label_max uint8_t v70 added at the end.                         */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
    bool     flights_route_lookup;
    uint8_t  flights_label_max;
} app_config_v70_t;

/* flights_icon_style (uint8_t, align 1) appends directly after
 * flights_label_max (uint8_t, align 1), so no alignment padding can sit
 * between them. Nothing moved in v71 — the field is a pure append — so this
 * is the plain end-of-last-field equality the v62..v69 asserts use. */
_Static_assert(offsetof(app_config_t, flights_icon_style) ==
                   offsetof(app_config_v70_t, flights_label_max) +
                       sizeof(((app_config_v70_t *)0)->flights_label_max),
               "app_config_v70_t snapshot drifted from app_config_t layout");

/* migrate_from_v70 memcpy's sizeof(app_config_v70_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v70_t) <= sizeof(app_config_t),
               "v70 snapshot must not exceed the current config struct");

/* ── Version 71 config struct — used only for NVS migration to v72 ────── */
/* Byte-identical to app_config_t minus the trailing clock_layout field    */
/* v72 appended. Same body as the v70 snapshot plus the                    */
/* flights_icon_style uint8_t v71 added at the end.                        */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
    bool     flights_route_lookup;
    uint8_t  flights_label_max;
    uint8_t  flights_icon_style;
} app_config_v71_t;

/* clock_layout (uint8_t, align 1) appends directly after flights_icon_style
 * (uint8_t, align 1), so no alignment padding can sit between them. Nothing
 * moved in v72 — the field is a pure append — so this is the plain
 * end-of-last-field equality the v62..v70 asserts use. */
_Static_assert(offsetof(app_config_t, clock_layout) ==
                   offsetof(app_config_v71_t, flights_icon_style) +
                       sizeof(((app_config_v71_t *)0)->flights_icon_style),
               "app_config_v71_t snapshot drifted from app_config_t layout");

/* migrate_from_v71 memcpy's sizeof(app_config_v71_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v71_t) <= sizeof(app_config_t),
               "v71 snapshot must not exceed the current config struct");

/* ── Version 72 config struct — used only for NVS migration to v73 ────── */
/* Byte-identical to app_config_t minus the trailing clouds_basemap field  */
/* v73 appended. Same body as the v71 snapshot plus the clock_layout       */
/* uint8_t v72 added at the end.                                          */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
    bool     flights_route_lookup;
    uint8_t  flights_label_max;
    uint8_t  flights_icon_style;
    uint8_t  clock_layout;
} app_config_v72_t;

/* clouds_basemap (uint8_t, align 1) appends directly after clock_layout
 * (uint8_t, align 1), so no alignment padding can sit between them. Nothing
 * moved in v73 — the field is a pure append — so this is the plain
 * end-of-last-field equality the v62..v71 asserts use. */
_Static_assert(offsetof(app_config_t, clouds_basemap) ==
                   offsetof(app_config_v72_t, clock_layout) +
                       sizeof(((app_config_v72_t *)0)->clock_layout),
               "app_config_v72_t snapshot drifted from app_config_t layout");

/* migrate_from_v72 memcpy's sizeof(app_config_v72_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v72_t) <= sizeof(app_config_t),
               "v72 snapshot must not exceed the current config struct");

/* ── Version 73 config struct — used only for NVS migration to v74 ──── */
/* Byte-identical to app_config_t minus the trailing clouds_show_location  */
/* bool v74 appended. Same body as the v72 snapshot plus the             */
/* clouds_basemap uint8_t v73 added at the end.                          */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
    bool     flights_route_lookup;
    uint8_t  flights_label_max;
    uint8_t  flights_icon_style;
    uint8_t  clock_layout;
    uint8_t  clouds_basemap;
} app_config_v73_t;

/* clouds_show_location (bool, align 1) appends directly after clouds_basemap
 * (uint8_t, align 1), so no alignment padding can sit between them. Nothing
 * moved in v74 — the field is a pure append — so this is the plain
 * end-of-last-field equality the v62..v72 asserts use. */
_Static_assert(offsetof(app_config_t, clouds_show_location) ==
                   offsetof(app_config_v73_t, clouds_basemap) +
                       sizeof(((app_config_v73_t *)0)->clouds_basemap),
               "app_config_v73_t snapshot drifted from app_config_t layout");

/* migrate_from_v73 memcpy's sizeof(app_config_v73_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v73_t) <= sizeof(app_config_t),
               "v73 snapshot must not exceed the current config struct");

/* ── Version 74 config struct — used only for NVS migration to v75 ──── */
/* Byte-identical to app_config_t minus the trailing nina_layout[]     */
/* uint8_t array v75 appended. Same body as the v73 snapshot plus the  */
/* clouds_show_location bool v74 added at the end.                     */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
    bool     flights_route_lookup;
    uint8_t  flights_label_max;
    uint8_t  flights_icon_style;
    uint8_t  clock_layout;
    uint8_t  clouds_basemap;
    bool     clouds_show_location;
} app_config_v74_t;

/* nina_layout[] (uint8_t[3], align 1) appends directly after
 * clouds_show_location (bool, align 1), so no alignment padding can sit
 * between them. Nothing moved in v75 — the field is a pure append — so
 * this is the plain end-of-last-field equality the v62..v73 asserts use. */
_Static_assert(offsetof(app_config_t, nina_layout) ==
                   offsetof(app_config_v74_t, clouds_show_location) +
                       sizeof(((app_config_v74_t *)0)->clouds_show_location),
               "app_config_v74_t snapshot drifted from app_config_t layout");

/* migrate_from_v74 memcpy's sizeof(app_config_v74_t) bytes into app_config_t;
 * it must never exceed the destination. */
_Static_assert(sizeof(app_config_v74_t) <= sizeof(app_config_t),
               "v74 snapshot must not exceed the current config struct");

/* ── Version 75 config struct — used only for NVS migration ─────────── */
/* v76 appended NO field: it only widened the value range of the existing */
/* voice_notify_mask (bits 12-26 become the per-event phrase gates), so   */
/* this snapshot is byte-identical to app_config_t AS OF v75/v76 (v77     */
/* then appended audio_muted past it). It is spelled out in full rather   */
/* than aliased to app_config_t so that a later version appending a field */
/* cannot silently change what a v75 blob means. Same body as the v74     */
/* snapshot plus the nina_layout[] uint8_t array v75 added at the end.    */
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
    uint8_t  goes_orientation;
    uint8_t  solar_orientation;
    uint16_t nav_grace_s;
    char     custom_image_url[256];
    uint8_t  custom_orientation;
    uint16_t custom_update_interval_s;
    uint8_t  auto_rotate_order2_retired[16];   /* the RETIRED mid-struct array,
                                        * named exactly as the live struct
                                        * names it — see the note there */
    uint8_t  goes_vflip;
    uint8_t  goes_hflip;
    uint8_t  solar_vflip;
    uint8_t  solar_hflip;
    uint8_t  custom_vflip;
    uint8_t  custom_hflip;
    bool     home_page_lock;
    bool     json_enabled;
    char     json_url[256];
    char     json_auth_header[256];
    uint16_t json_update_interval_s;
    bool     ha_enabled;
    char     ha_base_url[256];
    char     ha_token[256];
    uint16_t ha_update_interval_s;
    bool     setup_hint_dismissed;
    uint8_t  alert_voice_enabled;
    uint8_t  alert_voice_volume;
    uint8_t  alert_voice_types;
    uint8_t  alert_voice_repeat_min;
    bool     alert_voice_muted[3];
    uint32_t voice_notify_mask;
    uint8_t  boot_jingle_enabled;
    uint8_t  alert_voice_brief;
    uint8_t  alert_voice_conn;
    uint8_t  alert_voice_disc;
    uint8_t  wifi_max_tx_dbm;
    uint8_t  octoprint_enabled;
    char     octoprint_url[128];
    char     octoprint_api_key[64];
    uint16_t octoprint_update_interval_s;
    uint8_t  octoprint_image_source;
    uint8_t  octoprint_layout;
    char     octoprint_snapshot_url[128];
    bool     goes_enabled;
    bool     moon_enabled;
    bool     solar_enabled;
    bool     custom_enabled;
    uint16_t solar_update_interval_s;
    uint16_t moon_update_interval_s;
    bool     goes_crop;
    bool     solar_crop;
    bool     custom_crop;
    bool     goes_show_overlay;
    bool     moon_show_overlay;
    bool     solar_show_overlay;
    bool     custom_show_overlay;
    bool     octoprint_overlay_visible;
    bool     radar_enabled;
    char     radar_token[16];
    uint16_t radar_update_interval_s;
    bool     radar_show_overlay;
    uint8_t  radar_crop;
    uint8_t  radar_frames;
    uint8_t  auto_rotate_order2[ARP_ORDER_CAPACITY];
    bool     radar_dark_mode;
    uint8_t  radar_map_style;
    bool     clouds_enabled;
    bool     clouds_show_overlay;
    uint16_t clouds_update_interval_s;
    uint8_t  clouds_frames;
    uint8_t  clouds_zoom;
    char     custom_image_header[256];
    uint8_t  clouds_channel;
    bool     flights_enabled;
    char     flights_url[128];
    uint16_t flights_update_interval_s;
    uint16_t flights_range_nm;
    uint8_t  flights_min_el;
    uint16_t flights_up_azimuth;
    uint8_t  flights_mode;
    bool     flights_route_lookup;
    uint8_t  flights_label_max;
    uint8_t  flights_icon_style;
    uint8_t  clock_layout;
    uint8_t  clouds_basemap;
    bool     clouds_show_location;
    uint8_t  nina_layout[MAX_NINA_INSTANCES];
} app_config_v75_t;

/* v77 appended audio_muted, so the equality asserts v76 used are relaxed
 * to the <= / same-trailing-field-offset form the v62..v74 asserts use. */
_Static_assert(offsetof(app_config_v75_t, nina_layout) ==
                   offsetof(app_config_t, nina_layout),
               "app_config_v75_t snapshot drifted from app_config_t layout");
_Static_assert(sizeof(app_config_v75_t) <= sizeof(app_config_t),
               "v75 snapshot must not exceed the current config struct");

/* ── Version 76 config struct — used only for NVS migration to v77 ──── */
/* v76 appended NO field over v75 (it only widened the voice_notify_mask
 * value range), so its layout is byte-for-byte the v75 snapshot. Both
 * snapshots are FROZEN forever, so aliasing one to the other cannot
 * drift the way aliasing to the live app_config_t would; spelling the
 * ~180-line body out a second time would only invite copy divergence. */
typedef app_config_v75_t app_config_v76_t;

/* v75 and v76 snapshots are the same layout by construction (typedef);
 * the asserts below pin the relationship to the LIVE struct instead:
 * v77's audio_muted (bool, align 1) is the first field past the v76
 * snapshot's last field, and the snapshot never exceeds the live struct
 * (migrate_from_v76 memcpy's sizeof(app_config_v76_t) into it). */
_Static_assert(sizeof(app_config_v76_t) == sizeof(app_config_v75_t),
               "v76 layout must equal v75 (v76 appended no field)");
_Static_assert(offsetof(app_config_t, audio_muted) ==
                   offsetof(app_config_v76_t, nina_layout) +
                       sizeof(((app_config_v76_t *)0)->nina_layout),
               "app_config_v76_t snapshot drifted from app_config_t layout");
_Static_assert(sizeof(app_config_v76_t) <= sizeof(app_config_t),
               "v76 snapshot must not exceed the current config struct");

/* ── Version 77 layout -- used only for NVS migration to v78 ──── */
/* No snapshot struct: v78 appended a single bool at the very end, so the live
 * struct's prefix through audio_muted IS the v77 layout byte for byte (the
 * append-only rule). migrate_from_v77 in app_config.c therefore uses
 * offsetof(app_config_t, telemetry_enabled) as the v77 prefix size instead of
 * a ~180-line snapshot body. The assert pins the new field directly after
 * audio_muted so an insertion between them cannot silently shift that prefix. */
_Static_assert(offsetof(app_config_t, telemetry_enabled) ==
                   offsetof(app_config_t, audio_muted) +
                       sizeof(((app_config_t *)0)->audio_muted),
               "telemetry_enabled must directly follow audio_muted (v77 prefix rule)");
_Static_assert(offsetof(app_config_t, moon_round_size_pct) ==
                   offsetof(app_config_t, telemetry_enabled) +
                       sizeof(((app_config_t *)0)->telemetry_enabled),
               "moon_round_size_pct must directly follow telemetry_enabled (v78 prefix rule)");
_Static_assert(offsetof(app_config_t, nina_notify_scope) ==
                   offsetof(app_config_t, moon_round_size_pct) +
                       sizeof(((app_config_t *)0)->moon_round_size_pct),
               "nina_notify_scope must directly follow moon_round_size_pct (v79 prefix rule)");
_Static_assert(offsetof(app_config_t, radar_zoom) ==
                   offsetof(app_config_t, nina_notify_scope) +
                       sizeof(((app_config_t *)0)->nina_notify_scope),
               "radar_zoom must directly follow nina_notify_scope (v80 prefix rule)");
_Static_assert(offsetof(app_config_t, radar_palette) ==
                   offsetof(app_config_t, radar_zoom) +
                       sizeof(((app_config_t *)0)->radar_zoom),
               "radar_palette must directly follow radar_zoom (v81 append rule)");






// v17 snapshot — AllSky fields without allsky_enabled
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
} app_config_v17_t;

// v18 snapshot — current layout without demo_mode
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
} app_config_v18_t;

// v19 snapshot — current layout without Spotify fields
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
} app_config_v19_t;

// v21 snapshot — layout before spotify_minimal_mode was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
} app_config_v21_t;

// v22 snapshot — layout before spotify_scroll_text was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
} app_config_v22_t;

// v20 snapshot — current layout without spotify_overlay_timeout_s
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
} app_config_v20_t;

// v23 snapshot — layout before wifi_networks was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
} app_config_v23_t;

// v27 snapshot — layout before toast notification overhaul fields
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[7];
} app_config_v27_t;

// v28 snapshot — layout before weather/idle-override fields were added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[7];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
} app_config_v28_t;

// v29 snapshot — layout before idle_indicator_enabled was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
} app_config_v29_t;

// v30 snapshot — layout before admin_password was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
} app_config_v30_t;

// v31 snapshot — layout before auth_enabled was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
} app_config_v31_t;

// v32 snapshot — layout before Image Display fields were added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
} app_config_v32_t;

// v33 snapshot — layout before Moon phase fields were added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
} app_config_v33_t;

// v34 snapshot — layout before solar_band was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
} app_config_v34_t;

// v35 snapshot — layout before image_display_crop was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
} app_config_v35_t;

// v36 snapshot — layout before moon_drag_light_mode was added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
} app_config_v36_t;

// v37 snapshot — layout before moon orientation tuning fields were added
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
} app_config_v37_t;

// v38 snapshot — layout before moon_north_up was added (v37 layout + moon orientation tuning fields)
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
} app_config_v38_t;

// v39 snapshot — layout before moon_spin_mode/moon_spin_return_s were added (v38 layout + moon_north_up)
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
} app_config_v39_t;

// v40 snapshot — layout before crash_log_retention_days was added (v39 layout + moon touch-spin fields)
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
} app_config_v40_t;

// v41 snapshot — v40 layout plus crash_log_retention_days. This is the on-NVS
// layout before auto_rotate_pages_hi / auto_rotate_order_ext were appended in v42.
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
} app_config_v41_t;

// v42 snapshot — v41 layout plus auto_rotate_pages_hi / auto_rotate_order_ext.
// This is the on-NVS layout before goes_orientation / solar_orientation were
// appended in v43.
typedef struct {
    uint32_t config_version;
    char api_url[3][128];
    char ntp_server[64];
    char tz_string[64];
    char filter_colors[3][512];
    char rms_thresholds[3][256];
    char hfr_thresholds[3][256];
    int theme_index;
    int brightness;
    int color_brightness;
    bool mqtt_enabled;
    char mqtt_broker_url[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic_prefix[64];
    uint16_t mqtt_port;
    int8_t   active_page_override;
    bool     auto_rotate_enabled;
    uint16_t auto_rotate_interval_s;
    uint8_t  auto_rotate_effect;
    bool     auto_rotate_skip_disconnected;
    uint8_t  auto_rotate_pages;
    uint8_t  update_rate_s;
    uint8_t  graph_update_interval_s;
    uint8_t  connection_timeout_s;
    uint8_t  toast_duration_s;
    bool     debug_mode;
    bool     instance_enabled[3];
    bool     screen_sleep_enabled;
    uint16_t screen_sleep_timeout_s;
    bool     alert_flash_enabled;
    uint8_t  idle_poll_interval_s;
    bool     wifi_power_save;
    uint8_t  widget_style;
    uint8_t  auto_update_check;
    uint8_t  update_channel;
    bool     deep_sleep_enabled;
    uint32_t deep_sleep_wake_timer_s;
    bool     deep_sleep_on_idle;
    uint8_t  screen_rotation;
    char     hostname[32];
    char     allsky_hostname[128];
    uint16_t allsky_update_interval_s;
    float    allsky_dew_offset;
    char     allsky_field_config[1536];
    char     allsky_thresholds[1024];
    bool     allsky_enabled;
    bool     demo_mode;
    bool     spotify_enabled;
    char     spotify_client_id[64];
    uint16_t spotify_poll_interval_ms;
    bool     spotify_show_progress_bar;
    uint8_t  spotify_overlay_timeout_s;
    bool     spotify_minimal_mode;
    bool     spotify_scroll_text;
    wifi_network_t wifi_networks[3];
    bool     spotify_overlay_visible;
    uint8_t  auto_rotate_order[8];
    uint8_t  toast_aggregation_window_s;
    uint32_t toast_notify_mask;
    bool     toast_instance_muted[3];
    uint8_t  weather_provider;
    char     weather_api_key[64];
    float    weather_lat;
    float    weather_lon;
    char     weather_location_name[64];
    uint16_t weather_poll_interval_s;
    uint8_t  weather_units;
    uint8_t  weather_time_format;
    bool     idle_page_override_enabled;
    int8_t   idle_page_override_target;
    bool     idle_page_persistent;
    bool     idle_indicator_enabled;
    char     admin_password[33];
    bool     auth_enabled;
    bool     image_display_enabled;
    bool     image_display_show_overlay;
    char     goes_region[16];
    uint16_t goes_update_interval_s;
    uint8_t  image_display_source;
    uint8_t  moon_bg_style;
    float    moon_lat;
    float    moon_lon;
    uint8_t  solar_band;
    bool     image_display_crop;
    uint8_t  moon_drag_light_mode;
    uint8_t  moon_flip_u;
    uint8_t  moon_flip_v;
    float    moon_roll_offset;
    float    moon_yaw_offset;
    float    moon_pitch_offset;
    uint8_t  moon_north_up;
    uint8_t  moon_spin_mode;
    uint8_t  moon_spin_return_s;
    uint8_t  crash_log_retention_days;
    uint8_t  auto_rotate_pages_hi;
    uint8_t  auto_rotate_order_ext;
} app_config_v42_t;

// WiFi credentials are stored in app_config_t.wifi_networks[3] (up to 3
// priority-ordered networks). The AP provides headless access for initial
// configuration via the embedded web UI.

void app_config_init(void);
app_config_t *app_config_get(void);
/* Copy the live config into a caller-provided buffer under the config mutex.
 * app_config_t is ~20 KB — NEVER return/copy it by value onto a task stack
 * (overflows small poll/UI task stacks). Snapshot into a PSRAM heap buffer. */
void app_config_get_snapshot_into(app_config_t *dst);
void app_config_save(const app_config_t *config);
/* Same commit semantics as app_config_save(), but the ~350 ms NVS write is
 * debounced: the values go live in RAM immediately and the flash write happens
 * ~2 s after the LAST call, so a burst (an HA slider drag, a held keypad key)
 * costs one write instead of one per step. Safe from any task. A reboot via
 * esp_restart() flushes a pending write first (shutdown handler). */
void app_config_save_deferred(const app_config_t *config);
/* Write a pending deferred save to NVS now (no-op if nothing is pending). */
void app_config_flush_deferred(void);
void app_config_apply(const app_config_t *config);   // in-memory only, no NVS; marks dirty
/* Same as app_config_apply(), but leaves the unsaved-changes flag exactly as it
 * found it (neither set nor cleared). For the "preview":true path of the
 * page-config POST handlers: a preview is a live look at candidate values, not a
 * pending edit, so it must not raise the web UI's "unsaved changes" bar -- nor
 * clear it if a real live-apply already raised it. */
void app_config_apply_preview(const app_config_t *config);
/* Same mechanism as app_config_apply_preview(), different name for a different
 * caller: a config change COMMANDED FROM OUTSIDE (an HA automation, a macro
 * keypad, any stateless API client) is not an unsaved web edit, so it must not
 * raise the web UI's "unsaved changes" bar. The user never opened an editor;
 * there is nothing for them to Save, and a bar they cannot explain is worse than
 * no bar. Persistence semantics are identical to app_config_apply(): RAM only. */
void app_config_apply_external(const app_config_t *config);
esp_err_t app_config_revert(void);                    // reload NVS into memory
bool app_config_is_dirty(void);                       // true if apply called without save
int app_config_get_instance_count(void);
const char *app_config_get_instance_url(int index);
/* Extract the display hostname from an instance's configured URL.
 * "http://astromele2.lan:1888/v2/api/" -> "astromele2.lan". Writes "" when the
 * index is out of range or the URL is unset. Always NUL-terminates. */
void app_config_get_instance_host(int index, char *out, size_t out_size);
void app_config_factory_reset(void);

/* Tiles-config accessors. The value lives in a dedicated NVS key ("json_tiles"
 * / "ha_tiles"), mirrored in a process-lifetime PSRAM cache buffer.
 *
 * Getter: returns a stable, always-NUL-terminated const pointer ("" when unset).
 *   The buffer is allocated once at init and NEVER freed or reallocated, so the
 *   pointer is valid for the life of the process. A concurrent setter overwrites
 *   the buffer IN PLACE under the config mutex; a lock-free reader may therefore
 *   observe a torn string (same benign risk class as reading cfg->json_tiles_config
 *   pre-split while app_config_save() memcpy'd s_config). Callers use the pointer
 *   transiently (json_client_poll/ha_client_poll copy the content). Do NOT free.
 * Setter: validates length (clamps to MAX-1), writes the NVS key + commits, and
 *   updates the cache under the config mutex. Returns ESP_OK, or ESP_ERR_NO_MEM if
 *   the cache could not be allocated (NVS/struct blob remain uncorrupted). */
const char *app_config_get_json_tiles(void);
const char *app_config_get_ha_tiles(void);
esp_err_t   app_config_set_json_tiles(const char *s);
esp_err_t   app_config_set_ha_tiles(const char *s);

/* Preview (RAM-only) variants: identical to the setters above but SKIP the NVS
 * write, so the live page picks up the new tiles while the saved value is left
 * untouched. Same relationship as app_config_apply() vs app_config_save() for
 * the struct blob. Used by the "preview":true path of POST /api/json-config and
 * POST /api/ha-config. The change is device-wide and is NOT auto-reverted: a
 * later non-preview POST, or a preview POST carrying the saved values, restores
 * the cache; otherwise it reverts on the next reboot (NVS is authoritative). */
esp_err_t   app_config_set_json_tiles_ram(const char *s);
esp_err_t   app_config_set_ha_tiles_ram(const char *s);

bool app_config_is_instance_enabled(int index);
int app_config_get_enabled_instance_count(void);
uint32_t app_config_get_filter_color(const char *filter_name, int instance_index);
uint32_t app_config_get_rms_color(float rms_value, int instance_index);
uint32_t app_config_get_hfr_color(float hfr_value, int instance_index);

// Threshold configuration (values + colors) for graph overlay display
typedef struct {
    float good_max;
    float ok_max;
    uint32_t good_color;
    uint32_t ok_color;
    uint32_t bad_color;
} threshold_config_t;

void app_config_get_rms_threshold_config(int instance_index, threshold_config_t *out);
void app_config_get_hfr_threshold_config(int instance_index, threshold_config_t *out);
void app_config_sync_filters(const char *filter_names[], int count, int instance_index);
uint32_t app_config_apply_brightness(uint32_t color, int brightness);

/** Enforce nav-mode exclusivity in-place: home-page-lock, auto-rotate, and
 *  idle-override are mutually exclusive. Home-page-lock wins over both; between
 *  auto-rotate and idle-override, auto-rotate wins the tie-break. Idempotent. */
void app_config_normalize_nav_exclusivity(app_config_t *cfg);

/* v61 image-pages split: derive the thirteen per-page image fields from the
 * retired global image_display_* fields (goes/moon/solar/custom_enabled from
 * image_display_enabled, solar interval from goes interval, crops/overlays
 * per page; custom_crop = false). Idempotent. Used by the NVS load path for
 * every pre-v61 blob and by the backup-restore path for pre-v61 backups. */
void image_pages_derive_from_legacy(app_config_t *cfg);

#ifdef __cplusplus
}
#endif
