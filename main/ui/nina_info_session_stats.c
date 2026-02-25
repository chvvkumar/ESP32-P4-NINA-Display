/**
 * @file nina_info_session_stats.c
 * @brief Session Statistics info overlay — exposures, integration time,
 *        efficiency, RMS/HFR min/max/avg.
 *
 * All data comes from the on-device session_stats collector (no API fetch).
 */

#include "nina_info_internal.h"
#include "nina_info_overlay.h"
#include "info_overlay_types.h"
#include "nina_session_stats.h"
#include "themes.h"
#include "app_config.h"
#include "ui_helpers.h"
#include "ui_styles.h"
#include "lvgl.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <float.h>

/* current_theme is defined in nina_dashboard.c */
extern const theme_t *current_theme;

/* ── Static widget references ──────────────────────────────────────── */

/* Hero card */
static lv_obj_t *lbl_exposures_val   = NULL;
static lv_obj_t *lbl_integration_val = NULL;
static lv_obj_t *lbl_efficiency_val  = NULL;

/* RMS card */
static lv_obj_t *lbl_rms_avg_val = NULL;
static lv_obj_t *lbl_rms_min_val = NULL;
static lv_obj_t *lbl_rms_max_val = NULL;

/* HFR card */
static lv_obj_t *lbl_hfr_avg_val = NULL;
static lv_obj_t *lbl_hfr_min_val = NULL;
static lv_obj_t *lbl_hfr_max_val = NULL;

/* Session card */
static lv_obj_t *lbl_duration_val   = NULL;
static lv_obj_t *lbl_datapoints_val = NULL;

/* No-data label */
static lv_obj_t *lbl_no_data = NULL;

/* Content root */
static lv_obj_t *content_root = NULL;

/* ── Local helpers ─────────────────────────────────────────────────── */

static lv_obj_t *make_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_bento_box, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void make_section(lv_obj_t *parent, const char *title) {
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

static lv_obj_t *make_kv(lv_obj_t *parent, const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_key = lv_label_create(row);
    lv_label_set_text(lbl_key, key);
    lv_obj_set_style_text_font(lbl_key, &lv_font_montserrat_20, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_key,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_22, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_val,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    return lbl_val;
}

static lv_obj_t *make_hero_block(lv_obj_t *parent, const char *title,
                                  const lv_font_t *val_font, uint32_t color) {
    int gb = current_theme ? app_config_get()->color_brightness : 100;

    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(block, 4, 0);

    lv_obj_t *lbl = lv_label_create(block);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    lv_obj_t *val = lv_label_create(block);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, val_font, 0);
    lv_obj_set_style_text_color(val,
        lv_color_hex(app_config_apply_brightness(color, gb)), 0);

    return val;
}

static lv_obj_t *make_divider(lv_obj_t *parent) {
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 1, 48);
    if (current_theme) {
        lv_obj_set_style_bg_color(div, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    }
    return div;
}

/* Format seconds as "Xh Ym" or "Ym Zs" */
static void fmt_duration(char *buf, size_t sz, float seconds) {
    int total = (int)seconds;
    if (total >= 3600) {
        snprintf(buf, sz, "%dh %dm", total / 3600, (total % 3600) / 60);
    } else if (total >= 60) {
        snprintf(buf, sz, "%dm %ds", total / 60, total % 60);
    } else {
        snprintf(buf, sz, "%ds", total);
    }
}

/* ── Build ─────────────────────────────────────────────────────────── */

void build_session_stats_content(lv_obj_t *content) {
    content_root = content;

    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    int gb = current_theme ? app_config_get()->color_brightness : 100;
    uint32_t text_color = current_theme ? current_theme->text_color : 0xFFFFFF;
    uint32_t progress_color = current_theme ? current_theme->progress_color : 0x4FC3F7;

    /* ── Hero: Exposures | Integration | Efficiency ── */
    {
        lv_obj_t *card = make_card(content);
        make_section(card, "SESSION TOTALS");

        lv_obj_t *hero_row = lv_obj_create(card);
        lv_obj_remove_style_all(hero_row);
        lv_obj_set_width(hero_row, LV_PCT(100));
        lv_obj_set_height(hero_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hero_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hero_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lbl_exposures_val = make_hero_block(hero_row, "EXPOSURES",
                                            &lv_font_montserrat_36, text_color);
        make_divider(hero_row);
        lbl_integration_val = make_hero_block(hero_row, "INTEGRATION",
                                              &lv_font_montserrat_28, text_color);
        make_divider(hero_row);
        lbl_efficiency_val = make_hero_block(hero_row, "EFFICIENCY",
                                             &lv_font_montserrat_36, progress_color);
    }

    /* ── Two-column: RMS + HFR ── */
    {
        lv_obj_t *cols = lv_obj_create(content);
        lv_obj_remove_style_all(cols);
        lv_obj_set_width(cols, LV_PCT(100));
        lv_obj_set_flex_grow(cols, 1);
        lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(cols, 10, 0);
        lv_obj_remove_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

        /* Left: RMS */
        lv_obj_t *col_left = lv_obj_create(cols);
        lv_obj_remove_style_all(col_left);
        lv_obj_set_flex_grow(col_left, 1);
        lv_obj_set_height(col_left, LV_PCT(100));
        lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col_left, 10, 0);
        lv_obj_remove_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);

        {
            lv_obj_t *card = make_card(col_left);
            lv_obj_set_flex_grow(card, 1);
            make_section(card, "GUIDING (RMS)");
            lbl_rms_avg_val = make_kv(card, "Average");
            lbl_rms_min_val = make_kv(card, "Best");
            lbl_rms_max_val = make_kv(card, "Worst");

            /* Color the average value with RMS color */
            if (current_theme)
                lv_obj_set_style_text_color(lbl_rms_avg_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->rms_color, gb)), 0);
        }

        /* Right: HFR */
        lv_obj_t *col_right = lv_obj_create(cols);
        lv_obj_remove_style_all(col_right);
        lv_obj_set_flex_grow(col_right, 1);
        lv_obj_set_height(col_right, LV_PCT(100));
        lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col_right, 10, 0);
        lv_obj_remove_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);

        {
            lv_obj_t *card = make_card(col_right);
            lv_obj_set_flex_grow(card, 1);
            make_section(card, "FOCUS (HFR)");
            lbl_hfr_avg_val = make_kv(card, "Average");
            lbl_hfr_min_val = make_kv(card, "Best");
            lbl_hfr_max_val = make_kv(card, "Worst");

            /* Color the average value with HFR color */
            if (current_theme)
                lv_obj_set_style_text_color(lbl_hfr_avg_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->hfr_color, gb)), 0);
        }
    }

    /* ── Session info row ── */
    {
        lv_obj_t *card = make_card(content);
        lv_obj_set_style_pad_right(card, INFO_BACK_BTN_ZONE, 0);
        make_section(card, "SESSION");
        lbl_duration_val   = make_kv(card, "Duration");
        lbl_datapoints_val = make_kv(card, "Data Points");
    }

    /* ── No-data label ── */
    lbl_no_data = lv_label_create(content);
    lv_label_set_text(lbl_no_data, "No session data yet");
    lv_obj_set_style_text_font(lbl_no_data, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(lbl_no_data, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_align(lbl_no_data, LV_ALIGN_CENTER);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_no_data,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    lv_obj_add_flag(lbl_no_data, LV_OBJ_FLAG_HIDDEN);
}

/* ── Populate ──────────────────────────────────────────────────────── */

void populate_session_stats_data(int instance) {
    if (!content_root) return;

    const session_stats_t *st = nina_session_stats_get(instance);

    if (!st || st->total_exposures == 0) {
        /* Show no-data, hide everything else */
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

    char buf[64];

    /* Hero: exposures */
    snprintf(buf, sizeof(buf), "%d", st->total_exposures);
    lv_label_set_text(lbl_exposures_val, buf);

    /* Hero: integration time */
    fmt_duration(buf, sizeof(buf), st->total_exposure_time_s);
    lv_label_set_text(lbl_integration_val, buf);

    /* Hero: efficiency = integration / session_elapsed */
    if (st->session_start_ms > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        float elapsed_s = (float)(now_ms - st->session_start_ms) / 1000.0f;
        if (elapsed_s > 0 && st->total_exposure_time_s > 0) {
            float eff = (st->total_exposure_time_s / elapsed_s) * 100.0f;
            if (eff > 100.0f) eff = 100.0f;
            snprintf(buf, sizeof(buf), "%.0f%%", eff);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(lbl_efficiency_val, buf);

    /* RMS stats */
    if (st->rms_count > 0) {
        float avg = st->rms_sum / (float)st->rms_count;
        snprintf(buf, sizeof(buf), "%.2f\"", avg);
        lv_label_set_text(lbl_rms_avg_val, buf);
        snprintf(buf, sizeof(buf), "%.2f\"", st->rms_min);
        lv_label_set_text(lbl_rms_min_val, buf);
        snprintf(buf, sizeof(buf), "%.2f\"", st->rms_max);
        lv_label_set_text(lbl_rms_max_val, buf);
    } else {
        lv_label_set_text(lbl_rms_avg_val, "--");
        lv_label_set_text(lbl_rms_min_val, "--");
        lv_label_set_text(lbl_rms_max_val, "--");
    }

    /* HFR stats */
    if (st->hfr_count > 0) {
        float avg = st->hfr_sum / (float)st->hfr_count;
        snprintf(buf, sizeof(buf), "%.2f", avg);
        lv_label_set_text(lbl_hfr_avg_val, buf);
        snprintf(buf, sizeof(buf), "%.2f", st->hfr_min);
        lv_label_set_text(lbl_hfr_min_val, buf);
        snprintf(buf, sizeof(buf), "%.2f", st->hfr_max);
        lv_label_set_text(lbl_hfr_max_val, buf);
    } else {
        lv_label_set_text(lbl_hfr_avg_val, "--");
        lv_label_set_text(lbl_hfr_min_val, "--");
        lv_label_set_text(lbl_hfr_max_val, "--");
    }

    /* Session duration */
    if (st->session_start_ms > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        float elapsed_s = (float)(now_ms - st->session_start_ms) / 1000.0f;
        fmt_duration(buf, sizeof(buf), elapsed_s);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(lbl_duration_val, buf);

    /* Data points */
    snprintf(buf, sizeof(buf), "%d", st->count);
    lv_label_set_text(lbl_datapoints_val, buf);
}

/* ── Theme ─────────────────────────────────────────────────────────── */

void theme_session_stats_content(void) {
    /* Rebuild on next show to pick up new theme colors */
}
