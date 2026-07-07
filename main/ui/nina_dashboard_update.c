/**
 * @file nina_dashboard_update.c
 * @brief Data update functions and arc animation for the NINA dashboard.
 */

#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "nina_empty_state.h"
#include "nina_connection.h"
#include "app_config.h"
#include "themes.h"
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

/* Set shadow color only if it actually changed */
static inline void set_shadow_color_if_changed(lv_obj_t *obj, lv_color_t color, lv_style_selector_t sel) {
    if (!lv_color_eq(lv_obj_get_style_shadow_color(obj, sel), color))
        lv_obj_set_style_shadow_color(obj, color, sel);
}

/* Pick the largest font that fits the label's parent width */
static void auto_fit_value_font(lv_obj_t *label) {
    static const lv_font_t *fonts[] = {
        &lv_font_montserrat_48, &lv_font_montserrat_36,
        &lv_font_montserrat_32, &lv_font_montserrat_28,
    };
    const char *text = lv_label_get_text(label);
    int32_t letter_space = lv_obj_get_style_text_letter_space(label, 0);
    int32_t avail = lv_obj_get_content_width(lv_obj_get_parent(label));
    const lv_font_t *pick = fonts[sizeof(fonts) / sizeof(fonts[0]) - 1];
    for (int i = 0; i < (int)(sizeof(fonts) / sizeof(fonts[0])); i++) {
        lv_point_t size;
        lv_text_get_size(&size, text, fonts[i], letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= avail) {
            pick = fonts[i];
            break;
        }
    }
    if (lv_obj_get_style_text_font(label, 0) != pick)
        lv_obj_set_style_text_font(label, pick, 0);
}

/* Pick the largest font that fits the label's parent width (wider ladder for target names) */
static void auto_fit_target_name_font(lv_obj_t *label) {
    static const lv_font_t *fonts[] = {
        &lv_font_montserrat_48, &lv_font_montserrat_36,
        &lv_font_montserrat_32, &lv_font_montserrat_28,
        &lv_font_montserrat_24, &lv_font_montserrat_20,
        &lv_font_montserrat_16,
    };
    const char *text = lv_label_get_text(label);
    int32_t letter_space = lv_obj_get_style_text_letter_space(label, 0);
    int32_t avail = lv_obj_get_content_width(lv_obj_get_parent(label));
    const lv_font_t *pick = fonts[sizeof(fonts) / sizeof(fonts[0]) - 1];
    for (int i = 0; i < (int)(sizeof(fonts) / sizeof(fonts[0])); i++) {
        lv_point_t size;
        lv_text_get_size(&size, text, fonts[i], letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= avail) {
            pick = fonts[i];
            break;
        }
    }
    if (lv_obj_get_style_text_font(label, 0) != pick)
        lv_obj_set_style_text_font(label, pick, 0);
}

static void arc_start_exposure_anim(dashboard_page_t *p);

void arc_interp_timer_cb(lv_timer_t *timer) {
    dashboard_page_t *p = (dashboard_page_t *)lv_timer_get_user_data(timer);
    if (!p || !p->arc_exposure || p->arc_completing) return;
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
    lv_anim_t *existing = lv_anim_get(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);
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

static void update_disconnected_state(dashboard_page_t *p, int instance_idx, int gb, nina_conn_state_t conn_state) {
    const char *url = app_config_get_instance_url(instance_idx);
    char host[64] = {0};
    extract_host_from_url(url, host, sizeof(host));
    const char *state_text;
    if (!app_config_is_instance_enabled(instance_idx)) {
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
    set_label_if_changed(p->lbl_target_name, "----");
    auto_fit_target_name_font(p->lbl_target_name);
    set_label_if_changed(p->lbl_seq_container, "----");
    set_label_if_changed(p->lbl_seq_step, "----");
    set_label_if_changed(p->lbl_exposure_total, "");
    set_label_if_changed(p->lbl_loop_count, "");
    set_label_if_changed(p->lbl_exposure_current, "--");
    /* Drop any active exposure anchor so a stale anchor can't keep the 200ms
     * timer driving the arc while this instance is disconnected. */
    lv_anim_delete(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);
    p->exp_anchor_us = 0;
    p->exp_anchor_elapsed = 0;
    p->cached_is_exposing = false;
    p->arc_completing = false;
    p->cached_end_epoch = 0;
    p->cached_total = 0;
    p->gap_start_epoch = 0;
    p->cached_nina_epoch = 0;
    p->cached_nina_mono_us = 0;
    lv_arc_set_value(p->arc_exposure, 0);
    lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
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
    set_label_if_changed(p->lbl_flip_value, "--");
    set_label_if_changed(p->lbl_stars_value, "--");
    set_label_if_changed(p->lbl_target_time_value, "--");
    auto_fit_value_font(p->lbl_target_time_value);
    set_label_if_changed(p->lbl_target_time_header, "TIME LIMIT");
    for (int i = 0; i < MAX_POWER_WIDGETS; i++) {
        lv_obj_add_flag(p->box_pwr[i], LV_OBJ_FLAG_HIDDEN);
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
    // Telescope + camera on one line
    if (d->telescope_name[0] && d->camera_name[0]) {
        char buf[132];
        snprintf(buf, sizeof(buf), "%s | %s", d->telescope_name, d->camera_name);
        set_label_if_changed(p->lbl_instance_name, buf);
    } else if (d->telescope_name[0]) {
        set_label_if_changed(p->lbl_instance_name, d->telescope_name);
    } else if (d->camera_name[0]) {
        set_label_if_changed(p->lbl_instance_name, d->camera_name);
    } else {
        set_label_if_changed(p->lbl_instance_name, "N.I.N.A.");
    }

    set_label_if_changed(p->lbl_target_name, d->target_name[0] != '\0' ? d->target_name : "----");
    auto_fit_target_name_font(p->lbl_target_name);
}

static void update_sequence_info(dashboard_page_t *p, const nina_client_t *d) {
    set_label_if_changed(p->lbl_seq_container,
        d->container_name[0] != '\0' ? d->container_name : "----");
    set_label_if_changed(p->lbl_seq_step,
        d->container_step[0] != '\0' ? d->container_step : "----");
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
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
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

    // Detect filter change — reset arc state
    if (d->current_filter[0] != '\0' && strcmp(p->prev_filter, d->current_filter) != 0) {
        lv_anim_delete(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);
        p->arc_completing = false;
        p->cached_end_epoch = 0;
        p->cached_total = 0;
        p->gap_start_epoch = 0;
        p->exp_anchor_us = 0;
        p->exp_anchor_elapsed = 0;
        p->cached_nina_epoch = 0;
        p->cached_nina_mono_us = 0;
        lv_arc_set_value(p->arc_exposure, 0);
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
        lv_anim_delete(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);
        p->arc_completing = true;
        p->exp_anchor_us = 0;
        int current_fill = lv_arc_get_value(p->arc_exposure);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, p->arc_exposure);
        lv_anim_set_values(&a, current_fill, ARC_RANGE);
        lv_anim_set_time(&a, ARC_TRANSITION_MS);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    if (d->exposure_total > 0 && d->exposure_end_epoch > 0 && d->is_exposing) {
        // Camera is actively exposing
        p->gap_start_epoch = 0;  // Clear any gap timer

        // Show total exposure duration inside the arc
        int total_sec = (int)d->exposure_total;
        SET_LABEL_FMT_IF_CHANGED(p->lbl_exposure_current, 16, "%ds", total_sec);

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

            lv_anim_delete(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);
            /* Seed the arc value from the elapsed estimate so a mid-sub detection
             * does not snap back to zero. */
            int seed_val = (int)((seed * (float)ARC_RANGE) / d->exposure_total);
            if (seed_val < 0) seed_val = 0;
            if (seed_val > ARC_RANGE - 1) seed_val = ARC_RANGE - 1;
            lv_arc_set_value(p->arc_exposure, seed_val);

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

            lv_anim_delete(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);
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
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
        // Normal progress updates are handled by the 200ms timer (arc_interp_timer_cb)

        // Update exposure count labels
        if (d->exposure_iterations > 0) {
            SET_LABEL_FMT_IF_CHANGED(p->lbl_loop_count, 32, "x %d/%d",
                d->exposure_count, d->exposure_iterations);
        } else {
            set_label_if_changed(p->lbl_loop_count, "");
        }

        if (d->exposure_total_count > 0) {
            int total_secs = (int)(d->exposure_total_count * d->exposure_total);
            int h = total_secs / 3600;
            int m = (total_secs % 3600) / 60;
            if (h > 0) {
                SET_LABEL_FMT_IF_CHANGED(p->lbl_filter_done_value, 32, "%d / %dh %02dm",
                    d->exposure_total_count, h, m);
            } else {
                SET_LABEL_FMT_IF_CHANGED(p->lbl_filter_done_value, 32, "%d / %dm",
                    d->exposure_total_count, m);
            }
            set_text_color_if_changed(p->lbl_filter_done_value, lv_color_hex(filter_color), 0);
            lv_obj_clear_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
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
                set_label_if_changed(p->lbl_exposure_current, "--");
                lv_anim_delete(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);

                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, p->arc_exposure);
                lv_anim_set_values(&a, lv_arc_get_value(p->arc_exposure), 0);
                lv_anim_set_time(&a, 500);
                lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_start(&a);

                lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
            }
            // else: within grace period — do nothing, arc stays where it is
        } else {
            // Genuinely idle — no recent exposure data
            p->gap_start_epoch = 0;
            p->exp_anchor_us = 0;
            p->exp_anchor_elapsed = 0;
            set_label_if_changed(p->lbl_exposure_total, "");
            set_label_if_changed(p->lbl_loop_count, "");
            set_label_if_changed(p->lbl_exposure_current, "--");
            lv_arc_set_value(p->arc_exposure, 0);
            lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);
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
}

static void update_mount_and_image_stats(dashboard_page_t *p, const nina_client_t *d) {
    // Format flip time from "HH:MM:SS" to "Xh XXm"
    if (d->meridian_flip[0] != '\0' && strcmp(d->meridian_flip, "--") != 0
        && strcmp(d->meridian_flip, "FLIPPING") != 0) {
        int hh = 0, mm = 0;
        if (sscanf(d->meridian_flip, "%d:%d", &hh, &mm) >= 2) {
            SET_LABEL_FMT_IF_CHANGED(p->lbl_flip_value, 16, "%dh %02dm", hh, mm);
        } else {
            set_label_if_changed(p->lbl_flip_value, d->meridian_flip);
        }
    } else if (d->meridian_flip[0] != '\0' && strcmp(d->meridian_flip, "--") != 0) {
        set_label_if_changed(p->lbl_flip_value, d->meridian_flip);
    } else {
        set_label_if_changed(p->lbl_flip_value, "--");
    }

    if (d->stars >= 0) {
        SET_LABEL_FMT_IF_CHANGED(p->lbl_stars_value, 16, "%d", d->stars);
    } else {
        set_label_if_changed(p->lbl_stars_value, "--");
    }

    set_label_if_changed(p->lbl_target_time_value,
        d->target_time_remaining[0] != '\0' ? d->target_time_remaining : "--");
    auto_fit_value_font(p->lbl_target_time_value);

    // Update header to reflect the binding constraint (horizon, dawn, time, etc.)
    // Show "+" suffix when multiple conditions are active (e.g. time + horizon)
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

static void update_power(dashboard_page_t *p, const nina_client_t *d) {
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

    if (stale_ms > STALE_WARN_MS) {
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
}

/* Material Symbols codepoints (UTF-8 encoded) */
#define ICON_VERIFIED_USER  "\xee\xa3\xa8"  /* U+E8E8 — shield with check */
#define ICON_GPP_BAD        "\xef\x80\x92"  /* U+F012 — shield with X     */
#define ICON_GPP_MAYBE      "\xef\x80\x94"  /* U+F014 — shield with ?     */

static void update_safety_icon(dashboard_page_t *p, const nina_client_t *data, int inst) {
    if (!p->safety_icon) return;

    if (!nina_connection_is_connected(inst)) {
        lv_obj_add_flag(p->safety_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(p->safety_icon, LV_OBJ_FLAG_HIDDEN);

    int gb = app_config_get()->color_brightness;
    if (data->safety_connected) {
        if (data->safety_is_safe) {
            set_label_if_changed(p->safety_icon, ICON_VERIFIED_USER);
            set_text_color_if_changed(p->safety_icon,
                lv_color_hex(app_config_apply_brightness(
                    theme_is_red_night(current_theme) ? 0x7f1d1d : 0x4CAF50, gb)), 0);
        } else {
            set_label_if_changed(p->safety_icon, ICON_GPP_BAD);
            set_text_color_if_changed(p->safety_icon,
                lv_color_hex(app_config_apply_brightness(
                    theme_is_red_night(current_theme) ? 0xff0000 : 0xF44336, gb)), 0);
        }
    } else {
        set_label_if_changed(p->safety_icon, ICON_GPP_MAYBE);
        uint32_t unknown_color = theme_is_red_night(current_theme)
            ? current_theme->label_color : 0x999999;
        set_text_color_if_changed(p->safety_icon,
            lv_color_hex(app_config_apply_brightness(unknown_color, gb)), 0);
    }
}

void update_nina_dashboard_page(int instance, const nina_client_t *data) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    if (!data) return;
    if (!nina_slot_available[instance]) return;

    dashboard_page_t *p = &pages[instance];
    if (!p->page) return;

    int inst = instance;     /* config lookups use the instance index directly */

    int gb = app_config_get()->color_brightness;

    update_safety_icon(p, data, inst);

    nina_conn_state_t conn_state = nina_connection_get_state(inst);
    if (conn_state != NINA_CONN_CONNECTED) {
        update_disconnected_state(p, inst, gb, conn_state);
        update_stale_indicator(p, data);
        return;
    }

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
    update_guider_stats(p, data, inst, gb);
    update_mount_and_image_stats(p, data);
    update_power(p, data);
    update_stale_indicator(p, data);
}
