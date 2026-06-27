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
#include "app_config.h"
#include "nina_connection.h"
#include "nina_idle_indicator.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tasks.h"   /* image_source_* override API + goes_task_handle */

static const char *TAG = "nav_arb";

static struct {
    int      user_page;            /* last USER claim target, -1 if none */
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
    int8_t   pending_img_source;       /* image source the next/desired Image Display
                                        * stop wants (0-3), or -1 if not an image stop */
    int8_t   current_committed_img_source; /* image source last committed to the
                                        * Image Display page, or -1 */
} s_arb;

void nav_arbiter_init(void) {
    s_arb.user_page = -1;
    s_arb.user_stamp_ms = 0;
    s_arb.topology_dirty = false;
    s_arb.modal_depth = 0;
    s_arb.slideshow_advance = false;
    s_arb.auto_rotate_was_enabled = false;
    s_arb.idle_claim_active = false;
    s_arb.pending_img_source = -1;
    s_arb.current_committed_img_source = -1;
    s_arb.current_committed = nina_dashboard_get_active_page();
    ESP_LOGI(TAG, "nav arbiter init (committed page=%d)", s_arb.current_committed);
}

void nav_arbiter_submit_user(int abs_page, int64_t now_ms) {
    s_arb.user_page = abs_page;
    s_arb.user_stamp_ms = now_ms;
    /* Manual navigation to the Image Display page shows the persisted default
     * source, not whatever the slideshow last set. Clear the runtime override
     * and forget the last committed slideshow source so the commit block treats
     * this as an image-source change and re-applies the default. */
    if (abs_page == PAGE_IDX_IMAGE_DISPLAY) {
        image_source_set_override(-1);
        s_arb.current_committed_img_source = -1;
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

bool nav_arbiter_idle_active(void) { return s_arb.idle_claim_active; }

/* ── Ladder helpers (Task 3.2) ──
 *
 * Each rung is a small static helper consumed by nav_arbiter_resolve().
 * No helper takes the LVGL lock; resolve() is the single commit path and
 * takes the lock only around the committed page transition.
 */

/** Home Page: the configured active_page_override, validated. Never Settings,
 *  never out of range, never unavailable. -1 (Auto) maps to Summary. This is the
 *  single place that translates the -1 = Auto value. */
static int home_page(void) {
    int hp = app_config_get()->active_page_override;   /* "Home Page" value */
    int total = nina_dashboard_get_total_page_count();
    if (hp < 0) return PAGE_IDX_SUMMARY;               /* -1 = Auto = Summary */
    if (hp >= total) return PAGE_IDX_SUMMARY;
    if (hp == SETTINGS_PAGE_IDX(page_count)) return PAGE_IDX_SUMMARY;
    if (!nina_dashboard_page_is_available(hp)) return PAGE_IDX_SUMMARY;
    return hp;
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
    int hp = app_config_get()->active_page_override;
    /* Pinned rig: Home Page is a NINA page whose instance is online */
    int pin_inst = hp - NINA_PAGE_OFFSET;
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

/** Map the configured idle target (idle_target_t) to an absolute page index,
 *  falling back to Summary when the target page is unavailable. Local copy of
 *  tasks.c's idle_target_to_page_index using slot/page availability. */
static int idle_target_page(void) {
    const app_config_t *c = app_config_get();
    int idx;
    switch (c->idle_page_override_target) {
        case IDLE_TARGET_CLOCK:         idx = PAGE_IDX_CLOCK; break;
        case IDLE_TARGET_ALLSKY:        idx = PAGE_IDX_ALLSKY; break;
        case IDLE_TARGET_SPOTIFY:       idx = PAGE_IDX_SPOTIFY; break;
        case IDLE_TARGET_IMAGE_DISPLAY: idx = PAGE_IDX_IMAGE_DISPLAY; break;
        case IDLE_TARGET_SYSINFO:       idx = SYSINFO_PAGE_IDX(page_count); break;
        case IDLE_TARGET_NINA1:         idx = nina_dashboard_slot_available(0) ? NINA_PAGE_OFFSET+0 : PAGE_IDX_SUMMARY; break;
        case IDLE_TARGET_NINA2:         idx = nina_dashboard_slot_available(1) ? NINA_PAGE_OFFSET+1 : PAGE_IDX_SUMMARY; break;
        case IDLE_TARGET_NINA3:         idx = nina_dashboard_slot_available(2) ? NINA_PAGE_OFFSET+2 : PAGE_IDX_SUMMARY; break;
        default:                        idx = PAGE_IDX_SUMMARY; break;
    }
    if (!nina_dashboard_page_is_available(idx)) idx = PAGE_IDX_SUMMARY;
    return idx;
}

/** Build the ordered slideshow candidate list from auto_rotate_order2[0..15].
 *  Each entry is an ARP_IDX_* bit index mapped to an absolute page; unavailable
 *  pages and (when configured) disconnected NINA pages are skipped. For each
 *  surviving candidate the per-stop image source (0-3 for the four Image Display
 *  sources, else -1) is recorded in src_out[] in lockstep with cand_out[].
 *  Returns the candidate count. Pure: reads config + connection/availability
 *  level state only, writes no arbiter state. */
static int slideshow_build_candidates(int cand_out[ARP_ORDER_CAPACITY],
                                      int8_t src_out[ARP_ORDER_CAPACITY]) {
    const app_config_t *c = app_config_get();
    int n = 0;
    for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
        uint8_t bit = c->auto_rotate_order2[i];
        if (bit == 0xFF || bit >= ARP_IDX_MAX) continue;
        int p = -1;
        int8_t img_src = -1;
        switch (bit) {
            case 0: p = PAGE_IDX_SUMMARY; break;
            case 1: p = nina_dashboard_slot_available(0) ? NINA_PAGE_OFFSET+0 : -1; break;
            case 2: p = nina_dashboard_slot_available(1) ? NINA_PAGE_OFFSET+1 : -1; break;
            case 3: p = nina_dashboard_slot_available(2) ? NINA_PAGE_OFFSET+2 : -1; break;
            case 4: p = SYSINFO_PAGE_IDX(page_count); break;
            case 5: p = c->allsky_enabled ? PAGE_IDX_ALLSKY : -1; break;
            case 6: p = c->spotify_enabled ? PAGE_IDX_SPOTIFY : -1; break;
            case 7: p = PAGE_IDX_CLOCK; break;
            case 8: p = c->image_display_enabled ? PAGE_IDX_IMAGE_DISPLAY : -1; img_src = 0; break;
            case 9: p = c->image_display_enabled ? PAGE_IDX_IMAGE_DISPLAY : -1; img_src = 1; break;
            case 10: p = c->image_display_enabled ? PAGE_IDX_IMAGE_DISPLAY : -1; img_src = 2; break;
            case 11: {
                if (c->image_display_enabled && c->custom_image_url[0] != '\0') {
                    p = PAGE_IDX_IMAGE_DISPLAY;
                    img_src = 3;
                } else {
                    p = -1;
                }
                break;
            }
        }
        if (p < 0 || !nina_dashboard_page_is_available(p)) continue;
        int inst = p - NINA_PAGE_OFFSET;
        if (inst >= 0 && inst < MAX_NINA_INSTANCES
            && c->auto_rotate_skip_disconnected
            && !nina_connection_is_connected(inst)) continue;
        src_out[n] = img_src;
        cand_out[n] = p;
        n++;
    }
    return n;
}

/* Scan the slideshow order for the first image stop; return its image
 * source (0=GOES,1=Moon,2=Solar,3=Custom) or -1 if the order has none.
 * Iterates auto_rotate_order2[] with the same bounds/termination convention
 * as slideshow_build_candidates() (skip 0xFF and out-of-range entries). */
static int8_t first_image_source_in_order(const app_config_t *c) {
    for (int i = 0; i < ARP_ORDER_CAPACITY; i++) {
        uint8_t bit = c->auto_rotate_order2[i];
        if (bit == 0xFF || bit >= ARP_IDX_MAX) continue;
        if (bit == ARP_IDX_IMG_GOES || bit == ARP_IDX_IMG_MOON
            || bit == ARP_IDX_IMG_SOLAR || bit == ARP_IDX_IMG_CUSTOM) {
            return (int8_t)(bit - ARP_IDX_IMG_GOES);
        }
    }
    return -1;
}

/** Resolve the slideshow stop that follows `from_page` in the candidate order.
 *  Writes the chosen stop's image source (0-3, or -1) to *img_src_out.
 *  Returns Home Page (with *img_src_out = -1) if no candidate is available.
 *  Pure: does not mutate arbiter state, so it serves both the real advance and
 *  the prefetch lookahead peek. */
static int slideshow_advance_from(int from_page, int8_t from_img_src, int8_t *img_src_out) {
    int cand[ARP_ORDER_CAPACITY];
    int8_t src[ARP_ORDER_CAPACITY];
    int n = slideshow_build_candidates(cand, src);
    if (n == 0) {
        *img_src_out = -1;
        return home_page();
    }
    int cur = -1;
    for (int i = 0; i < n; i++) {
        if (cand[i] == from_page && src[i] == from_img_src) { cur = i; break; }
    }
    int next = (cur + 1) % n;
    *img_src_out = src[next];
    return cand[next];
}

/** Slideshow advance from the current committed position. Records the chosen
 *  stop's image source in s_arb.pending_img_source. Returns Home Page if no
 *  page is available. */
static int slideshow_next(void) {
    int8_t img_src = -1;
    int p = slideshow_advance_from(s_arb.current_committed, s_arb.current_committed_img_source, &img_src);
    s_arb.pending_img_source = img_src;
    return p;
}

void nav_arbiter_resolve(int64_t now_ms) {
    const app_config_t *c = app_config_get();

    /* Warm the first image stop the moment the slideshow turns on (false->true
     * edge), before any early return, so the activation edge is never missed and
     * the enabled-state flag stays in sync even while a modal is open. Firing the
     * prefetch during a modal is harmless: it only fills the spare buffer. */
    bool auto_rotate_now = c->auto_rotate_enabled;
    if (auto_rotate_now && !s_arb.auto_rotate_was_enabled) {
        int8_t first_src = first_image_source_in_order(c);
        if (first_src >= 0) {
            image_source_trigger_prefetch(first_src);
        }
    }
    s_arb.auto_rotate_was_enabled = auto_rotate_now;

    /* Rung 0: modal freeze. Closing a modal restamps grace in
     * nav_arbiter_notify_modal_close, so the next resolve holds the page. */
    if (s_arb.modal_depth > 0) return;

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

    bool user_active = (s_arb.user_page >= 0)
        && ((now_ms - s_arb.user_stamp_ms) < (int64_t)c->nav_grace_s * 1000);

    /* Tie-break: auto-rotate wins if both flags are somehow set. */
    bool auto_rotate = c->auto_rotate_enabled;

    /* Default: no slideshow image source for this resolve. Only slideshow_next()
     * sets a concrete 0-3 source; every other rung leaves the override cleared so
     * a non-slideshow arrival at the image page shows the persisted default. */
    s_arb.pending_img_source = -1;

    if (user_active) {
        desired = s_arb.user_page; src = NAV_SRC_USER;
    } else if (auto_rotate) {
        if (s_arb.slideshow_advance) {
            s_arb.slideshow_advance = false;
            desired = slideshow_next();
        } else {
            desired = s_arb.current_committed;   /* hold between intervals */
            /* Hold the committed source too, so img_src_changed stays false and
             * we do not spuriously re-commit / clear the override mid-dwell. */
            s_arb.pending_img_source = s_arb.current_committed_img_source;
        }
        src = NAV_SRC_SLIDESHOW;
    } else {
        int st = session_target();
        if (st >= 0) { desired = st; src = NAV_SRC_SESSION; }
        else if (idle_condition()) {
            desired = idle_target_page();
            src = NAV_SRC_IDLE;
        } else { desired = home_page(); src = NAV_SRC_DEFAULT; }
    }

    /* Validate vs Page Model: available and not Settings, else Home Page. */
    if (desired == SETTINGS_PAGE_IDX(page_count)
        || !nina_dashboard_page_is_available(desired)) {
        desired = home_page();
    }

    /* Idle indicator coupling. */
    bool now_idle = (src == NAV_SRC_IDLE);
    if (now_idle != s_arb.idle_claim_active) {
        s_arb.idle_claim_active = now_idle;
        nina_idle_indicator_set_active(now_idle && app_config_get()->idle_indicator_enabled);
    }

    /* Commit only on change. Single path: animated switch always fires the cb.
     * Mark the page committed ONLY on a successful lock+switch; a lock timeout
     * leaves current_committed unchanged so the next resolve retries (otherwise
     * desired==current_committed would suppress the retry forever). */
    bool img_src_changed = (desired == PAGE_IDX_IMAGE_DISPLAY)
        && (s_arb.pending_img_source != s_arb.current_committed_img_source);

    if (desired != s_arb.current_committed || img_src_changed) {
        int effect = (src == NAV_SRC_SLIDESHOW) ? c->auto_rotate_effect : 0;
        /* Apply the runtime image-source override BEFORE switching the page so
         * the Image Display page fetches/renders the right source. pending is -1
         * for non-image stops, which clears the override (persisted default). */
        image_source_set_override(s_arb.pending_img_source);
        if (desired == PAGE_IDX_IMAGE_DISPLAY && goes_task_handle) {
            xTaskNotifyGive(goes_task_handle);   /* wake fetch for new source */
        }
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_show_page_animated(desired, 0, effect);
            bsp_display_unlock();
            s_arb.current_committed = desired;
            s_arb.current_committed_img_source = s_arb.pending_img_source;
            ESP_LOGI(TAG, "commit page=%d src=%d img_src=%d",
                     desired, (int)src, (int)s_arb.pending_img_source);

            /* Prefetch lookahead (schedule only; fetch/swap is Phase 4). After
             * EVERY slideshow stop (image or not), peek the source the NEXT
             * advance would select WITHOUT mutating arbiter state, and ask the
             * goes task to warm it when the next stop is an image page. This
             * closes the gap where a non-image stop (e.g. Clock) preceding an
             * image stop never warmed it. next_src is 0-3 when the next stop is
             * an image page, else -1 (non-image next stop => nothing to
             * prefetch). */
            if (src == NAV_SRC_SLIDESHOW) {
                int8_t next_src = -1;
                (void)slideshow_advance_from(s_arb.current_committed, s_arb.current_committed_img_source, &next_src);
                if (next_src >= 0) {
                    image_source_trigger_prefetch(next_src);
                }
            }
        }
    }
}
