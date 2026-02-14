#include "nina_dashboard.h"
#include <stdio.h>
#include <time.h>
#include "esp_timer.h"

// =============================================================================
// ORBITAL REDESIGN - High Density, No Scroll, 720x720
// =============================================================================

// Display configuration
#define SCREEN_WIDTH  720
#define SCREEN_HEIGHT 720

// Color scheme - High Contrast / Cyberpunk
#define COLOR_BG           0x050505
#define COLOR_PANEL        0x1A1A1A
#define COLOR_TEXT_MAIN    0xFFFFFF
#define COLOR_TEXT_SUB     0xAAAAAA
#define COLOR_ACCENT_1     0x00E5FF  // Cyan Neon
#define COLOR_ACCENT_2     0xFFD600  // Gold Neon
#define COLOR_STATUS_OK    0x00E676  // Green
#define COLOR_STATUS_WARN  0xFF9100  // Orange
#define COLOR_STATUS_ERR   0xFF1744  // Red

// Globals
static lv_obj_t *scr_main;
static lv_obj_t *tv; // Tileview
static lv_obj_t *tile_summary;
static lv_obj_t *tile_nina2;
static lv_obj_t *tile_nina3;

// Styles
static lv_style_t style_screen;
static lv_style_t style_panel;
static lv_style_t style_card;
static lv_style_t style_text_huge;
static lv_style_t style_text_large;
static lv_style_t style_text_normal;
static lv_style_t style_text_small;

// UI Structure for each instance
typedef struct {
    // Summary Page Elements (Split View)
    lv_obj_t *sum_container;
    lv_obj_t *sum_lbl_profile;
    lv_obj_t *sum_lbl_target;
    lv_obj_t *sum_lbl_status;
    lv_obj_t *sum_lbl_filter;
    lv_obj_t *sum_lbl_rms;
    lv_obj_t *sum_lbl_exp_info; // "Exp 5/10 (120s left)"
    lv_obj_t *sum_bar_progress;

    // Detail Page Elements (Grid)
    lv_obj_t *det_container;
    lv_obj_t *det_lbl_profile;
    lv_obj_t *det_lbl_target;
    lv_obj_t *det_lbl_status;
    
    // Metrics Grid
    lv_obj_t *det_val_rms;
    lv_obj_t *det_val_hfr;
    lv_obj_t *det_val_stars;
    lv_obj_t *det_val_filter;
    lv_obj_t *det_val_exp_num;
    lv_obj_t *det_val_exp_dur;
    lv_obj_t *det_val_temp;     // Temp + Cooler
    lv_obj_t *det_val_sat;      // Saturated Pixels
    lv_obj_t *det_val_meridian; // Flip time

    // Arc for visual progress
    lv_obj_t *det_arc_exp;

} NinaUI;

static NinaUI ui_instances[2];

// =============================================================================
// STYLES
// =============================================================================

static void create_styles(void) {
    lv_style_init(&style_screen);
    lv_style_set_bg_color(&style_screen, lv_color_hex(COLOR_BG));
    lv_style_set_text_color(&style_screen, lv_color_hex(COLOR_TEXT_MAIN));

    lv_style_init(&style_panel);
    lv_style_set_bg_color(&style_panel, lv_color_hex(COLOR_PANEL));
    lv_style_set_radius(&style_panel, 8);
    lv_style_set_border_width(&style_panel, 0);
    lv_style_set_pad_all(&style_panel, 10);

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(0x222222));
    lv_style_set_radius(&style_card, 6);
    lv_style_set_pad_all(&style_card, 5);

    lv_style_init(&style_text_huge);
    lv_style_set_text_font(&style_text_huge, &lv_font_montserrat_26); // Fixed: 32 -> 26
    
    lv_style_init(&style_text_large);
    lv_style_set_text_font(&style_text_large, &lv_font_montserrat_24); 

    lv_style_init(&style_text_normal);
    lv_style_set_text_font(&style_text_normal, &lv_font_montserrat_18); 

    lv_style_init(&style_text_small);
    lv_style_set_text_font(&style_text_small, &lv_font_montserrat_14);
}

// =============================================================================
// UI BUILDER - HELPER
// =============================================================================

static lv_obj_t* create_kv_label(lv_obj_t *parent, const char *key, lv_obj_t **val_obj) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(30), 90); // 3x3 grid approx size
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    *val_obj = lv_label_create(cont);
    lv_label_set_text(*val_obj, "--");
    lv_obj_set_style_text_font(*val_obj, &lv_font_montserrat_20, 0); // Large Value
    lv_obj_set_style_text_color(*val_obj, lv_color_hex(COLOR_TEXT_MAIN), 0);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, key);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0); // Small Label
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT_SUB), 0);

    return cont;
}

// =============================================================================
// UI BUILDER - SUMMARY
// =============================================================================

static void create_summary_section(lv_obj_t *parent, int index) {
    NinaUI *ui = &ui_instances[index];
    uint32_t accent_color = (index == 0) ? COLOR_ACCENT_1 : COLOR_ACCENT_2;

    ui->sum_container = lv_obj_create(parent);
    lv_obj_set_size(ui->sum_container, 700, 340); // Half height minus padding
    lv_obj_add_style(ui->sum_container, &style_panel, 0);
    lv_obj_clear_flag(ui->sum_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_color(ui->sum_container, lv_color_hex(accent_color), 0);
    lv_obj_set_style_border_width(ui->sum_container, 2, 0);
    lv_obj_set_style_border_side(ui->sum_container, LV_BORDER_SIDE_LEFT, 0); // Accent bar on left

    // Profile Name (Title)
    ui->sum_lbl_profile = lv_label_create(ui->sum_container);
    lv_label_set_text_fmt(ui->sum_lbl_profile, "Profile %d", index + 1);
    lv_obj_set_style_text_font(ui->sum_lbl_profile, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui->sum_lbl_profile, lv_color_hex(accent_color), 0);
    lv_obj_align(ui->sum_lbl_profile, LV_ALIGN_TOP_LEFT, 0, 0);

    // Status (Right aligned)
    ui->sum_lbl_status = lv_label_create(ui->sum_container);
    lv_label_set_text(ui->sum_lbl_status, "IDLE");
    lv_obj_set_style_text_font(ui->sum_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(ui->sum_lbl_status, LV_ALIGN_TOP_RIGHT, 0, 5);

    // Target (Big)
    ui->sum_lbl_target = lv_label_create(ui->sum_container);
    lv_label_set_text(ui->sum_lbl_target, "No Target");
    lv_obj_set_style_text_font(ui->sum_lbl_target, &lv_font_montserrat_26, 0); // Fixed: 28 -> 26
    lv_obj_set_width(ui->sum_lbl_target, 650);
    lv_label_set_long_mode(ui->sum_lbl_target, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui->sum_lbl_target, LV_ALIGN_TOP_LEFT, 0, 40);

    // Metrics Row
    lv_obj_t *row = lv_obj_create(ui->sum_container);
    lv_obj_set_size(row, 660, 60);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 90);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Filter
    ui->sum_lbl_filter = lv_label_create(row);
    lv_label_set_text(ui->sum_lbl_filter, "Filter: --");
    lv_obj_set_style_text_font(ui->sum_lbl_filter, &lv_font_montserrat_18, 0);

    // RMS
    ui->sum_lbl_rms = lv_label_create(row);
    lv_label_set_text(ui->sum_lbl_rms, "RMS: --");
    lv_obj_set_style_text_font(ui->sum_lbl_rms, &lv_font_montserrat_18, 0);

    // Progress Bar
    ui->sum_bar_progress = lv_bar_create(ui->sum_container);
    lv_obj_set_size(ui->sum_bar_progress, 660, 15);
    lv_obj_align(ui->sum_bar_progress, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(ui->sum_bar_progress, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->sum_bar_progress, lv_color_hex(accent_color), LV_PART_INDICATOR);

    // Exp Info & Time
    ui->sum_lbl_exp_info = lv_label_create(ui->sum_container);
    lv_label_set_text(ui->sum_lbl_exp_info, "--/-- (--s left)");
    lv_obj_set_style_text_font(ui->sum_lbl_exp_info, &lv_font_montserrat_16, 0);
    lv_obj_align(ui->sum_lbl_exp_info, LV_ALIGN_BOTTOM_LEFT, 0, -5);
}

// =============================================================================
// UI BUILDER - DETAIL
// =============================================================================

static void create_detail_page(lv_obj_t *parent, int index) {
    NinaUI *ui = &ui_instances[index];
    uint32_t accent_color = (index == 0) ? COLOR_ACCENT_1 : COLOR_ACCENT_2;

    ui->det_container = lv_obj_create(parent);
    lv_obj_set_size(ui->det_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(ui->det_container, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_pad_all(ui->det_container, 20, 0);
    lv_obj_clear_flag(ui->det_container, LV_OBJ_FLAG_SCROLLABLE);

    // Header: Profile Name & Status
    lv_obj_t *header = lv_obj_create(ui->det_container);
    lv_obj_set_size(header, 680, 50);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    ui->det_lbl_profile = lv_label_create(header);
    lv_label_set_text(ui->det_lbl_profile, "Profile Name");
    lv_obj_set_style_text_font(ui->det_lbl_profile, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui->det_lbl_profile, lv_color_hex(accent_color), 0);

    ui->det_lbl_status = lv_label_create(header);
    lv_label_set_text(ui->det_lbl_status, "IDLE");
    lv_obj_set_style_text_font(ui->det_lbl_status, &lv_font_montserrat_18, 0);

    // Target Name (Large)
    ui->det_lbl_target = lv_label_create(ui->det_container);
    lv_label_set_text(ui->det_lbl_target, "Target Name");
    lv_obj_set_style_text_font(ui->det_lbl_target, &lv_font_montserrat_26, 0); // Fixed: 36 -> 26
    lv_obj_set_width(ui->det_lbl_target, 680);
    lv_label_set_long_mode(ui->det_lbl_target, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui->det_lbl_target, LV_ALIGN_TOP_MID, 0, 60);

    // Grid Container
    lv_obj_t *grid = lv_obj_create(ui->det_container);
    lv_obj_set_size(grid, 680, 400);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 50); // Shifted down a bit
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    // 3x3 Grid Items
    create_kv_label(grid, "RMS (\")", &ui->det_val_rms);
    create_kv_label(grid, "HFR", &ui->det_val_hfr);
    create_kv_label(grid, "Stars", &ui->det_val_stars);
    
    create_kv_label(grid, "Filter", &ui->det_val_filter);
    create_kv_label(grid, "Exposure #", &ui->det_val_exp_num);
    create_kv_label(grid, "Duration", &ui->det_val_exp_dur);
    
    create_kv_label(grid, "Cam Temp", &ui->det_val_temp);
    create_kv_label(grid, "Saturated", &ui->det_val_sat);
    create_kv_label(grid, "Meridian", &ui->det_val_meridian);

    // Progress Arc (Visual candy)
    ui->det_arc_exp = lv_arc_create(ui->det_container);
    lv_obj_set_size(ui->det_arc_exp, 680, 680);
    lv_obj_align(ui->det_arc_exp, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_range(ui->det_arc_exp, 0, 100); // Explicit range
    lv_arc_set_bg_angles(ui->det_arc_exp, 0, 360);
    lv_arc_set_rotation(ui->det_arc_exp, 270);
    lv_obj_remove_style(ui->det_arc_exp, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ui->det_arc_exp, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->det_arc_exp, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->det_arc_exp, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->det_arc_exp, lv_color_hex(accent_color), LV_PART_INDICATOR);
    lv_obj_move_background(ui->det_arc_exp); // Move to back
}

// =============================================================================
// PUBLIC API
// =============================================================================

void nina_dashboard_init(void) {
    create_styles();

    scr_main = lv_obj_create(NULL);
    lv_obj_add_style(scr_main, &style_screen, 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    tv = lv_tileview_create(scr_main);
    lv_obj_set_style_bg_color(tv, lv_color_hex(COLOR_BG), 0);
    lv_obj_remove_style(tv, NULL, LV_PART_SCROLLBAR);

    // Page 1: Summary (Both instances)
    tile_summary = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_set_flex_flow(tile_summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile_summary, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    create_summary_section(tile_summary, 0);
    create_summary_section(tile_summary, 1);

    // Page 2: NINA 2 Detail
    tile_nina2 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    create_detail_page(tile_nina2, 0);

    // Page 3: NINA 3 Detail
    tile_nina3 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
    create_detail_page(tile_nina3, 1);

    lv_scr_load(scr_main);
}

void nina_dashboard_cycle_view(void) {
    lv_obj_t *current_tile = lv_tileview_get_tile_act(tv);
    if (current_tile == tile_summary) lv_obj_set_tile(tv, tile_nina2, LV_ANIM_ON);
    else if (current_tile == tile_nina2) lv_obj_set_tile(tv, tile_nina3, LV_ANIM_ON);
    else lv_obj_set_tile(tv, tile_summary, LV_ANIM_ON);
}

void nina_dashboard_update(int instance_index, const NinaData *data) {
    if (instance_index < 0 || instance_index > 1 || !data) return;

    NinaUI *ui = &ui_instances[instance_index];
    uint32_t accent_color = (instance_index == 0) ? COLOR_ACCENT_1 : COLOR_ACCENT_2;
    
    // --- SUMMARY UPDATE ---
    lv_label_set_text(ui->sum_lbl_profile, data->profile_name);
    
    if (!data->connected) {
        lv_label_set_text(ui->sum_lbl_status, "DISCONNECTED");
        lv_obj_set_style_text_color(ui->sum_lbl_status, lv_color_hex(COLOR_STATUS_ERR), 0);
        lv_label_set_text(ui->sum_lbl_target, "Offline");
        lv_label_set_text(ui->sum_lbl_filter, "Filter: --");
        lv_label_set_text(ui->sum_lbl_rms, "RMS: --");
        lv_label_set_text(ui->sum_lbl_exp_info, "--");
        lv_bar_set_value(ui->sum_bar_progress, 0, LV_ANIM_OFF);
    } else {
        lv_label_set_text(ui->sum_lbl_status, data->status);
        lv_obj_set_style_text_color(ui->sum_lbl_status, lv_color_hex(data->is_dithering ? COLOR_STATUS_WARN : COLOR_STATUS_OK), 0);
        
        lv_label_set_text(ui->sum_lbl_target, data->target_name);
        lv_label_set_text_fmt(ui->sum_lbl_filter, "Filter: %s", data->current_filter);
        lv_label_set_text_fmt(ui->sum_lbl_rms, "RMS: %.2f\"", data->guide_rms);
        
        // Progress
        float pct = 0;
        if (data->exposure_total > 0) pct = (data->exposure_current / data->exposure_total) * 100.0f;
        lv_bar_set_value(ui->sum_bar_progress, (int32_t)pct, LV_ANIM_ON);
        
        lv_label_set_text_fmt(ui->sum_lbl_exp_info, "Exp %d/%d (%s left)", 
            data->exposure_count, data->exposure_iterations, data->time_remaining);
    }

    // --- DETAIL UPDATE ---
    lv_label_set_text(ui->det_lbl_profile, data->profile_name);
    
    if (!data->connected) {
        lv_label_set_text(ui->det_lbl_status, "OFFLINE");
        lv_obj_set_style_text_color(ui->det_lbl_status, lv_color_hex(COLOR_STATUS_ERR), 0);
        lv_label_set_text(ui->det_lbl_target, "Offline");
        
        lv_label_set_text(ui->det_val_rms, "--");
        lv_label_set_text(ui->det_val_hfr, "--");
        lv_label_set_text(ui->det_val_stars, "--");
        lv_label_set_text(ui->det_val_filter, "--");
        lv_label_set_text(ui->det_val_exp_num, "--");
        lv_label_set_text(ui->det_val_exp_dur, "--");
        lv_label_set_text(ui->det_val_temp, "--");
        lv_label_set_text(ui->det_val_sat, "--");
        lv_label_set_text(ui->det_val_meridian, "--");
        lv_arc_set_value(ui->det_arc_exp, 0);
    } else {
        lv_label_set_text(ui->det_lbl_status, data->status);
        lv_obj_set_style_text_color(ui->det_lbl_status, lv_color_hex(data->is_dithering ? COLOR_STATUS_WARN : accent_color), 0);
        lv_obj_set_style_arc_color(ui->det_arc_exp, lv_color_hex(data->is_dithering ? COLOR_STATUS_WARN : accent_color), LV_PART_INDICATOR);

        lv_label_set_text(ui->det_lbl_target, data->target_name);
        
        // Grid Values
        lv_label_set_text_fmt(ui->det_val_rms, "%.2f", data->guide_rms);
        lv_label_set_text_fmt(ui->det_val_hfr, "%.2f", data->hfr);
        lv_label_set_text_fmt(ui->det_val_stars, "%d", data->stars);
        lv_label_set_text(ui->det_val_filter, data->current_filter);
        lv_label_set_text_fmt(ui->det_val_exp_num, "%d/%d", data->exposure_count, data->exposure_iterations);
        lv_label_set_text_fmt(ui->det_val_exp_dur, "%.0fs/%.0fs", data->exposure_current, data->exposure_total);
        
        // Temp & Power
        lv_label_set_text_fmt(ui->det_val_temp, "%.1fC %d%%", data->cam_temp, data->cooler_power);
        
        // Saturated (Placeholder logic if -1)
        if (data->saturated_pixels < 0) lv_label_set_text(ui->det_val_sat, "N/A");
        else lv_label_set_text_fmt(ui->det_val_sat, "%d", data->saturated_pixels);
        
        lv_label_set_text(ui->det_val_meridian, data->meridian_flip);

        // Arc Progress
        float pct = 0;
        if (data->exposure_total > 0) pct = (data->exposure_current / data->exposure_total) * 100.0f;
        lv_arc_set_value(ui->det_arc_exp, (int32_t)pct);
    }
}