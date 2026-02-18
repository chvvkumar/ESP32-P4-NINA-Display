/**
 * @file nina_dashboard.c
 * @brief NINA dashboard UI - grid layout, page creation, theme application, gestures
 *
 * Each NINA instance gets its own page. Swipe left/right to switch pages.
 * Data updates and thumbnail overlay are in separate files.
 */

#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "nina_thumbnail.h"
#include "app_config.h"
#include "themes.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

/* Shared state — accessed by nina_dashboard_update.c and nina_thumbnail.c */
dashboard_page_t pages[MAX_NINA_INSTANCES];
int page_count = 0;
int active_page = 0;
const theme_t *current_theme = NULL;

/* Shared styles */
lv_style_t style_bento_box;
lv_style_t style_label_small;
lv_style_t style_value_large;
lv_style_t style_header_gradient;

/* Private state */
static lv_obj_t *scr_dashboard = NULL;
static lv_obj_t *main_cont = NULL;

// Page dots
static lv_obj_t *indicator_cont = NULL;
static lv_obj_t *indicator_dots[MAX_NINA_INSTANCES];

// Swipe callback
static nina_page_change_cb_t page_change_cb = NULL;

/* Strip http(s)://, path, and domain from URL, then sentence case */
void extract_host_from_url(const char *url, char *out, size_t out_size) {
    if (out_size == 0) return;

    const char *start = url;
    if (strncmp(start, "https://", 8) == 0) start += 8;
    else if (strncmp(start, "http://", 7) == 0) start += 7;

    const char *end = start;
    while (*end && *end != '/' && *end != ':' && *end != '.') end++;

    int len = (int)(end - start);
    if (len <= 0) { out[0] = '\0'; return; }
    if ((size_t)len >= out_size) len = (int)(out_size - 1);

    memcpy(out, start, (size_t)len);
    out[len] = '\0';

    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
    for (int i = 1; i < len; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;
    }
}

/* (Re)init styles from the active theme */
static void update_styles(void) {
    if (!current_theme) return;

    int gb = app_config_get()->color_brightness;

    lv_style_reset(&style_bento_box);
    lv_style_init(&style_bento_box);
    lv_style_set_bg_color(&style_bento_box, lv_color_hex(current_theme->bento_bg));
    lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_radius(&style_bento_box, BENTO_RADIUS);
    lv_style_set_border_width(&style_bento_box, 1);
    lv_style_set_border_color(&style_bento_box, lv_color_hex(current_theme->bento_bg));
    lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_pad_all(&style_bento_box, 20);

    lv_style_reset(&style_label_small);
    lv_style_init(&style_label_small);
    lv_style_set_text_color(&style_label_small, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)));
    lv_style_set_text_font(&style_label_small, &lv_font_montserrat_16);
    lv_style_set_text_letter_space(&style_label_small, 1);

    lv_style_reset(&style_value_large);
    lv_style_init(&style_value_large);
    lv_style_set_text_color(&style_value_large, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)));
#ifdef LV_FONT_MONTSERRAT_48
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_48);
#elif defined(LV_FONT_MONTSERRAT_32)
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_32);
#elif defined(LV_FONT_MONTSERRAT_28)
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_28);
#else
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_20);
#endif

    lv_style_reset(&style_header_gradient);
    lv_style_init(&style_header_gradient);
    lv_style_set_bg_color(&style_header_gradient, lv_color_hex(current_theme->header_grad_color));
    lv_style_set_bg_grad_color(&style_header_gradient, lv_color_hex(0x000000));
    lv_style_set_bg_grad_dir(&style_header_gradient, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&style_header_gradient, LV_OPA_30);
    lv_style_set_radius(&style_header_gradient, BENTO_RADIUS);
    lv_style_set_border_width(&style_header_gradient, 1);
    lv_style_set_border_color(&style_header_gradient, lv_color_hex(current_theme->header_grad_color));
    lv_style_set_border_opa(&style_header_gradient, LV_OPA_30);
    lv_style_set_pad_all(&style_header_gradient, 20);
}

lv_obj_t *create_bento_box(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &style_bento_box, 0);
    return box;
}

lv_obj_t *create_small_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &style_label_small, 0);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *create_value_label(lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &style_value_large, 0);
    lv_label_set_text(label, "--");
    return label;
}

/* Set theme colors on all widgets in a page */
static void apply_theme_to_page(dashboard_page_t *p) {
    if (!p->page || !current_theme) return;

    int gb = app_config_get()->color_brightness;

    if (p->lbl_target_name) lv_obj_set_style_text_color(p->lbl_target_name, lv_color_hex(app_config_apply_brightness(current_theme->target_name_color, gb)), 0);

    if (p->lbl_seq_container) lv_obj_set_style_text_color(p->lbl_seq_container, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    if (p->lbl_seq_step) lv_obj_set_style_text_color(p->lbl_seq_step, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    if (p->arc_exposure) {
        lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(current_theme->bg_main), LV_PART_MAIN);
    }

    if (p->lbl_loop_count) lv_obj_set_style_text_color(p->lbl_loop_count, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);

    if (p->lbl_rms_title) lv_obj_set_style_text_color(p->lbl_rms_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_hfr_title) lv_obj_set_style_text_color(p->lbl_hfr_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_flip_title) lv_obj_set_style_text_color(p->lbl_flip_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_stars_header) lv_obj_set_style_text_color(p->lbl_stars_header, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_target_time_header) lv_obj_set_style_text_color(p->lbl_target_time_header, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    if (p->lbl_rms_value) lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->rms_color, gb)), 0);
    if (p->lbl_hfr_value) lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->hfr_color, gb)), 0);

    for (int i = 0; i < MAX_POWER_WIDGETS; i++) {
        if (p->lbl_pwr_title[i]) lv_obj_set_style_text_color(p->lbl_pwr_title[i], lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
}

void nina_dashboard_apply_theme(int theme_index) {
    current_theme = themes_get(theme_index);
    update_styles();

    if (!scr_dashboard) return;

    if (main_cont) {
        lv_obj_set_style_bg_color(main_cont, lv_color_hex(current_theme->bg_main), 0);
    }

    lv_obj_report_style_change(&style_bento_box);
    lv_obj_report_style_change(&style_label_small);
    lv_obj_report_style_change(&style_value_large);
    lv_obj_report_style_change(&style_header_gradient);

    for (int i = 0; i < page_count; i++) {
        apply_theme_to_page(&pages[i]);
    }

    if (indicator_cont) {
        int gb = app_config_get()->color_brightness;
        for (int i = 0; i < page_count; i++) {
            if (indicator_dots[i]) {
                uint32_t dot_color = (i == active_page)
                    ? app_config_apply_brightness(current_theme->text_color, gb)
                    : app_config_apply_brightness(current_theme->label_color, gb);
                lv_obj_set_style_bg_color(indicator_dots[i], lv_color_hex(dot_color), 0);
            }
        }
    }

    lv_obj_invalidate(scr_dashboard);
}

/* Build all widgets for one dashboard page */
static void create_dashboard_page(dashboard_page_t *p, lv_obj_t *parent, int page_index) {
    memset(p, 0, sizeof(dashboard_page_t));

    p->page = lv_obj_create(parent);
    lv_obj_remove_style_all(p->page);
    lv_obj_set_size(p->page, SCREEN_SIZE - 2 * OUTER_PADDING, SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_style_pad_gap(p->page, GRID_GAP, 0);

    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {
        LV_GRID_FR(2), LV_GRID_FR(1), LV_GRID_FR(2),
        LV_GRID_FR(2), LV_GRID_FR(2), LV_GRID_FR(2),
        LV_GRID_TEMPLATE_LAST
    };
    lv_obj_set_layout(p->page, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(p->page, col_dsc, row_dsc);

    // Header (row 0, spans 2 cols)
    p->header_box = create_bento_box(p->page);
    lv_obj_set_grid_cell(p->header_box, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_flex_flow(p->header_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p->header_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(p->header_box, 12, 0);

    lv_obj_t *top_row = lv_obj_create(p->header_box);
    lv_obj_remove_style_all(top_row);
    lv_obj_set_width(top_row, LV_PCT(100));
    lv_obj_set_height(top_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left_cont = lv_obj_create(top_row);
    lv_obj_remove_style_all(left_cont);
    lv_obj_set_size(left_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_cont, 0, 0);

    p->lbl_instance_name = lv_label_create(left_cont);
    lv_obj_set_style_text_color(p->lbl_instance_name, lv_color_hex(0xf87171), 0);
    lv_obj_set_style_text_font(p->lbl_instance_name, &lv_font_montserrat_26, 0);
    {
        const char *init_url = app_config_get_instance_url(page_index);
        char host[64] = {0};
        extract_host_from_url(init_url, host, sizeof(host));
        if (host[0] != '\0') {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s - Not Connected", host);
            lv_label_set_text(p->lbl_instance_name, buf);
        } else {
            lv_label_set_text(p->lbl_instance_name, "N.I.N.A.");
        }
    }

    p->lbl_target_name = lv_label_create(p->header_box);
    lv_obj_add_style(p->lbl_target_name, &style_value_large, 0);
    lv_obj_set_width(p->lbl_target_name, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_target_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(p->lbl_target_name, "----");

    // Sequence info (row 1, spans 2 cols)
    lv_obj_t *box_seq = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_seq, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_flex_flow(box_seq, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_seq, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box_seq, 12, 0);

    lv_obj_t *seq_left = lv_obj_create(box_seq);
    lv_obj_remove_style_all(seq_left);
    lv_obj_set_size(seq_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_left, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *lbl_seq_title = create_small_label(seq_left, "SEQUENCE");
    lv_obj_set_style_text_font(lbl_seq_title, &lv_font_montserrat_14, 0);

    p->lbl_seq_container = lv_label_create(seq_left);
    lv_obj_set_style_text_color(p->lbl_seq_container, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_text_font(p->lbl_seq_container, &lv_font_montserrat_24, 0);
    lv_label_set_text(p->lbl_seq_container, "----");

    lv_obj_t *seq_right = lv_obj_create(box_seq);
    lv_obj_remove_style_all(seq_right);
    lv_obj_set_size(seq_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(seq_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_t *lbl_step_title = create_small_label(seq_right, "STEP");
    lv_obj_set_style_text_font(lbl_step_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl_step_title, LV_TEXT_ALIGN_RIGHT, 0);

    p->lbl_seq_step = lv_label_create(seq_right);
    lv_obj_set_style_text_color(p->lbl_seq_step, lv_color_hex(current_theme->text_color), 0);
    lv_obj_set_style_text_font(p->lbl_seq_step, &lv_font_montserrat_24, 0);
    lv_label_set_text(p->lbl_seq_step, "----");

    // Exposure arc (col 0, rows 2-4)
    lv_obj_t *box_exposure = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_exposure, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 3);
    lv_obj_set_flex_flow(box_exposure, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_exposure, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box_exposure, 0, 0);

    p->arc_exposure = lv_arc_create(box_exposure);
    lv_obj_set_size(p->arc_exposure, 300, 300);
    lv_arc_set_rotation(p->arc_exposure, 270);
    lv_arc_set_bg_angles(p->arc_exposure, 0, 360);
    lv_arc_set_value(p->arc_exposure, 0);
    lv_obj_remove_style(p->arc_exposure, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(current_theme->bg_main), LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->arc_exposure, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(p->arc_exposure, 12, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(p->arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(p->arc_exposure, 16, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_spread(p->arc_exposure, 10, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(p->arc_exposure, LV_OPA_30, LV_PART_INDICATOR);

    lv_obj_t *arc_center = lv_obj_create(p->arc_exposure);
    lv_obj_remove_style_all(arc_center);
    lv_obj_set_size(arc_center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(arc_center);
    lv_obj_set_flex_flow(arc_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(arc_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->lbl_exposure_current = lv_label_create(arc_center);
    lv_obj_add_style(p->lbl_exposure_current, &style_value_large, 0);
    lv_label_set_text(p->lbl_exposure_current, "----s");

    p->lbl_exposure_total = create_small_label(arc_center, "");
    lv_obj_set_style_text_color(p->lbl_exposure_total, lv_color_hex(current_theme->filter_text_color), 0);
    lv_obj_set_style_text_font(p->lbl_exposure_total, &lv_font_montserrat_28, 0);
    lv_obj_set_style_pad_top(p->lbl_exposure_total, 14, 0);

    p->lbl_loop_count = lv_label_create(arc_center);
    lv_obj_set_style_text_color(p->lbl_loop_count, lv_color_hex(current_theme->label_color), 0);
    lv_obj_set_style_text_font(p->lbl_loop_count, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_top(p->lbl_loop_count, 8, 0);
    lv_label_set_text(p->lbl_loop_count, "-- / --");

    // RMS + HFR (col 1, row 2)
    lv_obj_t *box_rms_hfr = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_rms_hfr, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_set_flex_flow(box_rms_hfr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_rms_hfr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box_rms_hfr, 8, 0);

    lv_obj_t *box_rms = lv_obj_create(box_rms_hfr);
    lv_obj_remove_style_all(box_rms);
    lv_obj_set_size(box_rms, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_rms, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_rms, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->lbl_rms_title = create_small_label(box_rms, "RMS");
    lv_obj_set_width(p->lbl_rms_title, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_rms_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_rms_title, lv_color_hex(current_theme->text_color), 0);

    p->lbl_rms_value = create_value_label(box_rms);
    lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(current_theme->rms_color), 0);
    lv_label_set_text(p->lbl_rms_value, "--");

    lv_obj_t *box_hfr = lv_obj_create(box_rms_hfr);
    lv_obj_remove_style_all(box_hfr);
    lv_obj_set_size(box_hfr, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_hfr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_hfr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->lbl_hfr_title = create_small_label(box_hfr, "HFR");
    lv_obj_set_width(p->lbl_hfr_title, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_hfr_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_hfr_title, lv_color_hex(current_theme->text_color), 0);

    p->lbl_hfr_value = create_value_label(box_hfr);
    lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(current_theme->hfr_color), 0);
    lv_label_set_text(p->lbl_hfr_value, "--");

    // Time to flip (col 1, row 3)
    lv_obj_t *box_flip = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_flip, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_set_flex_flow(box_flip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_flip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->lbl_flip_title = create_small_label(box_flip, "TIME UNTIL FLIP");
    lv_obj_set_width(p->lbl_flip_title, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_flip_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_flip_title, lv_color_hex(current_theme->text_color), 0);

    p->lbl_flip_value = create_value_label(box_flip);
    lv_label_set_text(p->lbl_flip_value, "--");

    // Target time + stars (col 1, row 4)
    lv_obj_t *box_sat_stars = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_sat_stars, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 4, 1);
    lv_obj_set_flex_flow(box_sat_stars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_sat_stars, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box_sat_stars, 8, 0);

    lv_obj_t *box_target_time = lv_obj_create(box_sat_stars);
    lv_obj_remove_style_all(box_target_time);
    lv_obj_set_size(box_target_time, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_target_time, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_target_time, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->lbl_target_time_header = create_small_label(box_target_time, "TIME LEFT (H:M)");
    lv_obj_set_width(p->lbl_target_time_header, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_target_time_header, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_target_time_header, lv_color_hex(current_theme->text_color), 0);

    p->lbl_target_time_value = create_value_label(box_target_time);
    lv_label_set_text(p->lbl_target_time_value, "--");

    lv_obj_t *box_stars = lv_obj_create(box_sat_stars);
    lv_obj_remove_style_all(box_stars);
    lv_obj_set_size(box_stars, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_stars, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_stars, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->lbl_stars_header = create_small_label(box_stars, "STARS");
    lv_obj_set_width(p->lbl_stars_header, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_stars_header, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_stars_header, lv_color_hex(current_theme->text_color), 0);

    p->lbl_stars_value = create_value_label(box_stars);
    lv_label_set_text(p->lbl_stars_value, "--");

    // Power row (row 5, spans 2 cols)
    lv_obj_t *box_power = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_power, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 5, 1);
    lv_obj_set_flex_flow(box_power, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_power, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box_power, 8, 0);

    for (int i = 0; i < MAX_POWER_WIDGETS; i++) {
        p->box_pwr[i] = lv_obj_create(box_power);
        lv_obj_remove_style_all(p->box_pwr[i]);
        lv_obj_set_flex_grow(p->box_pwr[i], 1);
        lv_obj_set_height(p->box_pwr[i], LV_PCT(100));
        lv_obj_set_flex_flow(p->box_pwr[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(p->box_pwr[i], LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        p->lbl_pwr_title[i] = create_small_label(p->box_pwr[i], "--");
        lv_obj_set_width(p->lbl_pwr_title[i], LV_PCT(100));
        lv_obj_set_style_text_align(p->lbl_pwr_title[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(p->lbl_pwr_title[i], lv_color_hex(current_theme->text_color), 0);

        p->lbl_pwr_value[i] = create_value_label(p->box_pwr[i]);
        lv_label_set_text(p->lbl_pwr_value[i], "--");
#ifdef LV_FONT_MONTSERRAT_36
        lv_obj_set_style_text_font(p->lbl_pwr_value[i], &lv_font_montserrat_36, 0);
#elif defined(LV_FONT_MONTSERRAT_28)
        lv_obj_set_style_text_font(p->lbl_pwr_value[i], &lv_font_montserrat_28, 0);
#elif defined(LV_FONT_MONTSERRAT_24)
        lv_obj_set_style_text_font(p->lbl_pwr_value[i], &lv_font_montserrat_24, 0);
#endif

        lv_obj_add_flag(p->box_pwr[i], LV_OBJ_FLAG_HIDDEN);
    }

    p->prev_target_progress = 0;
    p->pending_arc_progress = 0;
    p->arc_completing = false;
}

/* Page indicator dots at the bottom */
static void create_page_indicator(lv_obj_t *parent, int count) {
    if (count <= 1) return;

    indicator_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(indicator_cont);
    lv_obj_set_size(indicator_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(indicator_cont, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(indicator_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(indicator_cont, 8, 0);

    int gb = app_config_get()->color_brightness;

    for (int i = 0; i < count; i++) {
        indicator_dots[i] = lv_obj_create(indicator_cont);
        lv_obj_remove_style_all(indicator_dots[i]);
        lv_obj_set_size(indicator_dots[i], 10, 10);
        lv_obj_set_style_radius(indicator_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(indicator_dots[i], LV_OPA_COVER, 0);

        uint32_t dot_color = (i == 0)
            ? app_config_apply_brightness(current_theme->text_color, gb)
            : app_config_apply_brightness(current_theme->label_color, gb);
        lv_obj_set_style_bg_color(indicator_dots[i], lv_color_hex(dot_color), 0);
    }
}

void nina_dashboard_set_page_change_cb(nina_page_change_cb_t cb) {
    page_change_cb = cb;
}

/* Swipe left/right to change pages */
static void gesture_event_cb(lv_event_t *e) {
    if (page_count <= 1) return;
    if (thumbnail_overlay && !lv_obj_has_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN)) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    int new_page = active_page;

    if (dir == LV_DIR_LEFT) {
        new_page = (active_page + 1) % page_count;
    } else if (dir == LV_DIR_RIGHT) {
        new_page = (active_page - 1 + page_count) % page_count;
    } else {
        return;
    }

    nina_dashboard_show_page(new_page, page_count);

    if (page_change_cb) {
        page_change_cb(new_page);
    }
}

/* Target name: click to request thumbnail */
static void target_name_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_thumbnail_request();
}

/* Set up the dashboard with one page per NINA instance */
void create_nina_dashboard(lv_obj_t *parent, int instance_count) {
    app_config_t *cfg = app_config_get();
    current_theme = themes_get(cfg->theme_index);
    update_styles();

    scr_dashboard = parent;

    if (instance_count < 1) instance_count = 1;
    if (instance_count > MAX_NINA_INSTANCES) instance_count = MAX_NINA_INSTANCES;
    page_count = instance_count;
    active_page = 0;

    main_cont = lv_obj_create(scr_dashboard);
    lv_obj_remove_style_all(main_cont);
    lv_obj_set_size(main_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(main_cont, lv_color_hex(current_theme->bg_main), 0);
    lv_obj_set_style_bg_opa(main_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(main_cont, OUTER_PADDING, 0);

    for (int i = 0; i < page_count; i++) {
        create_dashboard_page(&pages[i], main_cont, i);
        if (i != 0) {
            lv_obj_add_flag(pages[i].page, LV_OBJ_FLAG_HIDDEN);
        }
    }

    create_page_indicator(scr_dashboard, page_count);

    if (page_count > 1) {
        lv_obj_add_event_cb(scr_dashboard, gesture_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_clear_flag(scr_dashboard, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    // Thumbnail overlay (on top of everything)
    nina_thumbnail_create(scr_dashboard);

    // Make header box clickable on all pages to open thumbnail
    for (int i = 0; i < page_count; i++) {
        if (pages[i].header_box) {
            lv_obj_add_flag(pages[i].header_box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(pages[i].header_box, target_name_click_cb, LV_EVENT_CLICKED, NULL);
        }
    }

    nina_dashboard_apply_theme(cfg->theme_index);
}

void nina_dashboard_show_page(int page_index, int instance_count) {
    if (page_index < 0 || page_index >= page_count) return;
    if (page_index == active_page) return;

    lv_obj_add_flag(pages[active_page].page, LV_OBJ_FLAG_HIDDEN);

    active_page = page_index;
    lv_obj_clear_flag(pages[active_page].page, LV_OBJ_FLAG_HIDDEN);

    if (indicator_cont) {
        int gb = app_config_get()->color_brightness;
        for (int i = 0; i < page_count; i++) {
            if (indicator_dots[i]) {
                uint32_t dot_color = (i == active_page)
                    ? app_config_apply_brightness(current_theme->text_color, gb)
                    : app_config_apply_brightness(current_theme->label_color, gb);
                lv_obj_set_style_bg_color(indicator_dots[i], lv_color_hex(dot_color), 0);
            }
        }
    }
}

int nina_dashboard_get_active_page(void) {
    return active_page;
}

static void fade_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void nina_dashboard_show_page_animated(int page_index, int instance_count, int effect)
{
    if (page_index < 0 || page_index >= page_count) return;
    if (page_index == active_page) return;

    lv_obj_add_flag(pages[active_page].page, LV_OBJ_FLAG_HIDDEN);
    active_page = page_index;

    if (effect == 1) {
        /* Fade-in: start transparent, animate to fully opaque */
        lv_obj_set_style_opa(pages[active_page].page, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(pages[active_page].page, LV_OBJ_FLAG_HIDDEN);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_exec_cb(&a, fade_anim_cb);
        lv_anim_set_var(&a, pages[active_page].page);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, 1000);
        lv_anim_start(&a);
    } else {
        lv_obj_clear_flag(pages[active_page].page, LV_OBJ_FLAG_HIDDEN);
    }

    /* Update indicator dots */
    if (indicator_cont) {
        int gb = app_config_get()->color_brightness;
        for (int i = 0; i < page_count; i++) {
            if (indicator_dots[i]) {
                uint32_t dot_color = (i == active_page)
                    ? app_config_apply_brightness(current_theme->text_color, gb)
                    : app_config_apply_brightness(current_theme->label_color, gb);
                lv_obj_set_style_bg_color(indicator_dots[i], lv_color_hex(dot_color), 0);
            }
        }
    }
}
