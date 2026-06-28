#pragma once

/**
 * @file nina_nav_arbiter.h
 * @brief Navigation Arbiter - single owner of page-commit decisions.
 *
 * The arbiter centralizes "what page should be shown" into one resolution
 * ladder.  External sources (user nav, slideshow, topology changes, modal
 * surfaces) submit claims/notifications; nav_arbiter_resolve() runs the
 * ladder once per cycle and commits at most one page change.
 *
 * Task 3.1 defines the public API and the internal claim/mode state with a
 * stubbed resolve().  The full resolution ladder is implemented in Task 3.2.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/** Reason a page was committed (for logging / idle-indicator coupling). */
typedef enum {
    NAV_SRC_BOOT = 0,   /* UNKNOWN/CONNECTING boot window -> Home Page */
    NAV_SRC_USER,       /* manual nav within grace window */
    NAV_SRC_SLIDESHOW,  /* auto-rotate advance */
    NAV_SRC_SESSION,    /* a rig is reachable */
    NAV_SRC_IDLE,       /* idle-override active, all rigs confirmed down */
    NAV_SRC_DEFAULT,    /* Home Page fallback */
    NAV_SRC_HOLD,       /* modal open: no change */
} nav_source_t;

/** One-time init. Call from app_main after the dashboard is created. */
void nav_arbiter_init(void);

/** Record a USER navigation claim (absolute page index + now_ms timestamp).
 *  Stamps the grace window and wakes the data task so resolve() runs on the next
 *  loop iteration. img_src pins the Image Display source (0-3) for an image
 *  target, or -1 for a non-image target (clears any runtime override). Does NOT
 *  itself move the page; resolve() does. */
void nav_arbiter_submit_user(int abs_page, int64_t now_ms, int8_t img_src);

/** Raise the topology-rebuild flag (instance enable/disable, URL, demo, mode
 *  toggles). The next resolve() consumes it and rebuilds affected slots. */
void nav_arbiter_notify_topology_changed(void);

/** Modal surface (Settings or any detail overlay) opened. Freezes the arbiter. */
void nav_arbiter_notify_modal_open(void);

/** Modal surface closed. Restamps the grace window so the page does not jump. */
void nav_arbiter_notify_modal_close(int64_t now_ms);

/** Advance the slideshow index on the interval timer (records an edge). */
void nav_arbiter_notify_slideshow_tick(void);

/** Re-resolve the ladder once and commit if the desired page differs from the
 *  current page. Called once per data_update_task cycle and on user/event wake.
 *  Must be called WITHOUT the LVGL lock held; the arbiter takes it internally
 *  around the commit. now_ms is the caller's monotonic millisecond clock. */
void nav_arbiter_resolve(int64_t now_ms);

/** True while the IDLE claim is the resolved source (for the idle indicator). */
bool nav_arbiter_idle_active(void);

/** Set or clear the in-memory navigation pin. While pinned, the arbiter holds
 *  the USER-selected page and skips slideshow/session/idle/default with no grace
 *  expiry. RUNTIME ONLY: resets to off on reboot.
 *  on=true:  pin on. If abs_page >= 0 the pin holds that page (with img_src as
 *            the Image Display source, 0-3 or -1); otherwise it holds whatever
 *            page is currently shown.
 *  on=false: pin off; the current page is restamped into the USER grace window
 *            so it holds for nav_grace_s before the automatic ladder resumes.
 *  Either way the data task is woken so resolve() runs promptly. now_ms is the
 *  caller's monotonic millisecond clock. */
void nav_arbiter_set_pin(bool on, int abs_page, int8_t img_src, int64_t now_ms);

/** True if the navigation pin is currently engaged. */
bool nav_arbiter_is_pinned(void);

#ifdef __cplusplus
}
#endif
