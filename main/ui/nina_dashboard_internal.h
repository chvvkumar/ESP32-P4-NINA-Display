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
#include "nina_subbar.h"

/* ── Page index constants ──
 *
 * Page index convention:
 *   PAGE_IDX_ALLSKY         (0)  = AllSky page
 *   PAGE_IDX_SPOTIFY        (1)  = Spotify page
 *   PAGE_IDX_CLOCK          (2)  = Clock page (always present)
 *   PAGE_IDX_IMG_GOES       (3)  = GOES Satellite image page
 *   PAGE_IDX_IMG_MOON       (4)  = Moon image page
 *   PAGE_IDX_IMG_SOLAR      (5)  = Solar image page
 *   PAGE_IDX_IMG_CUSTOM     (6)  = Custom Image page
 *   PAGE_IDX_IMG_RADAR      (7)  = Weather Radar image page
 *   PAGE_IDX_IMG_CLOUDS     (8)  = Clouds (GOES GeoColor satellite) image page
 *   PAGE_IDX_JSON           (9)  = JSON Display page
 *   PAGE_IDX_HA             (10) = Home Assistant page
 *   PAGE_IDX_OCTOPRINT      (11) = OctoPrint 3D Printer page
 *   PAGE_IDX_ADSB           (12) = ADS-B aircraft page
 *   PAGE_IDX_SUMMARY        (13) = Summary page
 *   NINA_PAGE_OFFSET        (14) .. NINA_PAGE_OFFSET + page_count - 1 = NINA instance pages
 *   page_count + NINA_PAGE_OFFSET     = settings page  (17)
 *   page_count + NINA_PAGE_OFFSET + 1 = sysinfo page   (18)
 *   total_page_count = page_count + EXTRA_PAGES        (19)
 *
 * These are INTERNAL absolute indices and may shift when an optional page is
 * inserted; nothing persists them. The stable external identity is the
 * page_ref_t id / slug in page_registry.h, which is what NVS and the web API
 * exchange. Every consumer must use the macros below, never a literal.
 *
 * The six image pages are contiguous and ordered as image_src_t
 * (ui/nina_image_page.h): GOES=0, Moon=1, Solar=2, Custom=3, Radar=4, Clouds=5,
 * so the page index encodes the source (PAGE_IDX_TO_IMG_SRC).
 */
#define PAGE_IDX_ALLSKY          0
#define PAGE_IDX_SPOTIFY         1
#define PAGE_IDX_CLOCK           2
#define PAGE_IDX_IMG_GOES        3
#define PAGE_IDX_IMG_MOON        4
#define PAGE_IDX_IMG_SOLAR       5
#define PAGE_IDX_IMG_CUSTOM      6
#define PAGE_IDX_IMG_RADAR       7
#define PAGE_IDX_IMG_CLOUDS      8
#define PAGE_IDX_JSON            9
#define PAGE_IDX_HA              10
#define PAGE_IDX_OCTOPRINT       11
#define PAGE_IDX_ADSB            12
#define PAGE_IDX_SUMMARY         13
#define NINA_PAGE_OFFSET         14  /* first NINA page index */
#define EXTRA_PAGES              16  /* allsky+spotify+clock+6 image+json+ha+octoprint+adsb+summary+settings+sysinfo */

/* Image page helpers: contiguous band [PAGE_IDX_IMG_GOES, PAGE_IDX_IMG_CLOUDS]. */
#define PAGE_IDX_IS_IMAGE(i)     ((i) >= PAGE_IDX_IMG_GOES && (i) <= PAGE_IDX_IMG_CLOUDS)
#define PAGE_IDX_TO_IMG_SRC(i)   ((i) - PAGE_IDX_IMG_GOES)   /* valid only when PAGE_IDX_IS_IMAGE(i) */

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

    /* Page layout for this instance, from app_config_t::nina_layout[i]:
     * 0 = arc dashboard (every field below the `alt` block applies),
     * 1 = Image-forward (nina_layout_alt.h).
     * On layout 1 the arc-path widgets are never created and the
     * arc-path updaters never run — only `subbar`, `alt` and the shared
     * pieces (stale label/overlay, empty_state_cont, exposure clock) exist. */
    uint8_t layout;

    /* Segmented sub-bar hero — layout 1 only. */
    nina_subbar_t subbar;

    /* Layout-specific widget pointers. Owned by the layout module that created
     * the page (nina_layout_image.c); the spine only
     * zeroes it. Add fields here rather than as loose dashboard_page_t members. */
    struct {
        lv_obj_t *lbl_target;       /* target name */

        /* Layout 1 — Image-forward (nina_layout_image.c) */
        lv_obj_t *cap_img;          /* full-bleed capture background */
        lv_obj_t *tile_ident;       /* top text group: identity (two rows) */
        lv_obj_t *tile_hero;        /* bottom group: value row + 12 px block ledge */
        lv_obj_t *row_seq;          /* row 1: identity | shield | step */
        lv_obj_t *lbl_safety;
        lv_obj_t *lbl_seq_step;
        lv_obj_t *row_vals;         /* bottom value row (one 64 px baseline) */
        lv_obj_t *grp_left;         /* RMS + counter, tap = RMS graph */
        lv_obj_t *lbl_rms;          /* TOTAL RMS, hanken 48, threshold colour */
        lv_obj_t *lbl_count;        /* "done / target", hanken 28 */
        lv_obj_t *lbl_elapsed;      /* "247s", hanken 64, written by the tick only */
        lv_obj_t *lbl_filter;       /* filter name, montserrat 24, filter colour */
        int       inst;             /* owning NINA instance index */
        /* end Layout 1 */
    } alt;

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
    int64_t  exp_anchor_us;         // esp_timer_get_time() at exposure-start anchor (0 = no active anchor)
    float    exp_anchor_elapsed;    // Elapsed seconds already done at the anchor moment (seed)
    bool     arc_completing;        // True during completion/transition animation
    bool     cached_is_exposing;    // Last-known is_exposing state from poll data
    char     prev_filter[32];       // Track previous filter for change detection

    // NINA-domain clock pair copied from nina_client_t in update_exposure_arc
    // (caller holds the data lock), read lock-free by arc_interp_timer_cb.
    // cached_nina_epoch == 0 -> unknown, fall back to time(NULL).
    int64_t  cached_nina_epoch;     // NINA-PC UTC epoch at capture (HTTP Date)
    int64_t  cached_nina_mono_us;   // esp_timer_get_time() at capture

    // Connection state (tracked for theme reapplication)
    bool nina_connected;

    // Smooth RMS/HFR value interpolation state (value × 100 as int32_t)
    int32_t anim_rms_total_x100;
    int32_t anim_rms_ra_x100;
    int32_t anim_rms_dec_x100;
    int32_t anim_hfr_x100;

    // Disconnected full-screen branded overlay (IDLE-04, Plan 02).
    // Created at page-create time; shown on disconnect, hidden on reconnect.
    lv_obj_t *empty_state_cont;
} dashboard_page_t;

/* AllSky page — defined in nina_dashboard.c */
extern lv_obj_t *allsky_obj;

/* Spotify page — defined in nina_dashboard.c */
extern lv_obj_t *spotify_obj;

/* Clock page — defined in nina_dashboard.c */
extern lv_obj_t *clock_obj;

/* Shared state — defined in nina_dashboard.c, used by update and thumbnail modules */
extern dashboard_page_t pages[MAX_NINA_INSTANCES];
/* Per-slot availability: slot i corresponds to NINA instance i (fixed identity,
 * no compaction). True iff instance i is enabled AND has a non-empty URL. The
 * backing LVGL page object pages[i].page is allocated only while available. */
extern bool nina_slot_available[MAX_NINA_INSTANCES];
/* Number of currently-available NINA slots (indicator dots + summary only). */
extern int nina_available_count;
extern int page_count;        /* RESERVED NINA band width = MAX_NINA_INSTANCES (constant). Keeps SETTINGS/SYSINFO indices fixed. */
extern int total_page_count;  /* page_count + EXTRA_PAGES (allsky + spotify + summary + settings + sysinfo) */
extern int active_page;
extern const theme_t *current_theme;

/* Thumbnail overlay state — defined in nina_thumbnail.c */
extern lv_obj_t *thumbnail_overlay;

/* Helper to extract hostname from a URL */
void extract_host_from_url(const char *url, char *out, size_t out_size);
