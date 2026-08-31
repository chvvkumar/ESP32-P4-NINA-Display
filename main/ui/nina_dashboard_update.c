/**
 * @file nina_dashboard_update.c
 * @brief Data update functions and arc animation for the NINA dashboard.
 */

#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "nina_layout_alt.h"
#if CONFIG_NINA_FAMILY_ROUND
#include "nina_round_overlay.h"
#endif
#include "nina_empty_state.h"
#include "nina_connection.h"
#include "ui_dial.h"
#include "ui_text_fit.h"
#include "app_config.h"
#include "themes.h"
#include "time_parse.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#define STALE_WARN_MS   30000   /* 30 s: show "Last update" label */
#define STALE_DIM_MS   120000   /* 2 min: dim the entire page */

/* ── Change-detection helpers ──────────────────────────────────────── */
static inline void set_label_if_changed(lv_obj_t *label, const char *text) {
    const char *cur = lv_label_get_text(label);
    if (strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

#define SET_LABEL_FMT_IF_CHANGED(label, bufsize, fmt, ...) do { \
    char _buf[bufsize]; \
    snprintf(_buf, sizeof(_buf), fmt, __VA_ARGS__); \
    const char *_cur = lv_label_get_text(label); \
    if (strcmp(_cur, _buf) != 0) lv_label_set_text(label, _buf); \
} while (0)

/* Set text color only if it actually changed (avoids marking objects dirty) */
static inline void set_text_color_if_changed(lv_obj_t *obj, lv_color_t color, lv_style_selector_t sel) {
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, sel), color))
        lv_obj_set_style_text_color(obj, color, sel);
}

/* Set arc color only if it actually changed */
static inline void set_arc_color_if_changed(lv_obj_t *obj, lv_color_t color, lv_style_selector_t sel) {
    if (!lv_color_eq(lv_obj_get_style_arc_color(obj, sel), color))
        lv_obj_set_style_arc_color(obj, color, sel);
}

/* Set arc opacity only if it actually changed (review C minor M-4: a local
 * style write always refreshes the object, and every invalidation on this
 * display is a full-frame redraw). */
static inline void set_arc_opa_if_changed(lv_obj_t *obj, lv_opa_t opa, lv_style_selector_t sel) {
    if (lv_obj_get_style_arc_opa(obj, sel) != opa)
        lv_obj_set_style_arc_opa(obj, opa, sel);
}

/* Set background color only if it actually changed. NULL-safe: the exposure
 * rings' leading-edge caps do not exist on every layout. */
static inline void set_bg_color_if_changed(lv_obj_t *obj, lv_color_t color) {
    if (!obj) return;
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, 0), color))
        lv_obj_set_style_bg_color(obj, color, 0);
}

/* Set background opacity only if it actually changed. NULL-safe, same reason */
static inline void set_bg_opa_if_changed(lv_obj_t *obj, lv_opa_t opa) {
    if (!obj) return;
    if (lv_obj_get_style_bg_opa(obj, 0) != opa)
        lv_obj_set_style_bg_opa(obj, opa, 0);
}

/* Set shadow color only if it actually changed */
static inline void set_shadow_color_if_changed(lv_obj_t *obj, lv_color_t color, lv_style_selector_t sel) {
    if (!lv_color_eq(lv_obj_get_style_shadow_color(obj, sel), color))
        lv_obj_set_style_shadow_color(obj, color, sel);
}

/* ── Font-ladder fit ───────────────────────────────────────────────────
 * The ladder and its per-label memo now live in ui_text_fit.c, so the round
 * layouts can fit a name row against a chord width instead of a parent's
 * content width. The two wrappers below keep this page's behaviour exactly as
 * it was: same ladders, same "available width is the parent's content width"
 * rule, font applied and nothing else touched. */

/* Pick the largest font that fits the label's parent width */
static void auto_fit_value_font(lv_obj_t *label) {
    static const lv_font_t *const fonts[] = {
        &lv_font_montserrat_48, &lv_font_montserrat_36,
        &lv_font_montserrat_32, &lv_font_montserrat_28,
    };
    ui_fit_label_font(label, fonts, (int)(sizeof(fonts) / sizeof(fonts[0])),
                      lv_obj_get_content_width(lv_obj_get_parent(label)));
}

/* Pick the largest font that fits the label's parent width (wider ladder for target names) */
static void auto_fit_target_name_font(lv_obj_t *label) {
    static const lv_font_t *const fonts[] = {
        &lv_font_montserrat_48, &lv_font_montserrat_36,
        &lv_font_montserrat_32, &lv_font_montserrat_28,
        &lv_font_montserrat_24, &lv_font_montserrat_20,
        &lv_font_montserrat_16,
    };
    ui_fit_label_font(label, fonts, (int)(sizeof(fonts) / sizeof(fonts[0])),
                      lv_obj_get_content_width(lv_obj_get_parent(label)));
}

static void arc_start_exposure_anim(dashboard_page_t *p);
static void arc_exec_cb(void *var, int32_t v);
static void alt_interp_tick(dashboard_page_t *p);
static void arc_interp_tick(dashboard_page_t *p);
static void alt_reset_elapsed(dashboard_page_t *p);

/* NINA-domain "now", derived from the page's cached Date-header epoch advanced
 * by monotonic time. Falls back to the device wall clock when the pair is
 * unknown. Read lock-free by the 200 ms timer; the pair is written under both
 * locks in update_exposure_arc / update_exposure_anchor. */
static int64_t page_now_nina(const dashboard_page_t *p) {
    if (p->cached_nina_epoch != 0) {
        return p->cached_nina_epoch +
               (esp_timer_get_time() - p->cached_nina_mono_us) / 1000000;
    }
    return (int64_t)time(NULL);
}

/* The capture layouts have no exposure arc of their own: drive whatever they
 * did build (the segmented sub bar, an exposure ring, an exposure bar, the hero
 * seconds) from the same monotonic anchor, the same backward-only wall
 * correction and the same 200 ms cadence the arc uses. No second timer exists,
 * and each sink is optional, so a layout only pays for what it draws. */
static void alt_interp_tick(dashboard_page_t *p) {
    if (!p->subbar.cont && !p->alt.arc_progress && !p->alt.arc_progress_num
        && !p->alt.bar_progress && !p->alt.elapsed_cb) {
        return;
    }
    if (p->exp_anchor_us == 0 || p->cached_total <= 0.0f || !p->cached_is_exposing) return;

    float elapsed = p->exp_anchor_elapsed +
                    (float)(esp_timer_get_time() - p->exp_anchor_us) / 1e6f;

    /* Backward-only wall correction (see arc_interp_timer_cb for the rationale).
     * Difference the epochs in int64 first — epoch seconds exceed float's
     * 24-bit integer precision — then cast the small result for the P4 FPU. */
    int64_t remaining_wall_ms = (p->cached_end_epoch - page_now_nina(p)) * 1000;
    float elapsed_wall = p->cached_total - (float)remaining_wall_ms / 1000.0f;
    if (elapsed_wall < elapsed - 1.0f) {
        p->exp_anchor_us = esp_timer_get_time();
        p->exp_anchor_elapsed = (elapsed_wall > 0.0f) ? elapsed_wall : 0.0f;
        elapsed = p->exp_anchor_elapsed;
    }

    if (elapsed < 0.0f) elapsed = 0.0f;
    if (elapsed > p->cached_total) elapsed = p->cached_total;

    const float frac = elapsed / p->cached_total;
    if (p->subbar.cont) nina_subbar_set_progress(&p->subbar, frac);
    if (p->alt.arc_progress) {
        lv_arc_set_value(p->alt.arc_progress, (int32_t)(frac * 1000.0f));
    }
    if (p->alt.arc_progress_num) {
        lv_arc_set_value(p->alt.arc_progress_num, (int32_t)(frac * 1000.0f));
    }
    /* The bands above only fill whole degrees; the caps ride the exact fraction
     * so the leading edge advances smoothly instead of stepping (ui_dial.h).
     * On round layout 0 cap_progress_num rides p->arc_exposure, whose
     * animations place it themselves through arc_exec_cb from the animated
     * value; placing it from this frac too put it a few degrees off the band. */
    ui_dial_cap_place(p->alt.cap_progress.obj, p->alt.cap_progress.r,
                      p->alt.cap_progress.a0, p->alt.cap_progress.sweep, frac);
    if (p->layout != 0) {
        ui_dial_cap_place(p->alt.cap_progress_num.obj, p->alt.cap_progress_num.r,
                          p->alt.cap_progress_num.a0, p->alt.cap_progress_num.sweep,
                          frac);
    }
    if (p->alt.bar_progress) {
        lv_bar_set_value(p->alt.bar_progress, (int32_t)(frac * 1000.0f), LV_ANIM_OFF);
    }
    if (p->alt.elapsed_cb) {
        int secs = (int)elapsed;
        if (secs > 9999) secs = 9999;      /* same cap the sub bar applies */
        p->alt.elapsed_cb(p, secs);
    }
}

/* The idle reset every "no exposure running" site shares: park the sub bar, the
 * exposure ring or bar and the hero digits together, through the same single
 * writer each of them has while an exposure is running. */
static void alt_reset_elapsed(dashboard_page_t *p) {
    if (!p) return;
    if (p->subbar.cont)      nina_subbar_reset_elapsed(&p->subbar);
    if (p->alt.elapsed_cb)   p->alt.elapsed_cb(p, -1);
    if (p->alt.arc_progress) lv_arc_set_value(p->alt.arc_progress, 0);
    if (p->alt.arc_progress_num) lv_arc_set_value(p->alt.arc_progress_num, 0);
    /* Zero hides the caps, so nothing is left sitting on an empty ring. */
    ui_dial_cap_place(p->alt.cap_progress.obj, p->alt.cap_progress.r,
                      p->alt.cap_progress.a0, p->alt.cap_progress.sweep, 0.0f);
    ui_dial_cap_place(p->alt.cap_progress_num.obj, p->alt.cap_progress_num.r,
                      p->alt.cap_progress_num.a0, p->alt.cap_progress_num.sweep,
                      0.0f);
    if (p->alt.bar_progress) lv_bar_set_value(p->alt.bar_progress, 0, LV_ANIM_OFF);
}

void arc_interp_timer_cb(lv_timer_t *timer) {
    dashboard_page_t *p = (dashboard_page_t *)lv_timer_get_user_data(timer);
    if (!p) return;
    /* Arc body FIRST, then the sub-bar tick (review C12 I-1). On round layout
     * 0 both bodies carry the same backward-only wall-clock correction and
     * both can re-anchor exp_anchor_us / exp_anchor_elapsed; running the arc
     * first means the sub ring below always reads the anchor the arc just
     * corrected, instead of re-anchoring itself and leaving the arc's long
     * linear fill animation pointed at a now-stale end time. On the square
     * bento layout p->layout is always 0 (arc dashboard, the only square
     * layout), so this is the same arc_interp_tick() body running in the same
     * place it always has: the reordering changes nothing there. */
    arc_interp_tick(p);
    /* alt_interp_tick() returns immediately when the page built none of the
     * sinks it drives, so this is a no-op on the square bento layout and drives
     * the round board's sub ring on layout 0. */
    alt_interp_tick(p);
}

/* The one exec callback every arc_exposure animation uses (fill, completion
 * snap, gap fade, idle drop). Sets the value and, when the round board hung
 * its leading-edge cap descriptor on the arc's user data, places the cap from
 * that SAME value, so band and cap can never disagree the way they did when
 * the cap was placed from the 200 ms tick's own fraction (a ball a couple of
 * degrees ahead of the band, or buried under it). Square never sets the user
 * data, so this is plain lv_arc_set_value there. */
static void arc_exec_cb(void *var, int32_t v) {
    lv_obj_t *arc = (lv_obj_t *)var;
    lv_arc_set_value(arc, v);
    nina_arc_cap_t *c = (nina_arc_cap_t *)lv_obj_get_user_data(arc);
    if (c) ui_dial_cap_place(c->obj, c->r, c->a0, c->sweep, (float)v / (float)ARC_RANGE);
}

/* Layout 0 only: the exposure arc's monotonic-anchor correction and long
 * linear fill animation. Split out of arc_interp_timer_cb so the timer can
 * run this before subbar_interp_tick(): see the call site. */
static void arc_interp_tick(dashboard_page_t *p) {
    if (p->layout != 0) return;
    if (!p->arc_exposure || p->arc_completing) return;
    if (p->exp_anchor_us == 0 || p->cached_total <= 0) return;

    /* Completion is handled on the IsExposing edge in update_exposure_arc.
     * Do not delete the anim here; just stop driving it while not exposing. */
    if (!p->cached_is_exposing) return;

    /* Monotonic elapsed: esp_timer is microseconds since boot, never skews and
     * never goes backward. This is the smooth source of truth. */
    float elapsed = p->exp_anchor_elapsed +
                    (float)(esp_timer_get_time() - p->exp_anchor_us) / 1e6f;

    /* Backward-only wall correction. Epoch seconds (~1.7e9) exceed float's
     * 24-bit integer precision, so difference in int64 first, then cast the
     * small result to float for the P4 single-precision FPU. If NINA is
     * genuinely slower than our anchor predicted (paused / dither / meridian
     * flip extended the sub), the wall clock shows less elapsed than us — only
     * then re-anchor backward. Never pull forward on wall drift.
     * "Wall" here is the NINA-PC clock domain (cached_end_epoch is a NINA
     * timestamp): advance the cached Date-header epoch by monotonic time.
     * This timer runs WITHOUT the data lock, so it reads the page's cached
     * pair (copied under the lock in update_exposure_arc), never d directly. */
    int64_t now_nina;
    if (p->cached_nina_epoch != 0) {
        now_nina = p->cached_nina_epoch +
                   (esp_timer_get_time() - p->cached_nina_mono_us) / 1000000;
    } else {
        now_nina = (int64_t)time(NULL);
    }
    int64_t remaining_wall_ms = (p->cached_end_epoch - now_nina) * 1000;
    float elapsed_wall = p->cached_total - (float)remaining_wall_ms / 1000.0f;

    if (elapsed_wall < elapsed - 1.0f) {
        p->exp_anchor_us = esp_timer_get_time();
        p->exp_anchor_elapsed = (elapsed_wall > 0.0f) ? elapsed_wall : 0.0f;
        arc_start_exposure_anim(p);
        return;
    }

    /* The long linear anim is the smooth source of truth; do NOT restart on
     * small drift. Only restart if no anim is running (e.g. a prior shorter
     * estimate ended the anim early) and we still have time left. */
    lv_anim_t *existing = lv_anim_get(p->arc_exposure, arc_exec_cb);
    if (!existing && elapsed < p->cached_total) {
        arc_start_exposure_anim(p);
    }
}

void nina_dashboard_update_status(int instance, int rssi, bool nina_connected, bool api_active) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    if (!nina_slot_available[instance]) return;
    dashboard_page_t *p = &pages[instance];
    if (!p->page) return;

    p->nina_connected = nina_connected;

    int gb = app_config_get()->color_brightness;
    uint32_t glow_color;
    if (theme_is_red_night(current_theme)) {
        glow_color = app_config_apply_brightness(
            nina_connected ? current_theme->text_color : current_theme->bento_border, gb);
    } else {
        glow_color = app_config_apply_brightness(
            nina_connected ? 0x4ade80 : 0xf87171, gb);
    }

    if (p->lbl_instance_name) {
        set_text_color_if_changed(p->lbl_instance_name, lv_color_hex(glow_color), 0);
    }
}

/* ── Smooth RMS / HFR value animation ─────────────────────────────── */
#define VALUE_ANIM_MS  500

static void arcsec_anim_exec(void *obj, int32_t v) {
    lv_label_set_text_fmt((lv_obj_t *)obj, "%.2f\"", v / 100.0f);
}

static void hfr_anim_exec(void *obj, int32_t v) {
    lv_label_set_text_fmt((lv_obj_t *)obj, "%.2f", v / 100.0f);
}

static void animate_value(lv_obj_t *label, int32_t from, int32_t to,
                          lv_anim_exec_xcb_t exec_cb) {
    lv_anim_delete(label, exec_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, VALUE_ANIM_MS);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ---- Sub-functions for update_nina_dashboard_page() ---- */

/* Edge gate for update_disconnected_state(). Its whole body (2 URL parses, 3
 * snprintf, ~14 label writes, font fitting) used to re-run on every poll while
 * an instance was disconnected, though everything it writes is a pure function
 * of these inputs. The reconnect side was already gated this way (see the
 * !p->nina_connected block in update_nina_dashboard_page); this is the matching
 * disconnect-side gate. Invalidated on the CONNECTED path so the next
 * disconnect always repaints, and keyed on theme/brightness so a theme change
 * while offline still restyles. */
typedef struct {
    bool              valid;
    nina_conn_state_t state;
    bool              enabled;
    int               gb;
    const theme_t    *theme;
    char              url[128];
} disc_gate_t;

static disc_gate_t s_disc_gate[MAX_NINA_INSTANCES];

/* Drop a slot's gate so its next poll repaints from scratch. Called by
 * nina_dashboard_rebuild_slot() after (re)creating a slot's widgets: the fresh
 * widgets carry creation defaults, not the offline layout. */
void nina_dashboard_invalidate_disconnect_gate(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    s_disc_gate[instance].valid = false;
}

static void update_disconnected_state(dashboard_page_t *p, int instance_idx, int gb, nina_conn_state_t conn_state) {
    const char *url = app_config_get_instance_url(instance_idx);
    bool enabled = app_config_is_instance_enabled(instance_idx);

    disc_gate_t *g = &s_disc_gate[instance_idx];
    if (g->valid && g->state == conn_state && g->enabled == enabled &&
        g->gb == gb && g->theme == current_theme &&
        strncmp(g->url, url, sizeof(g->url) - 1) == 0) {
        return;   /* nothing this body depends on has changed */
    }
    g->valid   = true;
    g->state   = conn_state;
    g->enabled = enabled;
    g->gb      = gb;
    g->theme   = current_theme;
    strncpy(g->url, url, sizeof(g->url) - 1);
    g->url[sizeof(g->url) - 1] = '\0';

    char host[64] = {0};
    extract_host_from_url(url, host, sizeof(host));
    const char *state_text;
    if (!enabled) {
        state_text = "Disabled";
    } else if (conn_state == NINA_CONN_UNKNOWN || conn_state == NINA_CONN_CONNECTING) {
        state_text = "Connecting...";
    } else {
        state_text = "Not Connected";
    }
    if (host[0] != '\0') {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s - %s", host, state_text);
        set_label_if_changed(p->lbl_instance_name, buf);
    } else {
        set_label_if_changed(p->lbl_instance_name, state_text);
    }
    if (p->lbl_target_name) {
        set_label_if_changed(p->lbl_target_name, "----");
        auto_fit_target_name_font(p->lbl_target_name);
    }
    if (p->alt.lbl_target)      set_label_if_changed(p->alt.lbl_target, "----");
    if (p->lbl_seq_container)   set_label_if_changed(p->lbl_seq_container, "----");
    if (p->lbl_seq_step)        set_label_if_changed(p->lbl_seq_step, "----");
    if (p->lbl_exposure_total)  set_label_if_changed(p->lbl_exposure_total, "");
    if (p->lbl_loop_count)      set_label_if_changed(p->lbl_loop_count, "");
    if (p->lbl_exposure_current) set_label_if_changed(p->lbl_exposure_current, "--");
    alt_reset_elapsed(p);
    /* Drop any active exposure anchor so a stale anchor can't keep the 200ms
     * timer driving the arc while this instance is disconnected. */
    lv_anim_delete(p->arc_exposure, arc_exec_cb);
    p->exp_anchor_us = 0;
    p->exp_anchor_elapsed = 0;
    p->cached_is_exposing = false;
    p->arc_completing = false;
    p->cached_end_epoch = 0;
    p->cached_total = 0;
    p->gap_start_epoch = 0;
    p->cached_nina_epoch = 0;
    p->cached_nina_mono_us = 0;
    if (p->arc_exposure)     arc_exec_cb(p->arc_exposure, 0);
    if (p->row_filter_total) lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(p->lbl_rms_value, arcsec_anim_exec);
    set_label_if_changed(p->lbl_rms_value, "--");
    set_text_color_if_changed(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    p->anim_rms_total_x100 = 0;
    if (p->lbl_rms_ra_value)  { lv_anim_delete(p->lbl_rms_ra_value, arcsec_anim_exec);  p->anim_rms_ra_x100 = 0; }
    if (p->lbl_rms_dec_value) { lv_anim_delete(p->lbl_rms_dec_value, arcsec_anim_exec); p->anim_rms_dec_x100 = 0; }
    lv_anim_delete(p->lbl_hfr_value, hfr_anim_exec);
    set_label_if_changed(p->lbl_hfr_value, "--");
    set_text_color_if_changed(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    p->anim_hfr_x100 = 0;
    if (p->lbl_flip_value)  set_label_if_changed(p->lbl_flip_value, "--");
    if (p->lbl_stars_value) set_label_if_changed(p->lbl_stars_value, "--");
    if (p->lbl_target_time_value) {
        set_label_if_changed(p->lbl_target_time_value, "--");
        auto_fit_value_font(p->lbl_target_time_value);
    }
    if (p->lbl_target_time_header) {
        set_label_if_changed(p->lbl_target_time_header, "TIME LIMIT");
    }
    /* Shapes go home: both trend graphs cleared, the flip tick away. */
    if (p->rms_chart) lv_chart_set_all_values(p->rms_chart, p->rms_ser, LV_CHART_POINT_NONE);
    if (p->hfr_chart) lv_chart_set_all_values(p->hfr_chart, p->hfr_ser, LV_CHART_POINT_NONE);
    if (p->ring_flip_tick) ui_dial_set_tick(p->ring_flip_tick, -1, UI_DIAL_FLIP_SPAN_MIN);
    if (p->box_pwr[0]) {
        for (int i = 0; i < MAX_POWER_WIDGETS; i++) {
            lv_obj_add_flag(p->box_pwr[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* IDLE-04: hide the header and arc so only the branded overlay is visible.
     * IDLE-05: arc hidden via LV_OBJ_FLAG_HIDDEN (not drawn at bg_main). */
    if (p->header_box) {
        lv_obj_add_flag(p->header_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (p->arc_exposure) {
        lv_obj_add_flag(p->arc_exposure, LV_OBJ_FLAG_HIDDEN);
    }

    /* Refresh the offline title to reflect the current configured hostname,
     * then show the branded empty-state overlay. */
    if (p->empty_state_cont) {
        char host[64] = {0};
        extract_host_from_url(app_config_get_instance_url(instance_idx), host, sizeof(host));
        char offline_title[96];
        if (host[0]) {
            snprintf(offline_title, sizeof(offline_title), "%s Offline", host);
        } else {
            snprintf(offline_title, sizeof(offline_title), "Node %d Offline", instance_idx + 1);
        }
        nina_empty_state_set_title(p->empty_state_cont, offline_title);
        nina_empty_state_show(p->empty_state_cont);
    }
}

static void update_header(dashboard_page_t *p, const nina_client_t *d) {
    /* Telescope + camera. Square puts them on one line in the header row; the
     * round dashboard spine is a centred column with room for two, and the
     * joined string wrapped mid-name there, so the break is explicit. */
    if (d->telescope_name[0] && d->camera_name[0]) {
        char buf[132];
        /* Two literal formats, not a ternary one: -Wformat-truncation can
           only bound the result when it can see the separator. */
        if (SCREEN_ROUND) {
            snprintf(buf, sizeof(buf), "%s\n%s", d->telescope_name, d->camera_name);
        } else {
            snprintf(buf, sizeof(buf), "%s | %s", d->telescope_name, d->camera_name);
        }
        set_label_if_changed(p->lbl_instance_name, buf);
    } else if (d->telescope_name[0]) {
        set_label_if_changed(p->lbl_instance_name, d->telescope_name);
    } else if (d->camera_name[0]) {
        set_label_if_changed(p->lbl_instance_name, d->camera_name);
    } else {
        set_label_if_changed(p->lbl_instance_name, "N.I.N.A.");
    }

    const char *tgt = (d->target_name[0] != '\0') ? d->target_name : "----";
    if (p->lbl_target_name) {
        set_label_if_changed(p->lbl_target_name, tgt);
        auto_fit_target_name_font(p->lbl_target_name);
    }
    /* The round spine's target label has a fixed 40 px face: the auto-fit
     * ladder would drop a long name below the round text floor. */
    if (p->alt.lbl_target) set_label_if_changed(p->alt.lbl_target, tgt);
}

static void update_sequence_info(dashboard_page_t *p, const nina_client_t *d) {
    if (p->lbl_seq_container) {
        set_label_if_changed(p->lbl_seq_container,
            d->container_name[0] != '\0' ? d->container_name : "----");
    }
    if (p->lbl_seq_step) {
        set_label_if_changed(p->lbl_seq_step,
            d->container_step[0] != '\0' ? d->container_step : "----");
    }
}

static void arc_start_exposure_anim(dashboard_page_t *p) {
    if (p->exp_anchor_us == 0 || p->cached_total <= 0 || !p->cached_is_exposing) return;

    /* Drive remaining time from the monotonic anchor (esp_timer), never wall
     * clock. The device SNTP clock is skewed vs the NINA-PC clock that stamped
     * ExposureEndTime, so wall-clock progress mistracks (worst near the end). */
    float since_anchor_s = (float)(esp_timer_get_time() - p->exp_anchor_us) / 1e6f;
    float remaining_s = p->cached_total - (p->exp_anchor_elapsed + since_anchor_s);
    if (remaining_s <= 0.1f) return;   /* <=100ms: completion edge will fill it */

    int current = lv_arc_get_value(p->arc_exposure);
    int remaining_ms = (int)(remaining_s * 1000.0f);

    /* Never reach full while exposing; only the IsExposing edge fills the circle. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p->arc_exposure);
    lv_anim_set_values(&a, current, ARC_RANGE - 1);
    lv_anim_set_time(&a, remaining_ms);
    lv_anim_set_exec_cb(&a, arc_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

static void update_exposure_arc(dashboard_page_t *p, const nina_client_t *d,
                                int instance_idx, int gb) {
    uint32_t filter_color = app_config_apply_brightness(current_theme->progress_color, gb);
    if (!theme_is_red_night(current_theme) && d->current_filter[0] != '\0' && strcmp(d->current_filter, "--") != 0) {
        filter_color = app_config_get_filter_color(d->current_filter, instance_idx);
    }
    set_arc_color_if_changed(p->arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);
    set_shadow_color_if_changed(p->arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);
    /* Round layout 0's leading-edge cap rides this ring, so it takes the same
     * filter colour. NULL on the square bento layout, which draws no cap. */
    set_bg_color_if_changed(p->alt.cap_progress_num.obj, lv_color_hex(filter_color));

    // Detect filter change — reset arc state
    if (d->current_filter[0] != '\0' && strcmp(p->prev_filter, d->current_filter) != 0) {
        lv_anim_delete(p->arc_exposure, arc_exec_cb);
        p->arc_completing = false;
        p->cached_end_epoch = 0;
        p->cached_total = 0;
        p->gap_start_epoch = 0;
        p->exp_anchor_us = 0;
        p->exp_anchor_elapsed = 0;
        p->cached_nina_epoch = 0;
        p->cached_nina_mono_us = 0;
        arc_exec_cb(p->arc_exposure, 0);
        snprintf(p->prev_filter, sizeof(p->prev_filter), "%s", d->current_filter);
    }

    /* NINA-domain "now" for all NINA-timestamp math (exposure_end_epoch is a
     * NINA-PC timestamp). The caller (update_nina_dashboard_page, via
     * data_update_task) holds the nina_client lock here, so reading d's clock
     * pair is safe. Copy the pair into the page cache for the lock-free
     * 200ms arc_interp_timer_cb.
     * Concurrency: the cached pair is serialized by the LVGL display lock,
     * not the nina_client lock — this writer runs under both locks, while
     * arc_interp_timer_cb reads it under the LVGL lock only (esp_lvgl_port
     * task). Keep any future readers inside the LVGL lock. */
    int64_t now_nina = nina_client_now_epoch(d);
    p->cached_nina_epoch = d->nina_clock_epoch;
    p->cached_nina_mono_us = d->nina_clock_mono_us;

    /* Detect the IsExposing true->false edge BEFORE updating cached_is_exposing.
     * NINA flips IsExposing->false at sub end and the poll usually sees that
     * before remaining hits zero, so this edge (not a wall-clock timeout) is
     * what drives the satisfying snap-to-full completion. */
    bool finished_edge = (p->cached_is_exposing && !d->is_exposing && p->exp_anchor_us != 0);
    p->cached_is_exposing = d->is_exposing;

    if (finished_edge) {
        /* Snap the arc to a full circle for a polished completion, then let the
         * inter-exposure gap logic below hold/fade it before the next sub. */
        lv_anim_delete(p->arc_exposure, arc_exec_cb);
        p->arc_completing = true;
        p->exp_anchor_us = 0;
        int current_fill = lv_arc_get_value(p->arc_exposure);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, p->arc_exposure);
        lv_anim_set_values(&a, current_fill, ARC_RANGE);
        lv_anim_set_time(&a, ARC_TRANSITION_MS);
        lv_anim_set_exec_cb(&a, arc_exec_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    if (d->exposure_total > 0 && d->exposure_end_epoch > 0 && d->is_exposing) {
        // Camera is actively exposing
        p->gap_start_epoch = 0;  // Clear any gap timer

        // Show total exposure duration inside the arc
        if (p->lbl_exposure_current) {
            int total_sec = (int)d->exposure_total;
            SET_LABEL_FMT_IF_CHANGED(p->lbl_exposure_current, 16, "%ds", total_sec);
        }

        // Update filter label
        if (d->current_filter[0] != '\0' && strcmp(d->current_filter, "--") != 0) {
            set_label_if_changed(p->lbl_exposure_total, d->current_filter);
            set_text_color_if_changed(p->lbl_exposure_total, lv_color_hex(filter_color), 0);
        } else {
            set_label_if_changed(p->lbl_exposure_total, "");
        }

        // Detect new exposure by end_epoch change, or idle->exposing with no anchor
        bool new_exposure = (d->exposure_end_epoch != p->cached_end_epoch
                             && d->exposure_end_epoch > now_nina)
                            || (p->exp_anchor_us == 0);

        /* Detect a material exposure_total change on the SAME ongoing exposure
         * (e.g. stale image-history total replaced by the real sequence total a
         * few seconds after boot). Computed against the OLD cached_* values, so
         * this must run BEFORE the cache assignments below. */
        bool same_exposure = (p->exp_anchor_us != 0
                              && d->exposure_end_epoch == p->cached_end_epoch);
        bool total_changed = (same_exposure && p->cached_total > 0.0f
                              && fabsf(p->cached_total - d->exposure_total) > 1.0f);

        // Update cached values for the timer
        p->cached_end_epoch = d->exposure_end_epoch;
        p->cached_total = d->exposure_total;

        if (new_exposure) {
            /* Anchor the monotonic clock at this moment. Seed exp_anchor_elapsed
             * with a ONE-TIME wall estimate of how far into the sub we already
             * are (detection can land mid-sub on page switch / first connect).
             * Difference the epochs in int64 first to preserve precision, then
             * cast the small result to float for the P4 single-precision FPU. */
            int64_t remaining_seed_ms = (d->exposure_end_epoch - now_nina) * 1000;
            float seed = d->exposure_total - (float)remaining_seed_ms / 1000.0f;
            if (seed < 0.0f) seed = 0.0f;
            if (seed > d->exposure_total) seed = d->exposure_total;

            p->exp_anchor_us = esp_timer_get_time();
            p->exp_anchor_elapsed = seed;
            p->arc_completing = false;

            lv_anim_delete(p->arc_exposure, arc_exec_cb);
            /* Seed the arc value from the elapsed estimate so a mid-sub detection
             * does not snap back to zero. */
            int seed_val = (int)((seed * (float)ARC_RANGE) / d->exposure_total);
            if (seed_val < 0) seed_val = 0;
            if (seed_val > ARC_RANGE - 1) seed_val = ARC_RANGE - 1;
            arc_exec_cb(p->arc_exposure, seed_val);

            /* Start one long linear anim toward (ARC_RANGE-1) over the monotonic
             * remaining time. arc_start_exposure_anim skips if <=100ms remain
             * (the completion edge fills it). */
            arc_start_exposure_anim(p);
        } else if (total_changed) {
            /* Same ongoing sub, but exposure_total was corrected (stale
             * image-history length replaced by the real sequence length).
             * Re-anchor against the corrected total and smoothly animate the
             * one-time position correction instead of hard-jumping the arc. */
            int64_t remaining_seed_ms = (d->exposure_end_epoch - now_nina) * 1000;
            float seed = d->exposure_total - (float)remaining_seed_ms / 1000.0f;
            if (seed < 0.0f) seed = 0.0f;
            if (seed > d->exposure_total) seed = d->exposure_total;

            p->exp_anchor_us = esp_timer_get_time();
            p->exp_anchor_elapsed = seed;

            int target_val = (int)((seed * (float)ARC_RANGE) / d->exposure_total);
            if (target_val < 0) target_val = 0;
            if (target_val > ARC_RANGE - 1) target_val = ARC_RANGE - 1;
            int cur_val = lv_arc_get_value(p->arc_exposure);

            lv_anim_delete(p->arc_exposure, arc_exec_cb);
            /* Smoothly move from the (mis-seeded) current value to the corrected
             * position; the long linear anim takes over toward ARC_RANGE-1 once
             * this short correction anim ends. Do NOT call
             * arc_start_exposure_anim here — it would delete this correction
             * anim. The 200ms arc_interp_timer_cb restarts the long progress
             * anim when no anim is running and elapsed < cached_total. */
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, p->arc_exposure);
            lv_anim_set_values(&a, cur_val, target_val);
            lv_anim_set_time(&a, ARC_TRANSITION_MS);
            lv_anim_set_exec_cb(&a, arc_exec_cb);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
        // Normal progress updates are handled by the 200ms timer (arc_interp_timer_cb)

        // Update exposure count labels
        if (d->exposure_iterations > 0) {
            /* Round mockup reads "5 / 10"; the square bento box keeps its
             * shipped "x 5/10" (review C12 M-4, ruling: round only). */
            if (p->ring_exposure) {
                SET_LABEL_FMT_IF_CHANGED(p->lbl_loop_count, 32, "%d / %d",
                    d->exposure_count, d->exposure_iterations);
            } else {
                SET_LABEL_FMT_IF_CHANGED(p->lbl_loop_count, 32, "x %d/%d",
                    d->exposure_count, d->exposure_iterations);
            }
        } else {
            set_label_if_changed(p->lbl_loop_count, "");
        }

        if (p->row_filter_total && p->lbl_filter_done_value) {
            if (d->exposure_total_count > 0) {
                int total_secs = (int)(d->exposure_total_count * d->exposure_total);
                char dur[16];
                fmt_duration(dur, sizeof(dur), total_secs, FMT_DUR_HM_COMPACT);
                SET_LABEL_FMT_IF_CHANGED(p->lbl_filter_done_value, 32, "%d / %s",
                    d->exposure_total_count, dur);
                set_text_color_if_changed(p->lbl_filter_done_value, lv_color_hex(filter_color), 0);
                lv_obj_clear_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        // No active exposure data — handle inter-exposure gap or idle state

        if (p->cached_end_epoch > 0 && p->cached_total > 0) {
            // Was recently exposing — hold arc position during gap
            bool camera_idle = (strcmp(d->status, "Idle") == 0
                             || strcmp(d->status, "NoState") == 0
                             || strcmp(d->status, "OFFLINE") == 0);

            /* Gap timing is device-only elapsed time (a duration, not a
             * NINA-timestamp comparison) — deliberately stays on time(NULL). */
            int64_t now_wall = (int64_t)time(NULL);
            if (p->gap_start_epoch == 0) {
                p->gap_start_epoch = now_wall;
            }

            int64_t gap_duration = now_wall - p->gap_start_epoch;
            if (camera_idle || gap_duration > ARC_GAP_GRACE_S) {
                // Grace period expired — transition to idle
                p->cached_end_epoch = 0;
                p->cached_total = 0;
                p->gap_start_epoch = 0;
                p->exp_anchor_us = 0;
                p->exp_anchor_elapsed = 0;
                p->arc_completing = false;
                p->cached_nina_epoch = 0;
                p->cached_nina_mono_us = 0;
                set_label_if_changed(p->lbl_exposure_total, "");
                set_label_if_changed(p->lbl_loop_count, "");
                if (p->lbl_exposure_current) {
                    set_label_if_changed(p->lbl_exposure_current, "--");
                }
                alt_reset_elapsed(p);
                lv_anim_delete(p->arc_exposure, arc_exec_cb);

                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, p->arc_exposure);
                lv_anim_set_values(&a, lv_arc_get_value(p->arc_exposure), 0);
                lv_anim_set_time(&a, 500);
                lv_anim_set_exec_cb(&a, arc_exec_cb);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_start(&a);

                if (p->row_filter_total) {
                    lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
                }
            }
            // else: within grace period — do nothing, arc stays where it is
        } else {
            // Genuinely idle — no recent exposure data
            p->gap_start_epoch = 0;
            p->exp_anchor_us = 0;
            p->exp_anchor_elapsed = 0;
            set_label_if_changed(p->lbl_exposure_total, "");
            set_label_if_changed(p->lbl_loop_count, "");
            if (p->lbl_exposure_current) {
                set_label_if_changed(p->lbl_exposure_current, "--");
            }
            alt_reset_elapsed(p);
            arc_exec_cb(p->arc_exposure, 0);
            if (p->row_filter_total) {
                lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void update_guider_stats(dashboard_page_t *p, const nina_client_t *d,
                                int instance_idx, int gb) {
    /* ── RMS Total ── */
    if (d->guider.rms_total > 0) {
        int32_t new_val = (int32_t)(d->guider.rms_total * 100.0f + 0.5f);
        uint32_t rms_color = theme_is_red_night(current_theme)
            ? app_config_apply_brightness(current_theme->rms_color, gb)
            : app_config_get_rms_color(d->guider.rms_total, instance_idx);
        set_text_color_if_changed(p->lbl_rms_value, lv_color_hex(rms_color), 0);

        if (p->anim_rms_total_x100 > 0 && new_val != p->anim_rms_total_x100) {
            animate_value(p->lbl_rms_value, p->anim_rms_total_x100, new_val, arcsec_anim_exec);
        } else {
            lv_anim_delete(p->lbl_rms_value, arcsec_anim_exec);
            lv_label_set_text_fmt(p->lbl_rms_value, "%.2f\"", d->guider.rms_total);
        }
        p->anim_rms_total_x100 = new_val;
    } else {
        lv_anim_delete(p->lbl_rms_value, arcsec_anim_exec);
        set_label_if_changed(p->lbl_rms_value, "--");
        set_text_color_if_changed(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        p->anim_rms_total_x100 = 0;
    }

    /* ── RMS RA ── */
    if (p->lbl_rms_ra_value) {
        if (d->guider.rms_ra > 0) {
            int32_t new_val = (int32_t)(d->guider.rms_ra * 100.0f + 0.5f);
            if (p->anim_rms_ra_x100 > 0 && new_val != p->anim_rms_ra_x100) {
                animate_value(p->lbl_rms_ra_value, p->anim_rms_ra_x100, new_val, arcsec_anim_exec);
            } else {
                lv_anim_delete(p->lbl_rms_ra_value, arcsec_anim_exec);
                lv_label_set_text_fmt(p->lbl_rms_ra_value, "%.2f\"", d->guider.rms_ra);
            }
            p->anim_rms_ra_x100 = new_val;
        } else {
            lv_anim_delete(p->lbl_rms_ra_value, arcsec_anim_exec);
            set_label_if_changed(p->lbl_rms_ra_value, "--");
            p->anim_rms_ra_x100 = 0;
        }
    }

    /* ── RMS DEC ── */
    if (p->lbl_rms_dec_value) {
        if (d->guider.rms_dec > 0) {
            int32_t new_val = (int32_t)(d->guider.rms_dec * 100.0f + 0.5f);
            if (p->anim_rms_dec_x100 > 0 && new_val != p->anim_rms_dec_x100) {
                animate_value(p->lbl_rms_dec_value, p->anim_rms_dec_x100, new_val, arcsec_anim_exec);
            } else {
                lv_anim_delete(p->lbl_rms_dec_value, arcsec_anim_exec);
                lv_label_set_text_fmt(p->lbl_rms_dec_value, "%.2f\"", d->guider.rms_dec);
            }
            p->anim_rms_dec_x100 = new_val;
        } else {
            lv_anim_delete(p->lbl_rms_dec_value, arcsec_anim_exec);
            set_label_if_changed(p->lbl_rms_dec_value, "--");
            p->anim_rms_dec_x100 = 0;
        }
    }

    /* ── HFR ── */
    if (d->hfr > 0) {
        int32_t new_val = (int32_t)(d->hfr * 100.0f + 0.5f);
        uint32_t hfr_color = theme_is_red_night(current_theme)
            ? app_config_apply_brightness(current_theme->hfr_color, gb)
            : app_config_get_hfr_color(d->hfr, instance_idx);
        set_text_color_if_changed(p->lbl_hfr_value, lv_color_hex(hfr_color), 0);

        if (p->anim_hfr_x100 > 0 && new_val != p->anim_hfr_x100) {
            animate_value(p->lbl_hfr_value, p->anim_hfr_x100, new_val, hfr_anim_exec);
        } else {
            lv_anim_delete(p->lbl_hfr_value, hfr_anim_exec);
            lv_label_set_text_fmt(p->lbl_hfr_value, "%.2f", d->hfr);
        }
        p->anim_hfr_x100 = new_val;
    } else {
        lv_anim_delete(p->lbl_hfr_value, hfr_anim_exec);
        set_label_if_changed(p->lbl_hfr_value, "--");
        set_text_color_if_changed(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        p->anim_hfr_x100 = 0;
    }

    /* Trend graphs: one point per poll cycle, the value plotted as its ratio
     * against the configured tolerance (ok_max) x100, clamped at
     * DR_GRAPH_RANGE (twice the tolerance) so a bad excursion stays on the
     * board. No reading pushes a gap, not a zero. */
    if (p->rms_chart && p->rms_ser) {
        threshold_config_t t;
        app_config_get_rms_threshold_config(instance_idx, &t);
        float lim = (t.ok_max > 0.01f) ? t.ok_max : 1.0f;
        int32_t v = LV_CHART_POINT_NONE;
        if (d->guider.rms_total > 0.0f) {
            float ratio = d->guider.rms_total / lim * 100.0f;
            v = (ratio > DR_GRAPH_RANGE) ? DR_GRAPH_RANGE : (int32_t)ratio;
        }
        lv_chart_set_next_value(p->rms_chart, p->rms_ser, v);
    }
    if (p->hfr_chart && p->hfr_ser) {
        threshold_config_t t;
        app_config_get_hfr_threshold_config(instance_idx, &t);
        float lim = (t.ok_max > 0.01f) ? t.ok_max : 3.5f;
        int32_t v = LV_CHART_POINT_NONE;
        if (d->hfr > 0.0f) {
            float ratio = d->hfr / lim * 100.0f;
            v = (ratio > DR_GRAPH_RANGE) ? DR_GRAPH_RANGE : (int32_t)ratio;
        }
        lv_chart_set_next_value(p->hfr_chart, p->hfr_ser, v);
    }
}

static void update_mount_and_image_stats(dashboard_page_t *p, const nina_client_t *d) {
    // Format flip time from "HH:MM:SS" to "Xh XXm"
    int flip_mins = -1;
    if (d->meridian_flip[0] != '\0' && strcmp(d->meridian_flip, "--") != 0
        && strcmp(d->meridian_flip, "FLIPPING") != 0) {
        int hh = 0, mm = 0;
        if (sscanf(d->meridian_flip, "%d:%d", &hh, &mm) >= 2) {
            flip_mins = hh * 60 + mm;
            if (p->lbl_flip_value) {
                SET_LABEL_FMT_IF_CHANGED(p->lbl_flip_value, 16, "%dh %02dm", hh, mm);
            }
        } else if (p->lbl_flip_value) {
            set_label_if_changed(p->lbl_flip_value, d->meridian_flip);
        }
    } else if (d->meridian_flip[0] != '\0' && strcmp(d->meridian_flip, "--") != 0) {
        if (strcmp(d->meridian_flip, "FLIPPING") == 0) flip_mins = 0;
        if (p->lbl_flip_value) set_label_if_changed(p->lbl_flip_value, d->meridian_flip);
    } else if (p->lbl_flip_value) {
        set_label_if_changed(p->lbl_flip_value, "--");
    }

    /* The rim tick is the same countdown as a shape: twelve o'clock at flip,
     * climbing counter-clockwise over a four hour window. */
    if (p->ring_flip_tick) {
        ui_dial_set_tick(p->ring_flip_tick, flip_mins, UI_DIAL_FLIP_SPAN_MIN);
    }

    if (p->lbl_stars_value) {
        if (d->stars >= 0) {
            SET_LABEL_FMT_IF_CHANGED(p->lbl_stars_value, 16, "%d", d->stars);
        } else {
            set_label_if_changed(p->lbl_stars_value, "--");
        }
    }

    if (p->lbl_target_time_value) {
        set_label_if_changed(p->lbl_target_time_value,
            d->target_time_remaining[0] != '\0' ? d->target_time_remaining : "--");
        auto_fit_value_font(p->lbl_target_time_value);
    }

    // Update header to reflect the binding constraint (horizon, dawn, time, etc.)
    // Show "+" suffix when multiple conditions are active (e.g. time + horizon)
    if (p->lbl_target_time_header) {
        if (d->target_time_reason[0] != '\0') {
            if (d->target_condition_count > 1) {
                SET_LABEL_FMT_IF_CHANGED(p->lbl_target_time_header, 24, "%s+", d->target_time_reason);
            } else {
                SET_LABEL_FMT_IF_CHANGED(p->lbl_target_time_header, 24, "%s", d->target_time_reason);
            }
        } else {
            set_label_if_changed(p->lbl_target_time_header, "TIME LIMIT");
        }
    }
}

static void update_power(dashboard_page_t *p, const nina_client_t *d) {
    if (!p->box_pwr[0]) return;    /* the round board draws no power row */
    int pwr_idx = 0;
    bool sw = d->power.switch_connected;

    #define UPPER(buf) do { for (int _c = 0; (buf)[_c]; _c++) \
        if ((buf)[_c] >= 'a' && (buf)[_c] <= 'z') (buf)[_c] -= 32; } while(0)

    if (sw && pwr_idx < MAX_POWER_WIDGETS) {
        char title[32];
        strncpy(title, d->power.amps_name[0] ? d->power.amps_name : "Amps", sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        UPPER(title);
        set_label_if_changed(p->lbl_pwr_title[pwr_idx], title);
        SET_LABEL_FMT_IF_CHANGED(p->lbl_pwr_value[pwr_idx], 16, "%.2fA", d->power.total_amps);
        lv_obj_clear_flag(p->box_pwr[pwr_idx], LV_OBJ_FLAG_HIDDEN);
        pwr_idx++;
    }
    if (sw && pwr_idx < MAX_POWER_WIDGETS) {
        char title[32];
        strncpy(title, d->power.watts_name[0] ? d->power.watts_name : "Watts", sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        UPPER(title);
        set_label_if_changed(p->lbl_pwr_title[pwr_idx], title);
        SET_LABEL_FMT_IF_CHANGED(p->lbl_pwr_value[pwr_idx], 16, "%.1fW", d->power.total_watts);
        lv_obj_clear_flag(p->box_pwr[pwr_idx], LV_OBJ_FLAG_HIDDEN);
        pwr_idx++;
    }
    if (sw) {
        for (int i = 0; i < d->power.pwm_count && pwr_idx < MAX_POWER_WIDGETS; i++, pwr_idx++) {
            char title[32];
            strncpy(title, d->power.pwm_names[i], sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';
            UPPER(title);
            set_label_if_changed(p->lbl_pwr_title[pwr_idx], title);
            SET_LABEL_FMT_IF_CHANGED(p->lbl_pwr_value[pwr_idx], 16, "%.0f%%", d->power.pwm[i]);
            lv_obj_clear_flag(p->box_pwr[pwr_idx], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = pwr_idx; i < MAX_POWER_WIDGETS; i++) {
        lv_obj_add_flag(p->box_pwr[i], LV_OBJ_FLAG_HIDDEN);
    }

    #undef UPPER
}

static void update_stale_indicator(dashboard_page_t *p, const nina_client_t *d) {
    if (!p->lbl_stale) return;

    /* Nothing to compare against until the first successful poll */
    if (d->last_successful_poll_ms == 0) {
        lv_obj_add_flag(p->lbl_stale, LV_OBJ_FLAG_HIDDEN);
        if (p->stale_overlay)
            lv_obj_add_flag(p->stale_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t stale_ms = now_ms - d->last_successful_poll_ms;

    /* The "Last update" label floats at the page root's top-right corner; on
     * round that root is the full square panel, so the label sits well
     * outside the visible circle. A dimmed ring is the round stale cue
     * instead (below): the exposure ring on round layout 0, the sub-bar ledge
     * ring on round layout 1. Either way, skip formatting and showing a label
     * nobody can see (review C12 M-3, review area 2 I-1). */
    if (stale_ms > STALE_WARN_MS) {
        if (!p->ring_exposure && !p->subbar.ring) {
            int stale_sec = (int)(stale_ms / 1000);
            if (stale_sec >= 120)
                lv_label_set_text_fmt(p->lbl_stale, "Last update: %dm ago", stale_sec / 60);
            else
                lv_label_set_text_fmt(p->lbl_stale, "Last update: %ds ago", stale_sec);

            /* Stale color: dim for warning, bright for severe */
            uint32_t stale_color;
            if (theme_is_red_night(current_theme)) {
                stale_color = (stale_ms > STALE_DIM_MS) ? 0xff0000 : current_theme->text_color;
            } else {
                stale_color = (stale_ms > STALE_DIM_MS) ? 0xf87171 : 0xfbbf24;
            }
            set_text_color_if_changed(p->lbl_stale, lv_color_hex(stale_color), 0);

            lv_obj_clear_flag(p->lbl_stale, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(p->lbl_stale, LV_OBJ_FLAG_HIDDEN);
    }

    /* Dim overlay when data is very stale (> 2 min) */
    if (p->stale_overlay) {
        if (stale_ms > STALE_DIM_MS)
            lv_obj_clear_flag(p->stale_overlay, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(p->stale_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    /* Round: the stale cue is the exposure ring dimmed to 40 %, no text
     * (addendum ruling for radial board 1). */
    if (p->ring_exposure) {
        set_arc_opa_if_changed(p->ring_exposure,
            (stale_ms > STALE_WARN_MS) ? LV_OPA_40 : LV_OPA_COVER, LV_PART_INDICATOR);
    }

    /* Round layout 1 (Image-forward) has no exposure ring, so its stale cue is
     * the sub-bar ledge ring dimming the same way. The setter is a no-op on the
     * square flex-row sub bar and when the flag has not moved. */
    if (p->subbar.ring && !p->ring_exposure) {
        nina_subbar_set_stale(&p->subbar, stale_ms > STALE_WARN_MS);
    }

    /* A capture layout's own exposure ring or bar dims the same way, so every
     * round page tells the same story about stale data. */
    {
        lv_opa_t opa = (stale_ms > STALE_WARN_MS) ? LV_OPA_40 : LV_OPA_COVER;
        if (p->alt.arc_progress) {
            set_arc_opa_if_changed(p->alt.arc_progress, opa, LV_PART_INDICATOR);
        }
        if (p->alt.arc_progress_num) {
            set_arc_opa_if_changed(p->alt.arc_progress_num, opa, LV_PART_INDICATOR);
        }
        if (p->alt.bar_progress
            && lv_obj_get_style_bg_opa(p->alt.bar_progress, LV_PART_INDICATOR) != opa) {
            lv_obj_set_style_bg_opa(p->alt.bar_progress, opa, LV_PART_INDICATOR);
        }
        /* The caps dim with the band they ride, including round layout 0's,
         * whose ring is p->ring_exposure dimmed a few lines above. */
        set_bg_opa_if_changed(p->alt.cap_progress.obj, opa);
        set_bg_opa_if_changed(p->alt.cap_progress_num.obj, opa);
    }
}

/* Material Symbols codepoints (UTF-8 encoded) */
#define ICON_VERIFIED_USER  "\xee\xa3\xa8"  /* U+E8E8 — shield with check */
#define ICON_GPP_BAD        "\xef\x80\x92"  /* U+F012 — shield with X     */
#define ICON_GPP_MAYBE      "\xef\x80\x94"  /* U+F014 — shield with ?     */

static void update_safety_icon(dashboard_page_t *p, const nina_client_t *data, int inst) {
    if (!p->safety_icon && !p->ring_crown) return;

    if (!nina_connection_is_connected(inst)) {
        if (p->safety_icon) lv_obj_add_flag(p->safety_icon, LV_OBJ_FLAG_HIDDEN);
        if (p->ring_crown) {
            set_arc_color_if_changed(p->ring_crown, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
        }
        return;
    }

    if (p->safety_icon) lv_obj_clear_flag(p->safety_icon, LV_OBJ_FLAG_HIDDEN);

    int gb = app_config_get()->color_brightness;
    const char *icon;
    uint32_t color;
    if (data->safety_connected) {
        if (data->safety_is_safe) {
            icon  = ICON_VERIFIED_USER;
            color = theme_is_red_night(current_theme) ? 0x7f1d1d : 0x4CAF50;
        } else {
            icon  = ICON_GPP_BAD;
            color = theme_is_red_night(current_theme) ? 0xff0000 : 0xF44336;
        }
    } else {
        icon  = ICON_GPP_MAYBE;
        color = theme_is_red_night(current_theme) ? current_theme->label_color : 0x999999;
    }

    uint32_t dim = app_config_apply_brightness(color, gb);
    if (p->safety_icon) {
        set_label_if_changed(p->safety_icon, icon);
        set_text_color_if_changed(p->safety_icon, lv_color_hex(dim), 0);
    }
    /* Radial board 1: safety is the crown at twelve o'clock, read before
     * anything else and needing no glyph. */
    if (p->ring_crown) {
        set_arc_color_if_changed(p->ring_crown, lv_color_hex(dim), LV_PART_MAIN);
    }
}

/* ── Alternate layouts (nina_layout_alt.h) ──────────────────────────────── */

/* Arc-free twin of update_exposure_arc's clock bookkeeping.
 *
 * Layouts 1 and 2 have no lv_arc, but the 200 ms sub-bar tick needs the same
 * state the arc path maintains: the monotonic exposure anchor, the cached
 * end-epoch / total, the NINA clock pair and the ARC_GAP_GRACE_S 60 s
 * inter-exposure grace. This runs on the poll path with both locks held.
 * It touches no widgets. */
static void update_exposure_anchor(dashboard_page_t *p, const nina_client_t *d) {
    /* Filter change clears the anchor so the new filter starts from zero. */
    if (d->current_filter[0] != '\0' && strcmp(p->prev_filter, d->current_filter) != 0) {
        p->cached_end_epoch = 0;
        p->cached_total = 0;
        p->gap_start_epoch = 0;
        p->exp_anchor_us = 0;
        p->exp_anchor_elapsed = 0;
        p->cached_nina_epoch = 0;
        p->cached_nina_mono_us = 0;
        snprintf(p->prev_filter, sizeof(p->prev_filter), "%s", d->current_filter);
    }

    int64_t now_nina = nina_client_now_epoch(d);
    p->cached_nina_epoch = d->nina_clock_epoch;
    p->cached_nina_mono_us = d->nina_clock_mono_us;
    p->cached_is_exposing = d->is_exposing;

    if (d->exposure_total > 0 && d->exposure_end_epoch > 0 && d->is_exposing) {
        p->gap_start_epoch = 0;

        bool new_exposure = (d->exposure_end_epoch != p->cached_end_epoch
                             && d->exposure_end_epoch > now_nina)
                            || (p->exp_anchor_us == 0);
        bool same_exposure = (p->exp_anchor_us != 0
                              && d->exposure_end_epoch == p->cached_end_epoch);
        bool total_changed = (same_exposure && p->cached_total > 0.0f
                              && fabsf(p->cached_total - d->exposure_total) > 1.0f);

        p->cached_end_epoch = d->exposure_end_epoch;
        p->cached_total = d->exposure_total;

        if (new_exposure || total_changed) {
            /* Seed the anchor with a one-time wall estimate of how far into the
             * sub we already are (detection can land mid-sub). int64 epoch
             * difference first, then cast the small result to float. */
            int64_t remaining_seed_ms = (d->exposure_end_epoch - now_nina) * 1000;
            float seed = d->exposure_total - (float)remaining_seed_ms / 1000.0f;
            if (seed < 0.0f) seed = 0.0f;
            if (seed > d->exposure_total) seed = d->exposure_total;
            p->exp_anchor_us = esp_timer_get_time();
            p->exp_anchor_elapsed = seed;
        }
        return;
    }

    /* Not exposing: hold the last position through the inter-exposure gap, then
     * drop to idle once the grace window expires or the camera reports idle. */
    if (p->cached_end_epoch > 0 && p->cached_total > 0) {
        bool camera_idle = (strcmp(d->status, "Idle") == 0
                         || strcmp(d->status, "NoState") == 0
                         || strcmp(d->status, "OFFLINE") == 0);
        /* Gap timing is a device-only duration, not a NINA-timestamp compare. */
        int64_t now_wall = (int64_t)time(NULL);
        if (p->gap_start_epoch == 0) p->gap_start_epoch = now_wall;
        if (camera_idle || (now_wall - p->gap_start_epoch) > ARC_GAP_GRACE_S) {
            p->cached_end_epoch = 0;
            p->cached_total = 0;
            p->gap_start_epoch = 0;
            p->exp_anchor_us = 0;
            p->exp_anchor_elapsed = 0;
            p->cached_nina_epoch = 0;
            p->cached_nina_mono_us = 0;
        }
    } else {
        p->gap_start_epoch = 0;
        p->exp_anchor_us = 0;
        p->exp_anchor_elapsed = 0;
    }
}

/* Update path for every capture layout (1 to 4). NONE of the arc-path updaters
 * may run here: they dereference widgets these layouts never create. The shared
 * pieces (disconnected empty state, stale label, stale overlay) are driven
 * from this function so every layout inherits them. */
static void update_alt_layout_page(dashboard_page_t *p, const nina_client_t *d,
                                   int inst, int gb) {
    nina_conn_state_t conn_state = nina_connection_get_state(inst);

    if (conn_state != NINA_CONN_CONNECTED) {
        disc_gate_t *g = &s_disc_gate[inst];
        if (!g->valid || g->state != conn_state || g->theme != current_theme
            || g->gb != gb) {
            g->valid = true;
            g->state = conn_state;
            g->theme = current_theme;
            g->gb = gb;
            /* Force a full sub-bar rebuild on reconnect. */
            p->subbar.cached_target = -1;
            p->subbar.cached_done = -1;
            p->exp_anchor_us = 0;
            p->exp_anchor_elapsed = 0;
            p->cached_is_exposing = false;
            p->cached_end_epoch = 0;
            p->cached_total = 0;
            p->gap_start_epoch = 0;
            /* Park the hero digits and the exposure ring or bar too, so a page that
             * comes back from the offline overlay is not still showing the last
             * second count of the sub that was running when the rig went away. */
            alt_reset_elapsed(p);
            if (p->empty_state_cont) {
                char host[64] = {0};
                extract_host_from_url(app_config_get_instance_url(inst), host, sizeof(host));
                char offline_title[96];
                if (host[0]) {
                    snprintf(offline_title, sizeof(offline_title), "%s Offline", host);
                } else {
                    snprintf(offline_title, sizeof(offline_title), "Node %d Offline", inst + 1);
                }
                nina_empty_state_set_title(p->empty_state_cont, offline_title);
                nina_empty_state_show(p->empty_state_cont);
            }
        }
        update_stale_indicator(p, d);
        return;
    }

    /* Reconnect edge only — dismissing the overlay every poll is needless
     * LVGL churn (same rule as the arc path's !p->nina_connected block). */
    if (s_disc_gate[inst].valid) {
        s_disc_gate[inst].valid = false;
        if (p->empty_state_cont) nina_empty_state_hide(p->empty_state_cont);
    }

    update_exposure_anchor(p, d);

    /* The anchor drops when the camera goes idle or the inter-exposure grace
     * expires, which is not the same event as NINA reporting an exposure length
     * of zero. Without this the hero digits and the exposure ring would sit at
     * the last sub value all night, reading as an exposure that never ends. */
    if (p->cached_total <= 0.0f) alt_reset_elapsed(p);

    switch (p->layout) {
        case 1: nina_layout_image_update(p, d, inst, gb); break;
#if CONFIG_NINA_FAMILY_ROUND
        case 2: nina_layout_halo_update(p, d, inst, gb); break;
        case 4: nina_layout_orbit_update(p, d, inst, gb); break;
#endif
        default: break;
    }

#if CONFIG_NINA_FAMILY_ROUND
    /* The shared overlay last, so it repaints over whatever the layout just
     * wrote. It is a no-op on a page that never built one. */
    if (p->alt.ov.crown) nina_round_overlay_update(p, d, inst, gb);
#endif

    update_stale_indicator(p, d);
}

void update_nina_dashboard_page(int instance, const nina_client_t *data) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    if (!data) return;
    if (!nina_slot_available[instance]) return;

    dashboard_page_t *p = &pages[instance];
    if (!p->page) return;

    int inst = instance;     /* config lookups use the instance index directly */

    int gb = app_config_get()->color_brightness;

    /* Layout 1 builds none of the arc-path widgets. Every updater below
     * dereferences those widgets unconditionally, so the alt path must return
     * before any of them runs. */
    if (p->layout != 0) {
        update_alt_layout_page(p, data, inst, gb);
        return;
    }

    update_safety_icon(p, data, inst);

    nina_conn_state_t conn_state = nina_connection_get_state(inst);
    if (conn_state != NINA_CONN_CONNECTED) {
        update_disconnected_state(p, inst, gb, conn_state);
        update_stale_indicator(p, data);
        return;
    }

    /* Connected: drop the disconnect-side edge gate so the next disconnect
     * repaints the offline layout the reconnect block below is about to undo. */
    s_disc_gate[inst].valid = false;

    /* Reconnect restore: on the first CONNECTED poll after a disconnected state,
     * un-hide the header and arc and dismiss the branded empty-state overlay.
     * Gated on nina_connected so this only runs once per transition, not every
     * poll cycle (T-05-05: avoids per-poll LVGL churn). */
    if (!p->nina_connected) {
        if (p->header_box) {
            lv_obj_clear_flag(p->header_box, LV_OBJ_FLAG_HIDDEN);
        }
        if (p->arc_exposure) {
            lv_obj_clear_flag(p->arc_exposure, LV_OBJ_FLAG_HIDDEN);
        }
        if (p->empty_state_cont) {
            nina_empty_state_hide(p->empty_state_cont);
        }
    }

    update_header(p, data);
    update_sequence_info(p, data);
    update_exposure_arc(p, data, inst, gb);
    /* The round board's sub ring; NULL on the square bento layout. */
    if (p->subbar.cont) nina_subbar_update(&p->subbar, data, inst, gb);
    update_guider_stats(p, data, inst, gb);
    update_mount_and_image_stats(p, data);
    update_power(p, data);
    update_stale_indicator(p, data);

#if CONFIG_NINA_FAMILY_ROUND
    /* Round layout 0 carries the shared picture overlay on top of every widget
     * above, so it is pushed last, after the classic updaters have written
     * their own. The guard is the overlay's own crown handle, so this is a
     * no-op on a page that never built one. */
    if (p->alt.ov.crown) nina_round_overlay_update(p, data, inst, gb);
#endif
}
