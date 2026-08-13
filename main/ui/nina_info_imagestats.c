/**
 * @file nina_info_imagestats.c
 * @brief Image Statistics info overlay content — stars, HFR, pixel stats, capture settings.
 */

#include "nina_info_overlay.h"
#include "info_overlay_types.h"
#include "nina_dashboard_internal.h"
#include "themes.h"
#include "app_config.h"
#include "ui_helpers.h"
#include "ui_styles.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

/* ── Static widget references ──────────────────────────────────────── */

/* Hero card */
static lv_obj_t *lbl_stars_val    = NULL;
static lv_obj_t *lbl_hfr_val     = NULL;
static lv_obj_t *lbl_hfr_sd_val  = NULL;

/* Pixel statistics card */
static lv_obj_t *lbl_mean_val    = NULL;
static lv_obj_t *lbl_median_val  = NULL;
static lv_obj_t *lbl_stdev_val   = NULL;
static lv_obj_t *lbl_min_val     = NULL;
static lv_obj_t *lbl_max_val     = NULL;

/* Capture card */
static lv_obj_t *lbl_exposure_val = NULL;
static lv_obj_t *lbl_filter_val   = NULL;
static lv_obj_t *lbl_gain_val     = NULL;
static lv_obj_t *lbl_offset_val   = NULL;
static lv_obj_t *lbl_temp_val     = NULL;

/* Equipment card */
static lv_obj_t *lbl_camera_val     = NULL;
static lv_obj_t *lbl_telescope_val  = NULL;
static lv_obj_t *lbl_focal_len_val  = NULL;

/* No-data label */
static lv_obj_t *lbl_no_data = NULL;

/* Content container (to hide/show children) */
static lv_obj_t *content_root = NULL;

/* Card/section/kv factories are shared: ui_card/ui_section_label/ui_kv (ui_helpers.h).
 * This page uses card pad 14 / row gap 8, key font 20, value font 22. */

/* ── Build ─────────────────────────────────────────────────────────── */

void build_imagestats_content(lv_obj_t *content) {
    content_root = content;

    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Hero: Stars + HFR + HFR SD ── */
    {
        lv_obj_t *card = ui_card(content, 14, 8);
        lv_obj_set_width(card, LV_PCT(100));
        ui_section_label(card, "LAST IMAGE");

        lv_obj_t *hero_row = lv_obj_create(card);
        lv_obj_remove_style_all(hero_row);
        lv_obj_set_width(hero_row, LV_PCT(100));
        lv_obj_set_height(hero_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hero_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hero_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Stars block */
        {
            lv_obj_t *block = lv_obj_create(hero_row);
            lv_obj_remove_style_all(block);
            lv_obj_set_size(block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(block, 4, 0);

            lv_obj_t *lbl = lv_label_create(block);
            lv_label_set_text(lbl, "STARS");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_letter_space(lbl, 2, 0);
            ui_set_theme_text_color(lbl, UI_THEME_COLOR(label_color));

            lbl_stars_val = ui_label(block, "--", &lv_font_montserrat_36, UI_THEME_COLOR(text_color));
        }

        /* Divider 1 */
        {
            lv_obj_t *div = lv_obj_create(hero_row);
            lv_obj_remove_style_all(div);
            lv_obj_set_size(div, 1, 48);
            if (current_theme) {
                lv_obj_set_style_bg_color(div, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
            }
        }

        /* HFR block */
        {
            lv_obj_t *block = lv_obj_create(hero_row);
            lv_obj_remove_style_all(block);
            lv_obj_set_size(block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(block, 4, 0);

            lv_obj_t *lbl = lv_label_create(block);
            lv_label_set_text(lbl, "HFR");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_letter_space(lbl, 2, 0);
            ui_set_theme_text_color(lbl, UI_THEME_COLOR(label_color));

            lbl_hfr_val = ui_label(block, "--", &lv_font_montserrat_36, UI_THEME_COLOR(hfr_color));
        }

        /* Divider 2 */
        {
            lv_obj_t *div = lv_obj_create(hero_row);
            lv_obj_remove_style_all(div);
            lv_obj_set_size(div, 1, 48);
            if (current_theme) {
                lv_obj_set_style_bg_color(div, lv_color_hex(current_theme->bento_border), 0);
                lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
            }
        }

        /* HFR StdDev block */
        {
            lv_obj_t *block = lv_obj_create(hero_row);
            lv_obj_remove_style_all(block);
            lv_obj_set_size(block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(block, 4, 0);

            lv_obj_t *lbl = lv_label_create(block);
            lv_label_set_text(lbl, "HFR SD");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_letter_space(lbl, 2, 0);
            ui_set_theme_text_color(lbl, UI_THEME_COLOR(label_color));

            lbl_hfr_sd_val = ui_label(block, "--", &lv_font_montserrat_28, UI_THEME_COLOR(text_color));
        }
    }

    /* ── Two-column cards ── */
    {
        lv_obj_t *cols = lv_obj_create(content);
        lv_obj_remove_style_all(cols);
        lv_obj_set_width(cols, LV_PCT(100));
        lv_obj_set_flex_grow(cols, 1);
        lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(cols, 10, 0);
        lv_obj_remove_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

        /* Left column: Pixel Statistics */
        lv_obj_t *col_left = lv_obj_create(cols);
        lv_obj_remove_style_all(col_left);
        lv_obj_set_flex_grow(col_left, 1);
        lv_obj_set_height(col_left, LV_PCT(100));
        lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col_left, 10, 0);
        lv_obj_remove_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);

        {
            lv_obj_t *card = ui_card(col_left, 14, 8);
            lv_obj_set_flex_grow(card, 1);
            ui_section_label(card, "PIXEL STATISTICS");
            lbl_mean_val   = ui_kv(card, "Mean", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_median_val = ui_kv(card, "Median", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_stdev_val  = ui_kv(card, "StdDev", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_min_val    = ui_kv(card, "Min", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_max_val    = ui_kv(card, "Max", &lv_font_montserrat_20, &lv_font_montserrat_22);
        }

        /* Right column: Capture Settings */
        lv_obj_t *col_right = lv_obj_create(cols);
        lv_obj_remove_style_all(col_right);
        lv_obj_set_flex_grow(col_right, 1);
        lv_obj_set_height(col_right, LV_PCT(100));
        lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col_right, 10, 0);
        lv_obj_remove_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);

        {
            lv_obj_t *card = ui_card(col_right, 14, 8);
            lv_obj_set_flex_grow(card, 1);
            ui_section_label(card, "CAPTURE");
            lbl_exposure_val = ui_kv(card, "Exposure", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_filter_val   = ui_kv(card, "Filter", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_gain_val     = ui_kv(card, "Gain", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_offset_val   = ui_kv(card, "Offset", &lv_font_montserrat_20, &lv_font_montserrat_22);
            lbl_temp_val     = ui_kv(card, "Temp", &lv_font_montserrat_20, &lv_font_montserrat_22);
        }
    }

    /* ── Equipment row (full width) ── */
    {
        lv_obj_t *card = ui_card(content, 14, 8);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_style_pad_right(card, 92, 0);
        ui_section_label(card, "EQUIPMENT");
        lbl_camera_val    = ui_kv(card, "Camera", &lv_font_montserrat_20, &lv_font_montserrat_22);
        lv_label_set_long_mode(lbl_camera_val, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_camera_val, LV_PCT(55));
        lv_obj_set_style_text_align(lbl_camera_val, LV_TEXT_ALIGN_RIGHT, 0);

        lbl_telescope_val = ui_kv(card, "Telescope", &lv_font_montserrat_20, &lv_font_montserrat_22);
        lv_label_set_long_mode(lbl_telescope_val, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_telescope_val, LV_PCT(55));
        lv_obj_set_style_text_align(lbl_telescope_val, LV_TEXT_ALIGN_RIGHT, 0);

        lbl_focal_len_val = ui_kv(card, "Focal Len", &lv_font_montserrat_20, &lv_font_montserrat_22);
    }

    /* ── No-data message (hidden by default) ── */
    lbl_no_data = lv_label_create(content);
    lv_label_set_text(lbl_no_data, "No image data");
    lv_obj_set_style_text_font(lbl_no_data, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(lbl_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_align(lbl_no_data, LV_ALIGN_CENTER);
    ui_set_theme_text_color(lbl_no_data, UI_THEME_COLOR(label_color));
    lv_obj_add_flag(lbl_no_data, LV_OBJ_FLAG_HIDDEN);
}

/* ── Populate ──────────────────────────────────────────────────────── */

void populate_imagestats_data(const imagestats_detail_data_t *data) {
    if (!content_root) return;

    char buf[64];

    if (!data || !data->has_data) {
        /* Show no-data message, hide children except the no-data label */
        uint32_t cnt = lv_obj_get_child_count(content_root);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(content_root, i);
            if (child == lbl_no_data) {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    /* Show all children, hide no-data */
    {
        uint32_t cnt = lv_obj_get_child_count(content_root);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(content_root, i);
            if (child == lbl_no_data) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    int gb = current_theme ? app_config_get()->color_brightness : 100;

    /* Hero values */
    snprintf(buf, sizeof(buf), "%d", data->stars);
    lv_label_set_text(lbl_stars_val, buf);

    snprintf(buf, sizeof(buf), "%.2f", data->hfr);
    lv_label_set_text(lbl_hfr_val, buf);
    ui_set_theme_text_color(lbl_hfr_val, UI_THEME_COLOR(hfr_color));

    snprintf(buf, sizeof(buf), "%.2f", data->hfr_stdev);
    lv_label_set_text(lbl_hfr_sd_val, buf);

    /* Pixel statistics */
    snprintf(buf, sizeof(buf), "%.0f", data->mean);
    lv_label_set_text(lbl_mean_val, buf);

    snprintf(buf, sizeof(buf), "%.0f", data->median);
    lv_label_set_text(lbl_median_val, buf);

    snprintf(buf, sizeof(buf), "%.0f", data->stdev);
    lv_label_set_text(lbl_stdev_val, buf);

    snprintf(buf, sizeof(buf), "%d", data->min_val);
    lv_label_set_text(lbl_min_val, buf);

    snprintf(buf, sizeof(buf), "%d", data->max_val);
    lv_label_set_text(lbl_max_val, buf);

    /* Capture settings */
    snprintf(buf, sizeof(buf), "%.1fs", data->exposure_time);
    lv_label_set_text(lbl_exposure_val, buf);

    lv_label_set_text(lbl_filter_val, data->filter[0] ? data->filter : "--");
    if (current_theme && data->filter[0]) {
        uint32_t fc = app_config_get_filter_color(data->filter, 0);
        if (fc != 0)
            lv_obj_set_style_text_color(lbl_filter_val,
                lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
        else
            lv_obj_set_style_text_color(lbl_filter_val,
                lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);
    }

    snprintf(buf, sizeof(buf), "%d", data->gain);
    lv_label_set_text(lbl_gain_val, buf);

    snprintf(buf, sizeof(buf), "%d", data->offset);
    lv_label_set_text(lbl_offset_val, buf);

    snprintf(buf, sizeof(buf), "%.1f C", data->temperature);
    lv_label_set_text(lbl_temp_val, buf);

    /* Equipment */
    lv_label_set_text(lbl_camera_val, data->camera_name[0] ? data->camera_name : "--");
    lv_label_set_text(lbl_telescope_val, data->telescope_name[0] ? data->telescope_name : "--");

    if (data->focal_length > 0)
        snprintf(buf, sizeof(buf), "%dmm", data->focal_length);
    else
        snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(lbl_focal_len_val, buf);
}
