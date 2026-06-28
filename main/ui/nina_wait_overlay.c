/**
 * @file nina_wait_overlay.c
 * @brief Generic full-screen wait/loading overlay.
 *
 * Visually mirrors the OTA progress screen (nina_ota_prompt): a full-screen
 * theme-colored cover with a centered title/subtitle and an OTA-style progress
 * bar (dim glow behind a brighter main bar) plus an optional large percentage
 * label.
 *
 * It supports two modes:
 *   - Determinate:   nina_wait_overlay_set_progress(0..100) — fills the bar and
 *                    shows "NN%".
 *   - Indeterminate: nina_wait_overlay_set_progress(-1) — pulses the bar outward
 *                    from the center in both directions and hides the
 *                    percentage. This is the default after _show() because the
 *                    solar download/decode time is not known in advance.
 *
 * Threading: every function here runs under the display lock held by the
 * CALLER (dashboard init, theme apply, goes_poll_task with an explicit
 * bsp_display_lock, or data_update_task). Do NOT take the display lock here —
 * matches the nina_ota_prompt convention.
 */

#include "nina_wait_overlay.h"
#include "nina_dashboard_internal.h"
#include "nina_nav_arbiter.h"
#include "app_config.h"
#include "display_defs.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

/* ── Widget pointers ────────────────────────────────────────────────── */
static lv_obj_t *wait_overlay = NULL;
static lv_obj_t *lbl_title    = NULL;
static lv_obj_t *lbl_subtitle = NULL;
static lv_obj_t *lbl_percent  = NULL;
static lv_obj_t *bar_progress = NULL;
static lv_obj_t *bar_glow     = NULL;

static bool indeterminate = false;

/* ── Stall-timer state ──────────────────────────────────────────────── */

/* One-shot 15 s watchdog that auto-dismisses the overlay if loading stalls.
 * Created in nina_wait_overlay_show, deleted in nina_wait_overlay_hide.
 * Always set to NULL after every lv_timer_delete (double-free guard). */
static lv_timer_t *s_stall_timer = NULL;

/* The page to return to when the overlay is dismissed by swipe or timeout.
 * Set via nina_wait_overlay_set_prior_page() before show(). */
static int s_prior_page = -1;

/* Forward declaration so stall_timer_cb can be defined before show/hide. */
static void stall_timer_cb(lv_timer_t *timer);

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Dim a color to ~30% brightness for bar track / glow (matches OTA prompt). */
static uint32_t dim_color(uint32_t c) {
    uint8_t r = ((c >> 16) & 0xFF) * 30 / 100;
    uint8_t g = ((c >> 8)  & 0xFF) * 30 / 100;
    uint8_t b = ((c)       & 0xFF) * 30 / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Indeterminate pulse: drive both bars together so the glow tracks the main
 * bar. The animation value `v` is the total span (0..100) and we expand it
 * symmetrically around the center (50), so the indicator grows outward in both
 * directions from the middle. lv_anim's playback (yoyo) then collapses it back
 * to the center and repeats. Both bars are in LV_BAR_MODE_RANGE. */
static void pulse_anim_cb(void *obj, int32_t v) {
    LV_UNUSED(obj);
    int32_t half  = v / 2;
    int32_t start = 50 - half;
    int32_t end   = 50 + half;
    if (bar_progress) {
        lv_bar_set_start_value(bar_progress, start, LV_ANIM_OFF);
        lv_bar_set_value(bar_progress, end, LV_ANIM_OFF);
    }
    if (bar_glow) {
        lv_bar_set_start_value(bar_glow, start, LV_ANIM_OFF);
        lv_bar_set_value(bar_glow, end, LV_ANIM_OFF);
    }
}

static void start_pulse(void) {
    if (!bar_progress) return;
    indeterminate = true;
    if (lbl_percent) lv_obj_add_flag(lbl_percent, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar_progress);          /* var is only an animation key */
    lv_anim_set_values(&a, 0, 100);             /* span: collapsed -> full width */
    lv_anim_set_duration(&a, 900);
    lv_anim_set_playback_duration(&a, 900);     /* sweep back */
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_start(&a);
}

static void stop_pulse(void) {
    indeterminate = false;
    /* Delete by the var+exec_cb pair used in start_pulse. */
    if (bar_progress) lv_anim_delete(bar_progress, pulse_anim_cb);
}

/* ── Create ─────────────────────────────────────────────────────────── */

void nina_wait_overlay_create(lv_obj_t *parent) {
    wait_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(wait_overlay);
    lv_obj_set_size(wait_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(wait_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wait_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(wait_overlay, LV_OBJ_FLAG_HIDDEN);
    /* Eat taps but let swipes bubble up to the screen's gesture_event_cb so
     * a horizontal swipe can cancel the load and return to the prior page. */
    lv_obj_add_flag(wait_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_flex_flow(wait_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wait_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wait_overlay, 16, 0);
    lv_obj_clear_flag(wait_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lbl_title = lv_label_create(wait_overlay);
    lv_label_set_text(lbl_title, "");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_title, LV_PCT(90));

    lbl_subtitle = lv_label_create(wait_overlay);
    lv_label_set_text(lbl_subtitle, "");
    lv_obj_set_style_text_font(lbl_subtitle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_subtitle, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_align(lbl_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_subtitle, LV_PCT(90));

    lbl_percent = lv_label_create(wait_overlay);
    lv_label_set_text(lbl_percent, "0%");
    lv_obj_set_style_text_font(lbl_percent, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_percent, lv_color_hex(0x3b82f6), 0);
    lv_obj_set_style_text_align(lbl_percent, LV_TEXT_ALIGN_CENTER, 0);

    /* Bar container — holds glow + main bar overlapping (mirrors OTA prompt). */
    lv_obj_t *bar_wrap = lv_obj_create(wait_overlay);
    lv_obj_remove_style_all(bar_wrap);
    lv_obj_set_size(bar_wrap, 580, 28);
    lv_obj_clear_flag(bar_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(bar_wrap, 16, 0);

    bar_glow = lv_bar_create(bar_wrap);
    lv_obj_remove_style_all(bar_glow);
    lv_obj_set_size(bar_glow, 580, 24);
    lv_bar_set_range(bar_glow, 0, 100);
    lv_bar_set_mode(bar_glow, LV_BAR_MODE_RANGE);   /* span between start..value */
    lv_bar_set_start_value(bar_glow, 50, LV_ANIM_OFF);
    lv_bar_set_value(bar_glow, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(bar_glow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_glow, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_glow, lv_color_hex(0x3b82f6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_glow, LV_OPA_40, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_glow, 12, LV_PART_INDICATOR);
    lv_obj_center(bar_glow);

    bar_progress = lv_bar_create(bar_wrap);
    lv_obj_remove_style_all(bar_progress);
    lv_obj_set_size(bar_progress, 560, 12);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_mode(bar_progress, LV_BAR_MODE_RANGE);   /* span between start..value */
    lv_bar_set_start_value(bar_progress, 50, LV_ANIM_OFF);
    lv_bar_set_value(bar_progress, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_progress, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x3b82f6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 6, LV_PART_INDICATOR);
    lv_obj_center(bar_progress);
}

/* ── Public API ─────────────────────────────────────────────────────── */

/* One-shot stall watchdog callback.  This runs inside the LVGL tick which
 * already holds the LVGL tick lock — do NOT call bsp_display_lock /
 * lvgl_port_lock here.  nav_arbiter_submit_user() is lock-safe (atomic
 * write + task notify) and is the ONLY navigation path used here. */
static void stall_timer_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    /* Null the pointer first: the just-fired one-shot is already deleted by
     * LVGL, and nina_wait_overlay_hide() will guard-check before re-deleting. */
    s_stall_timer = NULL;
    int prior = s_prior_page;
    s_prior_page = -1;
    nina_wait_overlay_hide();
    if (prior >= 0) {
        nav_arbiter_submit_user(prior, esp_timer_get_time() / 1000, -1);
    }
}

void nina_wait_overlay_show(const char *title, const char *subtitle) {
    if (!wait_overlay) return;

    nina_wait_overlay_apply_theme();

    lv_label_set_text(lbl_title, title ? title : "");
    if (subtitle && subtitle[0]) {
        lv_label_set_text(lbl_subtitle, subtitle);
        lv_obj_clear_flag(lbl_subtitle, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl_subtitle, "");
        lv_obj_add_flag(lbl_subtitle, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(wait_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Default to the indeterminate pulse — the download/decode time is unknown. */
    nina_wait_overlay_set_progress(-1);

    /* Arm the 15-second one-shot stall watchdog.  Delete any previous timer
     * first so a rapid show->show sequence does not leak a timer handle. */
    if (s_stall_timer) {
        lv_timer_delete(s_stall_timer);
        s_stall_timer = NULL;
    }
    s_stall_timer = lv_timer_create(stall_timer_cb, 15000, NULL);
    lv_timer_set_repeat_count(s_stall_timer, 1);
}

void nina_wait_overlay_set_progress(int percent) {
    if (!wait_overlay) return;

    if (percent < 0) {
        /* Indeterminate — start (or keep) the pulse, hide the percentage. */
        if (!indeterminate) start_pulse();
        return;
    }

    if (indeterminate) stop_pulse();
    if (percent > 100) percent = 100;

    if (lbl_percent) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        lv_label_set_text(lbl_percent, buf);
        lv_obj_clear_flag(lbl_percent, LV_OBJ_FLAG_HIDDEN);
    }
    /* Determinate: fill from the left edge, so reset the range start to 0
     * (the indeterminate pulse leaves it centered). */
    if (bar_progress) {
        lv_bar_set_start_value(bar_progress, 0, LV_ANIM_OFF);
        lv_bar_set_value(bar_progress, percent, LV_ANIM_ON);
    }
    if (bar_glow) {
        lv_bar_set_start_value(bar_glow, 0, LV_ANIM_OFF);
        lv_bar_set_value(bar_glow, percent, LV_ANIM_ON);
    }
}

void nina_wait_overlay_hide(void) {
    if (!wait_overlay) return;
    stop_pulse();
    /* Cancel the stall watchdog so a successful load does not trigger a
     * delayed nav-away.  Guard the delete with a NULL check (the callback
     * itself already nulled the pointer before calling here). */
    if (s_stall_timer) {
        lv_timer_delete(s_stall_timer);
        s_stall_timer = NULL;
    }
    lv_obj_add_flag(wait_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool nina_wait_overlay_visible(void) {
    if (!wait_overlay) return false;
    return !lv_obj_has_flag(wait_overlay, LV_OBJ_FLAG_HIDDEN);
}

void nina_wait_overlay_set_prior_page(int page) {
    s_prior_page = page;
}

void nina_wait_overlay_cancel(void) {
    /* Hide (also deletes the stall timer) then navigate back to the prior page
     * via the arbiter.  Caller holds the display lock; nav_arbiter_submit_user
     * is lock-safe and must not be wrapped in another lock call. */
    nina_wait_overlay_hide();
    int prior = s_prior_page;
    s_prior_page = -1;
    if (prior >= 0) {
        nav_arbiter_submit_user(prior, esp_timer_get_time() / 1000, -1);
    }
}

void nina_wait_overlay_apply_theme(void) {
    if (!wait_overlay || !current_theme) return;
    int gb = app_config_get()->color_brightness;

    uint32_t accent = app_config_apply_brightness(current_theme->progress_color, gb);
    uint32_t text   = app_config_apply_brightness(current_theme->text_color, gb);
    uint32_t label  = app_config_apply_brightness(current_theme->label_color, gb);
    uint32_t bg     = current_theme->bg_main;

    lv_obj_set_style_bg_color(wait_overlay, lv_color_hex(bg), 0);

    if (lbl_title)
        lv_obj_set_style_text_color(lbl_title, lv_color_hex(text), 0);
    if (lbl_subtitle)
        lv_obj_set_style_text_color(lbl_subtitle, lv_color_hex(label), 0);
    if (lbl_percent)
        lv_obj_set_style_text_color(lbl_percent, lv_color_hex(accent), 0);
    if (bar_glow)
        lv_obj_set_style_bg_color(bar_glow, lv_color_hex(accent), LV_PART_INDICATOR);
    if (bar_progress) {
        lv_obj_set_style_bg_color(bar_progress, lv_color_hex(dim_color(accent)), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_progress, lv_color_hex(accent), LV_PART_INDICATOR);
    }

    lv_obj_invalidate(wait_overlay);
}
