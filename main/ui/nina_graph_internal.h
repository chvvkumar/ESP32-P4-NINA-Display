#pragma once

/**
 * @file nina_graph_internal.h
 * @brief Shared internal state between graph overlay and controls modules.
 */

#include "nina_graph_overlay.h"
#include "lvgl.h"
#include "themes.h"
#include "app_config.h"

/* current_theme is defined in nina_dashboard.c */
extern const theme_t *current_theme;

/* Layout constants */
#define GR_HEADER_H     64
#define GR_CONTROLS_H   108
#define GR_PAD          12
#define GR_CHART_PAD    8
#define GR_Y_LABEL_W    90

/* History point options */
extern const int point_options[];
#define POINT_OPT_COUNT 5

/* Y-scale options for RMS (arcseconds x 100) */
extern const int rms_scale_values[];
extern const char *rms_scale_labels[];
#define RMS_SCALE_COUNT 6

/* Y-scale options for HFR (value x 100) */
extern const int hfr_scale_values[];
extern const char *hfr_scale_labels[];
#define HFR_SCALE_COUNT 5

/* Shared widget state */
extern lv_obj_t *overlay;
extern lv_obj_t *chart;
extern lv_obj_t *loading_lbl;
extern lv_obj_t *legend_cont;
extern lv_obj_t *lbl_summary;
extern lv_obj_t *lbl_title;
extern lv_obj_t *btn_back;
extern lv_obj_t *btn_back_lbl;
extern lv_obj_t *chart_area;
extern lv_obj_t *lbl_x_title;
extern lv_obj_t *controls_cont;

/* Y-axis labels */
extern lv_obj_t *y_label_col;
extern lv_obj_t *lbl_y_top;
extern lv_obj_t *lbl_y_q1;
extern lv_obj_t *lbl_y_mid;
extern lv_obj_t *lbl_y_q3;
extern lv_obj_t *lbl_y_bot;

/* Chart series */
extern lv_chart_series_t *ser_ra;
extern lv_chart_series_t *ser_dec;
extern lv_chart_series_t *ser_total;
extern lv_chart_series_t *ser_hfr;

/* Point selector buttons */
extern lv_obj_t *btn_points[];
extern int selected_points_idx;

/* Scale selector buttons */
extern lv_obj_t *btn_scale[];
extern int selected_scale_idx;
extern int scale_btn_count;

/* State */
extern graph_type_t current_type;
extern int return_page_index;
extern int current_y_min, current_y_max;
extern volatile bool graph_requested;

/* Legend toggle state */
extern bool legend_ra_hidden;
extern bool legend_dec_hidden;
extern bool legend_total_hidden;
extern bool legend_hfr_hidden;

/* Threshold lines */
#define MAX_THRESH_LINES 4
extern lv_obj_t *thresh_lines[];
extern lv_point_precise_t thresh_line_pts[][2];

/* Cross-file function declarations */
void back_btn_cb(lv_event_t *e);
void rebuild_controls(void);
void rebuild_legend(void);
void show_loading_state(void);
void apply_chart_theme(void);
void update_series_colors(void);
void update_y_labels(int y_min_x100, int y_max_x100);
void update_threshold_lines(int y_min_x100, int y_max_x100);

/* Theme-aware helpers */
bool is_red_night_theme(void);
uint32_t get_ra_color(void);
uint32_t get_dec_color(void);
uint32_t get_total_color(void);
uint32_t get_hfr_color(void);
uint32_t get_control_text_color(int gb);

/* Pill button factory (reusable) */
lv_obj_t *make_pill_btn(lv_obj_t *parent, const char *text, bool selected,
                        lv_event_cb_t cb, int user_data);
