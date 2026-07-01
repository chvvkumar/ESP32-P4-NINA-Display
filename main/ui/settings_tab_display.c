/**
 * @file settings_tab_display.c
 * @brief Display tab for on-device settings tabview.
 *
 * Contains three cards:
 *   1. Appearance — theme dropdown, widget style dropdown, rotation dropdown
 *   2. Brightness — screen backlight slider, text brightness slider
 *   3. Page Navigation — segmented control (Manual/Fixed/Cycle) with conditional
 *      sub-sections for Fixed (pinned page) and Cycle (interval, transition,
 *      skip-offline, page checkboxes) modes.
 *
 * All changes are applied live; the parent tabview handles save/dirty state.
 */

#include "settings_tab_display.h"
#include "nina_settings_tabview.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "page_registry.h"
#include "app_config.h"
#include "themes.h"
#include "ui_styles.h"
#include "tasks.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include <stdio.h>
#include <string.h>

/* ── Static widget references ───────────────────────────────────────── */
static lv_obj_t *tab_root = NULL;

/* Appearance */
static lv_obj_t *dd_theme = NULL;
static lv_obj_t *dd_widget_style = NULL;
static lv_obj_t *dd_rotation = NULL;

/* Brightness */
static lv_obj_t *slider_backlight = NULL;
static lv_obj_t *lbl_backlight_val = NULL;
static lv_obj_t *slider_text_bright = NULL;
static lv_obj_t *lbl_text_bright_val = NULL;

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

/* ── Rotation dropdown options ───────────────────────────────────────── */
static const char *rotation_opts = "0\xc2\xb0\n90\xc2\xb0\n180\xc2\xb0\n270\xc2\xb0";

/* ── Forward declarations ───────────────────────────────────────────── */
static void theme_dd_changed_cb(lv_event_t *e);
static void widget_style_dd_changed_cb(lv_event_t *e);
static void rotation_changed_cb(lv_event_t *e);
static void backlight_changed_cb(lv_event_t *e);
static void text_bright_changed_cb(lv_event_t *e);
static void page_mode_changed_cb(lv_event_t *e);
static void pinned_page_changed_cb(lv_event_t *e);
static void interval_minus_cb(lv_event_t *e);
static void interval_plus_cb(lv_event_t *e);
static void transition_changed_cb(lv_event_t *e);
static void skip_offline_changed_cb(lv_event_t *e);
static void page_checkbox_changed_cb(lv_event_t *e);
static void update_mode_visibility(uint32_t mode);

/* ── Home-page dropdown <-> page_ref registry id map ──
 *
 * The Home Page setting persists a page_ref registry id (0..PAGE_REF_ID_MAX-1)
 * in active_page_override. There is no "Auto" option; the default is Summary
 * (PAGE_REF_SUMMARY == 0).
 *
 * The dropdown lists only TARGETABLE entries whose kind is PAGE or
 * IMAGE_SOURCE, in registry order. Overlays and Settings are excluded, so a
 * dropdown option index is NOT equal to the page_ref id. s_home_dd_ids[] is the
 * parallel mapping: dropdown option index -> page_ref id. s_home_dd_count is
 * the number of options built. Both are populated by build_home_options(). */
static page_ref_t s_home_dd_ids[PAGE_REF_ID_MAX];
static int        s_home_dd_count = 0;

/* True if a registry entry belongs in the Home Page dropdown. */
static bool home_entry_is_listed(const page_ref_entry_t *e)
{
    if (e == NULL || !e->targetable) return false;
    return (e->kind == PAGE_REF_KIND_PAGE ||
            e->kind == PAGE_REF_KIND_IMAGE_SOURCE);
}

/* Build the dropdown options string (newline-separated labels in registry
 * order) and the parallel option-index -> id map. */
static void build_home_options(char *buf, size_t buf_size)
{
    buf[0] = '\0';
    s_home_dd_count = 0;

    int count = page_ref_count();
    for (int i = 0; i < count; i++) {
        const page_ref_entry_t *e = page_ref_get(i);
        if (!home_entry_is_listed(e)) continue;
        if (s_home_dd_count >= (int)(sizeof(s_home_dd_ids) / sizeof(s_home_dd_ids[0]))) break;

        if (s_home_dd_count > 0) {
            strncat(buf, "\n", buf_size - strlen(buf) - 1);
        }
        strncat(buf, e->label, buf_size - strlen(buf) - 1);
        s_home_dd_ids[s_home_dd_count] = e->id;
        s_home_dd_count++;
    }
}

/* Dropdown option index -> stored page_ref id. Falls back to Summary. */
static int home_dd_sel_to_abs(uint32_t sel)
{
    if ((int)sel < s_home_dd_count) {
        return (int)s_home_dd_ids[sel];
    }
    return (int)PAGE_REF_SUMMARY;
}

/* Stored page_ref id -> dropdown option index. Falls back to the Summary
 * option (or 0 if Summary is somehow not listed). */
static uint32_t home_abs_to_dd_sel(int id)
{
    int summary_opt = 0;
    for (int i = 0; i < s_home_dd_count; i++) {
        if ((int)s_home_dd_ids[i] == id) return (uint32_t)i;
        if (s_home_dd_ids[i] == PAGE_REF_SUMMARY) summary_opt = i;
    }
    return (uint32_t)summary_opt;
}

/* ── Slideshow order-list helpers (canonical membership) ──
 *
 * The nav arbiter slideshow reads ONLY auto_rotate_order[0..7] +
 * auto_rotate_order_ext (9 slots). Each slot holds a "bit code" page id:
 *   0=Summary, 1=NINA1, 2=NINA2, 3=NINA3, 4=SysInfo,
 *   5=AllSky, 6=Spotify, 7=Clock, 8=ImageDisplay; 0xFF = empty.
 * The on-device checkbox table exposes only codes 0..5 (page_names below);
 * codes 6/7/8 are preserved across edits since this UI cannot express them. */

#define AR_ORDER_SLOTS  9   /* 8 in auto_rotate_order[] + 1 ext */

/* Read slot i (0..8) of the canonical order list from cfg. */
static uint8_t ar_order_get(const app_config_t *cfg, int i)
{
    return (i < 8) ? cfg->auto_rotate_order[i] : cfg->auto_rotate_order_ext;
}

/* Write slot i (0..8) of the canonical order list in cfg. */
static void ar_order_set(app_config_t *cfg, int i, uint8_t v)
{
    if (i < 8) cfg->auto_rotate_order[i] = v;
    else       cfg->auto_rotate_order_ext = v;
}

/* True if bit-code page is present anywhere in the order list. */
static bool ar_order_contains(const app_config_t *cfg, uint8_t code)
{
    for (int i = 0; i < AR_ORDER_SLOTS; i++) {
        if (ar_order_get(cfg, i) == code) return true;
    }
    return false;
}

/* Add a bit-code page to the order list (append into first empty slot),
 * preserving existing order. No-op if already present or list is full. */
static void ar_order_add(app_config_t *cfg, uint8_t code)
{
    if (ar_order_contains(cfg, code)) return;
    for (int i = 0; i < AR_ORDER_SLOTS; i++) {
        if (ar_order_get(cfg, i) == 0xFF) { ar_order_set(cfg, i, code); return; }
    }
}

/* Remove a bit-code page from the order list and compact remaining entries
 * forward so order is preserved and trailing slots become 0xFF. */
static void ar_order_remove(app_config_t *cfg, uint8_t code)
{
    uint8_t kept[AR_ORDER_SLOTS];
    int n = 0;
    for (int i = 0; i < AR_ORDER_SLOTS; i++) {
        uint8_t v = ar_order_get(cfg, i);
        if (v == 0xFF || v == code) continue;
        kept[n++] = v;
    }
    for (int i = 0; i < AR_ORDER_SLOTS; i++) {
        ar_order_set(cfg, i, (i < n) ? kept[i] : 0xFF);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Appearance Card — Theme / Widget Style / Rotation
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
        lv_obj_t *row = settings_make_row_lg(card);

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
        lv_obj_t *row = settings_make_row_lg(card);

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

    /* ── Rotation row (moved from Behavior Hardware card) ── */
    {
        lv_obj_t *row = settings_make_row_lg(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Rotation");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_rotation = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_rotation, rotation_opts);
        lv_obj_set_width(dd_rotation, 160);
        lv_dropdown_set_selected(dd_rotation, app_config_get()->screen_rotation);

        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_bg_color(dd_rotation, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_rotation, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(dd_rotation,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_border_color(dd_rotation, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_rotation, 1, 0);
        }

        lv_obj_add_event_cb(dd_rotation, rotation_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Brightness Card — Screen backlight / Text brightness
 * ═══════════════════════════════════════════════════════════════════════ */

static void create_brightness_card(lv_obj_t *parent)
{
    lv_obj_t *card = settings_make_card(parent, "Brightness");

    /* ── Screen backlight row (moved from Behavior Hardware card) ── */
    {
        lv_obj_t *row = settings_make_row_lg(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Screen");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_min_width(lbl, 100, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        /* Slider + value in a sub-container */
        lv_obj_t *ctrl = lv_obj_create(row);
        lv_obj_remove_style_all(ctrl);
        lv_obj_set_flex_grow(ctrl, 1);
        lv_obj_set_height(ctrl, 50);
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
        lv_obj_set_style_text_font(lbl_backlight_val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_min_width(lbl_backlight_val, 50, 0);
        lv_obj_set_style_text_align(lbl_backlight_val, LV_TEXT_ALIGN_RIGHT, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl_backlight_val,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
        lv_label_set_text_fmt(lbl_backlight_val, "%d%%", app_config_get()->brightness);
    }

    settings_make_divider(card);

    /* ── Text brightness row (moved out of Appearance) ── */
    {
        lv_obj_t *row = settings_make_row_lg(card);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Text");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_min_width(lbl, 100, 0);
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
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Page Navigation Card — Manual / Fixed / Cycle
 * ═══════════════════════════════════════════════════════════════════════ */

static lv_obj_t *create_page_nav_card(lv_obj_t *parent)
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
        lv_label_set_text(lbl, "Home Page");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        dd_pinned_page = lv_dropdown_create(row);
        lv_obj_set_width(dd_pinned_page, 200);
        {
            /* Generous buffer: up to PAGE_REF_ID_MAX (24) labels, each well
             * under 24 chars including the newline separator. */
            char home_opts[PAGE_REF_ID_MAX * 24];
            build_home_options(home_opts, sizeof(home_opts));
            lv_dropdown_set_options(dd_pinned_page, home_opts);
        }

        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(dd_pinned_page,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lv_obj_set_style_bg_color(dd_pinned_page, lv_color_hex(current_theme->bento_bg), 0);
            lv_obj_set_style_bg_opa(dd_pinned_page, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(dd_pinned_page, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_border_width(dd_pinned_page, 1, 0);
        }

        /* Set initial selection from config (page_ref id -> option index). */
        lv_dropdown_set_selected(dd_pinned_page,
                                 home_abs_to_dd_sel(app_config_get()->active_page_override));

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

        /* Checkbox index i maps directly to canonical order-list bit code i:
         * 0=Summary, 1=NINA1, 2=NINA2, 3=NINA3, 4=SysInfo, 5=AllSky. */
        static const char *page_names[] = {
            "Summary", "NINA 1", "NINA 2", "NINA 3", "SysInfo", "AllSky"
        };

        const app_config_t *cfg_cb = app_config_get();

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
            lv_obj_set_style_text_font(cb_pages[i], &lv_font_montserrat_18, 0);
            lv_obj_set_style_min_width(cb_pages[i], 130, 0);

            if (ar_order_contains(cfg_cb, (uint8_t)i)) {
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
    lv_label_set_text(ar_hint, "When enabled, overrides \"Switch page when idle\"");
    lv_obj_set_style_text_font(ar_hint, &lv_font_montserrat_14, 0);
    if (theme_is_red_night(current_theme)) {
        lv_obj_set_style_text_color(ar_hint, lv_color_hex(current_theme->label_color), 0);
    } else {
        lv_obj_set_style_text_color(ar_hint, lv_color_hex(0x888888), 0);
    }

    /* ── Determine initial mode from config ──
     * active_page_override always holds a concrete page_ref id now (no -1/Auto),
     * so a non-cycling config is the Fixed (pinned Home Page) mode. */
    app_config_t *cfg = app_config_get();
    uint32_t initial_mode;
    if (cfg->auto_rotate_enabled) {
        initial_mode = 2;  /* Cycle */
    } else {
        initial_mode = 1;  /* Fixed */
    }
    /* Set the correct button as checked (no one_checked, so no auto-check on button 0) */
    lv_buttonmatrix_set_button_ctrl(seg_mode, initial_mode, LV_BUTTONMATRIX_CTRL_CHECKED);
    update_mode_visibility(initial_mode);

    return card;
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

static void rotation_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t idx = lv_dropdown_get_selected(dd_rotation);
    app_config_get()->screen_rotation = (uint8_t)idx;
    lv_display_set_rotation(lv_display_get_default(), idx);
    settings_mark_dirty(false);
}

static void backlight_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int val = lv_slider_get_value(slider_backlight);
    app_config_get()->brightness = val;
    bsp_display_brightness_set(val);
    lv_label_set_text_fmt(lbl_backlight_val, "%d%%", val);
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

    /* active_page_override now persists a page_ref registry id (see
     * page_registry.h); there is no -1/Auto sentinel. The Home Page is always a
     * concrete page_ref id taken from the Home Page dropdown. */
    int home_id = (int)PAGE_REF_SUMMARY;
    if (dd_pinned_page) {
        uint32_t dd_sel = lv_dropdown_get_selected(dd_pinned_page);
        home_id = home_dd_sel_to_abs(dd_sel);
    }

    if (sel == 0) {
        /* Manual */
        cfg->active_page_override = (int8_t)home_id;
        cfg->auto_rotate_enabled = false;
    } else if (sel == 1) {
        /* Fixed */
        cfg->auto_rotate_enabled = false;
        cfg->active_page_override = (int8_t)home_id;
        /* Live apply: navigate to the chosen Home Page now. page_ref_navigate
         * resolves the id (setting the image-source override for image-source
         * ids) and issues a USER-claim navigation. It does not take the LVGL
         * lock, so it is safe from this event callback. */
        page_ref_navigate((page_ref_t)home_id);
    } else if (sel == 2) {
        /* Cycle */
        cfg->active_page_override = (int8_t)home_id;
        cfg->auto_rotate_enabled = true;
        cfg->idle_page_override_enabled = false;   /* exclusivity: auto-rotate wins */
    }

    /* Centralized exclusivity rule (auto-rotate wins). The behavior tab's
     * idle-override switch picks up the cleared flag on its next refresh. */
    app_config_normalize_nav_exclusivity(cfg);

    settings_mark_dirty(false);
}

static void pinned_page_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t sel = lv_dropdown_get_selected(dd_pinned_page);
    int home_id = home_dd_sel_to_abs(sel);
    app_config_get()->active_page_override = (int8_t)home_id;
    /* Live apply: navigate to the newly selected Home Page now. Safe from this
     * event callback — page_ref_navigate does not take the LVGL lock. */
    page_ref_navigate((page_ref_t)home_id);
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
    int bit = (int)(uintptr_t)lv_event_get_user_data(e);   /* canonical order code */
    lv_obj_t *cb = lv_event_get_target(e);
    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);

    /* Mutate the live config so the nav arbiter slideshow sees the new
     * membership immediately, preserving existing order. Checked pages are
     * appended; unchecked pages are removed and the list is compacted.
     * Codes 6/7/8 (Spotify/Clock/ImageDisplay) are not in this checkbox set
     * and are preserved untouched. */
    app_config_t *cfg = app_config_get();
    if (checked) ar_order_add(cfg, (uint8_t)bit);
    else         ar_order_remove(cfg, (uint8_t)bit);

    /* Persist the canonical list to NVS (snapshot pattern). */
    app_config_t snap = app_config_get_snapshot();
    memcpy(snap.auto_rotate_order, cfg->auto_rotate_order, sizeof(snap.auto_rotate_order));
    snap.auto_rotate_order_ext = cfg->auto_rotate_order_ext;
    app_config_save(&snap);

    settings_mark_dirty(false);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void settings_tab_display_destroy(void) {
    tab_root = NULL;
    dd_theme = NULL;
    dd_widget_style = NULL;
    dd_rotation = NULL;
    slider_backlight = NULL;
    lbl_backlight_val = NULL;
    slider_text_bright = NULL;
    lbl_text_bright_val = NULL;
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
    create_brightness_card(parent);
    lv_obj_t *last_card = create_page_nav_card(parent);

    /* Fill the viewport: grow the final card so the empty band below it is
     * <100px (SETRD-01). The tab page is a COLUMN flex with concrete height,
     * so growing the last child consumes the remaining vertical space. */
    if (last_card) {
        lv_obj_set_flex_grow(last_card, 1);
    }
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

    if (dd_rotation) {
        lv_dropdown_set_selected(dd_rotation, (uint32_t)cfg->screen_rotation);
    }

    /* Brightness widgets */
    if (slider_backlight) {
        lv_slider_set_value(slider_backlight, cfg->brightness, LV_ANIM_OFF);
    }
    if (lbl_backlight_val) {
        lv_label_set_text_fmt(lbl_backlight_val, "%d%%", cfg->brightness);
    }
    if (slider_text_bright) {
        lv_slider_set_value(slider_text_bright, cfg->color_brightness, LV_ANIM_OFF);
    }
    if (lbl_text_bright_val) {
        lv_label_set_text_fmt(lbl_text_bright_val, "%d%%", cfg->color_brightness);
    }

    /* Page navigation — determine mode. active_page_override always holds a
     * concrete page_ref id now (no -1/Auto), so a non-cycling config is Fixed. */
    uint32_t mode;
    if (cfg->auto_rotate_enabled) {
        mode = 2;  /* Cycle */
    } else {
        mode = 1;  /* Fixed */
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

    /* Fixed sub-section (page_ref id -> dropdown option index) */
    if (dd_pinned_page) {
        lv_dropdown_set_selected(dd_pinned_page,
                                 home_abs_to_dd_sel(cfg->active_page_override));
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

    /* Page checkboxes — membership read from canonical order list.
     * Checkbox index i maps directly to order-list bit code i. */
    for (int i = 0; i < 6; i++) {
        if (cb_pages[i]) {
            if (ar_order_contains(cfg, (uint8_t)i)) {
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
