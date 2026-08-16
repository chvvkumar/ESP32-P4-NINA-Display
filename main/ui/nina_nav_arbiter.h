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
    NAV_SRC_HOME_LOCK,  /* home_page_lock rung won (appended v49+; keep last) */
} nav_source_t;

/** One-time init. Call from app_main after the dashboard is created. */
void nav_arbiter_init(void);

/** USER claim: manual navigation (swipe, BOOT, web goto, summary tap, Home
 *  Page change) to @p abs_page at @p now_ms. Wakes the data task so resolve()
 *  runs on the next loop iteration. Does NOT itself move the page; resolve()
 *  does. */
void nav_arbiter_submit_user(int abs_page, int64_t now_ms);

/** Raise the topology-rebuild flag (instance enable/disable, URL, demo, mode
 *  toggles). The next resolve() consumes it and rebuilds affected slots. */
void nav_arbiter_notify_topology_changed(void);

/** Modal surface (Settings or any detail overlay) opened. Freezes the arbiter. */
void nav_arbiter_notify_modal_open(void);

/** Modal surface closed. Restamps the grace window so the page does not jump. */
void nav_arbiter_notify_modal_close(int64_t now_ms);

/** Advance the slideshow index on the interval timer (records an edge). */
void nav_arbiter_notify_slideshow_tick(void);

/* A page's content finished loading (image frame committed / first image
 * pass landed). If page_idx is the page currently shown and no content-ready
 * has been counted since it was shown, restart its dwell: request a
 * slideshow-interval restart (consumed by tasks.c) and, if a USER grace window
 * is currently running for that page, restamp it. At most once per page visit,
 * so periodic refreshes cannot hold a page forever. Any task; no locks. */
void nav_arbiter_notify_content_ready(int page_idx);
/* tasks.c: returns true once per requested restart; caller resets last_rotate_ms. */
bool nav_arbiter_take_dwell_restart(void);

/** Re-resolve the ladder once and commit if the desired page differs from the
 *  current page. Called once per data_update_task cycle and on user/event wake.
 *  Must be called WITHOUT the LVGL lock held; the arbiter takes it internally
 *  around the commit. now_ms is the caller's monotonic millisecond clock. */
void nav_arbiter_resolve(int64_t now_ms);

/** Set or clear the in-memory navigation pin. While pinned, the arbiter holds
 *  the USER-selected page and skips slideshow/session/idle/default with no grace
 *  expiry. RUNTIME ONLY: resets to off on reboot.
 *  on=true:  pin on. If abs_page >= 0 the pin holds that page; else it holds
 *            the current USER page (or the last committed page).
 *  on=false: pin off, grace restamped so the page holds nav_grace_s before the
 *            ladder resumes.
 *  Either way the data task is woken so resolve() runs promptly. now_ms is the
 *  caller's monotonic millisecond clock. */
void nav_arbiter_set_pin(bool on, int abs_page, int64_t now_ms);

/** True if the navigation pin is currently engaged. */
bool nav_arbiter_is_pinned(void);

/** Snapshot of the arbiter's last resolve outcome, for the web API. */
typedef struct {
    nav_source_t level;             /* winning rung at the last resolve
                                     * (NAV_SRC_HOLD while a modal is open) */
    int          grace_remaining_s; /* seconds left in the USER grace window,
                                     * 0 when no grace claim is active (the
                                     * runtime pin has no expiry and reports 0) */
} nav_arbiter_web_status_t;

/** Fill @p out with the last resolve outcome. Safe to call from any task
 *  (httpd included): reads only the arbiter's atomic fields. */
void nav_arbiter_get_web_status(nav_arbiter_web_status_t *out);

#ifdef __cplusplus
}
#endif
