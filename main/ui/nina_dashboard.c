/**
 * @file nina_dashboard.c
 * @brief "Bento" NINA Dashboard - High-density grid layout for 720x720 displays
 * @version 3.0 (Bento Design System)
 */

#include "nina_dashboard.h"
#include "nina_client.h"
#include "app_config.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

/*********************
 * DESIGN CONSTANTS
 *********************/
#define SCREEN_SIZE     720
#define OUTER_PADDING   16
#define GRID_GAP        16
#define BENTO_RADIUS    24

// Color Palette
#define COLOR_BG_MAIN   lv_color_hex(0x0a0a0a)  // Near Black
#define COLOR_BENTO_BG  lv_color_hex(0x111111)  // Very Dark Gray
#define COLOR_BENTO_BORDER lv_color_hex(0x222222)
#define COLOR_LABEL     lv_color_hex(0x6b7280)  // Gray labels
#define COLOR_TEXT      lv_color_hex(0xFFFFFF)  // White values
#define COLOR_BLUE      lv_color_hex(0x60a5fa)  // Instance/Target
#define COLOR_BLUE_DARK lv_color_hex(0x1e3a8a)  // Header gradient
#define COLOR_AMBER     lv_color_hex(0xfbbf24)  // Filter
#define COLOR_PROGRESS  lv_color_hex(0x3b82f6)  // Arc indicator
#define COLOR_ROSE      lv_color_hex(0xf43f5e)  // RMS
#define COLOR_EMERALD   lv_color_hex(0x10b981)  // HFR
#define COLOR_YELLOW    lv_color_hex(0xeab308)  // Star icon
#define COLOR_PURPLE    lv_color_hex(0xa855f7)  // Saturated icon

/*********************
 * UI OBJECTS
 *********************/
static lv_obj_t * scr_dashboard = NULL;

// Header Elements
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
static lv_obj_t * lbl_stars_value;
static lv_obj_t * lbl_saturated_value;

/*********************
 * STYLES
 *********************/
static lv_style_t style_bento_box;
static lv_style_t style_label_small;   // Uppercase labels
static lv_style_t style_value_large;   // Large data values
static lv_style_t style_header_gradient;

/**
 * @brief Initialize all styles for the Bento Dashboard
 */
static void init_styles(void) {
    // Bento Box Style
    lv_style_init(&style_bento_box);
    lv_style_set_bg_color(&style_bento_box, COLOR_BENTO_BG);
    lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_radius(&style_bento_box, BENTO_RADIUS);
    lv_style_set_border_width(&style_bento_box, 1);
    lv_style_set_border_color(&style_bento_box, COLOR_BENTO_BORDER);
    lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_pad_all(&style_bento_box, 20);

    // Small Label Style (Uppercase, Bold, Gray)
    lv_style_init(&style_label_small);
    lv_style_set_text_color(&style_label_small, COLOR_LABEL);
    lv_style_set_text_font(&style_label_small, &lv_font_montserrat_16);
    lv_style_set_text_letter_space(&style_label_small, 1);

    // Large Value Style
    lv_style_init(&style_value_large);
    lv_style_set_text_color(&style_value_large, COLOR_TEXT);
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
    lv_style_init(&style_header_gradient);
    lv_style_set_bg_color(&style_header_gradient, COLOR_BLUE_DARK);
    lv_style_set_bg_grad_color(&style_header_gradient, lv_color_hex(0x000000));
    lv_style_set_bg_grad_dir(&style_header_gradient, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&style_header_gradient, LV_OPA_30);
    lv_style_set_radius(&style_header_gradient, BENTO_RADIUS);
    lv_style_set_border_width(&style_header_gradient, 1);
    lv_style_set_border_color(&style_header_gradient, COLOR_BLUE_DARK);
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


/**
 * @brief Create the Bento NINA Dashboard
 * Layout: 2 columns x 4 rows, 720x720px with 24px padding and 16px gaps
 */
void create_nina_dashboard(lv_obj_t * parent) {
    init_styles();
    scr_dashboard = parent;
    
    // Main Container - 720x720 with outer padding
    lv_obj_t * main_cont = lv_obj_create(scr_dashboard);
    lv_obj_remove_style_all(main_cont);
    lv_obj_set_size(main_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(main_cont, COLOR_BG_MAIN, 0);
    lv_obj_set_style_bg_opa(main_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(main_cont, OUTER_PADDING, 0);
    lv_obj_set_style_pad_gap(main_cont, GRID_GAP, 0);

    // Setup Grid: 2 Equal Columns x 5 Rows
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {
        LV_GRID_FR(2),  // Row 0 - Header (telescope + target stacked)
        LV_GRID_FR(1),  // Row 1 - Sequence Info (container + step)
        LV_GRID_FR(3),  // Row 2
        LV_GRID_FR(3),  // Row 3
        LV_GRID_FR(2),  // Row 4 - Bottom row
        LV_GRID_TEMPLATE_LAST
    };
    lv_obj_set_layout(main_cont, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(main_cont, col_dsc, row_dsc);

    /* ═══════════════════════════════════════════════════════════
     * HEADER (Row 0, Span 2 Columns)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * header = create_bento_box(main_cont);
    lv_obj_set_grid_cell(header, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(header, 12, 0);

    // Top: Telescope Name
    lbl_instance_name = lv_label_create(header);
    lv_obj_set_style_text_color(lbl_instance_name, COLOR_BLUE, 0);
    lv_obj_set_style_text_font(lbl_instance_name, &lv_font_montserrat_26, 0);
    lv_label_set_text(lbl_instance_name, "N.I.N.A.");

    // Bottom: Target Name (right-aligned)
    lbl_target_name = lv_label_create(header);
    lv_obj_add_style(lbl_target_name, &style_value_large, 0);
    lv_obj_set_style_text_color(lbl_target_name, COLOR_TEXT, 0);
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
    lv_obj_set_style_text_color(lbl_seq_container, COLOR_BLUE, 0);
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
    lv_obj_set_style_text_color(lbl_seq_step, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl_seq_step, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl_seq_step, "----");

    /* ═══════════════════════════════════════════════════════════
     * EXPOSURE DISPLAY (Col 0, Rows 2-3, Span 2 Rows)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_exposure = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_exposure, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 2);
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
    lv_obj_set_style_arc_color(arc_exposure, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_exposure, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_exposure, COLOR_PROGRESS, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_exposure, 12, LV_PART_INDICATOR);
    
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
    lv_obj_set_style_text_color(lbl_exposure_total, COLOR_AMBER, 0);
    lv_obj_set_style_text_font(lbl_exposure_total, &lv_font_montserrat_28, 0);
    lv_obj_set_style_pad_top(lbl_exposure_total, 14, 0);

    // Loop Count (moved into circle)
    lbl_loop_count = lv_label_create(arc_center);
    lv_obj_set_style_text_color(lbl_loop_count, COLOR_LABEL, 0);
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

    create_small_label(box_rms, "GUIDING RMS");

    // Total RMS (main value)
    lbl_rms_value = create_value_label(box_rms);
    lv_obj_set_style_text_color(lbl_rms_value, COLOR_ROSE, 0);
    lv_label_set_text(lbl_rms_value, "--.--\"");

    // RA and DEC RMS (smaller values below)
    // lv_obj_t * rms_details = lv_obj_create(box_rms);
    // lv_obj_remove_style_all(rms_details);
    // lv_obj_set_size(rms_details, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    // lv_obj_set_flex_flow(rms_details, LV_FLEX_FLOW_ROW);
    // lv_obj_set_flex_align(rms_details, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // lv_obj_set_style_pad_column(rms_details, 16, 0);

    // // RA container
    // lv_obj_t * ra_cont = lv_obj_create(rms_details);
    // lv_obj_remove_style_all(ra_cont);
    // lv_obj_set_size(ra_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    // lv_obj_set_flex_flow(ra_cont, LV_FLEX_FLOW_COLUMN);
    // lv_obj_set_flex_align(ra_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // create_small_label(ra_cont, "RA");
    // lbl_rms_ra_value = lv_label_create(ra_cont);
    // lv_obj_set_style_text_color(lbl_rms_ra_value, COLOR_ROSE, 0);
    // lv_obj_set_style_text_font(lbl_rms_ra_value, &lv_font_montserrat_24, 0);
    // lv_label_set_text(lbl_rms_ra_value, "0.25\"");

    // // DEC container
    // lv_obj_t * dec_cont = lv_obj_create(rms_details);
    // lv_obj_remove_style_all(dec_cont);
    // lv_obj_set_size(dec_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    // lv_obj_set_flex_flow(dec_cont, LV_FLEX_FLOW_COLUMN);
    // lv_obj_set_flex_align(dec_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // create_small_label(dec_cont, "DEC");
    // lbl_rms_dec_value = lv_label_create(dec_cont);
    // lv_obj_set_style_text_color(lbl_rms_dec_value, COLOR_ROSE, 0);
    // lv_obj_set_style_text_font(lbl_rms_dec_value, &lv_font_montserrat_24, 0);
    // lv_label_set_text(lbl_rms_dec_value, "0.28\"");

    /* ═══════════════════════════════════════════════════════════
     * HFR / SHARPNESS (Col 1, Row 3)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_hfr = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_hfr, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_set_flex_flow(box_hfr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_hfr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    create_small_label(box_hfr, "HFR");
    
    lbl_hfr_value = create_value_label(box_hfr);
    lv_obj_set_style_text_color(lbl_hfr_value, COLOR_EMERALD, 0);
    lv_label_set_text(lbl_hfr_value, "2.15");

    /* ═══════════════════════════════════════════════════════════
     * STAR COUNT (Col 0, Row 4)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_stars = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_stars, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 4, 1);
    lv_obj_set_flex_flow(box_stars, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_stars, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Label with Star Icon (Unicode star)
    lv_obj_t * lbl_stars_header = create_small_label(box_stars, "STARS");
    lv_obj_set_style_text_color(lbl_stars_header, COLOR_YELLOW, 0);
    
    lbl_stars_value = create_value_label(box_stars);
    lv_label_set_text(lbl_stars_value, "2451");

    /* ═══════════════════════════════════════════════════════════
     * SATURATED PIXELS (Col 1, Row 4)
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t * box_saturated = create_bento_box(main_cont);
    lv_obj_set_grid_cell(box_saturated, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 4, 1);
    lv_obj_set_flex_flow(box_saturated, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_saturated, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Label with Lightning Icon (Unicode lightning bolt)
    lv_obj_t * lbl_sat_header = create_small_label(box_saturated, "SATURATED PIXELS");
    lv_obj_set_style_text_color(lbl_sat_header, COLOR_PURPLE, 0);
    
    lbl_saturated_value = create_value_label(box_saturated);
    lv_label_set_text(lbl_saturated_value, "84");
}


/**
 * @brief Update the Bento Dashboard with live data from NINA client
 */
void update_nina_dashboard_ui(const nina_client_t *data) {
    if (!scr_dashboard || !data) return;

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
    uint32_t filter_color = 0x3b82f6;  // Default color (blue)
    if (data->current_filter[0] != '\0' && strcmp(data->current_filter, "--") != 0) {
        filter_color = app_config_get_filter_color(data->current_filter);
        lv_obj_set_style_arc_color(arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);
    }

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
        
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, arc_exposure);
        lv_anim_set_values(&a, lv_arc_get_value(arc_exposure), progress);
        lv_anim_set_time(&a, 250);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
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
        lv_obj_set_style_text_color(lbl_rms_value, COLOR_LABEL, 0);
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
        lv_obj_set_style_text_color(lbl_hfr_value, COLOR_LABEL, 0);
    }

    // 8. Star Count
    if (data->stars >= 0) {
        lv_label_set_text_fmt(lbl_stars_value, "%d", data->stars);
    } else {
        lv_label_set_text(lbl_stars_value, "--");
    }

    // 9. Saturated Pixels
    if (data->saturated_pixels >= 0) {
        lv_label_set_text_fmt(lbl_saturated_value, "%d", data->saturated_pixels);
    } else {
        lv_label_set_text(lbl_saturated_value, "--");
    }
}