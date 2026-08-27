#pragma once

#include "app_config.h"   /* app_config_t */
#include "cJSON.h"        /* cJSON, used by settings_json_serialize/parse */

/* X-macro table of "simple" app_config_t settings: one row per field that has
 * a plain 1:1 default value and (optionally) a range check. Driven by
 * settings_defaults_apply() / settings_clamp_apply() in settings_table.c,
 * which are called from set_defaults() / validate_config() in app_config.c.
 *
 * Ranges below are transcribed VERBATIM from the pre-existing per-field
 * checks in validate_config() (or, where noted "no prior clamp", from the
 * nearest existing bound already enforced elsewhere, e.g. the web POST
 * handler in web_handlers_config.c). Do not "improve" a range without
 * updating both the firmware behavior and this comment.
 *
 * Kinds:
 *   SETTING_BOOL      (field, json_key, def)
 *       Default only. Clamp is a deliberate no-op: apart from
 *       home_page_lock (excluded below — see app_config.c), no bool field in
 *       the live struct is canonicalized in validate_config() today, so a
 *       blanket "make every bool literally 0/1" clamp would be new behavior.
 *
 *   SETTING_INT       (field, json_key, def, min, max)
 *       True two-sided clamp-to-bound: value < min -> min; value > max -> max.
 *       Works for any integer width (uint8/16/32_t, int8_t, int).
 *
 *   SETTING_INT_RESET (field, json_key, def, min, max)
 *       RESET semantics: value < min OR value > max -> def (NOT the nearest
 *       bound). Use only where the existing code assigns one fixed constant
 *       regardless of which side was violated, and that constant equals
 *       `def`. If the reset-target differs from the set_defaults() default,
 *       the field must NOT be migrated (see exclusions in app_config.c) —
 *       one table row cannot carry two different constants without lying
 *       about one of them.
 *
 *   SETTING_FLOAT       (field, json_key, def, min, max)      -- true clamp
 *   SETTING_FLOAT_RESET (field, json_key, def, min, max)      -- reset semantics
 *       Float analogues of SETTING_INT / SETTING_INT_RESET.
 *
 *   SETTING_ENUM      (field, json_key, def, count_expr)
 *       value < 0 OR value >= count_expr -> def. count_expr may be a runtime
 *       call (themes_get_count()) or a compile-time constant
 *       (WIDGET_STYLE_COUNT). This is itself reset-to-def semantics (matches
 *       the existing theme_index / widget_style checks, which reset to 0
 *       rather than clamping to count_expr-1).
 *
 *   SETTING_STR       (field, json_key, def)
 *       Default = safe bounded copy of `def` into cfg->field. Clamp = force
 *       cfg->field[sizeof(cfg->field)-1] = '\0'. This duplicates the
 *       existing top-of-validate_config() NUL-termination block for the
 *       same field; that block is intentionally left untouched (it covers
 *       many fields, including ones NOT migrated here), so the duplication
 *       is harmless (idempotent) rather than deleted.
 *
 *   SETTING_STR_RESET (field, json_key, def)
 *       Same as SETTING_STR, plus: if cfg->field[0] == '\0' after
 *       NUL-termination, reset to `def`. Matches the existing
 *       "reset to default if empty" checks for mqtt_topic_prefix and
 *       goes_region.
 */
#define SETTINGS_TABLE(BOOL, INT, INT_RESET, FLT, FLT_RESET, ENUM, STR, STR_RESET) \
    /* -- MQTT -- */ \
    BOOL      (mqtt_enabled,                 "mqtt_enabled",                 false) \
    STR       (mqtt_broker_url,              "mqtt_broker_url",              "mqtt://192.168.1.250") \
    STR       (mqtt_username,                "mqtt_username",                "") \
    STR_RESET (mqtt_topic_prefix,            "mqtt_topic_prefix",            "ninadisplay") \
    INT_RESET (mqtt_port,                    "mqtt_port",                    1883,  1,     65535) \
    /* -- Display / theme -- */ \
    ENUM      (theme_index,                  "theme_index",                  0,     themes_get_count()) \
    INT_RESET (brightness,                   "brightness",                   50,    0,     100) \
    INT_RESET (color_brightness,             "color_brightness",             100,   0,     100) \
    ENUM      (widget_style,                 "widget_style",                 0,     WIDGET_STYLE_COUNT) \
    INT_RESET (screen_rotation,              "screen_rotation",              0,     0,     3) \
    /* -- Auto-rotate (scalars only; arrays/bitmask excluded — see app_config.c) -- */ \
    BOOL      (auto_rotate_enabled,          "auto_rotate_enabled",          false) \
    INT_RESET (auto_rotate_interval_s,       "auto_rotate_interval_s",       30,    1,     3600) \
    INT_RESET (auto_rotate_effect,           "auto_rotate_effect",           0,     0,     3) \
    BOOL      (auto_rotate_skip_disconnected,"auto_rotate_skip_disconnected",true) \
    /* -- Polling / timing -- */ \
    INT_RESET (connection_timeout_s,         "connection_timeout_s",         6,     2,     30) \
    INT_RESET (toast_duration_s,             "toast_duration_s",             8,     3,     30) \
    INT_RESET (screen_sleep_timeout_s,       "screen_sleep_timeout_s",       60,    10,    3600) \
    INT_RESET (idle_poll_interval_s,         "idle_poll_interval_s",         30,    5,     120) \
    /* -- Misc toggles -- */ \
    BOOL      (debug_mode,                   "debug_mode",                   false) \
    BOOL      (screen_sleep_enabled,         "screen_sleep_enabled",         false) \
    BOOL      (alert_flash_enabled,          "alert_flash_enabled",          true) \
    BOOL      (wifi_power_save,              "wifi_power_save",              false) \
    INT_RESET (wifi_max_tx_dbm,              "wifi_max_tx_dbm",              0,     0,     20)    /* 0 = no cap (radio default); the only other legal values are 8/11/14/17/20. RESET, not clamp: an out-of-range byte (e.g. a stale 255) must fail toward connectivity (0 = uncapped), never toward a silent 20 dBm cap the user never chose. This row bounds the range only; the exact whitelist is enforced at the BOTTOM of validate_config() in app_config.c, which runs after settings_clamp_apply() and resets an in-range off-list survivor (e.g. 13) to 0. Strictest check last — the two compose, they do not fight */ \
    BOOL      (auto_update_check,            "auto_update_check",            1) \
    INT_RESET (update_channel,               "update_channel",               0,     0,     2) \
    /* -- Deep sleep -- */ \
    BOOL      (deep_sleep_enabled,           "deep_sleep_enabled",           false) \
    INT       (deep_sleep_wake_timer_s,      "deep_sleep_wake_timer_s",      28800, 0,     259200) /* no prior clamp; bound sourced from POST handler */ \
    BOOL      (deep_sleep_on_idle,           "deep_sleep_on_idle",           false) \
    /* -- Hostname / NTP / TZ -- */ \
    STR       (hostname,                     "hostname",                    "NINA-DISPLAY") \
    STR       (ntp_server,                   "ntp",                          "pool.ntp.org") \
    STR       (tz_string,                    "timezone",                     "CST6CDT,M3.2.0,M11.1.0") \
    /* -- AllSky -- */ \
    STR       (allsky_hostname,              "allsky_hostname",              "allskypi5.lan") \
    INT_RESET (allsky_update_interval_s,     "allsky_update_interval_s",     5,     1,     300) \
    FLT_RESET (allsky_dew_offset,            "allsky_dew_offset",            5.0f,  -50.0f,50.0f) \
    BOOL      (allsky_enabled,               "allsky_enabled",               false) \
    BOOL      (demo_mode,                    "demo_mode",                    false) \
    /* -- Spotify -- */ \
    BOOL      (spotify_enabled,              "spotify_enabled",              false) \
    INT_RESET (spotify_poll_interval_ms,     "spotify_poll_interval_ms",     3000,  1000,  30000) \
    BOOL      (spotify_show_progress_bar,    "spotify_show_progress_bar",    true) \
    INT       (spotify_overlay_timeout_s,    "spotify_overlay_timeout_s",    5,     0,     255)   /* whole uint8 domain valid; clamp is a documented no-op */ \
    BOOL      (spotify_minimal_mode,         "spotify_minimal_mode",         false) \
    BOOL      (spotify_scroll_text,          "spotify_scroll_text",          true) \
    BOOL      (spotify_overlay_visible,      "spotify_overlay_visible",      false) \
    /* -- Toast (scalar only; mask/array excluded — see app_config.c) -- */ \
    INT       (toast_aggregation_window_s,   "toast_aggregation_window_s",   5,     0,     15)    /* no prior clamp; bound sourced from POST handler */ \
    /* -- Weather -- */ \
    INT_RESET (weather_provider,             "weather_provider",             0,     0,     2) \
    STR       (weather_api_key,              "weather_api_key",              "")               /* SECRET: redacted by config_get_handler; is_sensitive in s_backup_fields so strip_masked_secrets() preserves it on a sentinel POST */ \
    FLT       (weather_lat,                  "weather_lat",                  0.0f,  -90.0f, 90.0f)  /* no prior clamp; obviously-correct latitude bound */ \
    FLT       (weather_lon,                  "weather_lon",                  0.0f,  -180.0f,180.0f) /* no prior clamp; obviously-correct longitude bound */ \
    STR       (weather_location_name,        "weather_location_name",        "") \
    INT       (weather_poll_interval_s,      "weather_poll_interval_s",      900,   900,   3600)  /* no prior clamp in validate_config; bound sourced from POST handler */ \
    INT       (weather_units,                "weather_units",                0,     0,     1) \
    INT       (weather_time_format,          "weather_time_format",          0,     0,     1) \
    /* -- Idle page override (target excluded — cross-field page-registry semantics) -- */ \
    BOOL      (idle_page_override_enabled,   "idle_page_override_enabled",   false) \
    BOOL      (idle_indicator_enabled,       "idle_indicator_enabled",       true) \
    /* -- Auth (admin_password excluded — secret) -- */ \
    BOOL      (auth_enabled,                 "auth_enabled",                 true) \
    /* -- Image Display / GOES -- */ \
    BOOL      (image_display_enabled,        "image_display_enabled",        false) \
    BOOL      (image_display_show_overlay,   "image_display_show_overlay",   true) \
    STR_RESET (goes_region,                  "goes_region",                  "umv") \
    INT_RESET (goes_update_interval_s,       "goes_update_interval_s",       600,   300,   7200) \
    INT_RESET (image_display_source,         "image_display_source",         0,     0,     3) \
    BOOL      (image_display_crop,           "image_display_crop",           false) \
    INT       (goes_orientation,             "goes_orientation",             0,     0,     3)     /* no prior clamp; web UI already reads/writes this key but the firmware previously ignored it both directions (dead field) — now wired */ \
    INT       (solar_orientation,            "solar_orientation",            0,     0,     3)     /* no prior clamp; web UI already reads/writes this key but the firmware previously ignored it both directions (dead field) — now wired */ \
    INT       (goes_vflip,                   "goes_vflip",                   0,     0,     1) \
    INT       (goes_hflip,                   "goes_hflip",                   0,     0,     1) \
    INT       (solar_vflip,                  "solar_vflip",                  0,     0,     1) \
    INT       (solar_hflip,                  "solar_hflip",                  0,     0,     1) \
    INT       (custom_vflip,                 "custom_vflip",                 0,     0,     1) \
    INT       (custom_hflip,                 "custom_hflip",                 0,     0,     1) \
    /* -- Moon phase -- */ \
    INT_RESET (moon_bg_style,                "moon_bg_style",                0,     0,     3) \
    FLT       (moon_lat,                     "moon_lat",                     0.0f,  -90.0f, 90.0f)  /* no prior clamp; obviously-correct latitude bound */ \
    FLT       (moon_lon,                     "moon_lon",                     0.0f,  -180.0f,180.0f) /* no prior clamp; obviously-correct longitude bound */ \
    INT_RESET (solar_band,                   "solar_band",                   0,     0,     23) \
    INT       (moon_flip_u,                  "moon_flip_u",                  0,     0,     1) \
    INT       (moon_flip_v,                  "moon_flip_v",                  0,     0,     1) \
    FLT       (moon_roll_offset,             "moon_roll_offset",             -7.0f, -180.0f,180.0f) \
    FLT       (moon_yaw_offset,              "moon_yaw_offset",              0.0f,  -180.0f,180.0f) \
    FLT       (moon_pitch_offset,            "moon_pitch_offset",            -5.0f, -90.0f, 90.0f) \
    INT       (moon_north_up,                "moon_north_up",                1,     0,     1) \
    INT       (moon_spin_mode,               "moon_spin_mode",               0,     0,     1) \
    INT       (moon_spin_return_s,           "moon_spin_return_s",           3,     3,     60) \
    /* -- Crash log -- */ \
    INT       (crash_log_retention_days,     "crash_log_retention_days",     30,    0,     255)   /* no prior clamp; bound sourced from POST handler */ \
    /* -- Navigation -- */ \
    INT       (nav_grace_s,                  "nav_grace_s",                  10,    10,    300) \
    /* -- Custom Image URL source -- */ \
    STR       (custom_image_url,             "custom_image_url",             "https://picsum.photos/720") \
    STR       (custom_image_header,          "custom_image_header",          "")               /* SECRET: optional raw "Name: value" header sent with the Custom Image fetch. Marked is_sensitive+mask_preview in s_backup_fields, so strip_masked_secrets() preserves it on a sentinel POST and config_get_handler redacts it on GET */ \
    INT_RESET (custom_orientation,           "custom_orientation",           0,     0,     3) \
    INT_RESET (custom_update_interval_s,     "custom_update_interval_s",     60,    10,    7200) \
    /* -- First-boot setup hint -- */ \
    BOOL      (setup_hint_dismissed,         "setup_hint_dismissed",         false) \
    /* -- Spoken voice alerts (v54) -- */ \
    INT       (alert_voice_enabled,          "alert_voice_enabled",          1,     0,     1)     /* default on; no prior clamp; whole 0/1 domain, matches the goes_vflip-style uint8 flags above */ \
    INT       (alert_voice_volume,           "alert_voice_volume",           90,    0,     100) \
    INT       (alert_voice_types,            "alert_voice_types",            ALERT_VOICE_TYPE_ALL, 0, ALERT_VOICE_TYPE_ALL) /* bitmask 1=RMS 2=HFR 4=SAFETY; two-sided clamp is exact here because every value 0..7 is a legal mask */ \
    INT       (alert_voice_repeat_min,       "alert_voice_repeat_min",       5,     0,     60)    /* 0 = announce once only */ \
    INT       (alert_voice_brief,            "alert_voice_brief",            0,     0,     1)     /* v57: 0 = detailed breach sentence with value, 1 = brief */ \
    INT       (alert_voice_conn,             "alert_voice_conn",             1,     0,     1)     /* v58: announce NINA link connect (default on) */ \
    INT       (alert_voice_disc,             "alert_voice_disc",             1,     0,     1)     /* v58: announce NINA link disconnect (default on) */ \
    /* -- Startup jingle (v56) -- */ \
    INT       (boot_jingle_enabled,          "boot_jingle_enabled",          1,     0,     1)     /* play the boot jingle once at startup; default on */ \
    /* -- OctoPrint 3D Printer page (v60) -- */ \
    INT       (octoprint_enabled,            "octoprint_enabled",            0,     0,     1)     /* uint8 flag, not bool — matches the goes_vflip/alert_voice_conn style above; whole 0/1 domain is legal so the two-sided clamp is exact */ \
    STR       (octoprint_url,                "octoprint_url",                "")               /* base URL, scheme+host+port; also listed in s_url_fields (web_handlers_config.c) so restore preview and write path both run validate_url_format() */ \
    STR       (octoprint_api_key,            "octoprint_api_key",            "")               /* SECRET: marked is_sensitive+mask_preview in s_backup_fields, so strip_masked_secrets() preserves it on a sentinel POST and config_get_handler redacts it on GET */ \
    INT       (octoprint_update_interval_s,  "octoprint_update_interval_s",  10,    2,     300)   /* true clamp: a too-fast or too-slow value is meaningful at the nearest bound, unlike the RESET fields below */ \
    INT_RESET (octoprint_image_source,       "octoprint_image_source",       0,     0,     1)     /* 0 = G-code preview, 1 = webcam snapshot. RESET, not clamp: an unknown source must fall back to the always-available preview, never to whichever source happens to sit at the far bound */ \
    INT_RESET (octoprint_layout,             "octoprint_layout",             0,     0,     6)     /* 0=bento 2=glass 5=overlay 6=letterbox; 1, 3 and 4 are retired, render Bento and stay reserved (legal in NVS). RESET, not clamp: layouts are unordered names, so a stale/unknown index means "no opinion" -> bento, not "the highest layout" */ \
    STR       (octoprint_snapshot_url,       "octoprint_snapshot_url",       "")               /* "" = derive the snapshot URL from octoprint_url (the normal state). A non-empty override is fetched verbatim, so it must be a full URL: listed in s_url_fields (web_handlers_config.c) like octoprint_url, and validate_url_format() still lets the empty string through */ \
    BOOL      (octoprint_overlay_visible,    "octoprint_overlay_visible",    true)  /* saved default for "show the readings over the picture"; a tap on the device toggles it for the screen only and never reaches NVS. Bento (Grid) has no overlay layer and ignores it */ \
    /* v61 image pages split: one enable / overlay / crop / interval per page. */ \
    BOOL      (goes_enabled,                 "goes_enabled",                 false) \
    BOOL      (moon_enabled,                 "moon_enabled",                 false) \
    BOOL      (solar_enabled,                "solar_enabled",                false) \
    BOOL      (custom_enabled,               "custom_enabled",               false) \
    INT       (solar_update_interval_s,      "solar_update_interval_s",      600,   300,   7200)  /* true clamp, same as the image POST handler's clamp-to-bound (both write paths agree) */ \
    INT       (moon_update_interval_s,       "moon_update_interval_s",       60,    10,    3600)  /* moon re-render cadence; the hardcoded 60 s becomes the default; true clamp */ \
    BOOL      (goes_crop,                    "goes_crop",                    false) \
    BOOL      (solar_crop,                   "solar_crop",                   false) \
    BOOL      (custom_crop,                  "custom_crop",                  false) \
    BOOL      (goes_show_overlay,            "goes_show_overlay",            true) \
    BOOL      (moon_show_overlay,            "moon_show_overlay",            true) \
    BOOL      (solar_show_overlay,           "solar_show_overlay",           true) \
    BOOL      (custom_show_overlay,          "custom_show_overlay",          true) \
    /* -- Weather Radar page (v63) -- */ \
    BOOL      (radar_enabled,                "radar_enabled",                false) \
    STR       (radar_token,                  "radar_token",                  "")    /* WSR-88D site id ("KTLX"), a regional/CONUS name, or "" = resolve the nearest site at runtime. Pasted into the image URL, so it is a trust boundary: the charset rule lives in validate_config() (load path) and in parse_config_from_json() (web save path) — STR here only bounds the copy and NUL-terminates */ \
    BOOL      (radar_show_overlay,           "radar_show_overlay",           false) \
    INT       (radar_crop,                   "radar_crop",                   0,     0,     1)     /* how the picture fits the panel: 0 = off (whole image, bars above and below), 1 = crop (banner and colour scale trimmed, fills the panel). Stays a uint8 INT row rather than BOOL because the retired middle value (2) can still be in NVS: a true clamp maps that 2 to 1 (crop), which is the same resolution the page code applies (radar_crop >= 1 -> crop). A RESET row would send it to 0 instead and silently un-crop those devices */ \
    INT       (radar_update_interval_s,      "radar_update_interval_s",      900,   120,   7200)  /* 15 min default. True clamp, matching the image POST handler's clamp-to-bound (both write paths agree). validate_config()'s extra "0 -> 900" case for a zeroed blob now lands on 120 instead; both are legal intervals */ \
    INT_RESET (radar_frames,                 "radar_frames",                 10,    1,     10)    /* how many radar images the page animates. RESET reproduces validate_config() exactly: both 0 (unset blob) and >10 fall back to the full 10-frame loop, never to the opposite bound */ \
    BOOL      (radar_dark_mode,              "radar_dark_mode",              true)  /* v64: true = dark basemap (the pre-v64 behaviour, and the default), false = the NWS image as published */ \
    INT_RESET (radar_map_style,              "radar_map_style",              1,     0,     2)     /* v65: which map the radar echoes are drawn over: 0 = standard NWS picture with roads and city names (the pre-v65 behaviour), 1 = state lines only (the default), 2 = state and county lines. RESET, not clamp: an unknown value (stale blob byte, or a future style this firmware does not know) falls back to state lines only rather than the nearest bound */ \
    /* -- Clouds page (v66): NASA GIBS GOES GeoColor around weather_lat/lon -- */ \
    BOOL      (clouds_enabled,               "clouds_enabled",               false) \
    BOOL      (clouds_show_overlay,          "clouds_show_overlay",          true) \
    INT       (clouds_update_interval_s,     "clouds_update_interval_s",     900,   300,   7200)  /* 15 min default (GIBS publishes every 10 min). True clamp, matching the image POST handler's clamp-to-bound */ \
    INT_RESET (clouds_frames,                "clouds_frames",                6,     1,     10)    /* animation depth, ~1 MB PSRAM per 720x720 frame. RESET: 0 (unset blob) and >10 both fall back to the default */ \
    INT_RESET (clouds_zoom,                  "clouds_zoom",                  7,     5,     9)     /* Web-Mercator zoom of the panel-sized picture: 5 ~2500 km wide .. 9 ~150 km. RESET: an unknown value falls back to the default, never to a bound */ \
    /* -- Clouds satellite channel (v67) -- */ \
    INT_RESET (clouds_channel,               "clouds_channel",               0,     0,     2)     /* which GOES ABI product the Clouds page shows: 0 = GeoColor (default), 1 = Clean Infrared (Band 13), 2 = Air Mass. RESET, not clamp: an unknown channel falls back to GeoColor, which every satellite publishes, rather than to the nearest bound */ \
    /* -- Clouds map overlay (v73) -- */ \
    INT_RESET (clouds_basemap,               "clouds_basemap",               0,     0,     3)     /* which map overlay is drawn over the Clouds imagery: 0 = borders and roads (default), 1 = coastlines only, 2 = borders, roads and grid, 3 = none. RESET, not clamp: an unknown value falls back to the default overlay rather than to the nearest bound, which would silently mean "none" */ \
    /* -- Clouds location marker (v74) -- */ \
    BOOL      (clouds_show_location,         "clouds_show_location",         true)  /* draw a small marker at the user's location (weather_lat/weather_lon), which is the frame centre. On by default */ \
    /* -- ADS-B page (v68): tar1090/readsb aircraft feed -- */ \
    BOOL      (flights_enabled,              "flights_enabled",              false) \
    STR       (flights_url,                  "flights_url",                  "")    /* tar1090/readsb base URL, no trailing path (the poller appends /data/aircraft.json). Not a secret. STR only bounds the copy and NUL-terminates; the "empty or http(s)://" scheme rule lives in validate_config(), the same trust-boundary split radar_token uses */ \
    INT_RESET (flights_update_interval_s,    "flights_update_interval_s",    3,     2,     60)    /* poll cadence of a local receiver; 3 s tracks a jet smoothly. RESET, not clamp: an unset blob (0) falls back to the default rather than to the 2 s bound */ \
    INT_RESET (flights_range_nm,             "flights_range_nm",             50,    10,    250)   /* Radar Scope outer ring, nautical miles. RESET for the same reason: 0 from an unset blob must not become a 10 nm scope */ \
    INT_RESET (flights_min_el,               "flights_min_el",               10,    0,     89)    /* elevation gate in degrees; contacts below it are counted, not drawn. RESET: 0 is a legal value here, so only an out-of-range byte (>89) falls back to the default */ \
    INT_RESET (flights_up_azimuth,           "flights_up_azimuth",           0,     0,     359)   /* which azimuth is drawn "up". RESET rather than wrap: both writers (the on-device drag and the web field) already store a wrapped 0-359 value, so anything outside it is a stale byte and north-up is the safe fallback */ \
    INT_RESET (flights_mode,                 "flights_mode",                 0,     0,     2)     /* 0 = Sky Dome (default), 1 = Radar Scope, 2 = Board. RESET, not clamp: an unknown mode (stale byte, or a mode a future firmware knows) falls back to Sky Dome rather than to the nearest bound */     /* -- ADS-B route lookup (v69) -- */     BOOL      (flights_route_lookup,         "flights_route_lookup",         true)  /* look up each flight's origin-destination pair online (adsb.im). Default on; off keeps the ADS-B page fully local */ \
    /* -- ADS-B Radar Scope labels (v70) -- */ \
    INT_RESET (flights_label_max,            "flights_label_max",            64,    0,     64)    /* how many Radar Scope contacts get a text label: 0 = none, 1..63 = at most N, nearest first, 64 (= ADSB_MAX_AC) = every drawn contact. RESET, not clamp: a stale byte above 64 falls back to "all", never to a partial count the user never chose */ \
    /* -- ADS-B Radar Scope icon style (v71) -- */ \
    INT_RESET (flights_icon_style,           "flights_icon_style",           0,     0,     1)     /* Radar Scope contact glyph: 0 = arrows, 1 = aircraft silhouettes */ \
    /* -- Clock page layout (v72) -- */ \
    INT_RESET (clock_layout,                 "clock_layout",                 0,     0,     6)     /* 0 = Classic (default), 1 = Console 92, 2 = Broadside, 3 = Evensong, 4 = Blueprint, 5 = Transit Line, 6 = Night Network. RESET, not clamp: layouts are unordered names, so an unknown index falls back to Classic, never to whichever layout sits at the far bound */ \
    /* -- Global audio mute (v77) -- */ \
    BOOL      (audio_muted,                  "audio_muted",                  false) /* silence EVERY sound (voice alerts, event phrases, connection announcements, boot jingle) at the audio_alert enqueue gate; the web test/preview endpoints bypass it so the speaker stays testable while muted */ \
    /* -- Anonymous telemetry (v78) -- */ \
    BOOL      (telemetry_enabled,            "telemetry_enabled",            true)  /* one anonymous daily health report (fw version, uptime, crash counters, memory, feature bitmask; never URLs, names, coordinates or secrets). Fresh installs default ON; the v78 migration tail forces OFF for every upgrader (opt in) */

/* Apply every row's default value to *cfg. Called from set_defaults()
 * immediately after the memset(). Does not touch excluded/complex fields
 * (arrays, JSON blobs, secrets, cross-field page targets) — those keep their
 * existing hand-written assignments in app_config.c. */
void settings_defaults_apply(app_config_t *cfg);

/* Apply every row's range/reset check to *cfg in place. Called from
 * validate_config() in place of the equivalent per-field checks it replaces.
 * Returns true if any field was changed (out of range), matching the
 * `fixed` accumulation pattern used by the rest of validate_config(). */
bool settings_clamp_apply(app_config_t *cfg);

/* Serialize every row's current value into `root` under its json_key, one
 * cJSON_Add*ToObject call per row (bool->AddBool, string kinds->AddString,
 * everything else->AddNumber). Does not touch excluded/complex fields
 * (arrays, JSON blobs, secrets, cross-field page targets) —
 * callers that need those keep their existing hand-written cJSON_Add*
 * calls in web_handlers_config.c. */
void settings_json_serialize(const app_config_t *cfg, cJSON *root);

/* For every row: look up json_key in `root`. If present and its JSON type
 * matches the row's kind (number for INT/INT_RESET/FLT/FLT_RESET/ENUM, bool
 * for BOOL, string for STR/STR_RESET), assign into cfg->field and then apply
 * that row's clamp/reset rule (identical bound-check code to
 * settings_clamp_apply(), reused via the shared CLAMP_* macros in
 * settings_table.c). If the key is absent, or present with the wrong JSON
 * type, cfg->field is left completely untouched. */
void settings_json_parse(const cJSON *root, app_config_t *cfg);
