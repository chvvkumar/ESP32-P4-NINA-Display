/**
 * @file nina_info_imagestats.c
 * @brief Image Statistics info overlay content — stars, HFR, pixel stats, capture settings.
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

/* ── Local helpers ─────────────────────────────────────────────────── */

static lv_obj_t *make_info_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_bento_box, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void make_info_section(lv_obj_t *parent, const char *title) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
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
    lv_obj_set_style_text_font(lbl_key, &lv_font_montserrat_16, 0);
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

void build_imagestats_content(lv_obj_t *content) {
    content_root = content;

    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    int gb = current_theme ? app_config_get()->color_brightness : 100;

    /* ── Hero: Stars + HFR + HFR SD ── */
    {
        lv_obj_t *card = make_info_card(content);
        lv_obj_set_width(card, LV_PCT(100));
        make_info_section(card, "LAST IMAGE");

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
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_letter_space(lbl, 2, 0);
            if (current_theme)
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

            lbl_stars_val = lv_label_create(block);
            lv_label_set_text(lbl_stars_val, "--");
            lv_obj_set_style_text_font(lbl_stars_val, &lv_font_montserrat_28, 0);
            if (current_theme)
                lv_obj_set_style_text_color(lbl_stars_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        /* Divider 1 */
        {
            lv_obj_t *div = lv_obj_create(hero_row);
            lv_obj_remove_style_all(div);
            lv_obj_set_size(div, 1, 40);
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
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_letter_space(lbl, 2, 0);
            if (current_theme)
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

            lbl_hfr_val = lv_label_create(block);
            lv_label_set_text(lbl_hfr_val, "--");
            lv_obj_set_style_text_font(lbl_hfr_val, &lv_font_montserrat_28, 0);
            if (current_theme)
                lv_obj_set_style_text_color(lbl_hfr_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->hfr_color, gb)), 0);
        }

        /* Divider 2 */
        {
            lv_obj_t *div = lv_obj_create(hero_row);
            lv_obj_remove_style_all(div);
            lv_obj_set_size(div, 1, 40);
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
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_letter_space(lbl, 2, 0);
            if (current_theme)
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

            lbl_hfr_sd_val = lv_label_create(block);
            lv_label_set_text(lbl_hfr_sd_val, "--");
            lv_obj_set_style_text_font(lbl_hfr_sd_val, &lv_font_montserrat_24, 0);
            if (current_theme)
                lv_obj_set_style_text_color(lbl_hfr_sd_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
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
            lv_obj_t *card = make_info_card(col_left);
            lv_obj_set_flex_grow(card, 1);
            make_info_section(card, "PIXEL STATISTICS");
            lbl_mean_val   = make_info_kv(card, "Mean");
            lbl_median_val = make_info_kv(card, "Median");
            lbl_stdev_val  = make_info_kv(card, "StdDev");
            lbl_min_val    = make_info_kv(card, "Min");
            lbl_max_val    = make_info_kv(card, "Max");
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
            lv_obj_t *card = make_info_card(col_right);
            lv_obj_set_flex_grow(card, 1);
            make_info_section(card, "CAPTURE");
            lbl_exposure_val = make_info_kv(card, "Exposure");
            lbl_filter_val   = make_info_kv(card, "Filter");
            lbl_gain_val     = make_info_kv(card, "Gain");
            lbl_offset_val   = make_info_kv(card, "Offset");
            lbl_temp_val     = make_info_kv(card, "Temp");
        }
    }

    /* ── Equipment row (full width) ── */
    {
        lv_obj_t *card = make_info_card(content);
        lv_obj_set_width(card, LV_PCT(100));
        make_info_section(card, "EQUIPMENT");
        lbl_camera_val    = make_info_kv(card, "Camera");
        lv_label_set_long_mode(lbl_camera_val, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_camera_val, LV_PCT(55));
        lv_obj_set_style_text_align(lbl_camera_val, LV_TEXT_ALIGN_RIGHT, 0);

        lbl_telescope_val = make_info_kv(card, "Telescope");
        lv_label_set_long_mode(lbl_telescope_val, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_telescope_val, LV_PCT(55));
        lv_obj_set_style_text_align(lbl_telescope_val, LV_TEXT_ALIGN_RIGHT, 0);

        lbl_focal_len_val = make_info_kv(card, "Focal Len");
    }

    /* ── No-data message (hidden by default) ── */
    lbl_no_data = lv_label_create(content);
    lv_label_set_text(lbl_no_data, "No image data");
    lv_obj_set_style_text_font(lbl_no_data, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_align(lbl_no_data, LV_ALIGN_CENTER);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_no_data,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
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
    if (current_theme)
        lv_obj_set_style_text_color(lbl_hfr_val,
            lv_color_hex(app_config_apply_brightness(current_theme->hfr_color, gb)), 0);

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

    snprintf(buf, sizeof(buf), "%.1fC", data->temperature);
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
