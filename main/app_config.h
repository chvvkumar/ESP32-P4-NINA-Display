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
 *   0  ARP_IDX_SUMMARY    -> PAGE_IDX_SUMMARY (4)
 *   1  ARP_IDX_NINA1      -> NINA_PAGE_OFFSET+0 (5)
 *   2  ARP_IDX_NINA2      -> NINA_PAGE_OFFSET+1 (6)
 *   3  ARP_IDX_NINA3      -> NINA_PAGE_OFFSET+2 (7)
 *   4  ARP_IDX_SYSINFO    -> SYSINFO_PAGE_IDX
 *   5  ARP_IDX_ALLSKY     -> PAGE_IDX_ALLSKY (0)
 *   6  ARP_IDX_SPOTIFY    -> PAGE_IDX_SPOTIFY (1)
 *   7  ARP_IDX_CLOCK      -> PAGE_IDX_CLOCK (2)
 *   8  ARP_IDX_IMG_GOES   -> PAGE_IDX_IMAGE_DISPLAY (3), image source 0
 *   9  ARP_IDX_IMG_MOON   -> PAGE_IDX_IMAGE_DISPLAY (3), image source 1
 *  10  ARP_IDX_IMG_SOLAR  -> PAGE_IDX_IMAGE_DISPLAY (3), image source 2
 *  11  ARP_IDX_IMG_CUSTOM -> PAGE_IDX_IMAGE_DISPLAY (3), image source 3
 *
 * Stop values are page_ref_t ids (ids 0..11 are frozen identical to the
 * ARP_IDX_* constants above). Pages added to the registry after the 0..11
 * anchored block keep their frozen registry id as their slideshow stop value,
 * so the range is no longer contiguous: ids 12..23 name non-slideshow entries
 * (image-default sentinel, Settings, overlays) and are NOT valid stops.
 *  24  ARP_IDX_JSON       -> PAGE_IDX_JSON (4)  (== PAGE_REF_JSON)
 *  25  ARP_IDX_HA         -> PAGE_IDX_HA (5)    (== PAGE_REF_HA)
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
/* True if @p b names a valid slideshow stop. */
#define ARP_STOP_IS_VALID(b) ((b) < ARP_IDX_MAX || (b) == ARP_IDX_JSON || (b) == ARP_IDX_HA)
#define ARP_ORDER_CAPACITY 16   /* size of auto_rotate_order2[] */

// Current config struct version — bump on every layout change.
#define APP_CONFIG_VERSION 52

/* Tiles-config blobs no longer live inside app_config_t (v52 split them out to
 * dedicated NVS string keys "json_tiles"/"ha_tiles"). These bound the value
 * length; the web length-guards use them since they can no longer sizeof() a
 * removed struct field. Value length includes the terminating NUL. */
#define JSON_TILES_CONFIG_MAX 6144   /* max bytes (incl NUL) for the "json_tiles" NVS value */
#define HA_TILES_CONFIG_MAX   6144   /* max bytes (incl NUL) for the "ha_tiles"  NVS value */

#define WIDGET_STYLE_COUNT 13

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
    bool     wifi_power_save;        // Enable WiFi modem sleep for power savings (default true)
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
    // Image Display page
    bool     image_display_enabled;          // Enable Image Display page + poll task (default false)
    bool     image_display_show_overlay;     // Show region/timestamp overlay bar (default true)
    char     goes_region[16];                // NESDIS sector code, e.g. "umv" (default "umv")
    uint16_t goes_update_interval_s;         // Poll interval in seconds, 300-7200 (default 600)

    // Added after v33 — must stay at end to preserve NVS binary compatibility
    uint8_t  image_display_source;   // 0=GOES (default), 1=Moon
    uint8_t  moon_bg_style;          // 0=black, 1=stars, 2=glow, 3=stars+glow
    float    moon_lat;               // observer latitude (deg), 0=unset
    float    moon_lon;               // observer longitude (deg), 0=unset

    // Added after v34 — must stay at end to preserve NVS binary compatibility
    uint8_t  solar_band;             // SDO/AIA band index 0..9 (default 0)

    // Added after v35 — must stay at end to preserve NVS binary compatibility
    bool     image_display_crop;     // crop/zoom image to fill & hide baked-in labels (default false)

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
    // Ordered slideshow stop list: array of ARP_IDX_* values, 0xFF terminator.
    // Replaces the legacy auto_rotate_order[8] + auto_rotate_order_ext encoding;
    // each image source is now its own distinct stop (see ARP_IDX_* above).
    uint8_t  auto_rotate_order2[16];

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
_Static_assert(offsetof(app_config_t, auto_rotate_order2) ==
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
void app_config_apply(const app_config_t *config);   // in-memory only, no NVS
esp_err_t app_config_revert(void);                    // reload NVS into memory
bool app_config_is_dirty(void);                       // true if apply called without save
int app_config_get_instance_count(void);
const char *app_config_get_instance_url(int index);
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

#ifdef __cplusplus
}
#endif
