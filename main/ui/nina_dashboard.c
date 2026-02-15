/**
 * @file nina_dashboard.c
 * @brief "Bento" NINA Dashboard - High-density grid layout for 720x720 displays
 * @version 3.0 (Bento Design System)
 */

#include "nina_dashboard.h"
#include "nina_client.h"
#include "app_config.h"
#include "lvgl.h"
#include "themes.h"
#include <stdio.h>
#include <string.h>

/*********************
 * DESIGN CONSTANTS
 *********************/
#define SCREEN_SIZE     720
#define OUTER_PADDING   16
#define GRID_GAP        16
#define BENTO_RADIUS    24

/*********************
 * UI OBJECTS
 *********************/
static lv_obj_t * scr_dashboard = NULL;
static lv_obj_t * main_cont = NULL;

// Header Elements
static lv_obj_t * header_box;
static lv_obj_t * lbl_instance_name;
static lv_obj_t * lbl_target_name;

// Exposure Arc/Circle
static lv_obj_t * arc_exposure;
static lv_obj_t * lbl_exposure_current;
static lv_obj_t * lbl_exposure_total;
static lv_obj_t * lbl_loop_count;

// Sequence Info
static lv_obj_t * lbl_seq_container;
static lv_obj_t * lbl_seq_step;

// Data Display Labels
static lv_obj_t * lbl_rms_value;
static lv_obj_t * lbl_rms_ra_value;
static lv_obj_t * lbl_rms_dec_value;
static lv_obj_t * lbl_hfr_value;
static lv_obj_t * lbl_stars_header;
static lv_obj_t * lbl_stars_value;
static lv_obj_t * lbl_sat_header;
static lv_obj_t * lbl_saturated_value;
static lv_obj_t * lbl_rms_title;
static lv_obj_t * lbl_hfr_title;
static lv_obj_t * lbl_flip_title;
static lv_obj_t * lbl_flip_value;

static const theme_t *current_theme = NULL;

// Last computed progress value (from data, not from arc widget) to detect resets
static int prev_target_progress = 0;
// Pending arc progress to animate to after the "fill to 100%" animation completes
static int pending_arc_progress = 0;
// True while the "fill to 100%" + "reset to 0" animation sequence is playing
static bool arc_completing = false;

/*********************
 * STYLES
 *********************/
static lv_style_t style_bento_box;
static lv_style_t style_label_small;   // Uppercase labels
static lv_style_t style_value_large;   // Large data values
static lv_style_t style_header_gradient;

/**
 * @brief Initialize or update all styles for the Bento Dashboard based on current theme
 */
static void update_styles(void) {
    if (!current_theme) return;

    int gb = app_config_get()->color_brightness;

    // Bento Box Style
    lv_style_reset(&style_bento_box);
    lv_style_init(&style_bento_box);
    lv_style_set_bg_color(&style_bento_box, lv_color_hex(current_theme->bento_bg));
    lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_radius(&style_bento_box, BENTO_RADIUS);
    lv_style_set_border_width(&style_bento_box, 1);
    lv_style_set_border_color(&style_bento_box, lv_color_hex(current_theme->bento_bg));
    lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_pad_all(&style_bento_box, 20);

    // Small Label Style (Uppercase, Bold, Gray)
    lv_style_reset(&style_label_small);
    lv_style_init(&style_label_small);
    lv_style_set_text_color(&style_label_small, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)));
    lv_style_set_text_font(&style_label_small, &lv_font_montserrat_16);
    lv_style_set_text_letter_space(&style_label_small, 1);

    // Large Value Style
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

    // Header Gradient Style
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

/**
 * @brief Helper to create a standard Bento Box container
 */
static lv_obj_t* create_bento_box(lv_obj_t * parent) {
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &style_bento_box, 0);
    return box;
}

/**
 * @brief Helper to create a label with small uppercase style
 */
static lv_obj_t* create_small_label(lv_obj_t * parent, const char * text) {
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_add_style(label, &style_label_small, 0);
    lv_label_set_text(label, text);
    return label;
}

/**
 * @brief Helper to create a large value label
 */
static lv_obj_t* create_value_label(lv_obj_t * parent) {
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_add_style(label, &style_value_large, 0);
    lv_label_set_text(label, "--");
    return label;
}

void nina_dashboard_apply_theme(int theme_index) {
    current_theme = themes_get(theme_index);
    update_styles();

    if (!scr_dashboard) return;
    
    int gb = app_config_get()->color_brightness;

    // Update Main Container BG
    if (main_cont) {
        lv_obj_set_style_bg_color(main_cont, lv_color_hex(current_theme->bg_main), 0);
    }

    // Refresh Objects using standard styles
    // Since we updated the styles using lv_style_reset/init, we might need to notify LVGL
    lv_obj_report_style_change(&style_bento_box);
    lv_obj_report_style_change(&style_label_small);
    lv_obj_report_style_change(&style_value_large);
    lv_obj_report_style_change(&style_header_gradient);

    // Update specific colors that aren't covered by shared styles
    if (lbl_instance_name) lv_obj_set_style_text_color(lbl_instance_name, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    if (lbl_target_name) lv_obj_set_style_text_color(lbl_target_name, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0); // Is large value
    
    if (lbl_seq_container) lv_obj_set_style_text_color(lbl_seq_container, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    if (lbl_seq_step) lv_obj_set_style_text_color(lbl_seq_step, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    if (arc_exposure) {
         // Background of arc often needs to be darker than theme bg
         lv_obj_set_style_arc_color(arc_exposure, lv_color_hex(current_theme->bg_main), LV_PART_MAIN);
         lv_obj_set_style_arc_color(arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
         lv_obj_set_style_shadow_color(arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    }
    
    if (lbl_exposure_total) lv_obj_set_style_text_color(lbl_exposure_total, lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);
    if (lbl_loop_count) lv_obj_set_style_text_color(lbl_loop_count, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    
    // Widget title colors (all use text_color, same as Target text)
    if (lbl_rms_title) lv_obj_set_style_text_color(lbl_rms_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (lbl_hfr_title) lv_obj_set_style_text_color(lbl_hfr_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (lbl_flip_title) lv_obj_set_style_text_color(lbl_flip_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (lbl_stars_header) lv_obj_set_style_text_color(lbl_stars_header, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (lbl_sat_header) lv_obj_set_style_text_color(lbl_sat_header, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    // Semantic value colors
    if (lbl_rms_value) lv_obj_set_style_text_color(lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->rms_color, gb)), 0);
    if (lbl_hfr_value) lv_obj_set_style_text_color(lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->hfr_color, gb)), 0);
    
    lv_obj_invalidate(scr_dashboard);
}


/**
 * @brief Create the Bento NINA Dashboard
 * Layout: 2 columns x 4 rows, 720x720px with 24px padding and 16px gaps
 */
void create_nina_dashboard(lv_obj_t * parent) {
    // Initial theme load
    app_config_t *cfg = app_config_get();
    current_theme = themes_get(cfg->theme_index);
    update_styles();

    scr_dashboard = parent;
    
    // Main Container - 720x720 with outer padding
    main_cont = lv_obj_create(scr_dashboard);
    lv_obj_remove_style_all(main_cont);
    lv_obj_set_size(main_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(main_cont, lv_color_hex(current_theme->bg_main), 0);
    lv_obj_set_style_bg_opa(main_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(main_cont, OUTER_PADDING, 0);
    lv_obj_set_style_pad_gap(main_cont, GRID_GAP, 0);

    // Setup Grid: 2 Equal Columns x 6 Rows
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {
        LV_GRID_FR(2),  // Row 0 - Header (telescope + target stacked)
        LV_GRID_FR(1),  // Row 1 - Sequence Info (container + step)
        LV_GRID_FR(2),  // Row 2 - RMS / Exposure arc top
        LV_GRID_FR(2),  // Row 3 - HFR / Exposure arc mid
        LV_GRID_FR(2),  // Row 4 - Time to Flip / Exposure arc bottom
        LV_GRID_FR(2),  // Row 5 - Bottom row (Stars / Saturated)
        LV_GRID_TEMPLATE_LAST
    };
    lv_obj_set_layout(main_cont, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(main_cont, col_dsc, row_dsc);

    /* ═══════════════════════════════════════════════════════════
     * HEADER (Row 0, Span 2 Columns)
     * ═══════════════════════════════════════════════════════════ */
    header_box = create_bento_box(main_cont);
    lv_obj_set_grid_cell(header_box, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_flex_flow(header_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(header_box, 12, 0);

    // Top: Telescope Name
    lbl_instance_name = lv_label_create(header_box);
    lv_obj_set_style_text_color(lbl_instance_name, lv_color_hex(current_theme->header_text_color), 0);
    lv_obj_set_style_text_font(lbl_instance_name, &lv_font_montserrat_26, 0);
    lv_label_set_text(lbl_instance_name, "N.I.N.A.");

    // Bottom: Target Name (right-aligned)
    lbl_target_name = lv_label_create(header_box);
    lv_obj_add_style(lbl_target_name, &style_value_large, 0);
    // lv_obj_set_style_text_color(lbl_target_name, COLOR_TEXT, 0); // Handled by style
    lv_obj_set_width(lbl_target_name, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_target_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lbl_target_name, "----");

    /* ═══════════════════════════════════════════════════════════
     * SEQUENCE INFO (Row 1, Span 2 Columns)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_seq = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_seq, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_flex_flow(box_seq, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_seq, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box_seq, 12, 0);

    // Left: Container name
    lv_obj_t * seq_left = lv_obj_create(box_seq);
    lv_obj_remove_style_all(seq_left);
    lv_obj_set_size(seq_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_left, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * lbl_seq_title = create_small_label(seq_left, "SEQUENCE");
    lv_obj_set_style_text_font(lbl_seq_title, &lv_font_montserrat_14, 0);

    lbl_seq_container = lv_label_create(seq_left);
    lv_obj_set_style_text_color(lbl_seq_container, lv_color_hex(current_theme->header_text_color), 0);
    lv_obj_set_style_text_font(lbl_seq_container, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl_seq_container, "----");

    // Right: Step name
    lv_obj_t * seq_right = lv_obj_create(box_seq);
    lv_obj_remove_style_all(seq_right);
    lv_obj_set_size(seq_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(seq_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_t * lbl_step_title = create_small_label(seq_right, "STEP");
    lv_obj_set_style_text_font(lbl_step_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl_step_title, LV_TEXT_ALIGN_RIGHT, 0);

    lbl_seq_step = lv_label_create(seq_right);
    lv_obj_set_style_text_color(lbl_seq_step, lv_color_hex(current_theme->text_color), 0);
    lv_obj_set_style_text_font(lbl_seq_step, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl_seq_step, "----");

    /* ═══════════════════════════════════════════════════════════
     * EXPOSURE DISPLAY (Col 0, Rows 2-3, Span 2 Rows)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_exposure = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_exposure, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 3);
    lv_obj_set_flex_flow(box_exposure, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_exposure, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box_exposure, 0, 0);

    // Arc/Circle Widget (fills the full exposure box)
    arc_exposure = lv_arc_create(box_exposure);
    lv_obj_set_size(arc_exposure, 300, 300);
    lv_arc_set_rotation(arc_exposure, 270);
    lv_arc_set_bg_angles(arc_exposure, 0, 360);
    lv_arc_set_value(arc_exposure, 0);
    lv_obj_remove_style(arc_exposure, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc_exposure, lv_color_hex(current_theme->bg_main), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_exposure, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_exposure, 12, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(arc_exposure, 16, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_spread(arc_exposure, 10, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(arc_exposure, LV_OPA_100, LV_PART_INDICATOR);

    // Center Text in Arc
    lv_obj_t * arc_center = lv_obj_create(arc_exposure);
    lv_obj_remove_style_all(arc_center);
    lv_obj_set_size(arc_center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(arc_center);
    lv_obj_set_flex_flow(arc_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(arc_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_exposure_current = lv_label_create(arc_center);
    lv_obj_add_style(lbl_exposure_current, &style_value_large, 0);
    lv_label_set_text(lbl_exposure_current, "----s");

    lbl_exposure_total = create_small_label(arc_center, "Filter");
    lv_obj_set_style_text_color(lbl_exposure_total, lv_color_hex(current_theme->filter_text_color), 0);
    lv_obj_set_style_text_font(lbl_exposure_total, &lv_font_montserrat_28, 0);
    lv_obj_set_style_pad_top(lbl_exposure_total, 14, 0);

    // Loop Count (moved into circle)
    lbl_loop_count = lv_label_create(arc_center);
    lv_obj_set_style_text_color(lbl_loop_count, lv_color_hex(current_theme->label_color), 0);
    lv_obj_set_style_text_font(lbl_loop_count, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_top(lbl_loop_count, 8, 0);
    lv_label_set_text(lbl_loop_count, "-- / --");

    /* ═══════════════════════════════════════════════════════════
     * GUIDING RMS (Col 1, Row 2)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_rms = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_rms, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_set_flex_flow(box_rms, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_rms, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_rms_title = create_small_label(box_rms, "RMS");
    lv_obj_set_width(lbl_rms_title, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_rms_title, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_rms_title, lv_color_hex(current_theme->text_color), 0);

    // Total RMS (main value)
    lbl_rms_value = create_value_label(box_rms);
    lv_obj_set_style_text_color(lbl_rms_value, lv_color_hex(current_theme->rms_color), 0);
    lv_label_set_text(lbl_rms_value, "--.--\"");

    /* ═══════════════════════════════════════════════════════════
     * HFR / SHARPNESS (Col 1, Row 3)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_hfr = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_hfr, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_set_flex_flow(box_hfr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_hfr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_hfr_title = create_small_label(box_hfr, "HFR");
    lv_obj_set_width(lbl_hfr_title, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_hfr_title, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_hfr_title, lv_color_hex(current_theme->text_color), 0);

    lbl_hfr_value = create_value_label(box_hfr);
    lv_obj_set_style_text_color(lbl_hfr_value, lv_color_hex(current_theme->hfr_color), 0);
    lv_label_set_text(lbl_hfr_value, "2.15");

    /* ═══════════════════════════════════════════════════════════
     * TIME TO FLIP (Col 1, Row 4)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_flip = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_flip, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 4, 1);
    lv_obj_set_flex_flow(box_flip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_flip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_flip_title = create_small_label(box_flip, "TIME TO FLIP");
    lv_obj_set_width(lbl_flip_title, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_flip_title, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_flip_title, lv_color_hex(current_theme->text_color), 0);

    lbl_flip_value = create_value_label(box_flip);
    lv_label_set_text(lbl_flip_value, "--");

    /* ═══════════════════════════════════════════════════════════
     * STAR COUNT (Col 0, Row 5)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_stars = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_stars, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 5, 1);
    lv_obj_set_flex_flow(box_stars, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_stars, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_stars_header = create_small_label(box_stars, "STARS");
    lv_obj_set_width(lbl_stars_header, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_stars_header, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_stars_header, lv_color_hex(current_theme->text_color), 0);

    lbl_stars_value = create_value_label(box_stars);
    lv_label_set_text(lbl_stars_value, "2451");

    /* ═══════════════════════════════════════════════════════════
     * SATURATED PIXELS (Col 1, Row 5)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_saturated = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_saturated, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 5, 1);
    lv_obj_set_flex_flow(box_saturated, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_saturated, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_sat_header = create_small_label(box_saturated, "SATURATED PIXELS");
    lv_obj_set_width(lbl_sat_header, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_sat_header, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_sat_header, lv_color_hex(current_theme->text_color), 0);

    lbl_saturated_value = create_value_label(box_saturated);
    lv_label_set_text(lbl_saturated_value, "84");

    // Apply theme again to ensure all brightness adjustments are correct
    nina_dashboard_apply_theme(cfg->theme_index);
}


// Callback: after arc fills to 100%, animate from 0 to the new exposure's progress
static void arc_reset_complete_cb(lv_anim_t *a) {
    arc_completing = false;
}

static void arc_fill_complete_cb(lv_anim_t *a) {
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, arc_exposure);
    lv_anim_set_values(&a2, 0, pending_arc_progress);
    lv_anim_set_time(&a2, 350);
    lv_anim_set_exec_cb(&a2, (lv_anim_exec_xcb_t)lv_arc_set_value);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a2, arc_reset_complete_cb);
    lv_anim_start(&a2);
}

/**
 * @brief Update the Bento Dashboard with live data from NINA client
 */
void update_nina_dashboard_ui(const nina_client_t *data) {
    if (!scr_dashboard || !data) return;

    int gb = app_config_get()->color_brightness;

    // 1. Header - Instance Name (Telescope) & Target Name
    if (data->telescope_name[0] != '\0') {
        lv_label_set_text(lbl_instance_name, data->telescope_name);
    }

    if (data->target_name[0] != '\0') {
        lv_label_set_text(lbl_target_name, data->target_name);
    }

    // 2. Sequence Info - Container name and current step
    if (data->container_name[0] != '\0') {
        lv_label_set_text(lbl_seq_container, data->container_name);
    } else {
        lv_label_set_text(lbl_seq_container, "----");
    }

    if (data->container_step[0] != '\0') {
        lv_label_set_text(lbl_seq_step, data->container_step);
    } else {
        lv_label_set_text(lbl_seq_step, "----");
    }

    // 3. Filter - Update arc color based on current filter
    uint32_t filter_color = app_config_apply_brightness(current_theme->progress_color, gb);  // Default theme color
    if (data->current_filter[0] != '\0' && strcmp(data->current_filter, "--") != 0) {
        filter_color = app_config_get_filter_color(data->current_filter);
    }
    lv_obj_set_style_arc_color(arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);

    // 4. Exposure Progress (Current Exposure Time)
    if (data->exposure_total > 0) {
        float elapsed = data->exposure_current;
        float total = data->exposure_total;
        int total_sec = (int)total;

        // Center label: Always show total exposure length in seconds
        lv_label_set_text_fmt(lbl_exposure_current, "%ds", total_sec);

        // Secondary label: Show filter name with matching color
        if (data->current_filter[0] != '\0' && strcmp(data->current_filter, "--") != 0) {
            lv_label_set_text(lbl_exposure_total, data->current_filter);
            lv_obj_set_style_text_color(lbl_exposure_total, lv_color_hex(filter_color), 0);
        } else {
            lv_label_set_text(lbl_exposure_total, "");
        }

        // Update Arc progress (0-100)
        int progress = (int)((elapsed * 100) / total);
        if (progress > 100) progress = 100;
        if (progress < 0) progress = 0;

        int current_val = lv_arc_get_value(arc_exposure);

        // Detect new exposure: computed progress dropped significantly
        bool new_exposure = (prev_target_progress > 70 && progress < 30);
        prev_target_progress = progress;

        if (new_exposure && current_val > 0) {
            // New exposure started: first animate arc to 100% (complete the circle),
            // then chain a second animation from 0 to new progress via callback.
            arc_completing = true;
            pending_arc_progress = progress;

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, arc_exposure);
            lv_anim_set_values(&a, current_val, 100);
            lv_anim_set_time(&a, 300);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_ready_cb(&a, arc_fill_complete_cb);
            lv_anim_start(&a);
        } else if (arc_completing) {
            // Completion animation still in flight — update the pending target
            // so the reset animation lands on the latest progress value.
            pending_arc_progress = progress;
            // Safety: if progress has advanced well past the start, the animation
            // likely finished but the flag wasn't cleared — force-clear it.
            if (progress > 30) {
                arc_completing = false;
                lv_arc_set_value(arc_exposure, progress);
            }
        } else {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, arc_exposure);
            lv_anim_set_values(&a, current_val, progress);
            lv_anim_set_time(&a, 350);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
    } else {
        lv_label_set_text(lbl_exposure_current, "--");
        lv_label_set_text(lbl_exposure_total, "");
        lv_arc_set_value(arc_exposure, 0);
    }

    // 5. Loop Count (Completed Exposures / Total Exposures for current filter)
    if (data->exposure_iterations > 0) {
        lv_label_set_text_fmt(lbl_loop_count, "%d / %d",
            data->exposure_count, data->exposure_iterations);
    } else {
         lv_label_set_text(lbl_loop_count, "-- / --");
    }

    // 6. Guiding RMS (Total, RA, DEC) - color based on threshold
    if (data->guider.rms_total > 0) {
        lv_label_set_text_fmt(lbl_rms_value, "%.2f\"", data->guider.rms_total);
        uint32_t rms_color = app_config_get_rms_color(data->guider.rms_total);
        lv_obj_set_style_text_color(lbl_rms_value, lv_color_hex(rms_color), 0);
    } else {
        lv_label_set_text(lbl_rms_value, "--");
        lv_obj_set_style_text_color(lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    if (lbl_rms_ra_value) {
        if (data->guider.rms_ra > 0) {
            lv_label_set_text_fmt(lbl_rms_ra_value, "%.2f\"", data->guider.rms_ra);
        } else {
            lv_label_set_text(lbl_rms_ra_value, "--");
        }
    }

    if (lbl_rms_dec_value) {
        if (data->guider.rms_dec > 0) {
            lv_label_set_text_fmt(lbl_rms_dec_value, "%.2f\"", data->guider.rms_dec);
        } else {
            lv_label_set_text(lbl_rms_dec_value, "--");
        }
    }

    // 7. HFR / Sharpness - color based on threshold
    if (data->hfr > 0) {
        lv_label_set_text_fmt(lbl_hfr_value, "%.2f", data->hfr);
        uint32_t hfr_color = app_config_get_hfr_color(data->hfr);
        lv_obj_set_style_text_color(lbl_hfr_value, lv_color_hex(hfr_color), 0);
    } else {
        lv_label_set_text(lbl_hfr_value, "--");
        lv_obj_set_style_text_color(lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    // 8. Time to Meridian Flip
    if (data->meridian_flip[0] != '\0' && strcmp(data->meridian_flip, "--") != 0) {
        lv_label_set_text(lbl_flip_value, data->meridian_flip);
    } else {
        lv_label_set_text(lbl_flip_value, "--");
    }

    // 9. Star Count
    if (data->stars >= 0) {
        lv_label_set_text_fmt(lbl_stars_value, "%d", data->stars);
    } else {
        lv_label_set_text(lbl_stars_value, "--");
    }

    // 10. Saturated Pixels
    if (data->saturated_pixels >= 0) {
        lv_label_set_text_fmt(lbl_saturated_value, "%d", data->saturated_pixels);
    } else {
        lv_label_set_text(lbl_saturated_value, "--");
    }
}
