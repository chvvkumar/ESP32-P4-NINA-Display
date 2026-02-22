/**
 * @file nina_info_autofocus.c
 * @brief Autofocus V-Curve info overlay content — chart, best focus, status.
 *
 * Layout mirrors nina_graph_overlay.c: full-width chart with floating
 * Y-axis labels inside, X-axis position labels below, status row at bottom.
 */

#include "nina_info_internal.h"
#include "ui_helpers.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ── Layout constants ──────────────────────────────────────────────── */
#define AF_CHART_PAD   10

/* Red Night: muted red for the series line; normal: orange (matches graph overlay) */
#define AF_COLOR_NORMAL   0xFFA726
#define AF_COLOR_RED      0xCC0000
#define AF_Y_LABEL_W   60

/* ── Static widget references ──────────────────────────────────────── */

/* Chart */
static lv_obj_t *af_chart       = NULL;
static lv_chart_series_t *ser_af = NULL;

/* Y-axis labels (floating inside chart) */
static lv_obj_t *y_label_col    = NULL;
static lv_obj_t *lbl_y_top      = NULL;
static lv_obj_t *lbl_y_mid      = NULL;
static lv_obj_t *lbl_y_bot      = NULL;

/* X-axis labels (row below chart) */
static lv_obj_t *x_label_row    = NULL;
static lv_obj_t *lbl_x_min      = NULL;
static lv_obj_t *lbl_x_mid      = NULL;
static lv_obj_t *lbl_x_max      = NULL;

/* Floating labels on chart */
static lv_obj_t *lbl_chart_title = NULL;
static lv_obj_t *lbl_summary     = NULL;
static lv_obj_t *lbl_loading     = NULL;

/* X-axis title */
static lv_obj_t *lbl_x_title    = NULL;

/* Status */
static lv_obj_t *lbl_af_status  = NULL;

/* Content container */
static lv_obj_t *content_root   = NULL;

/* ── Build ─────────────────────────────────────────────────────────── */

void build_autofocus_content(lv_obj_t *content) {
    content_root = content;

    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    int gb = current_theme ? app_config_get()->color_brightness : 100;

    /* ── Chart area (simple container, chart fills it) ── */
    lv_obj_t *chart_area = lv_obj_create(content);
    lv_obj_remove_style_all(chart_area);
    lv_obj_set_width(chart_area, LV_PCT(100));
    lv_obj_set_flex_grow(chart_area, 1);
    lv_obj_clear_flag(chart_area, LV_OBJ_FLAG_SCROLLABLE);

    /* Chart — full width/height like graph overlay */
    af_chart = lv_chart_create(chart_area);
    lv_obj_set_size(af_chart, LV_PCT(100), LV_PCT(100));
    lv_chart_set_type(af_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(af_chart, MAX_AF_POINTS);
    lv_chart_set_div_line_count(af_chart, 4, 4);
    lv_obj_set_style_pad_all(af_chart, AF_CHART_PAD, 0);
    lv_obj_set_style_radius(af_chart, 16, 0);
    lv_obj_set_style_border_width(af_chart, 1, 0);
    lv_obj_clear_flag(af_chart, LV_OBJ_FLAG_SCROLLABLE);

    if (current_theme) {
        lv_obj_set_style_bg_color(af_chart, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(af_chart, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(af_chart, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_line_color(af_chart, lv_color_hex(current_theme->bento_border), LV_PART_MAIN);
        lv_obj_set_style_line_width(af_chart, 1, LV_PART_MAIN);
    }

    /* AF data series — Red Night gets muted red, normal gets orange */
    uint32_t series_color = info_is_red_night()
        ? app_config_apply_brightness(AF_COLOR_RED, gb)
        : app_config_apply_brightness(AF_COLOR_NORMAL, gb);
    ser_af = lv_chart_add_series(af_chart, lv_color_hex(series_color), LV_CHART_AXIS_PRIMARY_Y);

    /* Series line and point styling */
    lv_obj_set_style_line_width(af_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(af_chart, 6, 6, LV_PART_INDICATOR);

    /* Initialize all points to LV_CHART_POINT_NONE */
    for (int i = 0; i < MAX_AF_POINTS; i++) {
        lv_chart_set_next_value(af_chart, ser_af, LV_CHART_POINT_NONE);
    }

    /* Y-axis label column (floating inside chart, left edge) — matches graph overlay */
    y_label_col = lv_obj_create(af_chart);
    lv_obj_remove_style_all(y_label_col);
    lv_obj_set_width(y_label_col, AF_Y_LABEL_W);
    lv_obj_set_height(y_label_col, LV_PCT(100));
    lv_obj_set_flex_flow(y_label_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(y_label_col, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(y_label_col, AF_CHART_PAD + 1, 0);
    lv_obj_set_style_pad_bottom(y_label_col, AF_CHART_PAD + 1, 0);
    lv_obj_set_style_pad_left(y_label_col, 4, 0);
    lv_obj_add_flag(y_label_col, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(y_label_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(y_label_col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(y_label_col, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_y_top = lv_label_create(y_label_col);
    lv_label_set_text(lbl_y_top, "");
    lv_obj_set_style_text_font(lbl_y_top, &lv_font_montserrat_14, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_y_top,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    lbl_y_mid = lv_label_create(y_label_col);
    lv_label_set_text(lbl_y_mid, "");
    lv_obj_set_style_text_font(lbl_y_mid, &lv_font_montserrat_14, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_y_mid,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    lbl_y_bot = lv_label_create(y_label_col);
    lv_label_set_text(lbl_y_bot, "");
    lv_obj_set_style_text_font(lbl_y_bot, &lv_font_montserrat_14, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_y_bot,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    /* Floating title on chart (top center) */
    lbl_chart_title = lv_label_create(af_chart);
    lv_label_set_text(lbl_chart_title, "Autofocus Curve");
    lv_obj_set_style_text_font(lbl_chart_title, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(lbl_chart_title, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(lbl_chart_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lbl_chart_title, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_radius(lbl_chart_title, 8, 0);
    lv_obj_set_style_pad_hor(lbl_chart_title, 10, 0);
    lv_obj_set_style_pad_ver(lbl_chart_title, 4, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_chart_title,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_bg_color(lbl_chart_title, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(lbl_chart_title, LV_OPA_50, 0);
    }

    /* Floating summary on chart (bottom center) */
    lbl_summary = lv_label_create(af_chart);
    lv_label_set_text(lbl_summary, "");
    lv_obj_set_style_text_font(lbl_summary, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(lbl_summary, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(lbl_summary, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lbl_summary, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(lbl_summary, 8, 0);
    lv_obj_set_style_pad_hor(lbl_summary, 10, 0);
    lv_obj_set_style_pad_ver(lbl_summary, 4, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_summary,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_bg_color(lbl_summary, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(lbl_summary, LV_OPA_50, 0);
    }
    lv_obj_add_flag(lbl_summary, LV_OBJ_FLAG_HIDDEN);

    /* Loading label on chart */
    lbl_loading = lv_label_create(af_chart);
    lv_label_set_text(lbl_loading, "Waiting for autofocus data...");
    lv_obj_set_style_text_font(lbl_loading, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(lbl_loading, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(lbl_loading, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lbl_loading, LV_ALIGN_CENTER, 0, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_loading,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    /* ── X-axis labels row (below chart) ── */
    x_label_row = lv_obj_create(content);
    lv_obj_remove_style_all(x_label_row);
    lv_obj_set_width(x_label_row, LV_PCT(100));
    lv_obj_set_height(x_label_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(x_label_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(x_label_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(x_label_row, AF_CHART_PAD, 0);
    lv_obj_set_style_pad_right(x_label_row, AF_CHART_PAD, 0);

    lbl_x_min = lv_label_create(x_label_row);
    lv_label_set_text(lbl_x_min, "");
    lv_obj_set_style_text_font(lbl_x_min, &lv_font_montserrat_14, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_x_min,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    lbl_x_mid = lv_label_create(x_label_row);
    lv_label_set_text(lbl_x_mid, "");
    lv_obj_set_style_text_font(lbl_x_mid, &lv_font_montserrat_14, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_x_mid,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    lbl_x_max = lv_label_create(x_label_row);
    lv_label_set_text(lbl_x_max, "");
    lv_obj_set_style_text_font(lbl_x_max, &lv_font_montserrat_14, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_x_max,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    /* ── X-axis title ── */
    lbl_x_title = lv_label_create(content);
    lv_label_set_text(lbl_x_title, "Focuser Position");
    lv_obj_set_style_text_font(lbl_x_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(lbl_x_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_align(lbl_x_title, LV_ALIGN_CENTER);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_x_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    /* ── Status row ── */
    lbl_af_status = lv_label_create(content);
    lv_label_set_text(lbl_af_status, "");
    lv_obj_set_style_text_font(lbl_af_status, &lv_font_montserrat_16, 0);
    if (current_theme)
        lv_obj_set_style_text_color(lbl_af_status,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
}

/* ── Populate ──────────────────────────────────────────────────────── */

void populate_autofocus_data(const autofocus_data_t *data) {
    if (!content_root || !af_chart) return;

    char buf[64];

    if (!data || !data->has_data) {
        /* Show loading/no-data state */
        lv_obj_clear_flag(lbl_loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_summary, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_af_status, "");

        /* Clear chart */
        lv_chart_set_point_count(af_chart, MAX_AF_POINTS);
        for (int i = 0; i < MAX_AF_POINTS; i++) {
            lv_chart_set_next_value(af_chart, ser_af, LV_CHART_POINT_NONE);
        }
        lv_chart_refresh(af_chart);

        /* Clear axis labels */
        lv_label_set_text(lbl_y_top, "");
        lv_label_set_text(lbl_y_mid, "");
        lv_label_set_text(lbl_y_bot, "");
        lv_label_set_text(lbl_x_min, "");
        lv_label_set_text(lbl_x_mid, "");
        lv_label_set_text(lbl_x_max, "");
        return;
    }

    int gb = current_theme ? app_config_get()->color_brightness : 100;

    /* Hide loading, show data */
    lv_obj_add_flag(lbl_loading, LV_OBJ_FLAG_HIDDEN);

    /* Make a sorted copy of the points (ascending by focuser position)
     * so chart data aligns with X-axis labels (min left, max right).
     * NINA sends points in measurement order which may be high→low. */
    typedef struct { int position; float hfr; } af_pt_t;
    af_pt_t sorted[MAX_AF_POINTS];
    int n = data->count > MAX_AF_POINTS ? MAX_AF_POINTS : data->count;
    for (int i = 0; i < n; i++) {
        sorted[i].position = data->points[i].position;
        sorted[i].hfr      = data->points[i].hfr;
    }
    /* Simple insertion sort — count is small (typically <20) */
    for (int i = 1; i < n; i++) {
        af_pt_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j].position > key.position) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    /* Compute Y range (HFR values scaled x100 for integer chart) */
    float min_hfr = 1e6f, max_hfr = 0.0f;
    int min_pos = 0x7FFFFFFF, max_pos = 0;

    for (int i = 0; i < n; i++) {
        if (sorted[i].hfr < min_hfr) min_hfr = sorted[i].hfr;
        if (sorted[i].hfr > max_hfr) max_hfr = sorted[i].hfr;
        if (sorted[i].position < min_pos) min_pos = sorted[i].position;
        if (sorted[i].position > max_pos) max_pos = sorted[i].position;
    }

    /* Add headroom */
    int y_min = 0;
    int y_max = (int)(max_hfr * 120.0f);  /* 20% headroom */
    if (y_max < 100) y_max = 100;

    /* Set chart range and point count */
    lv_chart_set_point_count(af_chart, n > 0 ? n : 1);
    lv_chart_set_range(af_chart, LV_CHART_AXIS_PRIMARY_Y, y_min, y_max);

    /* Populate points (HFR scaled x100) — from sorted array */
    for (int i = 0; i < n; i++) {
        int val = (int)(sorted[i].hfr * 100.0f);
        lv_chart_set_value_by_id(af_chart, ser_af, i, val);
    }
    lv_chart_refresh(af_chart);

    /* Update series color — Red Night aware */
    {
        uint32_t sc = info_is_red_night()
            ? app_config_apply_brightness(AF_COLOR_RED, gb)
            : app_config_apply_brightness(AF_COLOR_NORMAL, gb);
        lv_chart_set_series_color(af_chart, ser_af, lv_color_hex(sc));
    }

    /* Y-axis labels */
    snprintf(buf, sizeof(buf), "%.1f", (float)y_max / 100.0f);
    lv_label_set_text(lbl_y_top, buf);

    snprintf(buf, sizeof(buf), "%.1f", (float)(y_max / 2) / 100.0f);
    lv_label_set_text(lbl_y_mid, buf);

    lv_label_set_text(lbl_y_bot, "0");

    /* X-axis labels (focuser positions) */
    if (n > 0) {
        snprintf(buf, sizeof(buf), "%d", min_pos);
        lv_label_set_text(lbl_x_min, buf);

        snprintf(buf, sizeof(buf), "%d", (min_pos + max_pos) / 2);
        lv_label_set_text(lbl_x_mid, buf);

        snprintf(buf, sizeof(buf), "%d", max_pos);
        lv_label_set_text(lbl_x_max, buf);
    }

    /* Summary label */
    if (data->best_position > 0) {
        snprintf(buf, sizeof(buf), "Best: %d  HFR: %.2f", data->best_position, data->best_hfr);
        lv_label_set_text(lbl_summary, buf);
        lv_obj_clear_flag(lbl_summary, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_summary, LV_OBJ_FLAG_HIDDEN);
    }

    /* Status */
    if (data->af_running) {
        snprintf(buf, sizeof(buf), "Autofocus Running... (%d points)", data->count);
        lv_label_set_text(lbl_af_status, buf);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_af_status,
                lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)), 0);
    } else {
        snprintf(buf, sizeof(buf), "Last AF: %d points", data->count);
        lv_label_set_text(lbl_af_status, buf);
        if (current_theme)
            lv_obj_set_style_text_color(lbl_af_status,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
}

/* ── Theme ─────────────────────────────────────────────────────────── */

void theme_autofocus_content(void) {
    if (!current_theme || !af_chart) return;

    int gb = app_config_get()->color_brightness;

    /* Chart background and grid */
    lv_obj_set_style_bg_color(af_chart, lv_color_hex(current_theme->bento_bg), 0);
    lv_obj_set_style_border_color(af_chart, lv_color_hex(current_theme->bento_border), 0);
    lv_obj_set_style_line_color(af_chart, lv_color_hex(current_theme->bento_border), LV_PART_MAIN);

    /* Series color — Red Night aware */
    uint32_t sc = info_is_red_night()
        ? app_config_apply_brightness(AF_COLOR_RED, gb)
        : app_config_apply_brightness(AF_COLOR_NORMAL, gb);
    lv_chart_set_series_color(af_chart, ser_af, lv_color_hex(sc));

    /* Y-axis labels */
    uint32_t y_color = app_config_apply_brightness(current_theme->text_color, gb);
    if (lbl_y_top) lv_obj_set_style_text_color(lbl_y_top, lv_color_hex(y_color), 0);
    if (lbl_y_mid) lv_obj_set_style_text_color(lbl_y_mid, lv_color_hex(y_color), 0);
    if (lbl_y_bot) lv_obj_set_style_text_color(lbl_y_bot, lv_color_hex(y_color), 0);

    /* X-axis labels */
    uint32_t x_color = app_config_apply_brightness(current_theme->label_color, gb);
    if (lbl_x_min) lv_obj_set_style_text_color(lbl_x_min, lv_color_hex(x_color), 0);
    if (lbl_x_mid) lv_obj_set_style_text_color(lbl_x_mid, lv_color_hex(x_color), 0);
    if (lbl_x_max) lv_obj_set_style_text_color(lbl_x_max, lv_color_hex(x_color), 0);

    /* X-axis title */
    if (lbl_x_title)
        lv_obj_set_style_text_color(lbl_x_title, lv_color_hex(x_color), 0);

    /* Chart title */
    if (lbl_chart_title) {
        lv_obj_set_style_text_color(lbl_chart_title,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_bg_color(lbl_chart_title, lv_color_hex(current_theme->bento_bg), 0);
    }

    /* Summary label */
    if (lbl_summary) {
        uint32_t sum_color = info_is_red_night()
            ? app_config_apply_brightness(current_theme->text_color, gb)
            : app_config_apply_brightness(0xaaaaaa, gb);
        lv_obj_set_style_text_color(lbl_summary, lv_color_hex(sum_color), 0);
        lv_obj_set_style_bg_color(lbl_summary, lv_color_hex(current_theme->bento_bg), 0);
    }

    /* Loading label */
    if (lbl_loading)
        lv_obj_set_style_text_color(lbl_loading,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    /* Status label */
    if (lbl_af_status)
        lv_obj_set_style_text_color(lbl_af_status,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
}
