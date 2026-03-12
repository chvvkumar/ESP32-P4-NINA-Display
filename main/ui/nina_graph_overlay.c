/**
 * @file nina_graph_overlay.c
 * @brief Fullscreen RMS/HFR history graph overlays.
 *
 * Shows an LVGL line chart with historical guider RMS or image HFR data.
 * Includes controls for history point count and Y-axis scaling.
 * Y-axis labels and X-axis title provide clear axis references.
 * Red Night theme forces all series to shades of red.
 */

#include "nina_graph_internal.h"
#include "nina_dashboard.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* -- Default series colors ----------------------------------------------- */
#define COLOR_RA    0x42A5F5   /* blue */
#define COLOR_DEC   0xEF5350   /* red */
#define COLOR_TOTAL 0x66BB6A   /* green */
#define COLOR_HFR   0xFFA726   /* orange */

/* -- Red Night series colors (different shades of red) ------------------- */
#define COLOR_RA_RED    0xE04040   /* bright red for RA */
#define COLOR_DEC_RED   0x8B1A1A   /* dark crimson for DEC */
#define COLOR_TOTAL_RED 0xAA2222   /* muted red for total */
#define COLOR_HFR_RED   0xCC0000   /* standard red for HFR */

/* -- History point options ----------------------------------------------- */
const int point_options[] = {25, 50, 100, 200, 400};

/* -- Y-scale options for RMS (arcseconds x 100) ------------------------- */
const int rms_scale_values[] = {0, 100, 200, 400, 800, 1600};  /* 0 = auto */
const char *rms_scale_labels[] = {"Auto", "1\"", "2\"", "4\"", "8\"", "16\""};

/* -- Y-scale options for HFR (value x 100) ------------------------------ */
const int hfr_scale_values[] = {0, 200, 400, 800, 1600};  /* 0 = auto */
const char *hfr_scale_labels[] = {"Auto", "2", "4", "8", "16"};

/* -- State --------------------------------------------------------------- */
lv_obj_t *overlay = NULL;
lv_obj_t *loading_lbl = NULL;
lv_obj_t *chart = NULL;
lv_obj_t *lbl_title = NULL;
lv_obj_t *btn_back = NULL;
lv_obj_t *btn_back_lbl = NULL;  /* Back button arrow label */

/* Point selector buttons */
lv_obj_t *btn_points[POINT_OPT_COUNT];
int selected_points_idx = 1;  /* default: 50 */

/* Scale selector buttons (sized to largest count) */
lv_obj_t *btn_scale[RMS_SCALE_COUNT];
int selected_scale_idx = 0;  /* default: auto */
int scale_btn_count = 0;     /* current number of scale buttons */

/* Legend labels */
lv_obj_t *legend_cont = NULL;

/* Chart series */
lv_chart_series_t *ser_ra = NULL;
lv_chart_series_t *ser_dec = NULL;
lv_chart_series_t *ser_total = NULL;
lv_chart_series_t *ser_hfr = NULL;

/* Summary labels (shown in header area) */
lv_obj_t *lbl_summary = NULL;

/* Chart area wrapper (row: y_label_col + chart) */
lv_obj_t *chart_area = NULL;

/* Y-axis labels (5 labels: top, +quarter, center, -quarter, bottom) */
lv_obj_t *y_label_col = NULL;
lv_obj_t *lbl_y_top = NULL;
lv_obj_t *lbl_y_q1 = NULL;   /* +Y/2 for RMS, 3Y/4 for HFR */
lv_obj_t *lbl_y_mid = NULL;  /* 0 for RMS, Y/2 for HFR */
lv_obj_t *lbl_y_q3 = NULL;   /* -Y/2 for RMS, Y/4 for HFR */
lv_obj_t *lbl_y_bot = NULL;

/* X-axis title */
lv_obj_t *lbl_x_title = NULL;

/* Request state */
volatile bool graph_requested = false;
graph_type_t current_type = GRAPH_TYPE_RMS;
int return_page_index = 0;

/* Controls container */
lv_obj_t *controls_cont = NULL;

/* Track current Y range for scale button callback */
int current_y_min = 0;
int current_y_max = 0;

/* Legend series toggle state (true = series hidden by user) */
bool legend_ra_hidden = false;
bool legend_dec_hidden = false;
bool legend_total_hidden = true;   /* Total hidden by default */
bool legend_hfr_hidden = false;

/* Threshold dashed lines */
lv_obj_t *thresh_lines[MAX_THRESH_LINES];
lv_point_precise_t thresh_line_pts[MAX_THRESH_LINES][2];

/* -- Red Night theme detection ------------------------------------------- */
bool is_red_night_theme(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

/* -- Theme-aware series colors ------------------------------------------- */
uint32_t get_ra_color(void) {
    return is_red_night_theme() ? COLOR_RA_RED : COLOR_RA;
}

uint32_t get_dec_color(void) {
    return is_red_night_theme() ? COLOR_DEC_RED : COLOR_DEC;
}

uint32_t get_total_color(void) {
    return is_red_night_theme() ? COLOR_TOTAL_RED : COLOR_TOTAL;
}

uint32_t get_hfr_color(void) {
    return is_red_night_theme() ? COLOR_HFR_RED : COLOR_HFR;
}

/* -- Theme-aware text color for controls (white normally, red for Red Night) */
uint32_t get_control_text_color(int gb) {
    if (is_red_night_theme()) {
        return app_config_apply_brightness(current_theme->text_color, gb);
    }
    return app_config_apply_brightness(0xffffff, gb);
}

/* -- Update Y-axis labels ------------------------------------------------ */
void update_y_labels(int y_min_x100, int y_max_x100) {
    current_y_min = y_min_x100;
    current_y_max = y_max_x100;

    if (!lbl_y_top || !lbl_y_q1 || !lbl_y_mid || !lbl_y_q3 || !lbl_y_bot) return;

    int gb = app_config_get()->color_brightness;
    uint32_t label_color = current_theme
        ? app_config_apply_brightness(current_theme->text_color, gb)
        : 0xaaaaaa;

    char buf[16];

    if (current_type == GRAPH_TYPE_RMS) {
        /* RMS: symmetric range centered on 0 */
        float top_val = y_max_x100 / 100.0f;
        float bot_val = y_min_x100 / 100.0f;
        float q1_val = top_val / 2.0f;   /* +half */
        float q3_val = bot_val / 2.0f;   /* -half */

        snprintf(buf, sizeof(buf), "+%.1f\"", top_val);
        lv_label_set_text(lbl_y_top, buf);

        snprintf(buf, sizeof(buf), "+%.1f\"", q1_val);
        lv_label_set_text(lbl_y_q1, buf);

        lv_label_set_text(lbl_y_mid, "0\"");

        snprintf(buf, sizeof(buf), "%.1f\"", q3_val);
        lv_label_set_text(lbl_y_q3, buf);

        snprintf(buf, sizeof(buf), "%.1f\"", bot_val);
        lv_label_set_text(lbl_y_bot, buf);
    } else {
        /* HFR: range 0 to max */
        float top_val = y_max_x100 / 100.0f;
        float q1_val = top_val * 0.75f;
        float mid_val = top_val * 0.50f;
        float q3_val = top_val * 0.25f;

        snprintf(buf, sizeof(buf), "%.1f", top_val);
        lv_label_set_text(lbl_y_top, buf);

        snprintf(buf, sizeof(buf), "%.1f", q1_val);
        lv_label_set_text(lbl_y_q1, buf);

        snprintf(buf, sizeof(buf), "%.1f", mid_val);
        lv_label_set_text(lbl_y_mid, buf);

        snprintf(buf, sizeof(buf), "%.1f", q3_val);
        lv_label_set_text(lbl_y_q3, buf);

        lv_label_set_text(lbl_y_bot, "0");
    }

    lv_obj_set_style_text_color(lbl_y_top, lv_color_hex(label_color), 0);
    lv_obj_set_style_text_color(lbl_y_q1, lv_color_hex(label_color), 0);
    lv_obj_set_style_text_color(lbl_y_mid, lv_color_hex(label_color), 0);
    lv_obj_set_style_text_color(lbl_y_q3, lv_color_hex(label_color), 0);
    lv_obj_set_style_text_color(lbl_y_bot, lv_color_hex(label_color), 0);
}

/* -- Update series colors based on current theme ------------------------- */
void update_series_colors(void) {
    if (!chart) return;
    lv_chart_set_series_color(chart, ser_ra, lv_color_hex(get_ra_color()));
    lv_chart_set_series_color(chart, ser_dec, lv_color_hex(get_dec_color()));
    lv_chart_set_series_color(chart, ser_total, lv_color_hex(get_total_color()));
    lv_chart_set_series_color(chart, ser_hfr, lv_color_hex(get_hfr_color()));
}

/* -- Update threshold dashed lines on the chart -------------------------- */
void update_threshold_lines(int y_min_x100, int y_max_x100) {
    if (!chart) return;

    int instance_idx = return_page_index - 1;
    if (instance_idx < 0) instance_idx = 0;
    if (instance_idx >= MAX_NINA_INSTANCES) instance_idx = MAX_NINA_INSTANCES - 1;

    threshold_config_t tcfg;
    if (current_type == GRAPH_TYPE_RMS) {
        app_config_get_rms_threshold_config(instance_idx, &tcfg);
    } else {
        app_config_get_hfr_threshold_config(instance_idx, &tcfg);
    }

    int gb = app_config_get()->color_brightness;
    int chart_h = lv_obj_get_height(chart);
    int chart_w = lv_obj_get_width(chart);
    int pad = GR_CHART_PAD;
    int data_h = chart_h - 2 * pad;
    int data_w = chart_w - 2 * pad;
    int y_range = y_max_x100 - y_min_x100;

    /* Build threshold entries */
    struct thresh_entry { int val_x100; uint32_t color; };
    struct thresh_entry entries[MAX_THRESH_LINES];
    int count = 0;

    int good_x100 = (int)(tcfg.good_max * 100.0f);
    int ok_x100 = (int)(tcfg.ok_max * 100.0f);

    if (current_type == GRAPH_TYPE_RMS) {
        /* Symmetric thresholds: +/- good_max and +/- ok_max */
        entries[count++] = (struct thresh_entry){ good_x100, tcfg.good_color};
        entries[count++] = (struct thresh_entry){-good_x100, tcfg.good_color};
        entries[count++] = (struct thresh_entry){ ok_x100, tcfg.ok_color};
        entries[count++] = (struct thresh_entry){-ok_x100, tcfg.ok_color};
    } else {
        entries[count++] = (struct thresh_entry){good_x100, tcfg.good_color};
        entries[count++] = (struct thresh_entry){ok_x100, tcfg.ok_color};
    }

    for (int i = 0; i < MAX_THRESH_LINES; i++) {
        if (!thresh_lines[i]) continue;

        if (i < count && y_range > 0 &&
            entries[i].val_x100 >= y_min_x100 && entries[i].val_x100 <= y_max_x100) {

            float frac = (float)(entries[i].val_x100 - y_min_x100) / (float)y_range;
            int pixel_y = pad + (int)(data_h * (1.0f - frac));

            thresh_line_pts[i][0].x = 0;
            thresh_line_pts[i][0].y = 0;
            thresh_line_pts[i][1].x = data_w;
            thresh_line_pts[i][1].y = 0;

            lv_line_set_points(thresh_lines[i], thresh_line_pts[i], 2);
            uint32_t color = app_config_apply_brightness(entries[i].color, gb);
            lv_obj_set_style_line_color(thresh_lines[i], lv_color_hex(color), 0);
            lv_obj_set_pos(thresh_lines[i], pad, pixel_y);
            lv_obj_clear_flag(thresh_lines[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(thresh_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* -- Show loading state: clear chart series and show loading label ------- */
void show_loading_state(void) {
    if (chart) {
        lv_chart_hide_series(chart, ser_ra, true);
        lv_chart_hide_series(chart, ser_dec, true);
        lv_chart_hide_series(chart, ser_total, true);
        lv_chart_hide_series(chart, ser_hfr, true);
        lv_chart_refresh(chart);
    }
    if (loading_lbl) {
        lv_obj_clear_flag(loading_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(loading_lbl, "Loading graph data...");
        lv_obj_center(loading_lbl);
    }
    for (int i = 0; i < MAX_THRESH_LINES; i++) {
        if (thresh_lines[i]) lv_obj_add_flag(thresh_lines[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (legend_cont) lv_obj_add_flag(legend_cont, LV_OBJ_FLAG_HIDDEN);
    if (lbl_summary) lv_label_set_text(lbl_summary, "");
}

/* -- Create the overlay (once) ------------------------------------------- */

void nina_graph_overlay_create(lv_obj_t *parent) {
    overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(overlay, GR_PAD, 0);
    lv_obj_set_style_pad_row(overlay, 6, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    int gb = app_config_get()->color_brightness;

    /* -- Chart area: container holding chart (full width) -- */
    chart_area = lv_obj_create(overlay);
    lv_obj_remove_style_all(chart_area);
    lv_obj_set_width(chart_area, LV_PCT(100));
    lv_obj_set_flex_grow(chart_area, 1);
    lv_obj_clear_flag(chart_area, LV_OBJ_FLAG_SCROLLABLE);

    /* Chart */
    chart = lv_chart_create(chart_area);
    lv_obj_set_size(chart, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart, 16, 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(chart, GR_CHART_PAD, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);  /* No point dots */
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 50);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);

    /* Y-axis label column (floating inside chart, left edge) */
    y_label_col = lv_obj_create(chart);
    lv_obj_remove_style_all(y_label_col);
    lv_obj_set_width(y_label_col, GR_Y_LABEL_W);
    lv_obj_set_height(y_label_col, LV_PCT(100));
    lv_obj_set_flex_flow(y_label_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(y_label_col, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(y_label_col, GR_CHART_PAD + 1, 0);
    lv_obj_set_style_pad_bottom(y_label_col, GR_CHART_PAD + 1, 0);
    lv_obj_set_style_pad_left(y_label_col, 4, 0);
    lv_obj_add_flag(y_label_col, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(y_label_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(y_label_col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(y_label_col, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_y_top = lv_label_create(y_label_col);
    lv_obj_set_style_text_font(lbl_y_top, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_y_top, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(lbl_y_top, "");

    lbl_y_q1 = lv_label_create(y_label_col);
    lv_obj_set_style_text_font(lbl_y_q1, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_y_q1, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(lbl_y_q1, "");

    lbl_y_mid = lv_label_create(y_label_col);
    lv_obj_set_style_text_font(lbl_y_mid, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_y_mid, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(lbl_y_mid, "");

    lbl_y_q3 = lv_label_create(y_label_col);
    lv_obj_set_style_text_font(lbl_y_q3, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_y_q3, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(lbl_y_q3, "");

    lbl_y_bot = lv_label_create(y_label_col);
    lv_obj_set_style_text_font(lbl_y_bot, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_y_bot, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(lbl_y_bot, "");

    /* Pre-create all series (hidden when not used) */
    ser_ra = lv_chart_add_series(chart, lv_color_hex(COLOR_RA), LV_CHART_AXIS_PRIMARY_Y);
    ser_dec = lv_chart_add_series(chart, lv_color_hex(COLOR_DEC), LV_CHART_AXIS_PRIMARY_Y);
    ser_total = lv_chart_add_series(chart, lv_color_hex(COLOR_TOTAL), LV_CHART_AXIS_PRIMARY_Y);
    ser_hfr = lv_chart_add_series(chart, lv_color_hex(COLOR_HFR), LV_CHART_AXIS_PRIMARY_Y);

    /* Pre-create threshold dashed lines (hidden by default) */
    for (int i = 0; i < MAX_THRESH_LINES; i++) {
        thresh_lines[i] = lv_line_create(chart);
        lv_obj_add_flag(thresh_lines[i], LV_OBJ_FLAG_FLOATING);
        lv_obj_clear_flag(thresh_lines[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(thresh_lines[i], 1, 0);
        lv_obj_set_style_line_dash_width(thresh_lines[i], 6, 0);
        lv_obj_set_style_line_dash_gap(thresh_lines[i], 4, 0);
        lv_obj_set_style_line_opa(thresh_lines[i], LV_OPA_70, 0);
        lv_obj_add_flag(thresh_lines[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* -- Loading label (floating inside chart) -- */
    loading_lbl = lv_label_create(chart);
    lv_obj_set_style_text_color(loading_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(loading_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(loading_lbl, "Loading graph data...");
    lv_obj_add_flag(loading_lbl, LV_OBJ_FLAG_FLOATING);
    lv_obj_center(loading_lbl);

    /* -- Title (floating inside chart, top center) -- */
    lbl_title = lv_label_create(chart);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(app_config_apply_brightness(0xffffff, gb)), 0);
    lv_obj_set_style_bg_color(lbl_title, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lbl_title, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(lbl_title, 10, 0);
    lv_obj_set_style_pad_ver(lbl_title, 4, 0);
    lv_obj_set_style_radius(lbl_title, 8, 0);
    lv_label_set_text(lbl_title, "RMS History");
    lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 6);

    /* -- Summary label (floating inside chart, bottom center) -- */
    lbl_summary = lv_label_create(chart);
    lv_obj_set_style_text_font(lbl_summary, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_summary, lv_color_hex(app_config_apply_brightness(0xaaaaaa, gb)), 0);
    lv_obj_set_style_bg_color(lbl_summary, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lbl_summary, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(lbl_summary, 10, 0);
    lv_obj_set_style_pad_ver(lbl_summary, 4, 0);
    lv_obj_set_style_radius(lbl_summary, 8, 0);
    lv_label_set_text(lbl_summary, "");
    lv_obj_add_flag(lbl_summary, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(lbl_summary, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lbl_summary, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* -- X-axis title -- */
    lbl_x_title = lv_label_create(overlay);
    lv_obj_set_style_text_font(lbl_x_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_x_title, lv_color_hex(app_config_apply_brightness(0x888888, gb)), 0);
    lv_label_set_text(lbl_x_title, "Samples");

    /* -- Legend toggle (inside chart, bottom-right corner) -- */
    legend_cont = lv_obj_create(chart);
    lv_obj_remove_style_all(legend_cont);
    lv_obj_set_size(legend_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend_cont, 12, 0);
    lv_obj_set_style_pad_all(legend_cont, 6, 0);
    lv_obj_set_style_bg_color(legend_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(legend_cont, LV_OPA_50, 0);
    lv_obj_set_style_radius(legend_cont, 8, 0);
    lv_obj_add_flag(legend_cont, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(legend_cont, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(legend_cont, LV_OBJ_FLAG_HIDDEN);

    /* -- Back button (floating on overlay, bottom-right corner) -- */
    btn_back = lv_button_create(overlay);
    lv_obj_set_size(btn_back, GR_BACK_BTN_W, GR_CONTROLS_H);
    lv_obj_set_style_radius(btn_back, 14, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme ? current_theme->bento_border : 0x333333), 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme ? current_theme->progress_color : 0x4FC3F7), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_add_flag(btn_back, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_RIGHT, -GR_PAD, -GR_PAD);

    btn_back_lbl = lv_label_create(btn_back);
    lv_label_set_text(btn_back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(btn_back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(btn_back_lbl, lv_color_hex(get_control_text_color(gb)), 0);
    lv_obj_center(btn_back_lbl);

    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* -- Public API ---------------------------------------------------------- */

void nina_graph_show(graph_type_t type, int page_index) {
    if (!overlay) return;

    current_type = type;
    return_page_index = page_index;
    selected_scale_idx = 0;  /* Reset to auto scale */

    /* Reset legend toggle state (Total hidden by default) */
    legend_ra_hidden = false;
    legend_dec_hidden = false;
    legend_total_hidden = true;
    legend_hfr_hidden = false;

    /* Update title */
    if (lbl_title) {
        lv_label_set_text(lbl_title, (type == GRAPH_TYPE_RMS) ? "RMS History" : "HFR History");
    }

    /* Show empty chart with loading text */
    show_loading_state();

    /* Update series colors for current theme */
    update_series_colors();

    /* Rebuild controls for this graph type */
    rebuild_controls();
    rebuild_legend();
    apply_chart_theme();

    /* Show overlay */
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    /* Request data fetch */
    graph_requested = true;
}

void nina_graph_hide(void) {
    if (overlay) {
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    }
    graph_requested = false;
}

bool nina_graph_visible(void) {
    return overlay && !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

bool nina_graph_requested(void) {
    return graph_requested;
}

void nina_graph_clear_request(void) {
    graph_requested = false;
}

void nina_graph_set_refresh_pending(void) {
    /* Silent refresh: set the request flag without showing loading indicator.
     * The chart stays visible while fresh data is fetched in the background. */
    graph_requested = true;
}

graph_type_t nina_graph_get_type(void) {
    return current_type;
}

int nina_graph_get_requested_points(void) {
    if (selected_points_idx >= 0 && selected_points_idx < POINT_OPT_COUNT) {
        return point_options[selected_points_idx];
    }
    return 50;
}

void nina_graph_set_rms_data(const graph_rms_data_t *data) {
    if (!overlay || !chart || !data) return;
    if (lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN)) return;

    int count = data->count;
    if (count <= 0) {
        if (loading_lbl) {
            lv_label_set_text(loading_lbl, "No guider data available");
            lv_obj_center(loading_lbl);
        }
        return;
    }
    if (count > GRAPH_MAX_POINTS) count = GRAPH_MAX_POINTS;

    /* Configure chart */
    lv_chart_set_point_count(chart, count);

    /* Hide HFR series, show RMS series (respecting legend toggle) */
    lv_chart_hide_series(chart, ser_hfr, true);
    lv_chart_hide_series(chart, ser_ra, legend_ra_hidden);
    lv_chart_hide_series(chart, ser_dec, legend_dec_hidden);
    lv_chart_hide_series(chart, ser_total, legend_total_hidden);

    /* Update series colors for current theme */
    update_series_colors();

    /* Find Y range (auto mode) */
    float max_val = 0.5f;  /* minimum visible range */
    for (int i = 0; i < count; i++) {
        float abs_ra = fabsf(data->ra[i]);
        float abs_dec = fabsf(data->dec[i]);
        if (abs_ra > max_val) max_val = abs_ra;
        if (abs_dec > max_val) max_val = abs_dec;
    }
    /* Add 20% headroom and round up */
    int range = (int)(max_val * 120.0f + 50.0f);  /* x100 then +headroom */
    if (range < 100) range = 100;

    /* Apply Y scale */
    int scale_val = 0;
    if (selected_scale_idx > 0 && selected_scale_idx < RMS_SCALE_COUNT) {
        scale_val = rms_scale_values[selected_scale_idx];
    }
    if (scale_val > 0) {
        range = scale_val;
    }

    /* Always center on 0: symmetric range */
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -range, range);
    update_y_labels(-range, range);

    /* Populate series data (values are x100 for int precision) */
    for (int i = 0; i < count; i++) {
        lv_chart_set_next_value(chart, ser_ra, (int32_t)(data->ra[i] * 100.0f));
        lv_chart_set_next_value(chart, ser_dec, (int32_t)(data->dec[i] * 100.0f));
        float total_i = sqrtf(data->ra[i] * data->ra[i] + data->dec[i] * data->dec[i]);
        lv_chart_set_next_value(chart, ser_total, (int32_t)(total_i * 100.0f));
    }

    /* Show threshold lines at configured good/ok boundaries */
    update_threshold_lines(-range, range);

    lv_chart_refresh(chart);

    /* Update summary */
    if (lbl_summary) {
        lv_label_set_text_fmt(lbl_summary, "RA:%.2f\" DEC:%.2f\" Tot:%.2f\"",
                              data->rms_ra, data->rms_dec, data->rms_total);
    }

    /* Hide loading, show legend */
    if (loading_lbl) lv_obj_add_flag(loading_lbl, LV_OBJ_FLAG_HIDDEN);
    if (legend_cont) lv_obj_clear_flag(legend_cont, LV_OBJ_FLAG_HIDDEN);
}

void nina_graph_set_hfr_data(const graph_hfr_data_t *data) {
    if (!overlay || !chart || !data) return;
    if (lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN)) return;

    int count = data->count;
    if (count <= 0) {
        if (loading_lbl) {
            lv_label_set_text(loading_lbl, "No HFR data available");
            lv_obj_center(loading_lbl);
        }
        return;
    }
    if (count > GRAPH_MAX_POINTS) count = GRAPH_MAX_POINTS;

    /* Configure chart */
    lv_chart_set_point_count(chart, count);

    /* Hide RMS series, show HFR series (respecting legend toggle) */
    lv_chart_hide_series(chart, ser_ra, true);
    lv_chart_hide_series(chart, ser_dec, true);
    lv_chart_hide_series(chart, ser_total, true);
    lv_chart_hide_series(chart, ser_hfr, legend_hfr_hidden);

    /* Update series colors for current theme */
    update_series_colors();

    /* Find Y range (auto mode) -- HFR is always positive */
    float max_val = 1.0f;
    float sum = 0;
    for (int i = 0; i < count; i++) {
        if (data->hfr[i] > max_val) max_val = data->hfr[i];
        sum += data->hfr[i];
    }
    int range = (int)(max_val * 120.0f + 0.5f);  /* x100 + 20% headroom */
    if (range < 200) range = 200;

    /* Apply Y scale */
    int scale_val = 0;
    if (selected_scale_idx > 0 && selected_scale_idx < HFR_SCALE_COUNT) {
        scale_val = hfr_scale_values[selected_scale_idx];
    }
    if (scale_val > 0) {
        range = scale_val;
    }

    /* 0 is always at the bottom */
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, range);
    update_y_labels(0, range);

    /* Populate series data (values are x100) */
    for (int i = 0; i < count; i++) {
        lv_chart_set_next_value(chart, ser_hfr, (int32_t)(data->hfr[i] * 100.0f));
    }

    /* Show threshold lines at configured good/ok boundaries */
    update_threshold_lines(0, range);

    lv_chart_refresh(chart);

    /* Update summary */
    if (lbl_summary) {
        float avg = (count > 0) ? (sum / count) : 0;
        lv_label_set_text_fmt(lbl_summary, "Avg:%.2f  (%d imgs)", avg, count);
    }

    /* Hide loading, show legend */
    if (loading_lbl) lv_obj_add_flag(loading_lbl, LV_OBJ_FLAG_HIDDEN);
    if (legend_cont) lv_obj_clear_flag(legend_cont, LV_OBJ_FLAG_HIDDEN);
}

/* -- Theme --------------------------------------------------------------- */

void apply_chart_theme(void) {
    if (!current_theme || !chart) return;

    int gb = app_config_get()->color_brightness;

    /* Overlay background */
    if (overlay) {
        lv_obj_set_style_bg_color(overlay, lv_color_hex(current_theme->bg_main), 0);
    }

    /* Chart background */
    lv_obj_set_style_bg_color(chart, lv_color_hex(current_theme->bento_bg), 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(current_theme->bento_border), 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), LV_PART_MAIN);

    /* Title (floating inside chart) */
    if (lbl_title) {
        lv_obj_set_style_text_color(lbl_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        lv_obj_set_style_bg_color(lbl_title, lv_color_hex(current_theme->bento_bg), 0);
    }

    /* Summary (floating inside chart) */
    if (lbl_summary) {
        uint32_t sum_color = is_red_night_theme()
            ? app_config_apply_brightness(current_theme->text_color, gb)
            : app_config_apply_brightness(0xaaaaaa, gb);
        lv_obj_set_style_text_color(lbl_summary, lv_color_hex(sum_color), 0);
        lv_obj_set_style_bg_color(lbl_summary, lv_color_hex(current_theme->bento_bg), 0);
    }

    /* Back button */
    if (btn_back) {
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    if (btn_back_lbl) {
        lv_obj_set_style_text_color(btn_back_lbl, lv_color_hex(get_control_text_color(gb)), 0);
    }

    /* Loading label */
    if (loading_lbl) {
        lv_obj_set_style_text_color(loading_lbl, lv_color_hex(get_control_text_color(gb)), 0);
    }

    /* X-axis title */
    if (lbl_x_title) {
        uint32_t x_color = is_red_night_theme()
            ? app_config_apply_brightness(current_theme->label_color, gb)
            : app_config_apply_brightness(0x888888, gb);
        lv_obj_set_style_text_color(lbl_x_title, lv_color_hex(x_color), 0);
    }

    /* Y-axis labels */
    if (lbl_y_top && lbl_y_q1 && lbl_y_mid && lbl_y_q3 && lbl_y_bot) {
        uint32_t y_color = current_theme
            ? app_config_apply_brightness(current_theme->text_color, gb)
            : 0xaaaaaa;
        lv_obj_set_style_text_color(lbl_y_top, lv_color_hex(y_color), 0);
        lv_obj_set_style_text_color(lbl_y_q1, lv_color_hex(y_color), 0);
        lv_obj_set_style_text_color(lbl_y_mid, lv_color_hex(y_color), 0);
        lv_obj_set_style_text_color(lbl_y_q3, lv_color_hex(y_color), 0);
        lv_obj_set_style_text_color(lbl_y_bot, lv_color_hex(y_color), 0);
    }

    /* Update series colors for theme */
    update_series_colors();
}

void nina_graph_overlay_apply_theme(void) {
    if (!overlay || !current_theme) return;
    apply_chart_theme();
    /* Rebuild controls to pick up new theme colors */
    if (!lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN)) {
        rebuild_controls();
        rebuild_legend();
        /* Refresh Y labels and threshold lines with current range */
        update_y_labels(current_y_min, current_y_max);
        update_threshold_lines(current_y_min, current_y_max);
    }
}
