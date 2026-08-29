/**
 * @file control_registry.c
 * @brief Control API registry table, lookups, label resolution, apply
 *        callbacks, and the special non-config "page" item helpers.
 */

#include "control_registry.h"
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "bsp/esp-bsp.h"            /* bsp_display_brightness_set, bsp_display_lock/unlock */
#include "bsp/display.h"
#include "display_defs.h"           /* LVGL_LOCK_TIMEOUT_MS */
#include "lvgl.h"
#include "mqtt_ha.h"               /* mqtt_ha_publish_state */
#include "ui/themes.h"              /* themes_get_count, themes_get */
#include "ui/nina_dashboard.h"      /* nina_dashboard_apply_theme, get_active_page */
#include "ui/nina_image_page.h"     /* image_page_config_apply_live */
#include "ui/nina_nav_arbiter.h"    /* nav_arbiter_notify_topology_changed, nav_arbiter_is_pinned */
#include "ui/nina_adsb.h"           /* nina_adsb_config_changed */
#include "ui/page_registry.h"       /* page_ref_*, PAGE_REF_* */

/* ===================================================================== */
/* GET callbacks                                                          */
/* ===================================================================== */

static int get_brightness(const control_item_t *it, const app_config_t *c)            { (void)it; return c->brightness; }
static int get_color_brightness(const control_item_t *it, const app_config_t *c)      { (void)it; return c->color_brightness; }
static int get_theme(const control_item_t *it, const app_config_t *c)                 { (void)it; return c->theme_index; }
static int get_widget_style(const control_item_t *it, const app_config_t *c)          { (void)it; return c->widget_style; }
static int get_screen_rotation(const control_item_t *it, const app_config_t *c)       { (void)it; return c->screen_rotation; }

/* Per-page image bools (v61 split): getter + setter pairs generated together. */
#define IMG_BOOL_ACCESSORS(field) \
    static int  get_##field(const control_item_t *it, const app_config_t *c) { (void)it; return c->field ? 1 : 0; } \
    static void set_##field(const control_item_t *it, app_config_t *c, int v)  { (void)it; c->field = (v != 0); }
IMG_BOOL_ACCESSORS(goes_enabled)
IMG_BOOL_ACCESSORS(moon_enabled)
IMG_BOOL_ACCESSORS(solar_enabled)
IMG_BOOL_ACCESSORS(custom_enabled)
IMG_BOOL_ACCESSORS(goes_show_overlay)
IMG_BOOL_ACCESSORS(moon_show_overlay)
IMG_BOOL_ACCESSORS(solar_show_overlay)
IMG_BOOL_ACCESSORS(custom_show_overlay)
IMG_BOOL_ACCESSORS(goes_crop)
IMG_BOOL_ACCESSORS(solar_crop)
IMG_BOOL_ACCESSORS(custom_crop)
#undef IMG_BOOL_ACCESSORS

static int get_goes_orientation(const control_item_t *it, const app_config_t *c)           { (void)it; return c->goes_orientation; }
static int get_solar_orientation(const control_item_t *it, const app_config_t *c)          { (void)it; return c->solar_orientation; }
static int get_custom_orientation(const control_item_t *it, const app_config_t *c)         { (void)it; return c->custom_orientation; }
static int get_goes_vflip(const control_item_t *it, const app_config_t *c)                 { (void)it; return c->goes_vflip ? 1 : 0; }
static int get_goes_hflip(const control_item_t *it, const app_config_t *c)                 { (void)it; return c->goes_hflip ? 1 : 0; }
static int get_solar_vflip(const control_item_t *it, const app_config_t *c)                { (void)it; return c->solar_vflip ? 1 : 0; }
static int get_solar_hflip(const control_item_t *it, const app_config_t *c)                { (void)it; return c->solar_hflip ? 1 : 0; }
static int get_custom_vflip(const control_item_t *it, const app_config_t *c)               { (void)it; return c->custom_vflip ? 1 : 0; }
static int get_custom_hflip(const control_item_t *it, const app_config_t *c)               { (void)it; return c->custom_hflip ? 1 : 0; }
static int get_solar_band(const control_item_t *it, const app_config_t *c)                 { (void)it; return c->solar_band; }
static int get_goes_update_interval_s(const control_item_t *it, const app_config_t *c)     { (void)it; return c->goes_update_interval_s; }
static int get_custom_update_interval_s(const control_item_t *it, const app_config_t *c)   { (void)it; return c->custom_update_interval_s; }

static int get_flights_mode(const control_item_t *it, const app_config_t *c)               { (void)it; return c->flights_mode; }
static int get_flights_up_azimuth(const control_item_t *it, const app_config_t *c)         { (void)it; return c->flights_up_azimuth; }

static int get_auto_rotate_enabled(const control_item_t *it, const app_config_t *c)          { (void)it; return c->auto_rotate_enabled ? 1 : 0; }
static int get_auto_rotate_interval_s(const control_item_t *it, const app_config_t *c)       { (void)it; return c->auto_rotate_interval_s; }
static int get_auto_rotate_effect(const control_item_t *it, const app_config_t *c)           { (void)it; return c->auto_rotate_effect; }
static int get_auto_rotate_skip_disconnected(const control_item_t *it, const app_config_t *c){ (void)it; return c->auto_rotate_skip_disconnected ? 1 : 0; }
static int get_alert_flash_enabled(const control_item_t *it, const app_config_t *c)          { (void)it; return c->alert_flash_enabled ? 1 : 0; }
static int get_audio_muted(const control_item_t *it, const app_config_t *c)                  { (void)it; return c->audio_muted ? 1 : 0; }
static int get_alert_voice_volume(const control_item_t *it, const app_config_t *c)           { (void)it; return c->alert_voice_volume; }
static int get_screen_sleep_enabled(const control_item_t *it, const app_config_t *c)         { (void)it; return c->screen_sleep_enabled ? 1 : 0; }
static int get_screen_sleep_timeout_s(const control_item_t *it, const app_config_t *c)       { (void)it; return c->screen_sleep_timeout_s; }
static int get_idle_indicator_enabled(const control_item_t *it, const app_config_t *c)       { (void)it; return c->idle_indicator_enabled ? 1 : 0; }
static int get_nav_grace_s(const control_item_t *it, const app_config_t *c)                  { (void)it; return c->nav_grace_s; }
static int get_update_rate_s(const control_item_t *it, const app_config_t *c)                { (void)it; return c->update_rate_s; }
static int get_idle_poll_interval_s(const control_item_t *it, const app_config_t *c)         { (void)it; return c->idle_poll_interval_s; }
static int get_graph_update_interval_s(const control_item_t *it, const app_config_t *c)      { (void)it; return c->graph_update_interval_s; }
static int get_connection_timeout_s(const control_item_t *it, const app_config_t *c)         { (void)it; return c->connection_timeout_s; }
static int get_toast_duration_s(const control_item_t *it, const app_config_t *c)             { (void)it; return c->toast_duration_s; }

/* PAGE item get — current page id (non-config). */
static int get_page(const control_item_t *it, const app_config_t *c)
{
    (void)it;
    (void)c;
    return control_page_current_id();
}

/* PIN item get — live navigation-pin state (non-config; cfg ignored). */
static int get_nav_pinned(const control_item_t *it, const app_config_t *c)
{
    (void)it;
    (void)c;
    return nav_arbiter_is_pinned() ? 1 : 0;
}

/* ===================================================================== */
/* SET callbacks (value already clamped by caller)                        */
/* ===================================================================== */

static void set_brightness(const control_item_t *it, app_config_t *c, int v)            { (void)it; c->brightness = v; }
static void set_color_brightness(const control_item_t *it, app_config_t *c, int v)      { (void)it; c->color_brightness = v; }
static void set_theme(const control_item_t *it, app_config_t *c, int v)                 { (void)it; c->theme_index = v; }
static void set_widget_style(const control_item_t *it, app_config_t *c, int v)          { (void)it; c->widget_style = (uint8_t)v; }
static void set_screen_rotation(const control_item_t *it, app_config_t *c, int v)       { (void)it; c->screen_rotation = (uint8_t)v; }

static void set_goes_orientation(const control_item_t *it, app_config_t *c, int v)           { (void)it; c->goes_orientation = (uint8_t)v; }
static void set_solar_orientation(const control_item_t *it, app_config_t *c, int v)          { (void)it; c->solar_orientation = (uint8_t)v; }
static void set_custom_orientation(const control_item_t *it, app_config_t *c, int v)         { (void)it; c->custom_orientation = (uint8_t)v; }
static void set_goes_vflip(const control_item_t *it, app_config_t *c, int v)                 { (void)it; c->goes_vflip = (uint8_t)(v != 0); }
static void set_goes_hflip(const control_item_t *it, app_config_t *c, int v)                 { (void)it; c->goes_hflip = (uint8_t)(v != 0); }
static void set_solar_vflip(const control_item_t *it, app_config_t *c, int v)                { (void)it; c->solar_vflip = (uint8_t)(v != 0); }
static void set_solar_hflip(const control_item_t *it, app_config_t *c, int v)                { (void)it; c->solar_hflip = (uint8_t)(v != 0); }
static void set_custom_vflip(const control_item_t *it, app_config_t *c, int v)               { (void)it; c->custom_vflip = (uint8_t)(v != 0); }
static void set_custom_hflip(const control_item_t *it, app_config_t *c, int v)               { (void)it; c->custom_hflip = (uint8_t)(v != 0); }
static void set_solar_band(const control_item_t *it, app_config_t *c, int v)                 { (void)it; c->solar_band = (uint8_t)v; }
static void set_goes_update_interval_s(const control_item_t *it, app_config_t *c, int v)     { (void)it; c->goes_update_interval_s = (uint16_t)v; }
static void set_custom_update_interval_s(const control_item_t *it, app_config_t *c, int v)   { (void)it; c->custom_update_interval_s = (uint16_t)v; }

static void set_flights_mode(const control_item_t *it, app_config_t *c, int v)               { (void)it; c->flights_mode = (uint8_t)v; }
static void set_flights_up_azimuth(const control_item_t *it, app_config_t *c, int v)         { (void)it; c->flights_up_azimuth = (uint16_t)v; }

static void set_auto_rotate_enabled(const control_item_t *it, app_config_t *c, int v)          { (void)it; c->auto_rotate_enabled = (v != 0); }
static void set_auto_rotate_interval_s(const control_item_t *it, app_config_t *c, int v)       { (void)it; c->auto_rotate_interval_s = (uint16_t)v; }
static void set_auto_rotate_effect(const control_item_t *it, app_config_t *c, int v)           { (void)it; c->auto_rotate_effect = (uint8_t)v; }
static void set_auto_rotate_skip_disconnected(const control_item_t *it, app_config_t *c, int v){ (void)it; c->auto_rotate_skip_disconnected = (v != 0); }
static void set_alert_flash_enabled(const control_item_t *it, app_config_t *c, int v)          { (void)it; c->alert_flash_enabled = (v != 0); }
static void set_audio_muted(const control_item_t *it, app_config_t *c, int v)                  { (void)it; c->audio_muted = (v != 0); }
static void set_alert_voice_volume(const control_item_t *it, app_config_t *c, int v)           { (void)it; c->alert_voice_volume = (uint8_t)v; }
static void set_screen_sleep_enabled(const control_item_t *it, app_config_t *c, int v)         { (void)it; c->screen_sleep_enabled = (v != 0); }
static void set_screen_sleep_timeout_s(const control_item_t *it, app_config_t *c, int v)       { (void)it; c->screen_sleep_timeout_s = (uint16_t)v; }
static void set_idle_indicator_enabled(const control_item_t *it, app_config_t *c, int v)       { (void)it; c->idle_indicator_enabled = (v != 0); }
static void set_nav_grace_s(const control_item_t *it, app_config_t *c, int v)                  { (void)it; c->nav_grace_s = (uint16_t)v; }
static void set_update_rate_s(const control_item_t *it, app_config_t *c, int v)                { (void)it; c->update_rate_s = (uint8_t)v; }
static void set_idle_poll_interval_s(const control_item_t *it, app_config_t *c, int v)         { (void)it; c->idle_poll_interval_s = (uint8_t)v; }
static void set_graph_update_interval_s(const control_item_t *it, app_config_t *c, int v)      { (void)it; c->graph_update_interval_s = (uint8_t)v; }
static void set_connection_timeout_s(const control_item_t *it, app_config_t *c, int v)         { (void)it; c->connection_timeout_s = (uint8_t)v; }
static void set_toast_duration_s(const control_item_t *it, app_config_t *c, int v)             { (void)it; c->toast_duration_s = (uint8_t)v; }

/* ===================================================================== */
/* APPLY callbacks (run AFTER app_config_save; receive prev + saved cur)  */
/* ===================================================================== */

static void apply_brightness(const app_config_t *prev, const app_config_t *cur)
{
    (void)prev;
    bsp_display_brightness_set(cur->brightness);
    mqtt_ha_publish_state();
}

static void apply_color_brightness(const app_config_t *prev, const app_config_t *cur)
{
    (void)prev;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_apply_theme(cur->theme_index);
        bsp_display_unlock();
    }
    mqtt_ha_publish_state();
}

static void apply_theme(const app_config_t *prev, const app_config_t *cur)
{
    (void)prev;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_apply_theme(cur->theme_index);
        bsp_display_unlock();
    }
}

static void apply_widget_style(const app_config_t *prev, const app_config_t *cur)
{
    /* Re-applying the theme picks up the new widget_style. */
    (void)prev;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_apply_theme(cur->theme_index);
        bsp_display_unlock();
    }
}

static void apply_screen_rotation(const app_config_t *prev, const app_config_t *cur)
{
    (void)prev;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        display_rotation_apply(cur->screen_rotation);
        bsp_display_unlock();
    }
}

static void apply_image_display(const app_config_t *prev, const app_config_t *cur)
{
    image_page_config_apply_live(prev, cur, false);
}

/* ADS-B view / up-direction: the page keeps its own copy so an on-device drag
 * can repaint without a config write per touch event, so a config-side change
 * has to tell it to re-read. Safe when the page does not exist yet. */
static void apply_adsb_view(const app_config_t *prev, const app_config_t *cur)
{
    (void)prev;
    (void)cur;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_adsb_config_changed();
        bsp_display_unlock();
    }
}

static void apply_auto_rotate_enabled(const app_config_t *prev, const app_config_t *cur)
{
    /* app_config_save() normalizes nav mode exclusivity internally; just nudge
     * the arbiter to re-resolve the page against the new mode on the next tick. */
    (void)prev;
    (void)cur;
    nav_arbiter_notify_topology_changed();
}

/* ===================================================================== */
/* Enum label arrays                                                      */
/* ===================================================================== */

static const char *const rot_labels[]    = { "0deg", "90deg", "180deg", "270deg" };
static const char *const orient_labels[] = { "0deg", "90deg", "180deg", "270deg" };
static const char *const effect_labels[] = { "Instant", "Fade", "Slide Left", "Slide Right" };
static const char *const flights_mode_labels[] = { "Sky Dome", "Radar Scope", "Board" };

/* ===================================================================== */
/* The registry table                                                     */
/* ===================================================================== */

#define LBL(a) (a), (int)(sizeof(a) / sizeof((a)[0]))

static const control_item_t s_items[] = {
    /* ---- Display ---- */
    { "brightness",       CTRL_TYPE_INT,  0, 100, 5, NULL, 0, get_brightness,       set_brightness,       apply_brightness },
    { "color_brightness", CTRL_TYPE_INT,  0, 100, 5, NULL, 0, get_color_brightness, set_color_brightness, apply_color_brightness },
    /* theme: runtime max sentinel (-1); labels resolved in control_item_label(). */
    { "theme",            CTRL_TYPE_ENUM, 0,  -1, 1, NULL, 0, get_theme,            set_theme,            apply_theme },
    /* widget_style: runtime max sentinel (-1); label is the numeric index. */
    { "widget_style",     CTRL_TYPE_ENUM, 0,  -1, 1, NULL, 0, get_widget_style,     set_widget_style,     apply_widget_style },
    { "screen_rotation",  CTRL_TYPE_ENUM, 0,   3, 1, LBL(rot_labels), get_screen_rotation, set_screen_rotation, apply_screen_rotation },

    /* ---- Image pages (all apply_image_display) ---- */
    { "goes_enabled",        CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_goes_enabled,        set_goes_enabled,        apply_image_display },
    { "moon_enabled",        CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_moon_enabled,        set_moon_enabled,        apply_image_display },
    { "solar_enabled",       CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_solar_enabled,       set_solar_enabled,       apply_image_display },
    { "custom_enabled",      CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_custom_enabled,      set_custom_enabled,      apply_image_display },
    { "goes_show_overlay",   CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_goes_show_overlay,   set_goes_show_overlay,   apply_image_display },
    { "moon_show_overlay",   CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_moon_show_overlay,   set_moon_show_overlay,   apply_image_display },
    { "solar_show_overlay",  CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_solar_show_overlay,  set_solar_show_overlay,  apply_image_display },
    { "custom_show_overlay", CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_custom_show_overlay, set_custom_show_overlay, apply_image_display },
    { "goes_crop",           CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_goes_crop,           set_goes_crop,           apply_image_display },
    { "solar_crop",          CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_solar_crop,          set_solar_crop,          apply_image_display },
    { "custom_crop",         CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_custom_crop,         set_custom_crop,         apply_image_display },
    { "goes_orientation",           CTRL_TYPE_ENUM, 0, 3, 1, LBL(orient_labels), get_goes_orientation,   set_goes_orientation,   apply_image_display },
    { "solar_orientation",          CTRL_TYPE_ENUM, 0, 3, 1, LBL(orient_labels), get_solar_orientation,  set_solar_orientation,  apply_image_display },
    { "custom_orientation",         CTRL_TYPE_ENUM, 0, 3, 1, LBL(orient_labels), get_custom_orientation, set_custom_orientation, apply_image_display },
    { "goes_vflip",                 CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_goes_vflip,         set_goes_vflip,         apply_image_display },
    { "goes_hflip",                 CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_goes_hflip,         set_goes_hflip,         apply_image_display },
    { "solar_vflip",                CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_solar_vflip,        set_solar_vflip,        apply_image_display },
    { "solar_hflip",                CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_solar_hflip,        set_solar_hflip,        apply_image_display },
    { "custom_vflip",               CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_custom_vflip,       set_custom_vflip,       apply_image_display },
    { "custom_hflip",               CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_custom_hflip,       set_custom_hflip,       apply_image_display },
    /* solar_band: enum with no labels -> control_item_label() returns NULL so the
     * handler formats the numeric band index as the label string. */
    { "solar_band",                 CTRL_TYPE_ENUM, 0, 23, 1, NULL, 0, get_solar_band,             set_solar_band,             apply_image_display },
    { "goes_update_interval_s",     CTRL_TYPE_INT,  300, 7200, 300, NULL, 0, get_goes_update_interval_s,   set_goes_update_interval_s,   apply_image_display },
    { "custom_update_interval_s",   CTRL_TYPE_INT,  10,  7200, 60,  NULL, 0, get_custom_update_interval_s, set_custom_update_interval_s, apply_image_display },

    /* ---- ADS-B page (both apply_adsb_view) ----
     * up_azimuth steps 15 deg so a keypad "adjust" turns the view in useful
     * bites; cycle wraps 359 -> 0 through the registry's own wrap. */
    { "flights_mode",        CTRL_TYPE_ENUM, 0,   2, 1,  LBL(flights_mode_labels), get_flights_mode,      set_flights_mode,      apply_adsb_view },
    { "flights_up_azimuth",  CTRL_TYPE_INT,  0, 359, 15, NULL, 0, get_flights_up_azimuth, set_flights_up_azimuth, apply_adsb_view },

    /* ---- Behavior ---- */
    { "auto_rotate_enabled",          CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_auto_rotate_enabled,          set_auto_rotate_enabled,          apply_auto_rotate_enabled },
    { "auto_rotate_interval_s",       CTRL_TYPE_INT,  1, 3600, 5, NULL, 0, get_auto_rotate_interval_s,    set_auto_rotate_interval_s,       NULL },
    { "auto_rotate_effect",           CTRL_TYPE_ENUM, 0, 3, 1, LBL(effect_labels), get_auto_rotate_effect, set_auto_rotate_effect,         NULL },
    { "auto_rotate_skip_disconnected",CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_auto_rotate_skip_disconnected,set_auto_rotate_skip_disconnected,NULL },
    { "alert_flash_enabled",          CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_alert_flash_enabled,          set_alert_flash_enabled,          NULL },
    /* audio_muted: no apply callback — the audio_alert enqueue gate reads
     * config live at speak time, so the saved value is effective at once. */
    { "audio_muted",                  CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_audio_muted,                  set_audio_muted,                  NULL },
    /* alert_voice_volume: same story — audio_alert reads the volume out of
     * config when it opens the codec, so no apply callback is needed. */
    { "alert_voice_volume",           CTRL_TYPE_INT,  0, 100, 5, NULL, 0, get_alert_voice_volume,           set_alert_voice_volume,           NULL },
    { "screen_sleep_enabled",         CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_screen_sleep_enabled,         set_screen_sleep_enabled,         NULL },
    { "screen_sleep_timeout_s",       CTRL_TYPE_INT,  10, 3600, 30, NULL, 0, get_screen_sleep_timeout_s,  set_screen_sleep_timeout_s,       NULL },
    { "idle_indicator_enabled",       CTRL_TYPE_BOOL, 0, 1, 1, NULL, 0, get_idle_indicator_enabled,       set_idle_indicator_enabled,       NULL },
    { "nav_grace_s",                  CTRL_TYPE_INT,  10, 300, 10, NULL, 0, get_nav_grace_s,              set_nav_grace_s,                  NULL },
    { "update_rate_s",                CTRL_TYPE_INT,  1, 10, 1, NULL, 0, get_update_rate_s,                set_update_rate_s,                NULL },
    { "idle_poll_interval_s",         CTRL_TYPE_INT,  5, 120, 5, NULL, 0, get_idle_poll_interval_s,        set_idle_poll_interval_s,         NULL },
    { "graph_update_interval_s",      CTRL_TYPE_INT,  2, 30, 1, NULL, 0, get_graph_update_interval_s,      set_graph_update_interval_s,      NULL },
    { "connection_timeout_s",         CTRL_TYPE_INT,  2, 30, 1, NULL, 0, get_connection_timeout_s,         set_connection_timeout_s,         NULL },
    { "toast_duration_s",             CTRL_TYPE_INT,  3, 30, 1, NULL, 0, get_toast_duration_s,             set_toast_duration_s,             NULL },

    /* ---- Special: current page (NON-CONFIG; routes through the nav arbiter) ----
     * Does not snapshot/save. set=NULL, apply=NULL. vmax=-1 runtime sentinel
     * (resolves to PAGE_REF_ID_MAX-1). The handler performs set/cycle specially
     * via control_page_set_by_id() / control_page_cycle_next(). */
    { "page",                         CTRL_TYPE_PAGE, 0, -1, 1, NULL, 0, get_page,                         NULL,                             NULL },

    /* ---- Special: navigation pin (NON-CONFIG; arbiter-backed bool) ----
     * Reported as a bool with On/Off labels. Does not snapshot/save: the handler
     * special-cases it (like the page item) and drives nav_arbiter_set_pin().
     * set=NULL, apply=NULL; get returns the live arbiter pin state. */
    { "nav_pinned",                   CTRL_TYPE_PIN,  0, 1, 1, NULL, 0, get_nav_pinned,                   NULL,                             NULL },
};

#undef LBL

#define CONTROL_ITEM_COUNT ((int)(sizeof(s_items) / sizeof(s_items[0])))

/* ===================================================================== */
/* Lookups                                                                */
/* ===================================================================== */

int control_registry_count(void)
{
    return CONTROL_ITEM_COUNT;
}

const control_item_t *control_registry_get(int i)
{
    if (i < 0 || i >= CONTROL_ITEM_COUNT) {
        return NULL;
    }
    return &s_items[i];
}

const control_item_t *control_registry_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < CONTROL_ITEM_COUNT; i++) {
        if (strcmp(s_items[i].name, name) == 0) {
            return &s_items[i];
        }
    }
    return NULL;
}

/* ===================================================================== */
/* Effective max + label                                                  */
/* ===================================================================== */

int control_item_effective_max(const control_item_t *item)
{
    if (!item) {
        return 0;
    }
    if (strcmp(item->name, "theme") == 0) {
        int n = themes_get_count();
        return (n > 0) ? (n - 1) : 0;
    }
    if (strcmp(item->name, "widget_style") == 0) {
        return WIDGET_STYLE_COUNT - 1;
    }
    if (item->type == CTRL_TYPE_PAGE) {
        return (int)PAGE_REF_ID_MAX - 1;
    }
    return item->vmax;
}

const char *control_item_label(const control_item_t *item, int value)
{
    if (!item) {
        return NULL;
    }
    if (item->type == CTRL_TYPE_BOOL || item->type == CTRL_TYPE_PIN) {
        return value ? "On" : "Off";
    }
    if (strcmp(item->name, "theme") == 0) {
        const theme_t *t = themes_get(value);
        return t ? t->name : NULL;
    }
    if (item->type == CTRL_TYPE_PAGE) {
        const page_ref_entry_t *e = page_ref_by_id((page_ref_t)value);
        return e ? e->label : NULL;
    }
    if (item->type == CTRL_TYPE_ENUM && item->labels &&
        value >= 0 && value < item->label_count) {
        return item->labels[value];
    }
    return NULL;  /* int / unlabeled enum -> caller formats the number */
}

/* ===================================================================== */
/* PAGE item helpers (non-config; route through the nav arbiter)          */
/* ===================================================================== */

static int s_page_cycle_anchor = -1;   /* last page id this cycle navigated to */

/* Map the current absolute page index to a page_ref id. */
static int control_current_page_id(void)
{
    int cur_idx = nina_dashboard_get_active_page();

    for (int i = 0; i < page_ref_count(); i++) {
        const page_ref_entry_t *e = page_ref_get(i);
        if (e && e->page_idx == cur_idx && e->targetable) {
            return (int)e->id;
        }
    }
    return (int)PAGE_REF_SUMMARY;
}

int control_page_current_id(void)
{
    return control_current_page_id();
}

bool control_page_set_by_id(int id)
{
    return page_ref_navigate((page_ref_t)id);
}

int control_page_cycle_next(void)
{
    int count = page_ref_count();
    int live = control_page_current_id();
    if (count <= 0) {
        return live;
    }

    /* Advance from the last issued target (anchor), not the laggy live page.
     * Reconcile to live only when the user has clearly moved elsewhere by
     * other means to a real, targetable page. */
    int base = live;
    if (s_page_cycle_anchor >= 0 &&
        page_ref_by_id((page_ref_t)s_page_cycle_anchor) != NULL) {
        base = s_page_cycle_anchor;
        const page_ref_entry_t *le = page_ref_by_id((page_ref_t)live);
        if (live != s_page_cycle_anchor && le && le->targetable) {
            base = live;
        }
    }

    /* Find base's table index (start point for the search). */
    int start = -1;
    for (int i = 0; i < count; i++) {
        const page_ref_entry_t *e = page_ref_get(i);
        if (e && (int)e->id == base) {
            start = i;
            break;
        }
    }
    if (start < 0) {
        start = 0;
    }

    for (int n = 1; n <= count; n++) {
        int idx = (start + n) % count;
        const page_ref_entry_t *e = page_ref_get(idx);
        if (e && e->targetable && page_ref_is_available(e->id)) {
            page_ref_navigate(e->id);
            s_page_cycle_anchor = (int)e->id;
            return (int)e->id;
        }
    }
    return base;
}
