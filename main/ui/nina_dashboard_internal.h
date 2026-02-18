#pragma once

/**
 * @file nina_dashboard_internal.h
 * @brief Shared internal types and state for the dashboard module.
 *
 * Only included by nina_dashboard.c, nina_dashboard_update.c, and nina_thumbnail.c.
 */

#include "lvgl.h"
#include "nina_client.h"
#include "app_config.h"
#include "themes.h"
#include "ui_helpers.h"

/* Layout constants */
#define SCREEN_SIZE     720
#define OUTER_PADDING   16
#define GRID_GAP        16
#define BENTO_RADIUS    24

#define MAX_POWER_WIDGETS 8

typedef struct {
    lv_obj_t *page;

    // Header
    lv_obj_t *header_box;
    lv_obj_t *lbl_instance_name;
    lv_obj_t *lbl_target_name;

    // Exposure Arc
    lv_obj_t *arc_exposure;
    lv_obj_t *lbl_exposure_current;
    lv_obj_t *lbl_exposure_total;
    lv_obj_t *lbl_loop_count;

    // Sequence Info
    lv_obj_t *lbl_seq_container;
    lv_obj_t *lbl_seq_step;

    // Data Labels
    lv_obj_t *lbl_rms_value;
    lv_obj_t *lbl_rms_ra_value;
    lv_obj_t *lbl_rms_dec_value;
    lv_obj_t *lbl_hfr_value;
    lv_obj_t *lbl_stars_header;
    lv_obj_t *lbl_stars_value;
    lv_obj_t *lbl_target_time_header;
    lv_obj_t *lbl_target_time_value;
    lv_obj_t *lbl_rms_title;
    lv_obj_t *lbl_hfr_title;
    lv_obj_t *lbl_flip_title;
    lv_obj_t *lbl_flip_value;

    // Power Row
    lv_obj_t *box_pwr[MAX_POWER_WIDGETS];
    lv_obj_t *lbl_pwr_title[MAX_POWER_WIDGETS];
    lv_obj_t *lbl_pwr_value[MAX_POWER_WIDGETS];

    // Arc animation state
    int prev_target_progress;
    int pending_arc_progress;
    bool arc_completing;
} dashboard_page_t;

/* Shared state — defined in nina_dashboard.c, used by update and thumbnail modules */
extern dashboard_page_t pages[MAX_NINA_INSTANCES];
extern int page_count;
extern int active_page;
extern const theme_t *current_theme;

/* Thumbnail overlay state — defined in nina_thumbnail.c */
extern lv_obj_t *thumbnail_overlay;

/* Helper to extract hostname from a URL */
void extract_host_from_url(const char *url, char *out, size_t out_size);
