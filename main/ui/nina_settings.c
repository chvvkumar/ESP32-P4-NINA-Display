/**
 * @file nina_settings.c
 * @brief Display settings page — theme, brightness, update rate, auto-rotate controls.
 *
 * Provides on-device access to the same display settings available in the web config.
 * All changes are applied live; the Save button persists to NVS.
 */

#include "nina_settings.h"
#include "nina_dashboard_internal.h"
#include "nina_dashboard.h"
#include "app_config.h"
#include "themes.h"
#include "bsp/esp-bsp.h"

#include <stdio.h>
#include <string.h>

/* ── Layout ──────────────────────────────────────────────────────────── */
#define ST_ROW_H     50    /* Uniform row height */
#define ST_GROUP_GAP 8     /* Gap between rows within a group */

/* ── Page root ───────────────────────────────────────────────────────── */
static lv_obj_t *st_page = NULL;

/* ── Theme card widgets ──────────────────────────────────────────────── */
static lv_obj_t *lbl_theme_name  = NULL;
static lv_obj_t *btn_theme_prev  = NULL;
static lv_obj_t *btn_theme_next  = NULL;

/* ── Widget style card widgets ──────────────────────────────────────── */
static lv_obj_t *lbl_widget_style_name = NULL;
static lv_obj_t *btn_wstyle_prev       = NULL;
static lv_obj_t *btn_wstyle_next       = NULL;

/* ── Brightness card widgets ─────────────────────────────────────────── */
static lv_obj_t *slider_backlight      = NULL;
static lv_obj_t *lbl_backlight_val     = NULL;
static lv_obj_t *slider_text_bright    = NULL;
static lv_obj_t *lbl_text_bright_val   = NULL;

/* ── Update rate card widgets ────────────────────────────────────────── */
static lv_obj_t *lbl_update_rate_val   = NULL;
static lv_obj_t *btn_rate_minus        = NULL;
static lv_obj_t *btn_rate_plus         = NULL;

/* ── Graph update interval card widgets ─────────────────────────────── */
static lv_obj_t *lbl_graph_interval_val = NULL;
static lv_obj_t *btn_graph_minus        = NULL;
static lv_obj_t *btn_graph_plus         = NULL;

/* ── Auto-rotate card widgets ────────────────────────────────────────── */
static lv_obj_t *sw_auto_rotate        = NULL;
static lv_obj_t *lbl_interval_val      = NULL;
static lv_obj_t *btn_interval_minus    = NULL;
static lv_obj_t *btn_interval_plus     = NULL;
static lv_obj_t *lbl_effect_val        = NULL;
static lv_obj_t *btn_effect_prev       = NULL;
static lv_obj_t *btn_effect_next       = NULL;
static lv_obj_t *sw_skip_disconnected  = NULL;

/* ── Save button ─────────────────────────────────────────────────────── */
static lv_obj_t *btn_save              = NULL;
static lv_obj_t *lbl_save              = NULL;

/* ── Header icon (needs special handling in theme apply) ─────────────── */
static lv_obj_t *lbl_header_icon       = NULL;

/* ── Effect names ────────────────────────────────────────────────────── */
static const char *effect_names[] = {"Instant", "Fade", "Slide Left", "Slide Right"};
#define EFFECT_COUNT 4

/* ── Widget style names ──────────────────────────────────────────────── */
static const char *widget_style_names[] = {"Default", "Subtle Border", "Wireframe", "Soft Inset", "Frosted Glass", "Accent Bar", "Chamfered"};

/* ── Helpers ─────────────────────────────────────────────────────────── */

/** Create a circular arrow button (50x50) */
static lv_obj_t *make_arrow_btn(lv_obj_t *parent, const char *symbol) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 50, 50);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_center(lbl);

    return btn;
}

/** Create a +/- stepper button (46x46) */
static lv_obj_t *make_stepper_btn(lv_obj_t *parent, const char *symbol) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 46, 46);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_center(lbl);

    return btn;
}

/** Row with label on left, widget on right — fixed height */
static lv_obj_t *make_setting_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, ST_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

/** Create a thin horizontal divider line */
static lv_obj_t *make_divider(lv_obj_t *parent) {
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_set_width(div, LV_PCT(100));
    lv_obj_set_height(div, 1);
    if (current_theme) {
        lv_obj_set_style_bg_color(div, lv_color_hex(current_theme->bento_border), 0);
    }
    lv_obj_set_style_bg_opa(div, LV_OPA_30, 0);
    lv_obj_set_style_pad_top(div, 0, 0);
    return div;
}

/* ── Event Callbacks ─────────────────────────────────────────────────── */

/** Swipe on header to navigate pages (rest of settings page blocks swipe) */
static void header_gesture_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (total_page_count <= 1) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    int new_page = active_page;

    if (dir == LV_DIR_LEFT) {
        new_page = (active_page + 1) % total_page_count;
    } else if (dir == LV_DIR_RIGHT) {
        new_page = (active_page - 1 + total_page_count) % total_page_count;
    } else {
        return;
    }

    nina_dashboard_show_page(new_page, total_page_count);
}

static void theme_prev_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int idx = cfg->theme_index - 1;
    if (idx < 0) idx = themes_get_count() - 1;
    cfg->theme_index = idx;
    nina_dashboard_apply_theme(idx);
    /* Refresh after theme apply since it rebuilds styles */
    settings_page_refresh();
}

static void theme_next_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int idx = cfg->theme_index + 1;
    if (idx >= themes_get_count()) idx = 0;
    cfg->theme_index = idx;
    nina_dashboard_apply_theme(idx);
    settings_page_refresh();
}

static void wstyle_prev_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int idx = (int)cfg->widget_style - 1;
    if (idx < 0) idx = WIDGET_STYLE_COUNT - 1;
    cfg->widget_style = (uint8_t)idx;
    nina_dashboard_apply_theme(cfg->theme_index);
    settings_page_refresh();
}

static void wstyle_next_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int idx = (int)cfg->widget_style + 1;
    if (idx >= WIDGET_STYLE_COUNT) idx = 0;
    cfg->widget_style = (uint8_t)idx;
    nina_dashboard_apply_theme(cfg->theme_index);
    settings_page_refresh();
}

static void backlight_changed_cb(lv_event_t *e) {
    LV_UNUSED(e);
    int val = lv_slider_get_value(slider_backlight);
    app_config_get()->brightness = val;
    bsp_display_brightness_set(val);
    lv_label_set_text_fmt(lbl_backlight_val, "%d%%", val);
}

static void text_bright_changed_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int val = lv_slider_get_value(slider_text_bright);
    app_config_get()->color_brightness = val;

    if (code == LV_EVENT_VALUE_CHANGED) {
        /* Lightweight: update the value label during drag */
        lv_label_set_text_fmt(lbl_text_bright_val, "%d%%", val);
    }
    if (code == LV_EVENT_RELEASED) {
        /* Heavyweight: apply full theme only when user releases the slider */
        lv_label_set_text_fmt(lbl_text_bright_val, "%d%%", val);
        nina_dashboard_apply_theme(app_config_get()->theme_index);
    }
}

static void rate_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->update_rate_s > 1) {
        cfg->update_rate_s--;
        lv_label_set_text_fmt(lbl_update_rate_val, "%d s", cfg->update_rate_s);
    }
}

static void rate_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->update_rate_s < 10) {
        cfg->update_rate_s++;
        lv_label_set_text_fmt(lbl_update_rate_val, "%d s", cfg->update_rate_s);
    }
}

static void graph_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->graph_update_interval_s > 2) {
        cfg->graph_update_interval_s--;
        lv_label_set_text_fmt(lbl_graph_interval_val, "%d s", cfg->graph_update_interval_s);
    }
}

static void graph_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    if (cfg->graph_update_interval_s < 30) {
        cfg->graph_update_interval_s++;
        lv_label_set_text_fmt(lbl_graph_interval_val, "%d s", cfg->graph_update_interval_s);
    }
}

static void auto_rotate_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_get()->auto_rotate_enabled = lv_obj_has_state(sw_auto_rotate, LV_STATE_CHECKED);
}

static void interval_minus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->auto_rotate_interval_s > 60 ? 10 : 5;
    int val = (int)cfg->auto_rotate_interval_s - step;
    if (val < 4) val = 4;
    cfg->auto_rotate_interval_s = (uint16_t)val;
    lv_label_set_text_fmt(lbl_interval_val, "%d s", val);
}

static void interval_plus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->auto_rotate_interval_s >= 60 ? 10 : 5;
    int val = (int)cfg->auto_rotate_interval_s + step;
    if (val > 3600) val = 3600;
    cfg->auto_rotate_interval_s = (uint16_t)val;
    lv_label_set_text_fmt(lbl_interval_val, "%d s", val);
}

static void effect_prev_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int idx = (int)cfg->auto_rotate_effect - 1;
    if (idx < 0) idx = EFFECT_COUNT - 1;
    cfg->auto_rotate_effect = (uint8_t)idx;
    lv_label_set_text(lbl_effect_val, effect_names[idx]);
}

static void effect_next_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int idx = (int)cfg->auto_rotate_effect + 1;
    if (idx >= EFFECT_COUNT) idx = 0;
    cfg->auto_rotate_effect = (uint8_t)idx;
    lv_label_set_text(lbl_effect_val, effect_names[idx]);
}

static void skip_disconnected_toggle_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_get()->auto_rotate_skip_disconnected =
        lv_obj_has_state(sw_skip_disconnected, LV_STATE_CHECKED);
}

static lv_timer_t *save_feedback_timer = NULL;

static void save_feedback_timer_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (lbl_save) lv_label_set_text(lbl_save, LV_SYMBOL_SAVE "  Save");
    save_feedback_timer = NULL;
}

static void save_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_save(app_config_get());
    if (lbl_save) lv_label_set_text(lbl_save, LV_SYMBOL_OK "  Saved");
    /* Revert label after 2 seconds */
    if (save_feedback_timer) lv_timer_delete(save_feedback_timer);
    save_feedback_timer = lv_timer_create(save_feedback_timer_cb, 2000, NULL);
    lv_timer_set_repeat_count(save_feedback_timer, 1);
}

/* ── Page Creation ───────────────────────────────────────────────────── */

lv_obj_t *settings_page_create(lv_obj_t *parent) {
    st_page = lv_obj_create(parent);
    lv_obj_remove_style_all(st_page);
    lv_obj_set_size(st_page, SCREEN_SIZE - 2 * OUTER_PADDING, SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_flex_flow(st_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(st_page, ST_GROUP_GAP, 0);
    lv_obj_set_scrollbar_mode(st_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(st_page, LV_OBJ_FLAG_SCROLLABLE);

    int gb = app_config_get()->color_brightness;

    /* ── Header ── */
    {
        lv_obj_t *hdr = lv_obj_create(st_page);
        lv_obj_remove_style_all(hdr);
        lv_obj_add_style(hdr, &style_header_gradient, 0);
        lv_obj_set_width(hdr, LV_PCT(100));
        lv_obj_set_height(hdr, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(hdr, 14, 0);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(hdr, 10, 0);

        lbl_header_icon = lv_label_create(hdr);
        lv_label_set_text(lbl_header_icon, LV_SYMBOL_SETTINGS);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_header_icon, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
        }

        lv_obj_t *title = lv_label_create(hdr);
        lv_label_set_text(title, "Display Settings");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(title, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
        }

        lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hdr, header_gesture_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    /* ── Divider ── */
    make_divider(st_page);

    /* ── Theme Row ── */
    {
        lv_obj_t *row = make_setting_row(st_page);
        lv_obj_set_height(row, 54);

        btn_theme_prev = make_arrow_btn(row, LV_SYMBOL_LEFT);
        lv_obj_add_event_cb(btn_theme_prev, theme_prev_cb, LV_EVENT_CLICKED, NULL);

        lbl_theme_name = lv_label_create(row);
        lv_obj_set_style_text_font(lbl_theme_name, &lv_font_montserrat_20, 0);
        lv_obj_set_flex_grow(lbl_theme_name, 1);
        lv_obj_set_style_text_align(lbl_theme_name, LV_TEXT_ALIGN_CENTER, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_theme_name, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_label_set_text(lbl_theme_name, current_theme->name);
        } else {
            lv_label_set_text(lbl_theme_name, "--");
        }

        btn_theme_next = make_arrow_btn(row, LV_SYMBOL_RIGHT);
        lv_obj_add_event_cb(btn_theme_next, theme_next_cb, LV_EVENT_CLICKED, NULL);
    }

    /* ── Widget Style Row ── */
    {
        lv_obj_t *row = make_setting_row(st_page);
        lv_obj_set_height(row, 54);

        btn_wstyle_prev = make_arrow_btn(row, LV_SYMBOL_LEFT);
        lv_obj_add_event_cb(btn_wstyle_prev, wstyle_prev_cb, LV_EVENT_CLICKED, NULL);

        lbl_widget_style_name = lv_label_create(row);
        lv_obj_set_style_text_font(lbl_widget_style_name, &lv_font_montserrat_20, 0);
        lv_obj_set_flex_grow(lbl_widget_style_name, 1);
        lv_obj_set_style_text_align(lbl_widget_style_name, LV_TEXT_ALIGN_CENTER, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_widget_style_name, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            int ws = app_config_get()->widget_style;
            if (ws < 0 || ws >= WIDGET_STYLE_COUNT) ws = 0;
            lv_label_set_text(lbl_widget_style_name, widget_style_names[ws]);
        } else {
            lv_label_set_text(lbl_widget_style_name, "--");
        }

        btn_wstyle_next = make_arrow_btn(row, LV_SYMBOL_RIGHT);
        lv_obj_add_event_cb(btn_wstyle_next, wstyle_next_cb, LV_EVENT_CLICKED, NULL);
    }

    /* ── Divider ── */
    make_divider(st_page);

    /* ── Backlight Row ── */
    {
        lv_obj_t *row = make_setting_row(st_page);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Backlight");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_min_width(lbl, 110, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        /* Slider + value in a sub-row */
        lv_obj_t *ctrl = lv_obj_create(row);
        lv_obj_remove_style_all(ctrl);
        lv_obj_set_flex_grow(ctrl, 1);
        lv_obj_set_height(ctrl, ST_ROW_H);
        lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(ctrl, 10, 0);

        slider_backlight = lv_slider_create(ctrl);
        lv_obj_set_flex_grow(slider_backlight, 1);
        lv_obj_set_height(slider_backlight, 16);
        lv_slider_set_range(slider_backlight, 0, 100);
        lv_slider_set_value(slider_backlight, app_config_get()->brightness, LV_ANIM_OFF);
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
        lv_obj_set_style_text_font(lbl_backlight_val, &lv_font_montserrat_18, 0);
        lv_obj_set_style_min_width(lbl_backlight_val, 50, 0);
        lv_obj_set_style_text_align(lbl_backlight_val, LV_TEXT_ALIGN_RIGHT, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_backlight_val, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
        lv_label_set_text_fmt(lbl_backlight_val, "%d%%", app_config_get()->brightness);
    }

    /* ── Text Color Row ── */
    {
        lv_obj_t *row = make_setting_row(st_page);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Text Color");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_min_width(lbl, 110, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        lv_obj_t *ctrl = lv_obj_create(row);
        lv_obj_remove_style_all(ctrl);
        lv_obj_set_flex_grow(ctrl, 1);
        lv_obj_set_height(ctrl, ST_ROW_H);
        lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(ctrl, 10, 0);

        slider_text_bright = lv_slider_create(ctrl);
        lv_obj_set_flex_grow(slider_text_bright, 1);
        lv_obj_set_height(slider_text_bright, 16);
        lv_slider_set_range(slider_text_bright, 0, 100);
        lv_slider_set_value(slider_text_bright, app_config_get()->color_brightness, LV_ANIM_OFF);
        lv_obj_set_style_radius(slider_text_bright, 8, 0);
        lv_obj_set_style_radius(slider_text_bright, 8, LV_PART_INDICATOR);
        lv_obj_set_style_radius(slider_text_bright, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider_text_bright, 8, LV_PART_KNOB);
        if (current_theme) {
            lv_obj_set_style_bg_color(slider_text_bright, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_bg_opa(slider_text_bright, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(slider_text_bright, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(slider_text_bright, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(slider_text_bright, lv_color_hex(current_theme->progress_color), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(slider_text_bright, LV_OPA_COVER, LV_PART_KNOB);
        }
        lv_obj_add_event_cb(slider_text_bright, text_bright_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(slider_text_bright, text_bright_changed_cb, LV_EVENT_RELEASED, NULL);

        lbl_text_bright_val = lv_label_create(ctrl);
        lv_obj_set_style_text_font(lbl_text_bright_val, &lv_font_montserrat_18, 0);
        lv_obj_set_style_min_width(lbl_text_bright_val, 50, 0);
        lv_obj_set_style_text_align(lbl_text_bright_val, LV_TEXT_ALIGN_RIGHT, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_text_bright_val, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
        lv_label_set_text_fmt(lbl_text_bright_val, "%d%%", app_config_get()->color_brightness);
    }

    /* ── Divider ── */
    make_divider(st_page);

    /* ── Data Rate Row ── */
    {
        lv_obj_t *row = make_setting_row(st_page);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Data Rate");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        lv_obj_t *stepper = lv_obj_create(row);
        lv_obj_remove_style_all(stepper);
        lv_obj_set_size(stepper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(stepper, 10, 0);

        btn_rate_minus = make_stepper_btn(stepper, LV_SYMBOL_MINUS);
        lv_obj_add_event_cb(btn_rate_minus, rate_minus_cb, LV_EVENT_CLICKED, NULL);

        lbl_update_rate_val = lv_label_create(stepper);
        lv_obj_set_style_text_font(lbl_update_rate_val, &lv_font_montserrat_22, 0);
        lv_obj_set_style_min_width(lbl_update_rate_val, 50, 0);
        lv_obj_set_style_text_align(lbl_update_rate_val, LV_TEXT_ALIGN_CENTER, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_update_rate_val, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
        lv_label_set_text_fmt(lbl_update_rate_val, "%d s", app_config_get()->update_rate_s);

        btn_rate_plus = make_stepper_btn(stepper, LV_SYMBOL_PLUS);
        lv_obj_add_event_cb(btn_rate_plus, rate_plus_cb, LV_EVENT_CLICKED, NULL);
    }

    /* ── Graph Rate Row ── */
    {
        lv_obj_t *row = make_setting_row(st_page);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Graph Rate");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        lv_obj_t *stepper = lv_obj_create(row);
        lv_obj_remove_style_all(stepper);
        lv_obj_set_size(stepper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(stepper, 10, 0);

        btn_graph_minus = make_stepper_btn(stepper, LV_SYMBOL_MINUS);
        lv_obj_add_event_cb(btn_graph_minus, graph_minus_cb, LV_EVENT_CLICKED, NULL);

        lbl_graph_interval_val = lv_label_create(stepper);
        lv_obj_set_style_text_font(lbl_graph_interval_val, &lv_font_montserrat_22, 0);
        lv_obj_set_style_min_width(lbl_graph_interval_val, 50, 0);
        lv_obj_set_style_text_align(lbl_graph_interval_val, LV_TEXT_ALIGN_CENTER, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_graph_interval_val, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
        lv_label_set_text_fmt(lbl_graph_interval_val, "%d s", app_config_get()->graph_update_interval_s);

        btn_graph_plus = make_stepper_btn(stepper, LV_SYMBOL_PLUS);
        lv_obj_add_event_cb(btn_graph_plus, graph_plus_cb, LV_EVENT_CLICKED, NULL);
    }

    /* ── Divider ── */
    make_divider(st_page);

    /* ── Auto-Rotate Rows ── */
    {
        app_config_t *cfg = app_config_get();

        /* Enable toggle row */
        {
            lv_obj_t *row = make_setting_row(st_page);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, "Auto-Rotate");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            sw_auto_rotate = lv_switch_create(row);
            lv_obj_set_size(sw_auto_rotate, 58, 32);
            if (cfg->auto_rotate_enabled) {
                lv_obj_add_state(sw_auto_rotate, LV_STATE_CHECKED);
            }
            if (current_theme) {
                lv_obj_set_style_bg_color(sw_auto_rotate, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_bg_color(sw_auto_rotate, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR | LV_STATE_CHECKED);
                lv_obj_set_style_bg_color(sw_auto_rotate, lv_color_hex(current_theme->text_color), LV_PART_KNOB);
                lv_obj_set_style_bg_opa(sw_auto_rotate, LV_OPA_COVER, LV_PART_KNOB);
            }
            lv_obj_add_event_cb(sw_auto_rotate, auto_rotate_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        /* Interval stepper row */
        {
            lv_obj_t *row = make_setting_row(st_page);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, "Interval");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            lv_obj_t *stepper = lv_obj_create(row);
            lv_obj_remove_style_all(stepper);
            lv_obj_set_size(stepper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(stepper, 8, 0);

            btn_interval_minus = make_stepper_btn(stepper, LV_SYMBOL_MINUS);
            lv_obj_add_event_cb(btn_interval_minus, interval_minus_cb, LV_EVENT_CLICKED, NULL);

            lbl_interval_val = lv_label_create(stepper);
            lv_obj_set_style_text_font(lbl_interval_val, &lv_font_montserrat_20, 0);
            lv_obj_set_style_min_width(lbl_interval_val, 55, 0);
            lv_obj_set_style_text_align(lbl_interval_val, LV_TEXT_ALIGN_CENTER, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl_interval_val, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }
            lv_label_set_text_fmt(lbl_interval_val, "%d s", cfg->auto_rotate_interval_s);

            btn_interval_plus = make_stepper_btn(stepper, LV_SYMBOL_PLUS);
            lv_obj_add_event_cb(btn_interval_plus, interval_plus_cb, LV_EVENT_CLICKED, NULL);
        }

        /* Effect selector row */
        {
            lv_obj_t *row = make_setting_row(st_page);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, "Effect");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            lv_obj_t *selector = lv_obj_create(row);
            lv_obj_remove_style_all(selector);
            lv_obj_set_size(selector, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(selector, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(selector, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(selector, 8, 0);

            btn_effect_prev = make_arrow_btn(selector, LV_SYMBOL_LEFT);
            lv_obj_add_event_cb(btn_effect_prev, effect_prev_cb, LV_EVENT_CLICKED, NULL);

            lbl_effect_val = lv_label_create(selector);
            lv_obj_set_style_text_font(lbl_effect_val, &lv_font_montserrat_18, 0);
            lv_obj_set_style_min_width(lbl_effect_val, 100, 0);
            lv_obj_set_style_text_align(lbl_effect_val, LV_TEXT_ALIGN_CENTER, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl_effect_val, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }
            int eff = cfg->auto_rotate_effect;
            if (eff < 0 || eff >= EFFECT_COUNT) eff = 0;
            lv_label_set_text(lbl_effect_val, effect_names[eff]);

            btn_effect_next = make_arrow_btn(selector, LV_SYMBOL_RIGHT);
            lv_obj_add_event_cb(btn_effect_next, effect_next_cb, LV_EVENT_CLICKED, NULL);
        }

        /* Skip disconnected toggle row */
        {
            lv_obj_t *row = make_setting_row(st_page);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, "Skip Offline");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
            if (current_theme) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            sw_skip_disconnected = lv_switch_create(row);
            lv_obj_set_size(sw_skip_disconnected, 58, 32);
            if (cfg->auto_rotate_skip_disconnected) {
                lv_obj_add_state(sw_skip_disconnected, LV_STATE_CHECKED);
            }
            if (current_theme) {
                lv_obj_set_style_bg_color(sw_skip_disconnected, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_bg_color(sw_skip_disconnected, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR | LV_STATE_CHECKED);
                lv_obj_set_style_bg_color(sw_skip_disconnected, lv_color_hex(current_theme->text_color), LV_PART_KNOB);
                lv_obj_set_style_bg_opa(sw_skip_disconnected, LV_OPA_COVER, LV_PART_KNOB);
            }
            lv_obj_add_event_cb(sw_skip_disconnected, skip_disconnected_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* ── Divider ── */
    make_divider(st_page);

    /* ── Save Button (ghost outline) ── */
    {
        btn_save = lv_button_create(st_page);
        lv_obj_set_width(btn_save, LV_PCT(100));
        lv_obj_set_height(btn_save, 54);
        lv_obj_set_style_radius(btn_save, 12, 0);
        lv_obj_set_style_shadow_width(btn_save, 0, 0);
        /* Ghost outline: transparent bg, thin border */
        lv_obj_set_style_bg_opa(btn_save, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn_save, 1, 0);
        if (current_theme) {
            lv_obj_set_style_border_color(btn_save, lv_color_hex(current_theme->bento_border), 0);
            /* Pressed: subtle fill */
            lv_obj_set_style_bg_color(btn_save, lv_color_hex(current_theme->bento_border), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(btn_save, LV_OPA_40, LV_STATE_PRESSED);
        }
        lv_obj_set_style_border_opa(btn_save, LV_OPA_COVER, 0);

        lbl_save = lv_label_create(btn_save);
        lv_label_set_text(lbl_save, LV_SYMBOL_SAVE "  Save");
        lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_18, 0);
        if (current_theme) {
            int gb2 = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl_save, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb2)), 0);
        }
        lv_obj_center(lbl_save);

        lv_obj_add_event_cb(btn_save, save_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    return st_page;
}

/* ── Refresh ─────────────────────────────────────────────────────────── */

void settings_page_refresh(void) {
    if (!st_page) return;

    app_config_t *cfg = app_config_get();

    /* Theme name */
    if (lbl_theme_name && current_theme) {
        lv_label_set_text(lbl_theme_name, current_theme->name);
    }

    /* Widget style name */
    if (lbl_widget_style_name) {
        int ws = cfg->widget_style;
        if (ws < 0 || ws >= WIDGET_STYLE_COUNT) ws = 0;
        lv_label_set_text(lbl_widget_style_name, widget_style_names[ws]);
    }

    /* Sliders */
    if (slider_backlight) {
        lv_slider_set_value(slider_backlight, cfg->brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(lbl_backlight_val, "%d%%", cfg->brightness);
    }
    if (slider_text_bright) {
        lv_slider_set_value(slider_text_bright, cfg->color_brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(lbl_text_bright_val, "%d%%", cfg->color_brightness);
    }

    /* Update rate */
    if (lbl_update_rate_val) {
        lv_label_set_text_fmt(lbl_update_rate_val, "%d s", cfg->update_rate_s);
    }

    /* Graph update interval */
    if (lbl_graph_interval_val) {
        lv_label_set_text_fmt(lbl_graph_interval_val, "%d s", cfg->graph_update_interval_s);
    }

    /* Auto-rotate */
    if (sw_auto_rotate) {
        if (cfg->auto_rotate_enabled)
            lv_obj_add_state(sw_auto_rotate, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_auto_rotate, LV_STATE_CHECKED);
    }
    if (lbl_interval_val) {
        lv_label_set_text_fmt(lbl_interval_val, "%d s", cfg->auto_rotate_interval_s);
    }
    if (lbl_effect_val) {
        int eff = cfg->auto_rotate_effect;
        if (eff < 0 || eff >= EFFECT_COUNT) eff = 0;
        lv_label_set_text(lbl_effect_val, effect_names[eff]);
    }
    if (sw_skip_disconnected) {
        if (cfg->auto_rotate_skip_disconnected)
            lv_obj_add_state(sw_skip_disconnected, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw_skip_disconnected, LV_STATE_CHECKED);
    }
}

/* ── Theme Application ───────────────────────────────────────────────── */

static void apply_theme_recursive_st(lv_obj_t *obj);

static void apply_theme_to_widget(lv_obj_t *obj) {
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;

    if (lv_obj_check_type(obj, &lv_label_class)) {
        if (obj == lbl_save) {
            /* Save button label — muted label_color */
            lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        } else if (obj == lbl_header_icon) {
            lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
        } else {
            const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
            if (font == &lv_font_montserrat_22) {
                /* Page title */
                lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
            } else {
                /* All other labels — text_color */
                lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }
        }
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color), LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->text_color), LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        if (obj == btn_save) {
            /* Ghost outline save button */
            lv_obj_set_style_border_color(obj, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), LV_STATE_PRESSED);
        } else {
            lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
        }
    } else {
        /* Plain containers that are 1px tall = dividers */
        lv_coord_t h = lv_obj_get_height(obj);
        if (h == 1) {
            lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
        }
    }
}

static void apply_theme_recursive_st(lv_obj_t *obj) {
    uint32_t child_cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        apply_theme_to_widget(child);
        apply_theme_recursive_st(child);
    }
}

void settings_page_apply_theme(void) {
    if (!st_page || !current_theme) return;
    apply_theme_recursive_st(st_page);
    lv_obj_invalidate(st_page);
}
