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
    bool     idle_claim_active;    /* IDLE was the resolved source last commit */
    int      current_committed;    /* last page the arbiter committed */
} s_arb;

void nav_arbiter_init(void) {
    s_arb.user_page = -1;
    s_arb.user_stamp_ms = 0;
    s_arb.topology_dirty = false;
    s_arb.modal_depth = 0;
    s_arb.slideshow_advance = false;
    s_arb.idle_claim_active = false;
    s_arb.current_committed = nina_dashboard_get_active_page();
    ESP_LOGI(TAG, "nav arbiter init (committed page=%d)", s_arb.current_committed);
}

void nav_arbiter_submit_user(int abs_page, int64_t now_ms) {
    s_arb.user_page = abs_page;
    s_arb.user_stamp_ms = now_ms;
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

/** Slideshow advance: build the ordered candidate list from the single
 *  membership list (auto_rotate_order[0..7] + auto_rotate_order_ext), map each
 *  bit index to an absolute page, skip unavailable pages and (when configured)
 *  disconnected NINA pages, then advance from the current committed position.
 *  Returns Home Page if no page is available. */
static int slideshow_next(void) {
    const app_config_t *c = app_config_get();
    int cand[9]; int n = 0;
    for (int i = 0; i < 9; i++) {
        uint8_t bit = (i < 8) ? c->auto_rotate_order[i] : c->auto_rotate_order_ext;
        if (bit == 0xFF || bit > 8) continue;
        int p = -1;
        switch (bit) {
            case 0: p = PAGE_IDX_SUMMARY; break;
            case 1: p = nina_dashboard_slot_available(0) ? NINA_PAGE_OFFSET+0 : -1; break;
            case 2: p = nina_dashboard_slot_available(1) ? NINA_PAGE_OFFSET+1 : -1; break;
            case 3: p = nina_dashboard_slot_available(2) ? NINA_PAGE_OFFSET+2 : -1; break;
            case 4: p = SYSINFO_PAGE_IDX(page_count); break;
            case 5: p = c->allsky_enabled ? PAGE_IDX_ALLSKY : -1; break;
            case 6: p = c->spotify_enabled ? PAGE_IDX_SPOTIFY : -1; break;
            case 7: p = PAGE_IDX_CLOCK; break;
            case 8: p = c->image_display_enabled ? PAGE_IDX_IMAGE_DISPLAY : -1; break;
        }
        if (p < 0 || !nina_dashboard_page_is_available(p)) continue;
        int inst = p - NINA_PAGE_OFFSET;
        if (inst >= 0 && inst < MAX_NINA_INSTANCES
            && c->auto_rotate_skip_disconnected
            && !nina_connection_is_connected(inst)) continue;
        cand[n++] = p;
    }
    if (n == 0) return home_page();
    /* advance from current committed position */
    int cur = -1;
    for (int i = 0; i < n; i++) if (cand[i] == s_arb.current_committed) { cur = i; break; }
    return cand[(cur + 1) % n];
}

void nav_arbiter_resolve(int64_t now_ms) {
    const app_config_t *c = app_config_get();

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

    if (user_active) {
        desired = s_arb.user_page; src = NAV_SRC_USER;
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
    if (desired != s_arb.current_committed) {
        int effect = (src == NAV_SRC_SLIDESHOW) ? c->auto_rotate_effect : 0;
        if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            nina_dashboard_show_page_animated(desired, 0, effect);
            bsp_display_unlock();
            s_arb.current_committed = desired;
            ESP_LOGI(TAG, "commit page=%d src=%d", desired, (int)src);
        }
    }
}
