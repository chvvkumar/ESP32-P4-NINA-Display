/**
 * @file settings_tab_display.c
 * @brief Display tab for on-device settings tabview.
 *
 * Contains two cards:
 *   1. Appearance — theme dropdown, widget style dropdown, text brightness slider
 *   2. Page Navigation — segmented control (Manual/Fixed/Cycle) with conditional
 *      sub-sections for Fixed (pinned page) and Cycle (interval, transition,
 *      skip-offline, page checkboxes) modes.
 *
 * All changes are applied live; the parent tabview handles save/dirty state.
 */

#include "settings_tab_display.h"
#include "nina_settings_tabview.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include "ui_styles.h"
#include "tasks.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

/* ── Static widget references ───────────────────────────────────────── */
static lv_obj_t *tab_root = NULL;

/* Appearance */
static lv_obj_t *dd_theme = NULL;
static lv_obj_t *dd_widget_style = NULL;
static lv_obj_t *slider_text_bright = NULL;
static lv_obj_t *lbl_text_bright_val = NULL;
static lv_obj_t *sw_demo_mode = NULL;

/* Page Navigation */
static lv_obj_t *seg_mode = NULL;
static lv_obj_t *cont_fixed = NULL;
static lv_obj_t *dd_pinned_page = NULL;
static lv_obj_t *cont_cycle = NULL;
static lv_obj_t *lbl_interval_val = NULL;
static lv_obj_t *btn_interval_minus = NULL;
static lv_obj_t *btn_interval_plus = NULL;
static lv_obj_t *dd_transition = NULL;
static lv_obj_t *sw_skip_offline = NULL;
static lv_obj_t *cb_pages[6];

/* ── Segment button map ─────────────────────────────────────────────── */
static const char *seg_map[] = {"Manual", "Fixed", "Cycle", ""};

/* ── Forward declarations ───────────────────────────────────────────── */
static void theme_dd_changed_cb(lv_event_t *e);
static void widget_style_dd_changed_cb(lv_event_t *e);
static void text_bright_changed_cb(lv_event_t *e);
static void demo_mode_changed_cb(lv_event_t *e);
static void page_mode_changed_cb(lv_event_t *e);
static void pinned_page_changed_cb(lv_event_t *e);
static void interval_minus_cb(lv_event_t *e);
static void interval_plus_cb(lv_event_t *e);
static void transition_changed_cb(lv_event_t *e);
static void skip_offline_changed_cb(lv_event_t *e);
static void page_checkbox_changed_cb(lv_event_t *e);
static void update_mode_visibility(uint32_t mode);

/* ═══════════════════════════════════════════════════════════════════════
 *  Appearance Card — Theme / Widget Style / Text Brightness
 * ═══════════════════════════════════════════════════════════════════════ */

static void build_theme_options(char *buf, size_t buf_size)
{
    buf[0] = '\0';
    int count = themes_get_count();
    for (int i = 0; i < count; i++) {
        if (i > 0) strncat(buf, "\n", buf_size - strlen(buf) - 1);
        strncat(buf, themes_get(i)->name, buf_size - strlen(buf) - 1);
    }
}

static void build_widget_style_options(char *buf, size_t buf_size)
{
    buf[0] = '\0';
    for (int i = 0; i < WIDGET_STYLE_COUNT; i++) {
        if (i > 0) strncat(buf, "\n", buf_size - strlen(buf) - 1);
        strncat(buf, ui_styles_get_widget_style_name(i), buf_size - strlen(buf) - 1);
    }
}

static void create_appearance_card(lv_obj_t *parent)
{
    lv_obj_t *card = settings_make_card(parent, "Appearance");

    /* ── Theme row ── */
    {
        lv_obj_t *row = settings_make_row(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Theme");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_theme = lv_dropdown_create(row);
        lv_obj_set_width(dd_theme, 220);

        char opts[512];
        build_theme_options(opts, sizeof(opts));
        lv_dropdown_set_options(dd_theme, opts);
        lv_dropdown_set_selected(dd_theme, (uint32_t)app_config_get()->theme_index);

        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(dd_theme,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_bg_color(dd_theme, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_theme, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(dd_theme, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_theme, 1, 0);
        }

        lv_obj_add_event_cb(dd_theme, theme_dd_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    settings_make_divider(card);

    /* ── Widget style row ── */
    {
        lv_obj_t *row = settings_make_row(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Widget Style");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_widget_style = lv_dropdown_create(row);
        lv_obj_set_width(dd_widget_style, 220);

        char opts[256];
        build_widget_style_options(opts, sizeof(opts));
        lv_dropdown_set_options(dd_widget_style, opts);
        lv_dropdown_set_selected(dd_widget_style, (uint32_t)app_config_get()->widget_style);

        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(dd_widget_style,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_bg_color(dd_widget_style, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_widget_style, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(dd_widget_style, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_widget_style, 1, 0);
        }

        lv_obj_add_event_cb(dd_widget_style, widget_style_dd_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    settings_make_divider(card);

    /* ── Text brightness row ── */
    {
        lv_obj_t *row = settings_make_row(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Text Brightness");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_min_width(lbl, 130, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        /* Slider + value in a sub-container */
        lv_obj_t *ctrl = lv_obj_create(row);
        lv_obj_remove_style_all(ctrl);
        lv_obj_set_flex_grow(ctrl, 1);
        lv_obj_set_height(ctrl, 48);
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
        lv_obj_set_style_text_font(lbl_text_bright_val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_min_width(lbl_text_bright_val, 50, 0);
        lv_obj_set_style_text_align(lbl_text_bright_val, LV_TEXT_ALIGN_RIGHT, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl_text_bright_val,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
        lv_label_set_text_fmt(lbl_text_bright_val, "%d%%", app_config_get()->color_brightness);
    }

    settings_make_divider(card);

    /* ── Demo mode toggle ── */
    {
        lv_obj_t *toggle_row = settings_make_toggle_row(card, "Demo Mode", &sw_demo_mode);
        LV_UNUSED(toggle_row);

        if (app_config_get()->demo_mode) {
            lv_obj_add_state(sw_demo_mode, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw_demo_mode, LV_STATE_CHECKED);
        }

        lv_obj_add_event_cb(sw_demo_mode, demo_mode_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Page Navigation Card — Manual / Fixed / Cycle
 * ═══════════════════════════════════════════════════════════════════════ */

static void create_page_nav_card(lv_obj_t *parent)
{
    lv_obj_t *card = settings_make_card(parent, "Page Navigation");

    /* ── Segmented control (buttonmatrix) ──
     * We do NOT use lv_buttonmatrix_set_one_checked() because it auto-checks
     * button 0 via make_one_button_checked() and the clear_button_ctrl loop
     * re-triggers it.  Instead we manage checked state manually. */
    seg_mode = lv_buttonmatrix_create(card);
    lv_obj_remove_style_all(seg_mode);        /* Remove ALL default theme styles */
    lv_buttonmatrix_set_map(seg_mode, seg_map);
    lv_obj_set_width(seg_mode, LV_PCT(100));
    lv_obj_set_height(seg_mode, 48);

    if (current_theme) {
        int gb = app_config_get()->color_brightness;

        /* Main container (the background strip) */
        lv_obj_set_style_bg_color(seg_mode, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(seg_mode, 10, 0);
        lv_obj_set_style_border_width(seg_mode, 0, 0);
        lv_obj_set_style_outline_width(seg_mode, 0, 0);
        lv_obj_set_style_pad_all(seg_mode, 4, 0);
        lv_obj_set_style_pad_gap(seg_mode, 4, 0);
        lv_obj_set_style_text_font(seg_mode, &lv_font_montserrat_20, 0);

        /* Unchecked items — transparent bg, theme text color */
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS);
        lv_obj_set_style_border_width(seg_mode, 0, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(seg_mode, 0, LV_PART_ITEMS);
        lv_obj_set_style_outline_width(seg_mode, 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(seg_mode, 8, LV_PART_ITEMS);
        lv_obj_set_style_text_color(seg_mode,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), LV_PART_ITEMS);
        lv_obj_set_style_text_font(seg_mode, &lv_font_montserrat_20, LV_PART_ITEMS);

        /* Checked item — progress_color bg, theme-aware text */
        lv_obj_set_style_bg_color(seg_mode, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(seg_mode,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
            LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(seg_mode, 0, LV_PART_ITEMS | LV_STATE_CHECKED);

        /* Suppress all other visual states that could show a false highlight */
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(seg_mode, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(seg_mode, 0, LV_PART_ITEMS | LV_STATE_PRESSED);
    }

    lv_obj_add_event_cb(seg_mode, page_mode_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Fixed mode sub-section ── */
    cont_fixed = lv_obj_create(card);
    lv_obj_remove_style_all(cont_fixed);
    lv_obj_set_width(cont_fixed, LV_PCT(100));
    lv_obj_set_height(cont_fixed, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont_fixed, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont_fixed, 8, 0);
    lv_obj_set_style_pad_top(cont_fixed, 8, 0);
    lv_obj_add_flag(cont_fixed, LV_OBJ_FLAG_HIDDEN);

    {
        lv_obj_t *row = settings_make_row(cont_fixed);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Pinned Page");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_pinned_page = lv_dropdown_create(row);
        lv_obj_set_width(dd_pinned_page, 200);
        lv_dropdown_set_options(dd_pinned_page,
                                "Auto\n"
                                "AllSky\n"
                                "Spotify\n"
                                "Clock\n"
                                "Summary\n"
                                "NINA 1\n"
                                "NINA 2\n"
                                "NINA 3\n"
                                "SysInfo");

        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(dd_pinned_page,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_bg_color(dd_pinned_page, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_pinned_page, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(dd_pinned_page, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_pinned_page, 1, 0);
        }

        /* Set initial selection from config.
         * Dropdown mapping: 0=Auto(-1), 1=AllSky(0), 2=Spotify(1), 3=Clock(2),
         * 4=Summary(3), 5=NINA1(4), 6=NINA2(5), 7=NINA3(6), 8=SysInfo(total-1) */
        {
            int8_t ov = app_config_get()->active_page_override;
            int total = nina_dashboard_get_total_page_count();
            int sysinfo_ov = total > 0 ? total - 1 : 8;
            if (ov < 0) {
                lv_dropdown_set_selected(dd_pinned_page, 0);  /* Auto */
            } else if (ov >= sysinfo_ov) {
                lv_dropdown_set_selected(dd_pinned_page, 8);  /* SysInfo */
            } else {
                lv_dropdown_set_selected(dd_pinned_page, (uint32_t)(ov + 1));
            }
        }

        lv_obj_add_event_cb(dd_pinned_page, pinned_page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ── Cycle mode sub-section ── */
    cont_cycle = lv_obj_create(card);
    lv_obj_remove_style_all(cont_cycle);
    lv_obj_set_width(cont_cycle, LV_PCT(100));
    lv_obj_set_height(cont_cycle, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont_cycle, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont_cycle, 8, 0);
    lv_obj_set_style_pad_top(cont_cycle, 8, 0);
    lv_obj_add_flag(cont_cycle, LV_OBJ_FLAG_HIDDEN);

    /* Interval stepper */
    {
        lv_obj_t *row = settings_make_row(cont_cycle);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Interval");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        lv_obj_t *stepper = settings_make_stepper(row, &btn_interval_minus,
                                                   &lbl_interval_val, &btn_interval_plus);
        LV_UNUSED(stepper);

        lv_label_set_text_fmt(lbl_interval_val, "%d s", app_config_get()->auto_rotate_interval_s);
        lv_obj_add_event_cb(btn_interval_minus, interval_minus_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn_interval_plus, interval_plus_cb, LV_EVENT_CLICKED, NULL);
    }

    settings_make_divider(cont_cycle);

    /* Transition dropdown */
    {
        lv_obj_t *row = settings_make_row(cont_cycle);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Transition");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_transition = lv_dropdown_create(row);
        lv_obj_set_width(dd_transition, 200);
        lv_dropdown_set_options(dd_transition,
                                "Instant\n"
                                "Fade\n"
                                "Slide Left\n"
                                "Slide Right");
        lv_dropdown_set_selected(dd_transition, (uint32_t)app_config_get()->auto_rotate_effect);

        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(dd_transition,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_bg_color(dd_transition, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_transition, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(dd_transition, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_transition, 1, 0);
        }

        lv_obj_add_event_cb(dd_transition, transition_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    settings_make_divider(cont_cycle);

    /* Skip offline toggle */
    {
        lv_obj_t *toggle_row = settings_make_toggle_row(cont_cycle, "Skip Offline", &sw_skip_offline);
        LV_UNUSED(toggle_row);

        if (app_config_get()->auto_rotate_skip_disconnected) {
            lv_obj_add_state(sw_skip_offline, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw_skip_offline, LV_STATE_CHECKED);
        }

        lv_obj_add_event_cb(sw_skip_offline, skip_offline_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    settings_make_divider(cont_cycle);

    /* Pages in rotation — checkboxes */
    {
        lv_obj_t *lbl_section = lv_label_create(cont_cycle);
        lv_label_set_text(lbl_section, "Pages in Rotation");
        lv_obj_set_style_text_font(lbl_section, &lv_font_montserrat_20, 0);
        lv_obj_set_style_pad_top(lbl_section, 4, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl_section,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        static const char *page_names[] = {
            "Summary", "NINA 1", "NINA 2", "NINA 3", "SysInfo", "AllSky"
        };

        uint8_t mask = app_config_get()->auto_rotate_pages;

        /* Wrap container for checkboxes (2 columns) */
        lv_obj_t *cb_grid = lv_obj_create(cont_cycle);
        lv_obj_remove_style_all(cb_grid);
        lv_obj_set_width(cb_grid, LV_PCT(100));
        lv_obj_set_height(cb_grid, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cb_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(cb_grid, 16, 0);
        lv_obj_set_style_pad_row(cb_grid, 8, 0);

        for (int i = 0; i < 6; i++) {
            cb_pages[i] = lv_checkbox_create(cb_grid);
            lv_checkbox_set_text(cb_pages[i], page_names[i]);
            lv_obj_set_style_text_font(cb_pages[i], &lv_font_montserrat_16, 0);
            lv_obj_set_style_min_width(cb_pages[i], 130, 0);

            if (mask & (1 << i)) {
                lv_obj_add_state(cb_pages[i], LV_STATE_CHECKED);
            }

            if (current_theme) {
                int gb = app_config_get()->color_brightness;
                lv_obj_set_style_text_color(cb_pages[i],
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
                /* Checkbox indicator styling */
                lv_obj_set_style_border_color(cb_pages[i],
                    lv_color_hex(current_theme->bento_border), LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(cb_pages[i],
                    lv_color_hex(current_theme->progress_color),
                    LV_PART_INDICATOR | LV_STATE_CHECKED);
                lv_obj_set_style_bg_opa(cb_pages[i], LV_OPA_COVER,
                    LV_PART_INDICATOR | LV_STATE_CHECKED);
            }

            lv_obj_add_event_cb(cb_pages[i], page_checkbox_changed_cb,
                                LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)i);
        }
    }

    /* Hint for cycle mode */
    lv_obj_t *ar_hint = lv_label_create(cont_cycle);
    lv_label_set_text(ar_hint, "When enabled, overrides idle page switching");
    lv_obj_set_style_text_font(ar_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ar_hint, lv_color_hex(0x888888), 0);

    /* ── Determine initial mode from config ── */
    app_config_t *cfg = app_config_get();
    uint32_t initial_mode;
    if (cfg->auto_rotate_enabled) {
        initial_mode = 2;  /* Cycle */
    } else if (cfg->active_page_override >= 0) {
        initial_mode = 1;  /* Fixed */
    } else {
        initial_mode = 0;  /* Manual */
    }
    /* Set the correct button as checked (no one_checked, so no auto-check on button 0) */
    lv_buttonmatrix_set_button_ctrl(seg_mode, initial_mode, LV_BUTTONMATRIX_CTRL_CHECKED);
    update_mode_visibility(initial_mode);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Event Callbacks
 * ═══════════════════════════════════════════════════════════════════════ */

static void theme_dd_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t sel = lv_dropdown_get_selected(dd_theme);
    app_config_get()->theme_index = (int)sel;
    nina_dashboard_apply_theme((int)sel);
    settings_tab_display_refresh();
    settings_mark_dirty(false);
}

static void widget_style_dd_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t sel = lv_dropdown_get_selected(dd_widget_style);
    app_config_t *cfg = app_config_get();
    cfg->widget_style = (uint8_t)sel;
    nina_dashboard_apply_theme(cfg->theme_index);
    settings_mark_dirty(false);
}

static void text_bright_changed_cb(lv_event_t *e)
{
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
        settings_mark_dirty(false);
    }
}

static void demo_mode_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!sw_demo_mode) return;
    app_config_get()->demo_mode = lv_obj_has_state(sw_demo_mode, LV_STATE_CHECKED);
    settings_mark_dirty(true);  /* requires reboot — demo task is spawned at startup */
}

static void update_mode_visibility(uint32_t mode)
{
    lv_obj_add_flag(cont_fixed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cont_cycle, LV_OBJ_FLAG_HIDDEN);

    if (mode == 1) {
        lv_obj_clear_flag(cont_fixed, LV_OBJ_FLAG_HIDDEN);
    } else if (mode == 2) {
        lv_obj_clear_flag(cont_cycle, LV_OBJ_FLAG_HIDDEN);
    }
}

static void page_mode_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t sel = lv_buttonmatrix_get_selected_button(seg_mode);
    app_config_t *cfg = app_config_get();

    /* Manually enforce one-checked (we don't use lv_buttonmatrix_set_one_checked) */
    for (uint32_t i = 0; i < 3; i++) {
        lv_buttonmatrix_clear_button_ctrl(seg_mode, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    lv_buttonmatrix_set_button_ctrl(seg_mode, sel, LV_BUTTONMATRIX_CTRL_CHECKED);

    update_mode_visibility(sel);

    if (sel == 0) {
        /* Manual */
        cfg->active_page_override = -1;
        cfg->auto_rotate_enabled = false;
    } else if (sel == 1) {
        /* Fixed */
        cfg->auto_rotate_enabled = false;
        /* Set override to the currently selected pinned page */
        if (dd_pinned_page) {
            uint32_t dd_sel = lv_dropdown_get_selected(dd_pinned_page);
            if (dd_sel == 0) {
                cfg->active_page_override = -1;
            } else if (dd_sel == 8) {
                int total = nina_dashboard_get_total_page_count();
                cfg->active_page_override = (int8_t)(total > 0 ? total - 1 : 8);
            } else {
                cfg->active_page_override = (int8_t)(dd_sel - 1);
            }
        }
    } else if (sel == 2) {
        /* Cycle */
        cfg->active_page_override = -1;
        cfg->auto_rotate_enabled = true;
    }

    settings_mark_dirty(false);
}

static void pinned_page_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t sel = lv_dropdown_get_selected(dd_pinned_page);
    /* Dropdown mapping: 0=Auto(-1), 1=AllSky(0), 2=Spotify(1), 3=Clock(2),
     * 4=Summary(3), 5=NINA1(4), 6=NINA2(5), 7=NINA3(6), 8=SysInfo */
    if (sel == 0) {
        app_config_get()->active_page_override = -1;  /* Auto */
    } else if (sel == 8) {
        int total = nina_dashboard_get_total_page_count();
        app_config_get()->active_page_override = (int8_t)(total > 0 ? total - 1 : 8);
    } else {
        app_config_get()->active_page_override = (int8_t)(sel - 1);
    }
    settings_mark_dirty(false);
}

static void interval_minus_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->auto_rotate_interval_s > 60 ? 10 : 5;
    int val = (int)cfg->auto_rotate_interval_s - step;
    if (val < 4) val = 4;
    cfg->auto_rotate_interval_s = (uint16_t)val;
    lv_label_set_text_fmt(lbl_interval_val, "%d s", val);
    settings_mark_dirty(false);
}

static void interval_plus_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_config_t *cfg = app_config_get();
    int step = cfg->auto_rotate_interval_s >= 60 ? 10 : 5;
    int val = (int)cfg->auto_rotate_interval_s + step;
    if (val > 3600) val = 3600;
    cfg->auto_rotate_interval_s = (uint16_t)val;
    lv_label_set_text_fmt(lbl_interval_val, "%d s", val);
    settings_mark_dirty(false);
}

static void transition_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t sel = lv_dropdown_get_selected(dd_transition);
    app_config_get()->auto_rotate_effect = (uint8_t)sel;
    settings_mark_dirty(false);
}

static void skip_offline_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_config_get()->auto_rotate_skip_disconnected =
        lv_obj_has_state(sw_skip_offline, LV_STATE_CHECKED);
    settings_mark_dirty(false);
}

static void page_checkbox_changed_cb(lv_event_t *e)
{
    int bit = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_target(e);
    app_config_t *cfg = app_config_get();

    if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
        cfg->auto_rotate_pages |= (1 << bit);
    } else {
        cfg->auto_rotate_pages &= ~(1 << bit);
    }
    settings_mark_dirty(false);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void settings_tab_display_destroy(void) {
    tab_root = NULL;
    dd_theme = NULL;
    dd_widget_style = NULL;
    slider_text_bright = NULL;
    lbl_text_bright_val = NULL;
    sw_demo_mode = NULL;
    seg_mode = NULL;
    cont_fixed = NULL;
    dd_pinned_page = NULL;
    cont_cycle = NULL;
    lbl_interval_val = NULL;
    btn_interval_minus = NULL;
    btn_interval_plus = NULL;
    dd_transition = NULL;
    sw_skip_offline = NULL;
    memset(cb_pages, 0, sizeof(cb_pages));
}

void settings_tab_display_create(lv_obj_t *parent)
{
    tab_root = parent;

    /* Make the tab content scrollable and use column layout */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);

    create_appearance_card(parent);
    create_page_nav_card(parent);
}

void settings_tab_display_refresh(void)
{
    if (!tab_root) return;

    app_config_t *cfg = app_config_get();

    /* Appearance widgets */
    if (dd_theme) {
        char opts[512];
        build_theme_options(opts, sizeof(opts));
        lv_dropdown_set_options(dd_theme, opts);
        lv_dropdown_set_selected(dd_theme, (uint32_t)cfg->theme_index);
    }

    if (dd_widget_style) {
        lv_dropdown_set_selected(dd_widget_style, (uint32_t)cfg->widget_style);
    }

    if (slider_text_bright) {
        lv_slider_set_value(slider_text_bright, cfg->color_brightness, LV_ANIM_OFF);
    }
    if (lbl_text_bright_val) {
        lv_label_set_text_fmt(lbl_text_bright_val, "%d%%", cfg->color_brightness);
    }

    if (sw_demo_mode) {
        if (cfg->demo_mode) lv_obj_add_state(sw_demo_mode, LV_STATE_CHECKED);
        else                lv_obj_remove_state(sw_demo_mode, LV_STATE_CHECKED);
    }

    /* Page navigation — determine mode */
    uint32_t mode;
    if (cfg->auto_rotate_enabled) {
        mode = 2;  /* Cycle */
    } else if (cfg->active_page_override >= 0) {
        mode = 1;  /* Fixed */
    } else {
        mode = 0;  /* Manual */
    }

    if (seg_mode) {
        /* Clear all checked states, then set the correct one.
         * Safe because we don't use one_checked (no auto-re-check of button 0). */
        for (uint32_t i = 0; i < 3; i++) {
            lv_buttonmatrix_clear_button_ctrl(seg_mode, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
        lv_buttonmatrix_set_button_ctrl(seg_mode, mode, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    update_mode_visibility(mode);

    /* Fixed sub-section */
    if (dd_pinned_page) {
        int8_t ov = cfg->active_page_override;
        int total = nina_dashboard_get_total_page_count();
        int sysinfo_ov = total > 0 ? total - 1 : 8;
        if (ov < 0) {
            lv_dropdown_set_selected(dd_pinned_page, 0);  /* Auto */
        } else if (ov >= sysinfo_ov) {
            lv_dropdown_set_selected(dd_pinned_page, 8);  /* SysInfo */
        } else {
            lv_dropdown_set_selected(dd_pinned_page, (uint32_t)(ov + 1));
        }
    }

    /* Cycle sub-section */
    if (lbl_interval_val) {
        lv_label_set_text_fmt(lbl_interval_val, "%d s", cfg->auto_rotate_interval_s);
    }
    if (dd_transition) {
        lv_dropdown_set_selected(dd_transition, (uint32_t)cfg->auto_rotate_effect);
    }
    if (sw_skip_offline) {
        if (cfg->auto_rotate_skip_disconnected) {
            lv_obj_add_state(sw_skip_offline, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw_skip_offline, LV_STATE_CHECKED);
        }
    }

    /* Page checkboxes */
    uint8_t mask = cfg->auto_rotate_pages;
    for (int i = 0; i < 6; i++) {
        if (cb_pages[i]) {
            if (mask & (1 << i)) {
                lv_obj_add_state(cb_pages[i], LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(cb_pages[i], LV_STATE_CHECKED);
            }
        }
    }
}

void settings_tab_display_apply_theme(void)
{
    if (tab_root) settings_apply_theme_recursive(tab_root);

    /* Re-apply segmented control styling after the theme walker
     * (the walker treats all buttonmatrices generically and overrides
     * our main bg to bento_bg instead of bento_border). */
    if (seg_mode && current_theme) {
        int gb = app_config_get()->color_brightness;
        /* Main strip bg */
        lv_obj_set_style_bg_color(seg_mode, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_COVER, 0);
        /* Unchecked items — transparent */
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS);
        lv_obj_set_style_text_color(seg_mode,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), LV_PART_ITEMS);
        /* Checked item */
        lv_obj_set_style_bg_color(seg_mode, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_COVER,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(seg_mode,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
            LV_PART_ITEMS | LV_STATE_CHECKED);
        /* Suppress FOCUS_KEY / PRESSED states (re-assert after theme walker) */
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_opa(seg_mode, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
    }
}
