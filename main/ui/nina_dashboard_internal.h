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
#include "display_defs.h"
#include "themes.h"
#include "ui_helpers.h"

/* ── Page index constants ──
 *
 * Page index convention:
 *   PAGE_IDX_ALLSKY         (0)  = AllSky page
 *   PAGE_IDX_SPOTIFY        (1)  = Spotify page
 *   PAGE_IDX_CLOCK          (2)  = Clock page (always present)
 *   PAGE_IDX_IMAGE_DISPLAY  (3)  = Image Display page
 *   PAGE_IDX_SUMMARY        (4)  = Summary page
 *   NINA_PAGE_OFFSET        (5)  .. NINA_PAGE_OFFSET + page_count - 1 = NINA instance pages
 *   page_count + NINA_PAGE_OFFSET     = settings page
 *   page_count + NINA_PAGE_OFFSET + 1 = sysinfo page
 *   total_page_count = page_count + EXTRA_PAGES
 */
#define PAGE_IDX_ALLSKY          0
#define PAGE_IDX_SPOTIFY         1
#define PAGE_IDX_CLOCK           2
#define PAGE_IDX_IMAGE_DISPLAY   3
#define PAGE_IDX_SUMMARY         4
#define NINA_PAGE_OFFSET         5   /* First NINA page index */
#define EXTRA_PAGES              7   /* allsky + spotify + clock + image_display + summary + settings + sysinfo */

/* Derived page index helpers (use these instead of hardcoded arithmetic) */
#define SETTINGS_PAGE_IDX(pc)  ((pc) + NINA_PAGE_OFFSET)
#define SYSINFO_PAGE_IDX(pc)   ((pc) + NINA_PAGE_OFFSET + 1)

/* Layout constants */
#define OUTER_PADDING   16
#define GRID_GAP        16
#define BENTO_RADIUS    24

#define MAX_POWER_WIDGETS 8

#define ARC_RANGE           3600
#define ARC_TIMER_MS        200
#define ARC_TRANSITION_MS   300
#define ARC_GAP_GRACE_S     60

void arc_interp_timer_cb(lv_timer_t *timer);

typedef struct {
    lv_obj_t *page;

    // Header
    lv_obj_t *header_box;
    lv_obj_t *instance_name_glow;
    lv_obj_t *lbl_instance_name;
    lv_obj_t *lbl_target_name;

    // Exposure Arc
    lv_obj_t *arc_exposure;
    lv_obj_t *lbl_exposure_current;
    lv_obj_t *lbl_exposure_total;
    lv_obj_t *lbl_loop_count;
    lv_obj_t *lbl_filter_done_header;  // "Done:" header
    lv_obj_t *lbl_filter_done_value;   // "11 / 0h 55m" count + integration time
    lv_obj_t *row_filter_total;        // Container for done header + value

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

    // Safety icon (inside exposure box, bottom-left)
    lv_obj_t *safety_icon;

    // Stale data indicator
    lv_obj_t *lbl_stale;        // "Last update: Xs ago" floating label
    lv_obj_t *stale_overlay;    // Semi-transparent dimming overlay (> 2 min stale)

    // Arc animation state
    int64_t  cached_end_epoch;      // Last-known exposure_end_epoch from poll data
    float    cached_total;          // Last-known exposure_total from poll data
    lv_timer_t *arc_timer;          // 200ms LVGL timer for real-time arc progress
    int64_t  gap_start_epoch;       // When the inter-exposure gap began (0 = not in gap)
    bool     arc_completing;        // True during completion/transition animation
    bool     cached_is_exposing;    // Last-known is_exposing state from poll data
    char     prev_filter[32];       // Track previous filter for change detection

    // Connection state (tracked for theme reapplication)
    bool nina_connected;

    // Smooth RMS/HFR value interpolation state (value × 100 as int32_t)
    int32_t anim_rms_total_x100;
    int32_t anim_rms_ra_x100;
    int32_t anim_rms_dec_x100;
    int32_t anim_hfr_x100;
} dashboard_page_t;

/* AllSky page — defined in nina_dashboard.c */
extern lv_obj_t *allsky_obj;

/* Spotify page — defined in nina_dashboard.c */
extern lv_obj_t *spotify_obj;

/* Clock page — defined in nina_dashboard.c */
extern lv_obj_t *clock_obj;

/* Image Display page — defined in nina_dashboard.c */
extern lv_obj_t *image_display_obj;

/* Shared state — defined in nina_dashboard.c, used by update and thumbnail modules */
extern dashboard_page_t pages[MAX_NINA_INSTANCES];
extern int page_count;        /* Number of NINA instance pages (only enabled instances) */
extern int total_page_count;  /* page_count + EXTRA_PAGES (allsky + spotify + summary + settings + sysinfo) */
extern int active_page;
extern const theme_t *current_theme;

/* Page-to-instance mapping: pages[i] corresponds to instance page_instance_map[i] */
extern int page_instance_map[MAX_NINA_INSTANCES];

/* Thumbnail overlay state — defined in nina_thumbnail.c */
extern lv_obj_t *thumbnail_overlay;

/* Helper to extract hostname from a URL */
void extract_host_from_url(const char *url, char *out, size_t out_size);
