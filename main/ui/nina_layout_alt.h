#pragma once

/**
 * @file nina_layout_alt.h
 * @brief Alternate NINA page layout (Image-forward).
 *
 * Selected per instance by app_config_t::nina_layout[i]:
 *   0 = arc dashboard (nina_dashboard.c, unchanged)
 *   1 = Image-forward (nina_layout_image.c)
 *
 * The spine (nina_dashboard.c / nina_dashboard_update.c) owns the dispatch, the
 * page root object, the stale indicator, the stale overlay, the disconnected
 * empty state and the 200 ms exposure clock. A layout module builds and updates
 * its own widgets inside the page root and nothing else.
 *
 * All entry points run with the LVGL display lock already held by the caller —
 * never call lvgl_port_lock() inside them.
 */

#include "lvgl.h"
#include "nina_dashboard_internal.h"
#include "nina_client.h"

/* ── Layout 1 — Image-forward ─────────────────────────────────────────────── */

/**
 * @brief Build the Image-forward widget tree.
 * @param p           Page state; store widget pointers in p->alt
 * @param parent      The page root created by the spine (p->page)
 * @param page_index  NINA instance index, 0..MAX_NINA_INSTANCES-1
 */
void nina_layout_image_create(dashboard_page_t *p, lv_obj_t *parent, int page_index);

/** @brief Push one poll's data into the Image-forward widgets. */
void nina_layout_image_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb);

/** @brief Re-theme the Image-forward widgets in place. */
void nina_layout_image_apply_theme(dashboard_page_t *p);

/**
 * @brief Hand a decoded capture to the Image-forward background.
 *
 * Takes ownership of @p rgb565 (PSRAM, released with free()). No-op plus an
 * immediate free if that instance is not on layout 1. LVGL lock held by caller.
 */
void nina_layout_image_set_capture(int instance, uint8_t *rgb565,
                                   uint32_t w, uint32_t h, uint32_t size);

/** @brief Called when the instance's page stops being visible — frees its capture. */
void nina_layout_image_release_capture(int instance);

/**
 * @brief True while a layout-1 page has no capture and none has been asked for.
 *
 * Lets the poll loop queue the existing FETCH_THUMBNAIL request on page entry
 * instead of waiting for the next new_image_available event. Pure read: it does
 * NOT latch, so the caller must call nina_layout_image_note_capture_request()
 * once the request is actually enqueued, or the same request is re-issued every
 * cycle. Touches no LVGL object, so the display lock is not required.
 */
bool nina_layout_image_needs_capture(int instance);

/**
 * @brief Latch (or clear) the "a capture was requested" flag for one instance.
 *
 * Set true at the point the FETCH_THUMBNAIL request is enqueued; set false when
 * that fetch fails, so the next event gets one honest retry. A successful
 * nina_layout_image_set_capture() and nina_layout_image_release_capture() clear
 * it themselves. LVGL lock held by caller (same writer set as set_capture()).
 */
void nina_layout_image_note_capture_request(int instance, bool asked);

/* -- Layout 0 -- Dashboard, round family only ----------------------------- */

/**
 * @brief Build the radial Dashboard widget tree (round family only).
 *
 * Defined in nina_layout_dashboard_round.c, which is compiled only when
 * CONFIG_NINA_FAMILY_ROUND is set. Fills the same dashboard_page_t handles the
 * square grid fills where the content matches, plus the shape handles; every
 * handle it leaves NULL is null-checked by nina_dashboard_update.c.
 *
 * @param p           Page state
 * @param parent      The full-panel page root created by the spine (p->page)
 * @param page_index  NINA instance index, 0..MAX_NINA_INSTANCES-1
 */
void nina_layout_dashboard_round_create(dashboard_page_t *p, lv_obj_t *parent,
                                        int page_index);

/* ── Shared ───────────────────────────────────────────────────────────────── */

/**
 * @brief Re-theme in place for layout 1.
 *
 * Implemented by the spine (nina_dashboard.c); dispatches on p->layout to the
 * per-layout apply_theme above. Called from apply_theme_to_page().
 */
void nina_layout_alt_apply_theme(dashboard_page_t *p);

/**
 * @brief Overlay tap targets a layout can attach to one of its widgets.
 *
 * The overlay callbacks are static inside nina_dashboard.c; this enum plus
 * nina_dashboard_bind_tap() is the only way a layout module reaches them.
 */
typedef enum {
    NINA_TAP_CAPTURE,       /* full-screen capture preview */
    NINA_TAP_SEQUENCE,      /* sequence detail overlay */
    NINA_TAP_RMS,           /* RMS history graph */
    NINA_TAP_FLIP,          /* mount overlay */
    NINA_TAP_SESSION,       /* session stats / time limit */
    NINA_TAP_FILTER,        /* filter detail */
    NINA_TAP_HFR,           /* HFR graph; long press opens the autofocus curve */
    NINA_TAP_EXPOSURE,      /* camera + weather overlay (the square arc box) */
} nina_tap_target_t;

/**
 * @brief Make @p obj clickable and route its CLICKED event to an overlay.
 *
 * Required bindings per the design handoff:
 *   Image-forward: capture background -> CAPTURE, sequence row -> SEQUENCE,
 *                  RMS vital -> RMS, flip vital -> FLIP, limit vital -> SESSION
 */
void nina_dashboard_bind_tap(lv_obj_t *obj, nina_tap_target_t which);
