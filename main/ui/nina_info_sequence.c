/**
 * @file nina_info_sequence.c
 * @brief Sequence Details info overlay content — target, current step, filter breakdown.
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

/* ── Static widget references ──────────────────────────────────────── */

/* Target card */
static lv_obj_t *lbl_target_name    = NULL;
static lv_obj_t *lbl_time_remaining = NULL;

/* Current step card */
static lv_obj_t *lbl_container_val  = NULL;
static lv_obj_t *lbl_step_val       = NULL;
static lv_obj_t *lbl_filter_val     = NULL;
static lv_obj_t *lbl_exp_time_val   = NULL;
static lv_obj_t *lbl_current_progress = NULL;
static lv_obj_t *bar_current        = NULL;

/* Filter breakdown */
static lv_obj_t *filter_rows[MAX_SEQ_FILTERS];
static lv_obj_t *lbl_filter_names[MAX_SEQ_FILTERS];
static lv_obj_t *bar_filters[MAX_SEQ_FILTERS];
static lv_obj_t *lbl_filter_counts[MAX_SEQ_FILTERS];

/* Totals */
static lv_obj_t *lbl_totals = NULL;

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

void build_sequence_content(lv_obj_t *content) {
    content_root = content;

    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    int gb = current_theme ? app_config_get()->color_brightness : 100;

    /* ── Target + Time remaining card ── */
    {
        lv_obj_t *card = make_info_card(content);

        lv_obj_t *target_row = lv_obj_create(card);
        lv_obj_remove_style_all(target_row);
        lv_obj_set_width(target_row, LV_PCT(100));
        lv_obj_set_height(target_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(target_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(target_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Target name */
        lbl_target_name = lv_label_create(target_row);
        lv_label_set_text(lbl_target_name, "--");
        lv_obj_set_style_text_font(lbl_target_name, &lv_font_montserrat_28, 0);
        lv_label_set_long_mode(lbl_target_name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(lbl_target_name, 1);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_target_name,
                lv_color_hex(app_config_apply_brightness(current_theme->target_name_color, gb)), 0);

        /* Time remaining block */
        lv_obj_t *time_block = lv_obj_create(target_row);
        lv_obj_remove_style_all(time_block);
        lv_obj_set_size(time_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(time_block, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(time_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_row(time_block, 2, 0);

        lv_obj_t *lbl_rem_hdr = lv_label_create(time_block);
        lv_label_set_text(lbl_rem_hdr, "REMAINING");
        lv_obj_set_style_text_font(lbl_rem_hdr, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(lbl_rem_hdr, 1, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_rem_hdr,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

        lbl_time_remaining = lv_label_create(time_block);
        lv_label_set_text(lbl_time_remaining, "--:--:--");
        lv_obj_set_style_text_font(lbl_time_remaining, &lv_font_montserrat_24, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_time_remaining,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* ── Current Step card ── */
    {
        lv_obj_t *card = make_info_card(content);
        make_info_section(card, "CURRENT STEP");

        /* Step info row: two columns side by side */
        lv_obj_t *step_row = lv_obj_create(card);
        lv_obj_remove_style_all(step_row);
        lv_obj_set_width(step_row, LV_PCT(100));
        lv_obj_set_height(step_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(step_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(step_row, 16, 0);
        lv_obj_remove_flag(step_row, LV_OBJ_FLAG_SCROLLABLE);

        /* Left: Container + Step */
        lv_obj_t *left = lv_obj_create(step_row);
        lv_obj_remove_style_all(left);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 4, 0);

        lbl_container_val = make_info_kv(left, "Container");
        lbl_step_val      = make_info_kv(left, "Step");

        /* Right: Filter + Exposure */
        lv_obj_t *right = lv_obj_create(step_row);
        lv_obj_remove_style_all(right);
        lv_obj_set_flex_grow(right, 1);
        lv_obj_set_height(right, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(right, 4, 0);

        lbl_filter_val   = make_info_kv(right, "Filter");
        lbl_exp_time_val = make_info_kv(right, "Exposure");

        /* Progress row */
        lbl_current_progress = lv_label_create(card);
        lv_label_set_text(lbl_current_progress, "-- / --");
        lv_obj_set_style_text_font(lbl_current_progress, &lv_font_montserrat_22, 0);
        lv_obj_set_style_pad_top(lbl_current_progress, 6, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_current_progress,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

        /* Progress bar */
        bar_current = lv_bar_create(card);
        lv_obj_set_width(bar_current, LV_PCT(100));
        lv_obj_set_height(bar_current, 8);
        lv_bar_set_range(bar_current, 0, 100);
        lv_bar_set_value(bar_current, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(bar_current, 4, 0);
        lv_obj_set_style_radius(bar_current, 4, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar_current, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(bar_current, LV_OPA_COVER, LV_PART_INDICATOR);
        if (current_theme) {
            lv_obj_set_style_bg_color(bar_current, lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_bg_color(bar_current,
                lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)),
                LV_PART_INDICATOR);
        }
    }

    /* ── Filter Breakdown card ── */
    {
        lv_obj_t *card = make_info_card(content);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_pad_all(card, 10, 0);
        make_info_section(card, "FILTER BREAKDOWN");

        for (int i = 0; i < MAX_SEQ_FILTERS; i++) {
            lv_obj_t *row = lv_obj_create(card);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 36);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            filter_rows[i] = row;

            /* Filter name (fixed width) */
            lv_obj_t *name = lv_label_create(row);
            lv_label_set_text(name, "");
            lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
            lv_obj_set_width(name, 54);
            lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
            if (current_theme)
                lv_obj_set_style_text_color(name,
                    lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);
            lbl_filter_names[i] = name;

            /* Progress bar (flexible) */
            lv_obj_t *bar = lv_bar_create(row);
            lv_obj_set_flex_grow(bar, 1);
            lv_obj_set_height(bar, 10);
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, 0, LV_ANIM_OFF);
            lv_obj_set_style_radius(bar, 5, 0);
            lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
            if (current_theme) {
                lv_obj_set_style_bg_color(bar, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_bg_color(bar,
                    lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)),
                    LV_PART_INDICATOR);
            }
            bar_filters[i] = bar;

            /* Count label (fixed width) */
            lv_obj_t *count = lv_label_create(row);
            lv_label_set_text(count, "");
            lv_obj_set_style_text_font(count, &lv_font_montserrat_16, 0);
            lv_obj_set_width(count, 64);
            if (current_theme)
                lv_obj_set_style_text_color(count,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            lbl_filter_counts[i] = count;
        }
    }

    /* ── Totals row ── */
    {
        lv_obj_t *totals_row = lv_obj_create(content);
        lv_obj_remove_style_all(totals_row);
        lv_obj_set_width(totals_row, LV_PCT(100));
        lv_obj_set_height(totals_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(totals_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(totals_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(totals_row, 4, 0);

        lv_obj_set_style_pad_right(totals_row, 92, 0);

        lv_obj_t *lbl_total_hdr = lv_label_create(totals_row);
        lv_label_set_text(lbl_total_hdr, "TOTAL EXPOSURES");
        lv_obj_set_style_text_font(lbl_total_hdr, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_letter_space(lbl_total_hdr, 2, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_total_hdr,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

        lbl_totals = lv_label_create(totals_row);
        lv_label_set_text(lbl_totals, "-- / --");
        lv_obj_set_style_text_font(lbl_totals, &lv_font_montserrat_24, 0);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_totals,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* ── No-data message ── */
    lbl_no_data = lv_label_create(content);
    lv_label_set_text(lbl_no_data, "No sequence data");
    lv_obj_set_style_text_font(lbl_no_data, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(lbl_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_align(lbl_no_data, LV_ALIGN_CENTER);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_no_data,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    lv_obj_add_flag(lbl_no_data, LV_OBJ_FLAG_HIDDEN);
}

/* ── Populate ──────────────────────────────────────────────────────── */

void populate_sequence_data(const sequence_detail_data_t *data) {
    if (!content_root) return;

    char buf[64];

    if (!data || !data->has_data) {
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

    /* Target name */
    lv_label_set_text(lbl_target_name,
        data->target_name[0] ? data->target_name : "--");
    if (current_theme)
        lv_obj_set_style_text_color(lbl_target_name,
            lv_color_hex(app_config_apply_brightness(current_theme->target_name_color, gb)), 0);

    /* Time remaining */
    lv_label_set_text(lbl_time_remaining,
        data->time_remaining[0] ? data->time_remaining : "--:--:--");

    /* Current step */
    lv_label_set_text(lbl_container_val,
        data->container_name[0] ? data->container_name : "--");
    lv_label_set_text(lbl_step_val,
        data->step_name[0] ? data->step_name : "--");

    lv_label_set_text(lbl_filter_val,
        data->current_filter[0] ? data->current_filter : "--");
    if (current_theme && data->current_filter[0]) {
        uint32_t fc = app_config_get_filter_color(data->current_filter, 0);
        if (fc != 0)
            lv_obj_set_style_text_color(lbl_filter_val,
                lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
        else
            lv_obj_set_style_text_color(lbl_filter_val,
                lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);
    }

    if (data->current_exposure_time > 0)
        snprintf(buf, sizeof(buf), "%.1fs", data->current_exposure_time);
    else
        snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(lbl_exp_time_val, buf);

    /* Current step progress */
    if (data->current_total > 0) {
        int pct = (data->current_completed * 100) / data->current_total;
        snprintf(buf, sizeof(buf), "%d / %d (%d%%)",
                 data->current_completed, data->current_total, pct);
        lv_label_set_text(lbl_current_progress, buf);
        lv_bar_set_value(bar_current, pct, LV_ANIM_ON);
    } else {
        lv_label_set_text(lbl_current_progress, "-- / --");
        lv_bar_set_value(bar_current, 0, LV_ANIM_OFF);
    }

    /* Filter breakdown */
    for (int i = 0; i < MAX_SEQ_FILTERS; i++) {
        if (i < data->filter_count) {
            lv_obj_clear_flag(filter_rows[i], LV_OBJ_FLAG_HIDDEN);

            lv_label_set_text(lbl_filter_names[i], data->filters[i].name);

            /* Apply per-filter color */
            uint32_t fc = app_config_get_filter_color(data->filters[i].name, 0);
            if (fc != 0 && current_theme) {
                lv_obj_set_style_text_color(lbl_filter_names[i],
                    lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
                lv_obj_set_style_bg_color(bar_filters[i],
                    lv_color_hex(app_config_apply_brightness(fc, gb)), LV_PART_INDICATOR);
            } else if (current_theme) {
                lv_obj_set_style_text_color(lbl_filter_names[i],
                    lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);
                lv_obj_set_style_bg_color(bar_filters[i],
                    lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)),
                    LV_PART_INDICATOR);
            }

            int pct = 0;
            if (data->filters[i].total > 0)
                pct = (data->filters[i].completed * 100) / data->filters[i].total;
            lv_bar_set_value(bar_filters[i], pct, LV_ANIM_ON);

            snprintf(buf, sizeof(buf), "%d/%d",
                     data->filters[i].completed, data->filters[i].total);
            lv_label_set_text(lbl_filter_counts[i], buf);
        } else {
            lv_obj_add_flag(filter_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Totals */
    snprintf(buf, sizeof(buf), "%d / %d", data->total_completed, data->total_total);
    lv_label_set_text(lbl_totals, buf);
}
