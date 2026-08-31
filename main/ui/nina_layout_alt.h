#pragma once

/**
 * @file nina_layout_alt.h
 * @brief Alternate NINA page layouts (the ones that draw over a capture).
 *
 * Selected per instance by app_config_t::nina_layout[i]:
 *   0 = arc dashboard (nina_dashboard.c, unchanged)
 *   1 = Image-forward (nina_layout_image.c / nina_layout_image_round.c)
 *   2 = Halo      (round family only, nina_layout_halo_round.c)
 *   4 = Orbit     (round family only, nina_layout_orbit_round.c)
 *
 * Id 3 is RETIRED (it was Meridian) and is never reused. Ids are global across
 * the families; layout_for_family() in nina_dashboard.c resolves an id this
 * binary cannot draw to 0 without rewriting the stored value, so moving a board
 * between a round and a square panel gives the user their choice back.
 *
 * The spine (nina_dashboard.c / nina_dashboard_update.c) owns the dispatch, the
 * page root object, the stale indicator, the stale overlay, the disconnected
 * empty state, the retained capture and the 200 ms exposure clock. A layout
 * module builds and updates its own widgets inside the page root and nothing
 * else.
 *
 * All entry points run with the LVGL display lock already held by the caller —
 * never call lvgl_port_lock() inside them.
 */

#include "lvgl.h"
#include "nina_dashboard_internal.h"
#include "nina_client.h"

/* ── View modes ───────────────────────────────────────────────────────────── */

/**
 * @brief The four compositions a capture layout cycles through on a tap.
 *
 * FULL:    picture, the crown, the exposure rim arc and the readings plate.
 * ARC:     picture, the crown and the rim arc; no plate and no readings.
 * PICTURE: the picture and nothing else.
 * NUMBERS: readings only. The picture is drawn fully transparent (never
 *          hidden, so it keeps carrying the tap), the crown stays, and the
 *          LAYOUT's own readings page is what shows.
 *
 * A short tap on the picture steps FULL, ARC, PICTURE, NUMBERS and round again;
 * a long press opens the full-screen preview, which fetches its own uncropped
 * frame.
 *
 * The mode is per instance, lives in RAM only (nina_dashboard.c), survives a
 * slot rebuild and resets to FULL on boot. It is never written to NVS.
 *
 * The EFFECTIVE mode is NUMBERS whenever that instance holds no capture, so a
 * page shows its readings composition before the first picture arrives and
 * after the capture is released on leave, and the tap does nothing there.
 */
typedef enum {
    NINA_VIEW_FULL    = 0,
    NINA_VIEW_ARC     = 1,
    NINA_VIEW_PICTURE = 2,
    NINA_VIEW_NUMBERS = 3,
} nina_view_mode_t;

/**
 * @brief Recompute the effective view mode for one instance and apply it.
 *
 * Call sites: after a page is created, when a capture is attached or released,
 * and from the page-background tap that cycles the mode. No-op for a slot with
 * no page or a layout that draws no capture. LVGL lock held by caller.
 */
void nina_dashboard_refresh_view(int instance);

/**
 * @brief Dispatch a view mode to the shared overlay and the page's layout.
 *
 * Implemented by the spine (nina_dashboard.c). The shared round overlay is told
 * first, then the layout, so the layout can react to what the overlay is
 * showing. Each set_view only toggles LV_OBJ_FLAG_HIDDEN on objects built at
 * create time and re-aligns what moves between compositions: no widget is
 * created or deleted inside one, and every one of them is idempotent.
 */
void nina_layout_alt_set_view(dashboard_page_t *p, nina_view_mode_t mode);

/* ── Capture ──────────────────────────────────────────────────────────────── */

/** @brief How a layout wants the retained capture placed on the panel. */
typedef enum {
    NINA_CAPTURE_FIT_CONTAIN = 0,  /* whole frame, letterboxed */
    NINA_CAPTURE_FIT_COVER   = 1,  /* centre square, filling the panel */
} nina_capture_fit_t;

/** @brief True for every layout that draws the retained capture: 1, 2 and 4,
 *  plus 0 on the round family, where the Dashboard is a picture layout too. */
bool nina_layout_uses_capture(uint8_t layout);

/** @brief True while that instance holds a decoded capture. */
bool nina_layout_image_has_capture(int instance);

/**
 * @brief The fit @p layout wants for the capture.
 *
 * COVER is applied ONCE, in nina_layout_image_set_capture(), as a single PPA
 * SRM job; it is never an LVGL cover align, which would rescale on every
 * redraw of a full-refresh panel. Both round capture layouts ask for COVER so
 * the picture fills the disc; the full frame is still one long press away.
 */
nina_capture_fit_t nina_layout_capture_fit(uint8_t layout);

/* ── Layout 1 — Image-forward ─────────────────────────────────────────────── */

/**
 * @brief Build the Image-forward widget tree.
 *
 * This entry point and the two below it are DEFINED PER FAMILY: the square
 * bodies live in nina_layout_image.c behind #if !CONFIG_NINA_FAMILY_ROUND, the
 * round ones (radial board 2) in nina_layout_image_round.c, which only the
 * round build compiles (nina_round_srcs). There is no runtime dispatch; the
 * linker takes the family's definitions. The retained-capture store and the
 * three hooks at the end of this section stay in nina_layout_image.c, which is
 * compiled on both families.
 *
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

/** @brief Show the Image-forward composition for @p mode. */
void nina_layout_image_set_view(dashboard_page_t *p, nina_view_mode_t mode);

/**
 * @brief Hand a decoded capture to the page background of a capture layout.
 *
 * Serves EVERY layout for which nina_layout_uses_capture() is true, not just
 * layout 1; for an instance on any other layout this is a no-op plus an
 * immediate free. Takes ownership of @p rgb565 (PSRAM, released with free()).
 *
 * When nina_layout_capture_fit() asks for COVER, the centre square is cropped
 * and scaled to the panel here, once, in a single PPA SRM pass, and the widget
 * is centred over the result; a refused PPA job keeps the original frame and
 * degrades that buffer to CONTAIN. The fit each buffer was prepared for is
 * recorded with it, so a later re-attach restores the same placement.
 *
 * LVGL lock held by caller.
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

/**
 * @brief Drop a retained capture that was remapped for the other Red Night state.
 *
 * Both family builders call this from their apply_theme; the capture store and
 * the remap rule stay in nina_layout_image.c, which is compiled in both.
 * LVGL lock held by caller.
 */
void nina_layout_image_note_theme_switch(int instance);

/**
 * @brief Re-attach a still-retained capture after a page rebuild.
 *
 * A rebuild that kept the frame (theme or URL edit rather than a page leave)
 * shows it again instead of an empty background. No-op with no retained frame.
 * LVGL lock held by caller.
 */
void nina_layout_image_reattach_capture(int instance);

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

/**
 * @brief Show the round Dashboard composition for @p mode (round family only).
 *
 * The round Dashboard is a picture board like Halo and Orbit: the spine creates
 * p->alt.cap_img before its create(), the shared overlay draws the crown, the
 * rim exposure arc and the readings plate over it, and this call decides what
 * the BOARD ITSELF shows on top of that. Same contract as every other set_view:
 * it only toggles LV_OBJ_FLAG_HIDDEN on objects built at create time and
 * re-aligns what moves, it creates and deletes nothing, and it is idempotent.
 *
 * NUMBERS is the board's own readings page and is the only mode where its full
 * widget tree belongs on screen; every other mode is a picture with the shared
 * overlay over it, so the board's own tiles and text are hidden there. The
 * board keeps its own rim exposure ring in every mode, which is why the overlay
 * drops its rim arc for layout 0 in NUMBERS.
 *
 * LVGL lock held by the caller.
 */
void nina_layout_dashboard_round_set_view(dashboard_page_t *p, nina_view_mode_t mode);

/* -- Layouts 2 and 4 -- Halo and Orbit, round family only ------------------ */

/**
 * @brief The four entry points every round capture layout defines.
 *
 * Same contract as the Image-forward ones above: create() builds the widget
 * tree inside the full-panel page root and stores its handles in p->alt,
 * update() pushes one poll's data in, apply_theme() re-tones in place and
 * set_view() only hides and shows what it already built.
 *
 * These files are in nina_round_srcs, so they exist only in the round binary;
 * the spine's dispatch for layouts 2 and 4 is inside CONFIG_NINA_FAMILY_ROUND
 * and the square build can never reach a stored value above 1 anyway
 * (layout_for_family() resolves it to the Dashboard at page-build time without
 * rewriting what is stored, so a panel swap restores the user's choice).
 *
 * WHAT THESE TWO DO NOT OWN. The spine creates p->alt.cap_img for them before
 * calling create(), and nina_round_overlay.c draws the crown, the rim exposure
 * arc and the readings plate over the picture for both of them. A layout builds
 * only its READINGS-ONLY page (the NUMBERS composition) plus any ring of its
 * own, keeps the top 60 px of the vertical axis clear for the crown, and its
 * set_view() shows its objects in NUMBERS and hides them in every other mode.
 *
 * A layout that draws its own exposure seconds registers p->alt.elapsed_hook,
 * NOT p->alt.elapsed_cb: the overlay owns elapsed_cb and calls the hook after
 * writing its own digits, so both heroes come from one writer. A readings-only
 * exposure ring goes in p->alt.arc_progress_num (range 0..1000), which the
 * spine drives and dims exactly like p->alt.arc_progress; leaving it NULL means
 * the readings page keeps the overlay's rim arc instead.
 */
void nina_layout_halo_create(dashboard_page_t *p, lv_obj_t *parent, int page_index);
void nina_layout_halo_update(dashboard_page_t *p, const nina_client_t *d,
                             int instance_idx, int gb);
void nina_layout_halo_apply_theme(dashboard_page_t *p);
void nina_layout_halo_set_view(dashboard_page_t *p, nina_view_mode_t mode);

void nina_layout_orbit_create(dashboard_page_t *p, lv_obj_t *parent, int page_index);
void nina_layout_orbit_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb);
void nina_layout_orbit_apply_theme(dashboard_page_t *p);
void nina_layout_orbit_set_view(dashboard_page_t *p, nina_view_mode_t mode);

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
    NINA_TAP_VIEW_CYCLE,    /* page background: cycle nina_view_mode_t */
    NINA_TAP_CAPTURE_LONG,  /* page background, LONG PRESS: capture preview */
} nina_tap_target_t;

/**
 * @brief Make @p obj clickable and route its CLICKED event to an overlay.
 *
 * Required bindings per the design notes:
 *   Image-forward: capture background -> CAPTURE, sequence row -> SEQUENCE,
 *                  RMS vital -> RMS, flip vital -> FLIP, limit vital -> SESSION
 *
 * NINA_TAP_VIEW_CYCLE routes SHORT_CLICKED (not CLICKED) so the same object can
 * also carry NINA_TAP_CAPTURE_LONG: LVGL raises CLICKED on the release of a
 * long press too, which would cycle the view behind the preview it just opened.
 * The round capture layouts bind both on their page background.
 */
void nina_dashboard_bind_tap(lv_obj_t *obj, nina_tap_target_t which);
