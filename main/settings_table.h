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
    STR       (weather_api_key,              "weather_api_key",              "") \
    FLT       (weather_lat,                  "weather_lat",                  0.0f,  -90.0f, 90.0f)  /* no prior clamp; obviously-correct latitude bound */ \
    FLT       (weather_lon,                  "weather_lon",                  0.0f,  -180.0f,180.0f) /* no prior clamp; obviously-correct longitude bound */ \
    STR       (weather_location_name,        "weather_location_name",        "") \
    INT       (weather_poll_interval_s,      "weather_poll_interval_s",      900,   900,   3600)  /* no prior clamp in validate_config; bound sourced from POST handler */ \
    INT       (weather_units,                "weather_units",                0,     0,     1) \
    INT       (weather_time_format,          "weather_time_format",          0,     0,     1) \
    /* -- Idle page override (target excluded — cross-field page-registry semantics) -- */ \
    BOOL      (idle_page_override_enabled,   "idle_page_override_enabled",   false) \
    BOOL      (idle_page_persistent,         "idle_page_persistent",         false) /* retired/unread; not currently serialized */ \
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
    INT_RESET (solar_band,                   "solar_band",                   0,     0,     17) \
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
    INT_RESET (custom_orientation,           "custom_orientation",           0,     0,     3) \
    INT_RESET (custom_update_interval_s,     "custom_update_interval_s",     60,    10,    7200)

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
 * (arrays, JSON blobs, secrets, cross-field page targets, idle_page_persistent) —
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
