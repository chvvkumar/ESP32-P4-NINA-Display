/**
 * @file nina_nav_arbiter.c
 * @brief Navigation Arbiter - claim/mode state and resolve entry point.
 *
 * Task 3.1: public API setters + internal claim/mode state.
 * Task 3.2: full resolution ladder + single commit path in resolve().
 */

#include "nina_nav_arbiter.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"   /* page-index constants */
#include "page_registry.h"             /* page_ref_resolve / page_ref_t */
#include "app_config.h"
#include "nina_connection.h"
#include "nina_idle_indicator.h"
#include "nina_wait_overlay.h"
#include "nina_image_page.h"   /* image_page_prefetch / has_image / label */
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"   /* nav_arbiter_get_web_status: grace-remaining clock */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tasks.h"   /* data_task_handle */
#include <stdatomic.h>

static const char *TAG = "nav_arb";

static struct {
    _Atomic int32_t user_page;     /* USER claim page (-1 = none); atomic for the
                                    * cross-core submit/resolve pair */
    _Atomic int64_t user_stamp_ms; /* when the USER claim was stamped; atomic to
                                    * close the cross-core torn-read window between
                                    * submit_user (LVGL/web task) and resolve
                                    * (data task) on this 64-bit field */
    bool     topology_dirty;       /* rebuild requested */
    int      modal_depth;          /* >0 = a modal surface is open */
    bool     slideshow_advance;    /* interval timer fired since last resolve */
    bool     auto_rotate_was_enabled; /* slideshow enabled at last resolve (edge
                                       * detect for first-stop image prefetch) */
    bool     idle_claim_active;    /* IDLE was the resolved source last commit */
    int      current_committed;    /* last page the arbiter committed */
    _Atomic bool pinned;           /* in-memory navigation pin: hold the USER page,
                                    * skip the automatic ladder; resets on reboot.
                                    * Atomic: written by the web/LVGL task
                                    * (set_pin), read by the data task (resolve),
                                    * mirroring user_stamp_ms's cross-core guard. */
    _Atomic uint8_t last_level;    /* nav_source_t rung that won the last resolve
                                    * (NAV_SRC_HOLD while modal-frozen). Written
                                    * by resolve() (data task), read by
                                    * nav_arbiter_get_web_status() (httpd task). */
    _Atomic bool content_ready_armed; /* set on commit, consumed by the first
                                       * notify_content_ready for that page:
                                       * one dwell restart per visit */
    _Atomic bool dwell_restart;    /* restart requested; drained by tasks.c via
                                    * nav_arbiter_take_dwell_restart */
} s_arb;

void nav_arbiter_init(void) {
    s_arb.user_page = -1;
    s_arb.user_stamp_ms = 0;
    s_arb.topology_dirty = false;
    s_arb.modal_depth = 0;
    s_arb.slideshow_advance = false;
    s_arb.auto_rotate_was_enabled = false;
    s_arb.idle_claim_active = false;
    s_arb.pinned = false;
    s_arb.last_level = (uint8_t)NAV_SRC_DEFAULT;
    s_arb.content_ready_armed = false;
    s_arb.dwell_restart = false;
    s_arb.current_committed = nina_dashboard_get_active_page();
    ESP_LOGI(TAG, "nav arbiter init (committed page=%d)", s_arb.current_committed);
}

void nav_arbiter_submit_user(int abs_page, int64_t now_ms) {
    s_arb.user_page = abs_page;
    s_arb.user_stamp_ms = now_ms;
    /* Wake the data task so the arbiter resolves this claim on the next loop
     * iteration instead of waiting up to a full update_rate_s cycle. */
    if (data_task_handle) {
        xTaskNotifyGive(data_task_handle);
    }
}

void nav_arbiter_notify_topology_changed(void) { s_arb.topology_dirty = true; }

void nav_arbiter_notify_modal_open(void)  { s_arb.modal_depth++; }

void nav_arbiter_notify_modal_close(int64_t now_ms) {
    if (s_arb.modal_depth > 0) s_arb.modal_depth--;
    s_arb.user_stamp_ms = now_ms;     /* restamp grace on close */
    if (s_arb.user_page < 0) s_arb.user_page = nina_dashboard_get_active_page();
}

void nav_arbiter_notify_slideshow_tick(void) { s_arb.slideshow_advance = true; }

void nav_arbiter_notify_content_ready(int page_idx) {
    if (page_idx != s_arb.current_committed) return;
    if (!atomic_exchange(&s_arb.content_ready_armed, false)) return;
    atomic_store(&s_arb.dwell_restart, true);
    /* Restamp grace only while a USER window is still running for this page;
     * restamping an expired window would re-pin the page. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_arb.user_page == page_idx
        && (now_ms - s_arb.user_stamp_ms) < (int64_t)app_config_get()->nav_grace_s * 1000) {
        s_arb.user_stamp_ms = now_ms;
    }
}

bool nav_arbiter_take_dwell_restart(void) {
    return atomic_exchange(&s_arb.dwell_restart, false);
}

void nav_arbiter_set_pin(bool on, int abs_page, int64_t now_ms) {
    if (on) {
        s_arb.pinned = true;
        if (abs_page >= 0) {
            s_arb.user_page = abs_page;
        } else if (s_arb.user_page < 0) {
            s_arb.user_page = s_arb.current_committed;
        }
    } else {
        s_arb.pinned = false;
        /* Restamp grace so the current page holds for nav_grace_s before the
         * automatic ladder resumes (no jarring instant jump). */
        s_arb.user_stamp_ms = now_ms;
    }
    /* Wake the data task so the arbiter re-resolves promptly (mirrors
     * nav_arbiter_submit_user's wake). */
    if (data_task_handle) {
        xTaskNotifyGive(data_task_handle);
    }
}

bool nav_arbiter_is_pinned(void) { return s_arb.pinned; }

void nav_arbiter_get_web_status(nav_arbiter_web_status_t *out)
{
    if (!out) return;
    out->level = (nav_source_t)s_arb.last_level;
    out->grace_remaining_s = 0;
    /* Grace remaining: recomputed here from the atomic claim fields rather than
     * stored at resolve time, so pollers see it count down between resolves.
     * The pin holds without expiry, so it reports 0. */
    int user_page = s_arb.user_page;
    if (user_page >= 0 && !s_arb.pinned) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t left_ms = (int64_t)app_config_get()->nav_grace_s * 1000
                        - (now_ms - s_arb.user_stamp_ms);
        if (left_ms > 0) {
            out->grace_remaining_s = (int)((left_ms + 999) / 1000);
        }
    }
}

/* ── Ladder helpers (Task 3.2) ──
 *
 * Each rung is a small static helper consumed by nav_arbiter_resolve().
 * No helper takes the LVGL lock; resolve() is the single commit path and
 * takes the lock only around the committed page transition.
 */

/** Home Page: the configured active_page_override (a page_ref id) resolved to
 *  an absolute page index. Never Settings, never out of range, never
 *  unavailable: any of those fall back to Summary. */
static int home_page(void) {
    page_ref_t id = (page_ref_t)app_config_get()->active_page_override;
    int idx = -1;
    if (!page_ref_resolve(id, &idx)) return PAGE_IDX_SUMMARY;
    if (idx == SETTINGS_PAGE_IDX(page_count)) return PAGE_IDX_SUMMARY;
    return idx;
}

/** SESSION target: pinned rig online -> that rig; else lone online rig -> it;
 *  else 2+ online -> Summary; else -1 (SESSION does not claim). */
static int session_target(void) {
    /* Count only instances that are BOTH slot-available AND connected, matching
     * the pinned/lone scans below. nina_connection_connected_count() ignores
     * slot availability, so a stale-CONNECTED disabled instance could otherwise
     * inflate the count and force a single online rig to resolve to Summary. */
    int online = 0;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (nina_dashboard_slot_available(i) && nina_connection_is_connected(i)) online++;
    }
    if (online <= 0) return -1;                     /* SESSION does not claim */
    /* active_page_override now persists a page_ref id (NINA1/2/3 = 1/2/3), not a
     * raw page index. Map the id DIRECTLY to an instance: page_ref_resolve() would
     * gate on availability and suppress a valid pin for a momentarily-offline rig,
     * whereas the old code derived the index regardless of online state (the
     * online/available guard below is the sole gate). Use the direct-id mapping
     * PAGE_REF_NINA1..NINA3 -> instance 0..2; any non-NINA home id yields a
     * pin_inst outside [0, MAX_NINA_INSTANCES) and is rejected by the guard,
     * falling through to the lone-online / Summary logic exactly as before. */
    page_ref_t home_id = (page_ref_t)app_config_get()->active_page_override;
    int pin_inst = (int)home_id - (int)PAGE_REF_NINA1;
    /* Pinned rig: Home Page is a NINA page whose instance is online */
    if (pin_inst >= 0 && pin_inst < MAX_NINA_INSTANCES
        && nina_dashboard_slot_available(pin_inst)
        && nina_connection_is_connected(pin_inst)) {
        return NINA_PAGE_OFFSET + pin_inst;
    }
    if (online == 1) {
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            if (nina_dashboard_slot_available(i) && nina_connection_is_connected(i))
                return NINA_PAGE_OFFSET + i;
        }
    }
    return PAGE_IDX_SUMMARY;                          /* 2+ online */
}

/** IDLE predicate: idle override enabled AND every available rig is CONFIRMED
 *  DISCONNECTED (not merely not-connected; UNKNOWN/CONNECTING does not count). */
static bool idle_condition(void) {
    const app_config_t *c = app_config_get();
    if (!c->idle_page_override_enabled) return false;
    bool any_enabled = false;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (!nina_dashboard_slot_available(i)) continue;
        any_enabled = true;
        if (nina_connection_get_state(i) != NINA_CONN_DISCONNECTED) return false;
    }
    return any_enabled;   /* all available rigs CONFIRMED down */
}

/** Resolve the configured idle target (idle_page_override_target, a page_ref id)
 *  to an absolute page index. Falls back to Summary when the id is unknown or
 *  its page is unavailable: page_ref_resolve() enforces availability and returns
 *  false in that case, preserving the old idle-target Summary fallback. */
static int idle_target_resolve(void) {
    page_ref_t id = (page_ref_t)app_config_get()->idle_page_override_target;
    int idx = -1;
    if (!page_ref_resolve(id, &idx)) return PAGE_IDX_SUMMARY;
    return idx;
}

/** Ordered slideshow candidate list from auto_rotate_order2[0..23]: each stop
 *  id resolved to an absolute page; unavailable pages and (when configured)
 *  disconnected NINA pages are skipped. Pure. */
static int slideshow_build_candidates(int cand_out[ARP_ORDER_CAPACITY]) {
    const app_config_t *c = app_config_get();
    int n = 0;
    for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
        uint8_t bit = c->auto_rotate_order2[i];
        if (bit == 0xFF || !ARP_STOP_IS_VALID(bit)) continue;
        int p = -1;
        if (!page_ref_resolve((page_ref_t)bit, &p)) continue;
        int inst = p - NINA_PAGE_OFFSET;
        if (inst >= 0 && inst < MAX_NINA_INSTANCES
            && c->auto_rotate_skip_disconnected
            && !nina_connection_is_connected(inst)) continue;
        cand_out[n++] = p;
    }
    return n;
}

/** First AVAILABLE image stop in the slideshow order (absolute page index), or
 *  -1 if the order has none. Used to warm its poller when the slideshow starts. */
static int first_image_stop_in_order(void) {
    int cand[ARP_ORDER_CAPACITY];
    int n = slideshow_build_candidates(cand);
    for (int i = 0; i < n; i++) {
        if (PAGE_IDX_IS_IMAGE(cand[i])) return cand[i];
    }
    return -1;
}

/** The stop that follows @p from_page in the candidate order (Home Page if
 *  none available). Pure, so it serves both the real advance and the lookahead. */
static int slideshow_advance_from(int from_page) {
    int cand[ARP_ORDER_CAPACITY];
    int n = slideshow_build_candidates(cand);
    if (n == 0) return home_page();
    int cur = -1;
    for (int i = 0; i < n; i++) {
        if (cand[i] == from_page) { cur = i; break; }
    }
    return cand[(cur + 1) % n];
}

static int slideshow_next(void) {
    return slideshow_advance_from(s_arb.current_committed);
}

void nav_arbiter_resolve(int64_t now_ms) {
    const app_config_t *c = app_config_get();

    /* Slideshow on/off edge: warm the first image stop's poller the moment the
     * slideshow turns on (so the first image stop is never cold), and release
     * any warm frame when it turns off. Runs before the modal early-return so
     * the edge is never missed. Warming touches no LVGL. */
    bool auto_rotate_now = c->auto_rotate_enabled;
    if (auto_rotate_now != s_arb.auto_rotate_was_enabled) {
        image_page_prefetch(auto_rotate_now ? first_image_stop_in_order() : -1);
    }
    s_arb.auto_rotate_was_enabled = auto_rotate_now;

    /* Rung 0: modal freeze. Closing a modal restamps grace in
     * nav_arbiter_notify_modal_close, so the next resolve holds the page. */
    if (s_arb.modal_depth > 0) {
        s_arb.last_level = (uint8_t)NAV_SRC_HOLD;
        return;
    }

    /* Topology rebuild consumed before resolution. */
    if (s_arb.topology_dirty) {
        s_arb.topology_dirty = false;
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            for (int i = 0; i < MAX_NINA_INSTANCES; i++) nina_dashboard_rebuild_slot(i);
            bsp_display_unlock();
        }
    }

    int desired;
    nav_source_t src;

    int user_page = s_arb.user_page;
    bool user_active = (user_page >= 0)
        && ((now_ms - s_arb.user_stamp_ms) < (int64_t)c->nav_grace_s * 1000);

    /* Tie-break: auto-rotate wins if both flags are somehow set. */
    bool auto_rotate = c->auto_rotate_enabled;

    if (s_arb.pinned) {
        /* PIN rung: hold the USER selection with no grace expiry; slideshow,
         * session, idle, and default are all skipped. Manual navigation while
         * pinned updates s_arb.user_page via nav_arbiter_submit_user(), so the
         * held page follows manual nav and persists until the pin is cleared. */
        if (user_page < 0) {
            user_page = s_arb.current_committed;
            s_arb.user_page = user_page;
        }
        desired = user_page;
        src = NAV_SRC_USER;
    } else if (user_active) {
        desired = user_page;
        src = NAV_SRC_USER;
    } else if (c->home_page_lock) {
        /* HOME LOCK rung: persisted always-show-Home-Page. Outranks slideshow,
         * session, idle, and default; yields to modal freeze, the runtime pin,
         * and the USER grace window above. */
        desired = home_page();
        src = NAV_SRC_HOME_LOCK;
    } else if (auto_rotate) {
        if (s_arb.slideshow_advance) {
            s_arb.slideshow_advance = false;
            desired = slideshow_next();
        } else {
            desired = s_arb.current_committed;   /* hold between intervals */
        }
        src = NAV_SRC_SLIDESHOW;
    } else {
        int st = session_target();
        if (st >= 0) {
            desired = st;
            src = NAV_SRC_SESSION;
        } else if (idle_condition()) {
            desired = idle_target_resolve();
            src = NAV_SRC_IDLE;
        } else {
            desired = home_page();
            src = NAV_SRC_DEFAULT;
        }
    }

    /* Validate vs Page Model: available and not Settings, else Home Page. */
    if (desired == SETTINGS_PAGE_IDX(page_count)
        || !nina_dashboard_page_is_available(desired)) {
        desired = home_page();
    }

    /* Publish the winning rung for the web API (level state, read atomically
     * by nav_arbiter_get_web_status from the httpd task). */
    s_arb.last_level = (uint8_t)src;

    /* Idle indicator coupling. */
    bool now_idle = (src == NAV_SRC_IDLE);
    if (now_idle != s_arb.idle_claim_active) {
        s_arb.idle_claim_active = now_idle;
        nina_idle_indicator_set_active(now_idle && app_config_get()->idle_indicator_enabled);
    }

    /* Commit only on change. Single path: animated switch always fires the cb.
     * current_committed moves ONLY on a successful lock+switch; a lock timeout
     * leaves it unchanged so the next resolve retries. */
    if (desired != s_arb.current_committed) {
        int effect = (src == NAV_SRC_SLIDESHOW) ? c->auto_rotate_effect : 0;
        /* Slideshow lookahead BEFORE the page switch: warm the NEXT stop's poller
         * now (touches no LVGL, so it is safe outside the lock), skipping the
         * degenerate single-stop case (next == desired) so the visible page is
         * never marked warm and left pinned. A non-image next stop un-warms
         * whatever was warm; retained frames stay resident under the cap. */
        if (src == NAV_SRC_SLIDESHOW) {
            int next = slideshow_advance_from(desired);
            if (next != desired) image_page_prefetch(PAGE_IDX_IS_IMAGE(next) ? next : -1);
        }
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_show_page_animated(desired, 0, effect);
            /* Manual navigation onto an image page that has nothing to show yet
             * (not warm): put the loading overlay up now, before the poller's
             * first pass, so the user never sees a black panel. A warm page shows
             * its frame instantly and needs no overlay; auto-cycle arrivals rely
             * on the warm path and never animate. */
            if (PAGE_IDX_IS_IMAGE(desired) && src == NAV_SRC_USER) {
                image_page_t *ip = image_page_by_page_idx(desired);
                if (ip && !image_page_has_image(ip)) {
                    char lbl[48];
                    image_page_label(ip, lbl, sizeof(lbl));
                    nina_wait_overlay_set_prior_page(s_arb.current_committed);
                    nina_wait_overlay_show("Loading image...", lbl[0] ? lbl : NULL);
                    nina_wait_overlay_set_progress(-1);
                }
            }
            /* Arm one content-ready dwell restart for this visit. An image page
             * that already shows a frame on arrival (retained/warm) is fully
             * loaded now, so disarm: no restart wanted. has_image is read here
             * while the display lock is still held (module convention). */
            bool loaded_on_arrival = false;
            if (PAGE_IDX_IS_IMAGE(desired)) {
                image_page_t *ip = image_page_by_page_idx(desired);
                loaded_on_arrival = ip && image_page_has_image(ip);
            }
            bsp_display_unlock();
            s_arb.current_committed = desired;
            atomic_store(&s_arb.content_ready_armed, !loaded_on_arrival);
            ESP_LOGI(TAG, "commit page=%d src=%d", desired, (int)src);
        }
    }
}
