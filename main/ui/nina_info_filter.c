/**
 * @file nina_info_filter.c
 * @brief Filter Wheel info overlay content — current filter, filter list, device status.
 */

#include "nina_info_overlay.h"
#include "info_overlay_types.h"
#include "themes.h"
#include "app_config.h"
#include "ui_helpers.h"
#include "ui_styles.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

/* current_theme is defined in nina_dashboard.c */
extern const theme_t *current_theme;

/* ── Constants ─────────────────────────────────────────────────────── */
#define MAX_FILTER_SLOTS 10

/* ── Static widget references ──────────────────────────────────────── */

/* Current filter hero */
static lv_obj_t *lbl_current_filter = NULL;
static lv_obj_t *lbl_position       = NULL;

/* Filter list */
static lv_obj_t *filter_slots[MAX_FILTER_SLOTS];
static lv_obj_t *lbl_slot_nums[MAX_FILTER_SLOTS];
static lv_obj_t *lbl_slot_names[MAX_FILTER_SLOTS];
static lv_obj_t *lbl_active_markers[MAX_FILTER_SLOTS];

/* Status */
static lv_obj_t *lbl_device_val  = NULL;
static lv_obj_t *lbl_moving_val  = NULL;

/* No-data label */
static lv_obj_t *lbl_no_data = NULL;

/* Content container */
static lv_obj_t *content_root = NULL;

/* ── Local helpers ─────────────────────────────────────────────────── */

static lv_obj_t *make_info_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_bento_box, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void make_info_section(lv_obj_t *parent, const char *title) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
}

static lv_obj_t *make_info_kv(lv_obj_t *parent, const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_key = lv_label_create(row);
    lv_label_set_text(lbl_key, key);
    lv_obj_set_style_text_font(lbl_key, &lv_font_montserrat_18, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_key,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_18, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_val,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    return lbl_val;
}

/* ── Build ─────────────────────────────────────────────────────────── */

void build_filter_content(lv_obj_t *content) {
    content_root = content;

    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 12, 0);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    int gb = current_theme ? app_config_get()->color_brightness : 100;

    /* ── Current Filter hero card ── */
    {
        lv_obj_t *card = make_info_card(content);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_style_pad_all(card, 16, 0);
        make_info_section(card, "CURRENT FILTER");

        lv_obj_t *current_row = lv_obj_create(card);
        lv_obj_remove_style_all(current_row);
        lv_obj_set_width(current_row, LV_PCT(100));
        lv_obj_set_height(current_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(current_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(current_row, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(current_row, 12, 0);
        lv_obj_set_style_pad_ver(current_row, 8, 0);

        /* Arrow indicator */
        lv_obj_t *lbl_arrow = lv_label_create(current_row);
        lv_label_set_text(lbl_arrow, LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(lbl_arrow, &lv_font_montserrat_22, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_arrow,
                lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)), 0);

        /* Current filter name (hero size) */
        lbl_current_filter = lv_label_create(current_row);
        lv_label_set_text(lbl_current_filter, "--");
        lv_obj_set_style_text_font(lbl_current_filter, &lv_font_montserrat_32, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_current_filter,
                lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);

        /* Position label */
        lbl_position = lv_label_create(current_row);
        lv_label_set_text(lbl_position, "");
        lv_obj_set_style_text_font(lbl_position, &lv_font_montserrat_20, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_position,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    /* ── Filter list card ── */
    {
        lv_obj_t *card = make_info_card(content);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_pad_all(card, 12, 0);
        make_info_section(card, "AVAILABLE FILTERS");

        for (int i = 0; i < MAX_FILTER_SLOTS; i++) {
            lv_obj_t *slot = lv_obj_create(card);
            lv_obj_remove_style_all(slot);
            lv_obj_set_width(slot, LV_PCT(100));
            lv_obj_set_height(slot, 40);
            lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(slot, 12, 0);
            lv_obj_set_style_pad_hor(slot, 8, 0);
            lv_obj_set_style_radius(slot, 12, 0);
            lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
            lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(slot, LV_OBJ_FLAG_HIDDEN);
            filter_slots[i] = slot;

            /* Position number */
            lv_obj_t *num = lv_label_create(slot);
            lv_label_set_text(num, "");
            lv_obj_set_style_text_font(num, &lv_font_montserrat_18, 0);
            lv_obj_set_width(num, 30);
            if (current_theme)
                lv_obj_set_style_text_color(num,
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
            lbl_slot_nums[i] = num;

            /* Filter name */
            lv_obj_t *name = lv_label_create(slot);
            lv_label_set_text(name, "");
            lv_obj_set_style_text_font(name, &lv_font_montserrat_22, 0);
            lv_obj_set_flex_grow(name, 1);
            if (current_theme)
                lv_obj_set_style_text_color(name,
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
            lbl_slot_names[i] = name;

            /* Active marker (check icon) */
            lv_obj_t *marker = lv_label_create(slot);
            lv_label_set_text(marker, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(marker, &lv_font_montserrat_16, 0);
            if (current_theme)
                lv_obj_set_style_text_color(marker,
                    lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)), 0);
            lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
            lbl_active_markers[i] = marker;
        }
    }

    /* ── Status card ── */
    {
        lv_obj_t *card = make_info_card(content);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_style_pad_all(card, 10, 0);
        make_info_section(card, "STATUS");

        lv_obj_t *status_row = lv_obj_create(card);
        lv_obj_remove_style_all(status_row);
        lv_obj_set_width(status_row, LV_PCT(100));
        lv_obj_set_height(status_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Left: Device */
        lv_obj_t *left = lv_obj_create(status_row);
        lv_obj_remove_style_all(left);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lbl_device_val = make_info_kv(left, "Device");

        /* Right: Moving */
        lv_obj_t *right_col = lv_obj_create(status_row);
        lv_obj_remove_style_all(right_col);
        lv_obj_set_flex_grow(right_col, 1);
        lv_obj_set_height(right_col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
        lbl_moving_val = make_info_kv(right_col, "Connected");
    }

    /* ── No-data message ── */
    lbl_no_data = lv_label_create(content);
    lv_label_set_text(lbl_no_data, "No filter wheel data");
    lv_obj_set_style_text_font(lbl_no_data, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(lbl_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_align(lbl_no_data, LV_ALIGN_CENTER);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_no_data,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    lv_obj_add_flag(lbl_no_data, LV_OBJ_FLAG_HIDDEN);
}

/* ── Populate ──────────────────────────────────────────────────────── */

void populate_filter_data(const filter_detail_data_t *data) {
    if (!content_root) return;

    char buf[64];

    if (!data || data->filter_count == 0) {
        uint32_t cnt = lv_obj_get_child_count(content_root);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(content_root, i);
            if (child == lbl_no_data)
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Show all, hide no-data */
    {
        uint32_t cnt = lv_obj_get_child_count(content_root);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(content_root, i);
            if (child == lbl_no_data)
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int gb = current_theme ? app_config_get()->color_brightness : 100;

    /* Current filter hero */
    lv_label_set_text(lbl_current_filter,
        data->current_filter[0] ? data->current_filter : "--");

    if (current_theme && data->current_filter[0]) {
        uint32_t fc = app_config_get_filter_color(data->current_filter, 0);
        if (fc != 0)
            lv_obj_set_style_text_color(lbl_current_filter,
                lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
        else
            lv_obj_set_style_text_color(lbl_current_filter,
                lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);
    }

    snprintf(buf, sizeof(buf), "(Position %d)", data->current_position);
    lv_label_set_text(lbl_position, buf);

    /* Filter list */
    for (int i = 0; i < MAX_FILTER_SLOTS; i++) {
        if (i < data->filter_count) {
            lv_obj_clear_flag(filter_slots[i], LV_OBJ_FLAG_HIDDEN);

            snprintf(buf, sizeof(buf), "#%d", data->filters[i].id);
            lv_label_set_text(lbl_slot_nums[i], buf);
            lv_label_set_text(lbl_slot_names[i], data->filters[i].name);

            bool is_active = (strcmp(data->filters[i].name, data->current_filter) == 0);

            /* Highlight active filter slot */
            if (is_active) {
                if (current_theme) {
                    lv_obj_set_style_bg_color(filter_slots[i],
                        lv_color_hex(current_theme->bento_border), 0);
                    lv_obj_set_style_bg_opa(filter_slots[i], LV_OPA_50, 0);
                    lv_obj_set_style_border_width(filter_slots[i], 1, 0);
                    lv_obj_set_style_border_color(filter_slots[i],
                        lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)), 0);
                }
                lv_obj_clear_flag(lbl_active_markers[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_style_bg_opa(filter_slots[i], LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(filter_slots[i], 0, 0);
                lv_obj_add_flag(lbl_active_markers[i], LV_OBJ_FLAG_HIDDEN);
            }

            /* Per-filter color */
            uint32_t fc = app_config_get_filter_color(data->filters[i].name, 0);
            if (fc != 0 && current_theme) {
                lv_obj_set_style_text_color(lbl_slot_names[i],
                    lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
            } else if (current_theme) {
                uint32_t col = is_active
                    ? current_theme->filter_text_color
                    : current_theme->label_color;
                lv_obj_set_style_text_color(lbl_slot_names[i],
                    lv_color_hex(app_config_apply_brightness(col, gb)), 0);
            }
        } else {
            lv_obj_add_flag(filter_slots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Status */
    lv_label_set_text(lbl_device_val,
        data->device_name[0] ? data->device_name : "--");
    lv_label_set_text(lbl_moving_val, data->connected ? "Yes" : "No");

    if (current_theme) {
        uint32_t conn_col = data->connected ? 0x4ade80 : current_theme->label_color;
        lv_obj_set_style_text_color(lbl_moving_val,
            lv_color_hex(app_config_apply_brightness(conn_col, gb)), 0);
    }
}
