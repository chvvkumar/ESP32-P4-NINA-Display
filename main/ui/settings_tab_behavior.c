/**
 * @file settings_tab_behavior.c
 * @brief Behavior tab for the settings tabview.
 *
 * Contains four cards:
 *   - Hardware:      screen rotation dropdown, backlight slider
 *   - Power Mgmt:    screen sleep, deep sleep, WiFi power save
 *   - Polling:       data rate, graph rate, connection timeout, toast duration
 *   - Notifications: border flash alerts
 */

#include "settings_tab_behavior.h"
#include "nina_settings_tabview.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include "display_defs.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_wifi.h"

#include <stdio.h>
#include <string.h>

/* ── Tab root ────────────────────────────────────────────────────────── */
static lv_obj_t *tab_root = NULL;

/* ── Hardware card ───────────────────────────────────────────────────── */
static lv_obj_t *dd_rotation       = NULL;
static lv_obj_t *slider_backlight  = NULL;
static lv_obj_t *lbl_backlight_val = NULL;

/* ── Power management card ───────────────────────────────────────────── */
static lv_obj_t *sw_screen_sleep      = NULL;
static lv_obj_t *cont_sleep_opts      = NULL;   /* hidden when screen sleep off */
static lv_obj_t *lbl_sleep_timeout    = NULL;
static lv_obj_t *lbl_idle_poll        = NULL;
static lv_obj_t *sw_wifi_power_save   = NULL;
static lv_obj_t *sw_deep_sleep        = NULL;
static lv_obj_t *cont_deep_sleep_opts = NULL;   /* hidden when deep sleep off */
static lv_obj_t *lbl_wake_timer       = NULL;
static lv_obj_t *sw_auto_power_off    = NULL;

/* ── Polling card ────────────────────────────────────────────────────── */
static lv_obj_t *lbl_data_rate        = NULL;
static lv_obj_t *lbl_graph_rate       = NULL;
static lv_obj_t *lbl_conn_timeout     = NULL;
static lv_obj_t *lbl_toast_duration   = NULL;

/* ── Notifications card ──────────────────────────────────────────────── */
static lv_obj_t *sw_alert_flash       = NULL;

/* ── Rotation dropdown options ───────────────────────────────────────── */
static const char *rotation_opts = "0\xc2\xb0\n90\xc2\xb0\n180\xc2\xb0\n270\xc2\xb0";

/* ════════════════════════════════════════════════════════════════════════
 *  Helper — format sleep timeout with appropriate unit
 * ════════════════════════════════════════════════════════════════════════ */

static void update_sleep_timeout_label(void) {
    app_config_t *cfg = app_config_get();
    if (cfg->screen_sleep_timeout_s >= 60) {
        lv_label_set_text_fmt(lbl_sleep_timeout, "%d min",
                              cfg->screen_sleep_timeout_s / 60);
    } else {
        lv_label_set_text_fmt(lbl_sleep_timeout, "%d s",
                              cfg->screen_sleep_timeout_s);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Hardware Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void rotation_changed_cb(lv_event_t *e) {
    LV_UNUSED(e);
    uint32_t idx = lv_dropdown_get_selected(dd_rotation);
    app_config_get()->screen_rotation = (uint8_t)idx;
    lv_display_set_rotation(lv_display_get_default(), idx);
    settings_mark_dirty(false);
}

static void backlight_changed_cb(lv_event_t *e) {
    LV_UNUSED(e);
    int val = lv_slider_get_value(slider_backlight);
    app_config_get()->brightness = val;
    bsp_display_brightness_set(val);
    lv_label_set_text_fmt(lbl_backlight_val, "%d%%", val);
    settings_mark_dirty(false);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Power Management Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void screen_sleep_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bool on = lv_obj_has_state(sw_screen_sleep, LV_STATE_CHECKED);
    app_config_get()->screen_sleep_enabled = on;
    if (on) lv_obj_clear_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    settings_mark_dirty(false);
}

static void sleep_timeout_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->screen_sleep_timeout_s > 120 ? 30 : 10;
    int val = (int)cfg->screen_sleep_timeout_s - step;
    if (val < 10) val = 10;
    cfg->screen_sleep_timeout_s = (uint16_t)val;
    update_sleep_timeout_label();
    settings_mark_dirty(false);
}

static void sleep_timeout_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->screen_sleep_timeout_s >= 120 ? 30 : 10;
    int val = (int)cfg->screen_sleep_timeout_s + step;
    if (val > 3600) val = 3600;
    cfg->screen_sleep_timeout_s = (uint16_t)val;
    update_sleep_timeout_label();
    settings_mark_dirty(false);
}

static void idle_poll_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->idle_poll_interval_s > 5) {
        cfg->idle_poll_interval_s -= 5;
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);
        settings_mark_dirty(false);
    }
}

static void idle_poll_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->idle_poll_interval_s < 120) {
        cfg->idle_poll_interval_s += 5;
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);
        settings_mark_dirty(false);
    }
}

static void wifi_power_save_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bool on = lv_obj_has_state(sw_wifi_power_save, LV_STATE_CHECKED);
    app_config_get()->wifi_power_save = on;
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    settings_mark_dirty(false);
}

static void deep_sleep_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bool on = lv_obj_has_state(sw_deep_sleep, LV_STATE_CHECKED);
    app_config_get()->deep_sleep_enabled = on;
    if (on) lv_obj_clear_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
    settings_mark_dirty(false);
}

static void wake_timer_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
    if (hours > 0) {
        hours--;
        cfg->deep_sleep_wake_timer_s = hours * 3600;
        lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
        settings_mark_dirty(false);
    }
}

static void wake_timer_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
    if (hours < 72) {
        hours++;
        cfg->deep_sleep_wake_timer_s = hours * 3600;
        lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
        settings_mark_dirty(false);
    }
}

static void auto_power_off_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_get()->deep_sleep_on_idle =
        lv_obj_has_state(sw_auto_power_off, LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Polling Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void data_rate_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->update_rate_s > 1) {
        cfg->update_rate_s--;
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);
        settings_mark_dirty(false);
    }
}

static void data_rate_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->update_rate_s < 10) {
        cfg->update_rate_s++;
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);
        settings_mark_dirty(false);
    }
}

static void graph_rate_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->graph_update_interval_s > 2) {
        cfg->graph_update_interval_s--;
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);
        settings_mark_dirty(false);
    }
}

static void graph_rate_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->graph_update_interval_s < 30) {
        cfg->graph_update_interval_s++;
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);
        settings_mark_dirty(false);
    }
}

static void conn_timeout_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->connection_timeout_s > 2) {
        cfg->connection_timeout_s--;
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);
        settings_mark_dirty(false);
    }
}

static void conn_timeout_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->connection_timeout_s < 30) {
        cfg->connection_timeout_s++;
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);
        settings_mark_dirty(false);
    }
}

static void toast_duration_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->toast_duration_s > 3) {
        cfg->toast_duration_s--;
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
        settings_mark_dirty(false);
    }
}

static void toast_duration_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->toast_duration_s < 30) {
        cfg->toast_duration_s++;
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
        settings_mark_dirty(false);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Notifications Card Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void alert_flash_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_get()->alert_flash_enabled =
        lv_obj_has_state(sw_alert_flash, LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Helper — create a labeled stepper row inside a card
 * ════════════════════════════════════════════════════════════════════════ */

static void make_labeled_stepper(lv_obj_t *card, const char *text,
                                 lv_event_cb_t minus_cb, lv_event_cb_t plus_cb,
                                 lv_obj_t **out_label)
{
    lv_obj_t *row = settings_make_row(card);

    int gb = app_config_get()->color_brightness;
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    lv_obj_t *btn_m = NULL, *lbl_val = NULL, *btn_p = NULL;
    settings_make_stepper(row, &btn_m, &lbl_val, &btn_p);

    lv_obj_add_event_cb(btn_m, minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_p, plus_cb,  LV_EVENT_CLICKED, NULL);

    if (out_label) *out_label = lbl_val;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Tab Creation
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_behavior_destroy(void) {
    tab_root = NULL;
    dd_rotation = NULL;
    slider_backlight = NULL;
    lbl_backlight_val = NULL;
    sw_screen_sleep = NULL;
    cont_sleep_opts = NULL;
    lbl_sleep_timeout = NULL;
    lbl_idle_poll = NULL;
    sw_wifi_power_save = NULL;
    sw_deep_sleep = NULL;
    cont_deep_sleep_opts = NULL;
    lbl_wake_timer = NULL;
    sw_auto_power_off = NULL;
    lbl_data_rate = NULL;
    lbl_graph_rate = NULL;
    lbl_conn_timeout = NULL;
    lbl_toast_duration = NULL;
    sw_alert_flash = NULL;
}

void settings_tab_behavior_create(lv_obj_t *parent) {
    tab_root = parent;
    app_config_t *cfg = app_config_get();
    int gb = cfg->color_brightness;

    /* ── Hardware Card ───────────────────────────────────────────────── */
    {
        lv_obj_t *card = settings_make_card(parent, "HARDWARE");

        /* Rotation row */
        {
            lv_obj_t *row = settings_make_row(card);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, "Rotation");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            dd_rotation = lv_dropdown_create(row);
            lv_dropdown_set_options(dd_rotation, rotation_opts);
            lv_obj_set_width(dd_rotation, 160);
            lv_dropdown_set_selected(dd_rotation, cfg->screen_rotation);

            /* Style the dropdown */
            if (current_theme) {
                lv_obj_set_style_bg_color(dd_rotation, lv_color_hex(current_theme->bento_bg), 0);
                lv_obj_set_style_bg_opa(dd_rotation, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(dd_rotation,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
                lv_obj_set_style_border_color(dd_rotation, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_border_width(dd_rotation, 1, 0);
            }

            lv_obj_add_event_cb(dd_rotation, rotation_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        settings_make_divider(card);

        /* Backlight row */
        {
            lv_obj_t *row = settings_make_row(card);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, "Backlight");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
            lv_obj_set_style_min_width(lbl, 100, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            /* Slider + value in a sub-container */
            lv_obj_t *ctrl = lv_obj_create(row);
            lv_obj_remove_style_all(ctrl);
            lv_obj_set_flex_grow(ctrl, 1);
            lv_obj_set_height(ctrl, 50);
            lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(ctrl, 10, 0);

            slider_backlight = lv_slider_create(ctrl);
            lv_obj_set_flex_grow(slider_backlight, 1);
            lv_obj_set_height(slider_backlight, 16);
            lv_slider_set_range(slider_backlight, 0, 100);
            lv_slider_set_value(slider_backlight, cfg->brightness, LV_ANIM_OFF);
            lv_obj_set_style_radius(slider_backlight, 8, 0);
            lv_obj_set_style_radius(slider_backlight, 8, LV_PART_INDICATOR);
            lv_obj_set_style_radius(slider_backlight, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_pad_all(slider_backlight, 8, LV_PART_KNOB);
            if (current_theme) {
                lv_obj_set_style_bg_color(slider_backlight, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_bg_opa(slider_backlight, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(slider_backlight, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
                lv_obj_set_style_bg_opa(slider_backlight, LV_OPA_COVER, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(slider_backlight, lv_color_hex(current_theme->progress_color), LV_PART_KNOB);
                lv_obj_set_style_bg_opa(slider_backlight, LV_OPA_COVER, LV_PART_KNOB);
            }
            lv_obj_add_event_cb(slider_backlight, backlight_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

            lbl_backlight_val = lv_label_create(ctrl);
            lv_obj_set_style_text_font(lbl_backlight_val, &lv_font_montserrat_20, 0);
            lv_obj_set_style_min_width(lbl_backlight_val, 50, 0);
            lv_obj_set_style_text_align(lbl_backlight_val, LV_TEXT_ALIGN_RIGHT, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl_backlight_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }
            lv_label_set_text_fmt(lbl_backlight_val, "%d%%", cfg->brightness);
        }
    }

    /* ── Power Management Card ───────────────────────────────────────── */
    {
        lv_obj_t *card = settings_make_card(parent, "POWER MANAGEMENT");

        /* Screen sleep toggle */
        settings_make_toggle_row(card, "Screen Sleep", &sw_screen_sleep);
        if (cfg->screen_sleep_enabled) {
            lv_obj_add_state(sw_screen_sleep, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_screen_sleep, screen_sleep_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* Collapsible sleep options container */
        cont_sleep_opts = lv_obj_create(card);
        lv_obj_remove_style_all(cont_sleep_opts);
        lv_obj_set_width(cont_sleep_opts, LV_PCT(100));
        lv_obj_set_height(cont_sleep_opts, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cont_sleep_opts, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cont_sleep_opts, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cont_sleep_opts, 4, 0);
        if (!cfg->screen_sleep_enabled) {
            lv_obj_add_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }

        /* Sleep timeout stepper */
        make_labeled_stepper(cont_sleep_opts, "Timeout",
                             sleep_timeout_minus_cb, sleep_timeout_plus_cb,
                             &lbl_sleep_timeout);
        update_sleep_timeout_label();

        /* Idle poll rate stepper */
        make_labeled_stepper(cont_sleep_opts, "Idle Poll",
                             idle_poll_minus_cb, idle_poll_plus_cb,
                             &lbl_idle_poll);
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);

        settings_make_divider(card);

        /* WiFi power save toggle */
        settings_make_toggle_row(card, "WiFi Power Save", &sw_wifi_power_save);
        if (cfg->wifi_power_save) {
            lv_obj_add_state(sw_wifi_power_save, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_wifi_power_save, wifi_power_save_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        settings_make_divider(card);

        /* Deep sleep toggle */
        settings_make_toggle_row(card, "Deep Sleep", &sw_deep_sleep);
        if (cfg->deep_sleep_enabled) {
            lv_obj_add_state(sw_deep_sleep, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_deep_sleep, deep_sleep_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* Collapsible deep sleep options container */
        cont_deep_sleep_opts = lv_obj_create(card);
        lv_obj_remove_style_all(cont_deep_sleep_opts);
        lv_obj_set_width(cont_deep_sleep_opts, LV_PCT(100));
        lv_obj_set_height(cont_deep_sleep_opts, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cont_deep_sleep_opts, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cont_deep_sleep_opts, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cont_deep_sleep_opts, 4, 0);
        if (!cfg->deep_sleep_enabled) {
            lv_obj_add_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }

        /* Wake timer stepper (hours) */
        {
            uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
            make_labeled_stepper(cont_deep_sleep_opts, "Wake Timer",
                                 wake_timer_minus_cb, wake_timer_plus_cb,
                                 &lbl_wake_timer);
            lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
        }

        /* Auto power off on idle toggle */
        settings_make_toggle_row(cont_deep_sleep_opts, "Auto Power Off", &sw_auto_power_off);
        if (cfg->deep_sleep_on_idle) {
            lv_obj_add_state(sw_auto_power_off, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_auto_power_off, auto_power_off_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ── Polling Card ────────────────────────────────────────────────── */
    {
        lv_obj_t *card = settings_make_card(parent, "POLLING");

        /* Data rate stepper */
        make_labeled_stepper(card, "Data Rate",
                             data_rate_minus_cb, data_rate_plus_cb,
                             &lbl_data_rate);
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);

        settings_make_divider(card);

        /* Graph rate stepper */
        make_labeled_stepper(card, "Graph Rate",
                             graph_rate_minus_cb, graph_rate_plus_cb,
                             &lbl_graph_rate);
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);

        settings_make_divider(card);

        /* Connection timeout stepper */
        make_labeled_stepper(card, "Conn Timeout",
                             conn_timeout_minus_cb, conn_timeout_plus_cb,
                             &lbl_conn_timeout);
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);

        settings_make_divider(card);

        /* Toast duration stepper */
        make_labeled_stepper(card, "Toast Duration",
                             toast_duration_minus_cb, toast_duration_plus_cb,
                             &lbl_toast_duration);
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
    }

    /* ── Notifications Card ──────────────────────────────────────────── */
    {
        lv_obj_t *card = settings_make_card(parent, "NOTIFICATIONS");

        settings_make_toggle_row(card, "Border Flash", &sw_alert_flash);
        if (cfg->alert_flash_enabled) {
            lv_obj_add_state(sw_alert_flash, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw_alert_flash, alert_flash_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Refresh — re-read config and update all widget states
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_behavior_refresh(void) {
    if (!tab_root) return;
    app_config_t *cfg = app_config_get();

    /* Hardware */
    if (dd_rotation) {
        lv_dropdown_set_selected(dd_rotation, cfg->screen_rotation);
    }
    if (slider_backlight) {
        lv_slider_set_value(slider_backlight, cfg->brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(lbl_backlight_val, "%d%%", cfg->brightness);
    }

    /* Screen sleep */
    if (sw_screen_sleep) {
        if (cfg->screen_sleep_enabled) {
            lv_obj_add_state(sw_screen_sleep, LV_STATE_CHECKED);
            lv_obj_clear_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(sw_screen_sleep, LV_STATE_CHECKED);
            lv_obj_add_flag(cont_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_sleep_timeout) {
        update_sleep_timeout_label();
    }
    if (lbl_idle_poll) {
        lv_label_set_text_fmt(lbl_idle_poll, "%d s", cfg->idle_poll_interval_s);
    }

    /* WiFi power save */
    if (sw_wifi_power_save) {
        if (cfg->wifi_power_save)
            lv_obj_add_state(sw_wifi_power_save, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_wifi_power_save, LV_STATE_CHECKED);
    }

    /* Deep sleep */
    if (sw_deep_sleep) {
        if (cfg->deep_sleep_enabled) {
            lv_obj_add_state(sw_deep_sleep, LV_STATE_CHECKED);
            lv_obj_clear_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(sw_deep_sleep, LV_STATE_CHECKED);
            lv_obj_add_flag(cont_deep_sleep_opts, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_wake_timer) {
        uint32_t hours = cfg->deep_sleep_wake_timer_s / 3600;
        lv_label_set_text_fmt(lbl_wake_timer, "%lu h", (unsigned long)hours);
    }
    if (sw_auto_power_off) {
        if (cfg->deep_sleep_on_idle)
            lv_obj_add_state(sw_auto_power_off, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_auto_power_off, LV_STATE_CHECKED);
    }

    /* Polling */
    if (lbl_data_rate) {
        lv_label_set_text_fmt(lbl_data_rate, "%d s", cfg->update_rate_s);
    }
    if (lbl_graph_rate) {
        lv_label_set_text_fmt(lbl_graph_rate, "%d s", cfg->graph_update_interval_s);
    }
    if (lbl_conn_timeout) {
        lv_label_set_text_fmt(lbl_conn_timeout, "%d s", cfg->connection_timeout_s);
    }
    if (lbl_toast_duration) {
        lv_label_set_text_fmt(lbl_toast_duration, "%d s", cfg->toast_duration_s);
    }

    /* Notifications */
    if (sw_alert_flash) {
        if (cfg->alert_flash_enabled)
            lv_obj_add_state(sw_alert_flash, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_alert_flash, LV_STATE_CHECKED);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Apply Theme — walk all children recursively
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tab_behavior_apply_theme(void) {
    if (tab_root) settings_apply_theme_recursive(tab_root);
}
