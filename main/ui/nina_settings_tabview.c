/**
 * @file nina_settings_tabview.c
 * @brief Tabbed settings page — 4-tab container replacing the old single-page settings.
 *
 * Creates an lv_tabview with Display / Nodes / Behavior / System tabs,
 * a back button, shared on-screen keyboard, sticky save bar, and
 * reusable widget factories used by all tab modules.
 */

#include "nina_settings_tabview.h"
#include "settings_tab_display.h"
#include "settings_tab_nodes.h"
#include "settings_tab_behavior.h"
#include "settings_tab_system.h"
#include "settings_color_picker.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include "ui_styles.h"
#include "display_defs.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

/* ── Layout ──────────────────────────────────────────────────────────── */
#define ST_ROW_H        50   /* Uniform row height for settings rows */
#define SAVE_BAR_H      48   /* Sticky save bar height */
#define KB_HEIGHT       280   /* On-screen keyboard height */
#define TAB_BAR_H        44   /* Tab bar height */
#define CARD_PAD         12   /* Card internal padding */
#define CARD_ROW_GAP      8   /* Row gap within cards */
#define STEPPER_BTN_SZ   46   /* +/- stepper button size */

/* ── Static State ────────────────────────────────────────────────────── */
static lv_obj_t *st_root      = NULL;  /* root container */
static lv_obj_t *tabview      = NULL;
static lv_obj_t *keyboard     = NULL;  /* shared keyboard overlay */
static lv_obj_t *save_bar     = NULL;  /* sticky save bar at bottom */
static lv_obj_t *lbl_save_btn = NULL;
static bool      kb_visible   = false;
static bool      needs_reboot = false;
static bool      dirty        = false;
static app_config_t config_snapshot;

static lv_timer_t *save_feedback_timer = NULL;

/* ── Forward Declarations ────────────────────────────────────────────── */
static void back_btn_cb(lv_event_t *e);
static void save_btn_cb(lv_event_t *e);
static void save_feedback_timer_cb(lv_timer_t *timer);
static void apply_theme_to_widget(lv_obj_t *obj);
static void ta_focus_cb(lv_event_t *e);
static void ta_defocus_cb(lv_event_t *e);

/* ════════════════════════════════════════════════════════════════════════
 *  Back Button Callback
 * ════════════════════════════════════════════════════════════════════════ */

static void back_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_dashboard_show_page(SYSINFO_PAGE_IDX(page_count), total_page_count);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Save Button Callbacks
 * ════════════════════════════════════════════════════════════════════════ */

static void save_feedback_timer_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (save_bar) lv_obj_add_flag(save_bar, LV_OBJ_FLAG_HIDDEN);
    dirty = false;
    needs_reboot = false;
    save_feedback_timer = NULL;
}

static void save_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_config_save(app_config_get());

    if (lbl_save_btn) {
        lv_label_set_text(lbl_save_btn, LV_SYMBOL_OK " Saved!");
    }

    /* Revert label and hide bar after 2 seconds */
    if (save_feedback_timer) lv_timer_delete(save_feedback_timer);

    if (needs_reboot) {
        /* Schedule reboot after brief visual feedback */
        save_feedback_timer = lv_timer_create(save_feedback_timer_cb, 1500, NULL);
        lv_timer_set_repeat_count(save_feedback_timer, 1);
        /* Trigger reboot via esp_restart after timer fires — for now just
         * mark saved.  The tab modules handle reboot-required scenarios. */
    } else {
        save_feedback_timer = lv_timer_create(save_feedback_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(save_feedback_timer, 1);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Textarea Focus / Defocus Callbacks (shared by settings_make_textarea_row)
 * ════════════════════════════════════════════════════════════════════════ */

static void ta_focus_cb(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    settings_show_keyboard(ta);
}

static void ta_defocus_cb(lv_event_t *e) {
    LV_UNUSED(e);
    settings_hide_keyboard();
}

/* ════════════════════════════════════════════════════════════════════════
 *  Keyboard Management
 * ════════════════════════════════════════════════════════════════════════ */

lv_obj_t *settings_get_keyboard(void) {
    if (!keyboard) {
        keyboard = lv_keyboard_create(st_root);
        lv_obj_set_size(keyboard, LV_PCT(100), KB_HEIGHT);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_style_border_width(keyboard, 0, 0);

        /* Theme the keyboard */
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_bg_color(keyboard, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(keyboard, lv_color_hex(current_theme->bento_border), LV_PART_ITEMS);
            lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
            lv_obj_set_style_text_color(keyboard,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), LV_PART_ITEMS);
            lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
            lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
        }
    }
    return keyboard;
}

void settings_show_keyboard(lv_obj_t *ta) {
    lv_obj_t *kb = settings_get_keyboard();
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
    kb_visible = true;

    /* Shrink tabview so the keyboard doesn't cover content */
    if (tabview) {
        lv_obj_set_height(tabview, lv_obj_get_height(st_root) - KB_HEIGHT);
    }

    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

void settings_hide_keyboard(void) {
    if (keyboard) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(keyboard, NULL);
    }
    kb_visible = false;

    if (tabview) {
        lv_obj_set_height(tabview, LV_PCT(100));
    }
}

bool settings_tabview_keyboard_active(void) {
    return kb_visible;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Dirty State / Save Bar
 * ════════════════════════════════════════════════════════════════════════ */

void settings_mark_dirty(bool reboot_required) {
    dirty = true;
    if (reboot_required) needs_reboot = true;

    if (save_bar) {
        lv_obj_clear_flag(save_bar, LV_OBJ_FLAG_HIDDEN);
        if (lbl_save_btn) {
            lv_label_set_text(lbl_save_btn,
                needs_reboot ? LV_SYMBOL_WARNING " Save & Reboot"
                             : LV_SYMBOL_SAVE " Save");
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Shared Widget Factories
 * ════════════════════════════════════════════════════════════════════════ */

lv_obj_t *settings_make_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_bento_box, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_style_pad_row(card, CARD_ROW_GAP, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    if (title) {
        int gb = app_config_get()->color_brightness;
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, title);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_letter_space(lbl, 1, 0);
        /* Uppercase via manual copy — LVGL 9 doesn't have auto-uppercase */
        if (current_theme) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }
    }

    ui_styles_set_widget_draw_cbs(card);

    return card;
}

lv_obj_t *settings_make_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, ST_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

lv_obj_t *settings_make_divider(lv_obj_t *parent) {
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

lv_obj_t *settings_make_stepper(lv_obj_t *parent, lv_obj_t **out_minus,
                                 lv_obj_t **out_label, lv_obj_t **out_plus)
{
    int gb = app_config_get()->color_brightness;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    /* [-] button */
    lv_obj_t *btn_minus = lv_button_create(row);
    lv_obj_set_size(btn_minus, STEPPER_BTN_SZ, STEPPER_BTN_SZ);
    lv_obj_set_style_radius(btn_minus, 10, 0);
    lv_obj_set_style_bg_opa(btn_minus, LV_OPA_COVER, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn_minus, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn_minus, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_set_style_border_width(btn_minus, 0, 0);
    lv_obj_set_style_shadow_width(btn_minus, 0, 0);

    lv_obj_t *lbl_m = lv_label_create(btn_minus);
    lv_label_set_text(lbl_m, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_m, &lv_font_montserrat_22, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_m,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_center(lbl_m);

    /* Value label */
    lv_obj_t *lbl_val = lv_label_create(row);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_22, 0);
    lv_obj_set_style_min_width(lbl_val, 50, 0);
    lv_obj_set_style_text_align(lbl_val, LV_TEXT_ALIGN_CENTER, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_val,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* [+] button */
    lv_obj_t *btn_plus = lv_button_create(row);
    lv_obj_set_size(btn_plus, STEPPER_BTN_SZ, STEPPER_BTN_SZ);
    lv_obj_set_style_radius(btn_plus, 10, 0);
    lv_obj_set_style_bg_opa(btn_plus, LV_OPA_COVER, 0);
    if (current_theme) {
        lv_obj_set_style_bg_color(btn_plus, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn_plus, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    lv_obj_set_style_border_width(btn_plus, 0, 0);
    lv_obj_set_style_shadow_width(btn_plus, 0, 0);

    lv_obj_t *lbl_p = lv_label_create(btn_plus);
    lv_label_set_text(lbl_p, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_22, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_p,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_center(lbl_p);

    /* Output pointers */
    if (out_minus) *out_minus = btn_minus;
    if (out_label) *out_label = lbl_val;
    if (out_plus)  *out_plus  = btn_plus;

    return row;
}

lv_obj_t *settings_make_toggle_row(lv_obj_t *parent, const char *text,
                                    lv_obj_t **out_sw)
{
    int gb = app_config_get()->color_brightness;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 58, 32);
    if (current_theme) {
        lv_obj_set_style_bg_color(sw, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(current_theme->progress_color),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(current_theme->text_color), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    }

    if (out_sw) *out_sw = sw;
    return row;
}

lv_obj_t *settings_make_textarea_row(lv_obj_t *parent, const char *label_text,
                                      const char *placeholder, bool password,
                                      lv_obj_t **out_ta)
{
    int gb = app_config_get()->color_brightness;

    /* Container for label + textarea (column layout) */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 4, 0);

    /* Label */
    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    /* Textarea */
    lv_obj_t *ta = lv_textarea_create(cont);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, 44);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder ? placeholder : "");
    if (password) {
        lv_textarea_set_password_mode(ta, true);
    }

    /* Style the textarea */
    if (current_theme) {
        lv_obj_set_style_bg_color(ta, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_80, 0);
        lv_obj_set_style_text_color(ta,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ta, 8, 0);
    }
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);

    /* Keyboard show/hide on focus/defocus */
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_defocus_cb, LV_EVENT_READY, NULL);

    if (out_ta) *out_ta = ta;
    return cont;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Recursive Theme Walker
 * ════════════════════════════════════════════════════════════════════════ */

static void apply_theme_to_widget(lv_obj_t *obj) {
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;

    if (lv_obj_check_type(obj, &lv_label_class)) {
        const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
        if (font == &lv_font_montserrat_16) {
            /* Card section titles / small labels */
            lv_obj_set_style_text_color(obj,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        } else if (font == &lv_font_montserrat_22 || font == &lv_font_montserrat_26) {
            /* Headers / large text */
            lv_obj_set_style_text_color(obj,
                lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
        } else {
            /* All other labels */
            lv_obj_set_style_text_color(obj,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color), LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->text_color), LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_border_width(obj, 1, 0);

        /* Style the dropdown list (opened popup) */
        lv_obj_t *list = lv_dropdown_get_list(obj);
        if (list) {
            lv_obj_set_style_bg_color(list, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(list,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_border_color(list, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_bg_color(list, lv_color_hex(current_theme->progress_color),
                                      LV_PART_SELECTED | LV_STATE_CHECKED);
        }
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_80, 0);
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(current_theme->bento_border), 0);
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(current_theme->bento_border), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    } else if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->progress_color),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), LV_PART_ITEMS);
    } else {
        /* Plain containers that are 1px tall = dividers */
        lv_coord_t h = lv_obj_get_height(obj);
        if (h == 1) {
            lv_obj_set_style_bg_color(obj, lv_color_hex(current_theme->bento_border), 0);
        }
    }
}

void settings_apply_theme_recursive(lv_obj_t *obj) {
    if (!obj) return;
    uint32_t child_cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        apply_theme_to_widget(child);
        settings_apply_theme_recursive(child);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Tabview Creation
 * ════════════════════════════════════════════════════════════════════════ */

lv_obj_t *settings_tabview_create(lv_obj_t *parent) {
    int gb = app_config_get()->color_brightness;

    /* Take a snapshot of config for dirty checking */
    config_snapshot = *app_config_get();

    /* Reset state */
    kb_visible   = false;
    needs_reboot = false;
    dirty        = false;
    keyboard     = NULL;
    save_bar     = NULL;
    lbl_save_btn = NULL;
    save_feedback_timer = NULL;

    /* ── Root Container ── */
    st_root = lv_obj_create(parent);
    lv_obj_remove_style_all(st_root);
    lv_obj_set_size(st_root,
                    SCREEN_SIZE - 2 * OUTER_PADDING,
                    SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_flex_flow(st_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(st_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Tabview ── */
    tabview = lv_tabview_create(st_root);
    lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tabview, TAB_BAR_H);
    lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));

    /* ── Add 4 Tabs ── */
    lv_obj_t *tab_display  = lv_tabview_add_tab(tabview, "Display");
    lv_obj_t *tab_nodes    = lv_tabview_add_tab(tabview, "Nodes");
    lv_obj_t *tab_behavior = lv_tabview_add_tab(tabview, "Behavior");
    lv_obj_t *tab_system   = lv_tabview_add_tab(tabview, "System");

    /* ── Configure each tab ── */
    lv_obj_t *tabs[] = { tab_display, tab_nodes, tab_behavior, tab_system };
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_pad_all(tabs[i], 8, 0);
        lv_obj_set_flex_flow(tabs[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tabs[i], LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tabs[i], 10, 0);
        lv_obj_set_scrollbar_mode(tabs[i], LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_opa(tabs[i], LV_OPA_TRANSP, 0);
    }

    /* ── Style Tab Bar (pad left for floating back button) ── */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tabview);
    if (current_theme) {
        lv_obj_set_style_bg_color(tab_bar, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tab_bar, 0, 0);
        lv_obj_set_style_pad_all(tab_bar, 0, 0);
    }
    lv_obj_set_style_pad_left(tab_bar, 48, 0);

    /* Style tab bar button text */
    if (current_theme) {
        uint32_t cnt = lv_obj_get_child_count(tab_bar);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
            lv_obj_set_style_text_color(btn,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
    }

    /* Remove default white bg from tabview content area */
    lv_obj_set_style_bg_opa(tabview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tabview, 0, 0);

    /* ── Back Button (floating overlay — does NOT affect tab indexing) ── */
    lv_obj_t *btn_back = lv_button_create(st_root);
    lv_obj_add_flag(btn_back, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(btn_back, 44, TAB_BAR_H);
    lv_obj_set_pos(btn_back, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme ? current_theme->bento_border : 0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_40, LV_STATE_PRESSED);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_back,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
    lv_obj_center(lbl_back);

    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ── Delegate Tab Content to Modules ── */
    settings_tab_display_create(tab_display);
    settings_tab_nodes_create(tab_nodes);
    settings_tab_behavior_create(tab_behavior);
    settings_tab_system_create(tab_system);

    /* ── Save Bar (initially hidden) ── */
    save_bar = lv_obj_create(st_root);
    lv_obj_remove_style_all(save_bar);
    lv_obj_set_size(save_bar, LV_PCT(100), SAVE_BAR_H);
    lv_obj_set_flex_flow(save_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(save_bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (current_theme) {
        lv_obj_set_style_bg_color(save_bar, lv_color_hex(current_theme->progress_color), 0);
    }
    lv_obj_set_style_bg_opa(save_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(save_bar, 12, 0);
    lv_obj_add_flag(save_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(save_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(save_bar, save_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_save_btn = lv_label_create(save_bar);
    lv_label_set_text(lbl_save_btn, LV_SYMBOL_SAVE " Save");
    lv_obj_set_style_text_font(lbl_save_btn, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_save_btn, lv_color_white(), 0);
    lv_obj_center(lbl_save_btn);

    return st_root;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Destroy — tear down all settings widgets to reclaim internal heap
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tabview_destroy(void) {
    /* Color picker is parented to lv_layer_top(), not st_root */
    color_picker_hide();

    /* Cancel any pending save feedback timer (not parented to any object) */
    if (save_feedback_timer) {
        lv_timer_delete(save_feedback_timer);
        save_feedback_timer = NULL;
    }

    /* Tell each tab module to NULL out its static pointers */
    settings_tab_display_destroy();
    settings_tab_nodes_destroy();
    settings_tab_behavior_destroy();
    settings_tab_system_destroy();

    /* Delete the root LVGL object — recursively frees tabview,
     * keyboard, save bar, back button, and all tab contents */
    if (st_root) {
        lv_obj_delete(st_root);
    }

    /* NULL out all module-level statics */
    st_root      = NULL;
    tabview      = NULL;
    keyboard     = NULL;
    save_bar     = NULL;
    lbl_save_btn = NULL;
    kb_visible   = false;
    needs_reboot = false;
    dirty        = false;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Refresh
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tabview_refresh(void) {
    config_snapshot = *app_config_get();
    needs_reboot = false;
    dirty = false;

    if (save_bar) lv_obj_add_flag(save_bar, LV_OBJ_FLAG_HIDDEN);

    settings_tab_display_refresh();
    settings_tab_nodes_refresh();
    settings_tab_behavior_refresh();
    settings_tab_system_refresh();
}

/* ════════════════════════════════════════════════════════════════════════
 *  Apply Theme
 * ════════════════════════════════════════════════════════════════════════ */

void settings_tabview_apply_theme(void) {
    if (!st_root || !current_theme) return;

    int gb = app_config_get()->color_brightness;

    /* Style tab bar */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(current_theme->bento_bg), 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);

    /* Style tab bar button text */
    uint32_t cnt = lv_obj_get_child_count(tab_bar);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
        lv_obj_set_style_text_color(btn,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* Style keyboard */
    if (keyboard) {
        lv_obj_set_style_bg_color(keyboard, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_color(keyboard, lv_color_hex(current_theme->bento_border), LV_PART_ITEMS);
        lv_obj_set_style_text_color(keyboard,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), LV_PART_ITEMS);
    }

    /* Style save bar */
    if (save_bar) {
        lv_obj_set_style_bg_color(save_bar, lv_color_hex(current_theme->progress_color), 0);
    }

    /* Delegate to tab modules */
    settings_tab_display_apply_theme();
    settings_tab_nodes_apply_theme();
    settings_tab_behavior_apply_theme();
    settings_tab_system_apply_theme();
    color_picker_apply_theme();

    lv_obj_invalidate(st_root);
}
