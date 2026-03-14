/**
 * @file nina_spotify.c
 * @brief Spotify immersive player page — full-screen album art, playback controls, idle management.
 *
 * Layers (bottom to top):
 *   1. lv_image: Album art (720x720 or 640x640 scaled to fill)
 *   2. lv_obj:   Dim overlay (semi-transparent black, animated opacity 0..190)
 *   3. lv_obj:   Track info container (title, artist, album — center-upper)
 *   4. lv_bar:   Progress bar + time labels (lower area)
 *   5. lv_obj:   Button row (prev/play-pause/next — bottom, no card)
 *
 * Idle timer hides overlay after 5s of inactivity; tap anywhere to show.
 * Playback controls send actions via xQueueSend to spotify_action_queue.
 * LVGL refresh rate is throttled to 5000ms when idle, 33ms when active.
 */

#include "nina_spotify.h"
#include "nina_dashboard_internal.h"
#include "display_defs.h"
#include "themes.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Embedded Spotify logo PNG (linked via EMBED_FILES) */
extern const uint8_t spotify_logo_png_start[] asm("_binary_spotify_logo_png_start");
extern const uint8_t spotify_logo_png_end[]   asm("_binary_spotify_logo_png_end");

static const char *TAG = "spotify_ui";

/* ── Layout constants ────────────────────────────────────────────────── */

#define IDLE_TIMEOUT_MS_DEFAULT 5000
#define DIM_OPACITY_ACTIVE  190       /* ~75% black overlay when controls shown */
#define DIM_OPACITY_IDLE    0         /* No overlay when idle (just album art) */
#define SIDE_MARGIN         60        /* Left/right margin for progress bar */
#define BTN_BOTTOM_MARGIN   48        /* Bottom margin for button row */
#define PROGRESS_Y          500       /* Y position for progress bar */
#define REFR_PERIOD_IDLE_MS 5000      /* Slow LVGL refresh when showing static art */
#define REFR_PERIOD_ACTIVE_MS 33      /* Normal ~30fps refresh when controls visible */

/* ── Static widget pointers ──────────────────────────────────────────── */

static lv_obj_t *spotify_page = NULL;
static lv_obj_t *img_album_art = NULL;
static lv_obj_t *dim_overlay = NULL;
static lv_obj_t *track_info_cont = NULL;
static lv_obj_t *lbl_track_title = NULL;
static lv_obj_t *lbl_artist_name = NULL;
static lv_obj_t *lbl_album_name = NULL;
static lv_obj_t *controls_zone = NULL;  /* Transparent click-absorber for bottom area */
static lv_obj_t *btn_row = NULL;
static lv_obj_t *btn_prev = NULL;
static lv_obj_t *btn_play_pause = NULL;
static lv_obj_t *btn_next = NULL;
static lv_obj_t *bar_progress = NULL;
static lv_obj_t *lbl_time_elapsed = NULL;
static lv_obj_t *lbl_time_total = NULL;

static lv_obj_t *loading_logo = NULL;      /* Spotify logo shown while loading */
static lv_image_dsc_t logo_dsc;            /* Persistent descriptor for logo PNG */

static lv_timer_t *idle_timer = NULL;
static bool is_idle = true;
static bool is_playing = false;
static bool has_art = false;               /* True once album art has been set */
static uint8_t *current_art_buf = NULL;    /* Owned RGB565 buffer */
static lv_image_dsc_t art_dsc;             /* Persistent image descriptor */

/* ── Forward declarations ────────────────────────────────────────────── */

static void create_track_info_container(void);
static void create_controls(void);
static void set_idle_state(bool idle);
static void dim_anim_cb(void *var, int32_t value);
static void idle_timer_cb(lv_timer_t *timer);
static void page_touch_cb(lv_event_t *e);
static void play_pause_click_cb(lv_event_t *e);
static void prev_click_cb(lv_event_t *e);
static void next_click_cb(lv_event_t *e);
static void set_refr_period(uint32_t period_ms);

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void set_refr_period(uint32_t period_ms)
{
    lv_timer_t *timer = lv_display_get_refr_timer(lv_display_get_default());
    if (timer) {
        lv_timer_set_period(timer, period_ms);
    }
}

/* Pulse animation: breathe opacity between 80 and 255 */
static void pulse_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_image_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void start_logo_pulse(void)
{
    if (!loading_logo) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, loading_logo);
    lv_anim_set_values(&a, 80, 255);
    lv_anim_set_time(&a, 1500);
    lv_anim_set_playback_time(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, pulse_opa_cb);
    lv_anim_start(&a);
}

static void stop_logo_pulse(void)
{
    if (!loading_logo) return;
    lv_anim_delete(loading_logo, pulse_opa_cb);
}

/** Update label text only if it changed — avoids restarting scroll animation. */
static void label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

/* ── Page creation ───────────────────────────────────────────────────── */

lv_obj_t *spotify_page_create(lv_obj_t *parent)
{
    spotify_page = lv_obj_create(parent);
    lv_obj_set_size(spotify_page, SCREEN_SIZE, SCREEN_SIZE);
    /* Negate the parent's OUTER_PADDING so album art fills edge-to-edge */
    lv_obj_set_pos(spotify_page, -OUTER_PADDING, -OUTER_PADDING);
    lv_obj_set_style_pad_all(spotify_page, 0, 0);
    lv_obj_set_style_border_width(spotify_page, 0, 0);
    lv_obj_set_style_radius(spotify_page, 0, 0);
    lv_obj_set_style_bg_color(spotify_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(spotify_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(spotify_page, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Album art image (full-screen, scaled from top-left to fill 720x720) */
    img_album_art = lv_image_create(spotify_page);
    lv_obj_set_pos(img_album_art, 0, 0);
    lv_image_set_pivot(img_album_art, 0, 0);
    lv_obj_add_flag(img_album_art, LV_OBJ_FLAG_HIDDEN); /* Hidden until art arrives */

    /* 1b. Loading indicator — Spotify logo PNG, centered, pulsing.
     * lodepng decoder (CONFIG_LV_USE_LODEPNG) handles decoding from
     * the embedded binary.  We pass the raw PNG data as an lv_image_dsc_t. */
    {
        size_t png_size = (size_t)(spotify_logo_png_end - spotify_logo_png_start);
        memset(&logo_dsc, 0, sizeof(logo_dsc));
        logo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        logo_dsc.header.cf = LV_COLOR_FORMAT_RAW;
        logo_dsc.data = spotify_logo_png_start;
        logo_dsc.data_size = (uint32_t)png_size;

        loading_logo = lv_image_create(spotify_page);
        lv_image_set_src(loading_logo, &logo_dsc);
        lv_obj_center(loading_logo);
        lv_obj_remove_flag(loading_logo, LV_OBJ_FLAG_CLICKABLE);
        has_art = false;
        start_logo_pulse();
    }

    /* 2. Dim overlay (semi-transparent black, animated) */
    dim_overlay = lv_obj_create(spotify_page);
    lv_obj_set_size(dim_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(dim_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dim_overlay, 0, 0);
    lv_obj_set_style_radius(dim_overlay, 0, 0);
    lv_obj_remove_flag(dim_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dim_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* 3. Track info container (centered upper area) */
    create_track_info_container();

    /* 4. Progress bar + time labels + buttons */
    create_controls();

    /* Touch handler on entire page */
    lv_obj_add_event_cb(spotify_page, page_touch_cb, LV_EVENT_CLICKED, NULL);

    /* Idle timer (paused initially, started when page shown).
     * Timeout comes from config; 0 = never auto-hide. */
    uint8_t timeout_s = app_config_get()->spotify_overlay_timeout_s;
    uint32_t timeout_ms = timeout_s > 0 ? (uint32_t)timeout_s * 1000 : IDLE_TIMEOUT_MS_DEFAULT;
    idle_timer = lv_timer_create(idle_timer_cb, timeout_ms, NULL);
    lv_timer_pause(idle_timer);

    /* Start in idle state (overlay hidden, just album art) */
    set_idle_state(true);

    return spotify_page;
}

/* ── Track info container (center-upper area) ────────────────────────── */

static void create_track_info_container(void)
{
    track_info_cont = lv_obj_create(spotify_page);
    lv_obj_set_size(track_info_cont, 600, LV_SIZE_CONTENT);
    lv_obj_align(track_info_cont, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_flex_flow(track_info_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(track_info_cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(track_info_cont, 12, 0);
    lv_obj_set_style_bg_opa(track_info_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(track_info_cont, 0, 0);
    lv_obj_remove_flag(track_info_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track_info_cont, LV_OBJ_FLAG_CLICKABLE);

    /* Track title (large, white, bold) */
    lbl_track_title = lv_label_create(track_info_cont);
    lv_label_set_text(lbl_track_title, "");
    lv_obj_set_style_text_font(lbl_track_title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_track_title, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl_track_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_track_title, 580);
    lv_label_set_long_mode(lbl_track_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    /* Smooth scroll: 60 px/s, clamped 1-20s — avoids default jerky 40px/s */
    lv_obj_set_style_anim_duration(lbl_track_title,
        lv_anim_speed_clamped(60, 1000, 20000), 0);

    /* Artist name (medium, light gray) */
    lbl_artist_name = lv_label_create(track_info_cont);
    lv_label_set_text(lbl_artist_name, "");
    lv_obj_set_style_text_font(lbl_artist_name, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_artist_name, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_align(lbl_artist_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_artist_name, 580);
    lv_label_set_long_mode(lbl_artist_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(lbl_artist_name,
        lv_anim_speed_clamped(60, 1000, 20000), 0);

    /* Album name (same size as artist, dim gray, letter-spaced) */
    lbl_album_name = lv_label_create(track_info_cont);
    lv_label_set_text(lbl_album_name, "");
    lv_obj_set_style_text_font(lbl_album_name, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_album_name, lv_color_make(0x77, 0x77, 0x77), 0);
    lv_obj_set_style_text_align(lbl_album_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(lbl_album_name, 2, 0);
    lv_obj_set_width(lbl_album_name, 580);
    lv_label_set_long_mode(lbl_album_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(lbl_album_name,
        lv_anim_speed_clamped(60, 1000, 20000), 0);
}

/* ── Progress bar + time labels + buttons ────────────────────────────── */

static void create_controls(void)
{
    /* Transparent click-absorbing zone covering the bottom controls area.
     * Prevents taps on progress bar, time labels, buttons, or gaps between
     * them from bubbling up to the page touch handler (which toggles overlay). */
    int zone_y = PROGRESS_Y - 16;   /* Start slightly above progress bar */
    int zone_h = 236;              /* Enough for: progress bar + time labels + buttons */
    controls_zone = lv_obj_create(spotify_page);
    lv_obj_set_pos(controls_zone, 0, zone_y);
    lv_obj_set_size(controls_zone, SCREEN_SIZE, zone_h);
    lv_obj_set_style_bg_opa(controls_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls_zone, 0, 0);
    lv_obj_set_style_pad_all(controls_zone, 0, 0);
    lv_obj_remove_flag(controls_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(controls_zone, LV_OBJ_FLAG_CLICKABLE);

    /* Progress bar — full-width thin line */
    bar_progress = lv_bar_create(controls_zone);
    lv_obj_set_size(bar_progress, SCREEN_SIZE - (SIDE_MARGIN * 2), 4);
    lv_obj_set_pos(bar_progress, SIDE_MARGIN, 16);  /* 16px below zone top */
    lv_obj_set_style_bg_color(bar_progress, lv_color_make(0x44, 0x44, 0x44), 0);
    lv_obj_set_style_bg_color(bar_progress, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 2, 0);
    lv_obj_set_style_radius(bar_progress, 2, LV_PART_INDICATOR);
    lv_bar_set_range(bar_progress, 0, 1000);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);

    /* Time labels below progress bar */
    lbl_time_elapsed = lv_label_create(controls_zone);
    lv_obj_set_style_text_font(lbl_time_elapsed, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_time_elapsed, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_label_set_text(lbl_time_elapsed, "0:00");
    lv_obj_align_to(lbl_time_elapsed, bar_progress, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    lbl_time_total = lv_label_create(controls_zone);
    lv_obj_set_style_text_font(lbl_time_total, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_time_total, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_label_set_text(lbl_time_total, "0:00");
    lv_obj_align_to(lbl_time_total, bar_progress, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 6);

    /* Button row — centered at bottom of zone, no background card */
    btn_row = lv_obj_create(controls_zone);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, -BTN_BOTTOM_MARGIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 32, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Previous button */
    btn_prev = lv_button_create(btn_row);
    lv_obj_set_size(btn_prev, 64, 64);
    lv_obj_set_style_radius(btn_prev, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_bg_opa(btn_prev, LV_OPA_70, 0);
    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(lbl_prev, lv_color_white(), 0);
    lv_obj_center(lbl_prev);
    lv_obj_add_event_cb(btn_prev, prev_click_cb, LV_EVENT_CLICKED, NULL);

    /* Play/Pause button (larger, white circle) */
    btn_play_pause = lv_button_create(btn_row);
    lv_obj_set_size(btn_play_pause, 88, 88);
    lv_obj_set_style_radius(btn_play_pause, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_play_pause, lv_color_white(), 0);
    lv_obj_t *lbl_pp = lv_label_create(btn_play_pause);
    lv_label_set_text(lbl_pp, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(lbl_pp, lv_color_black(), 0);
    lv_obj_set_style_text_font(lbl_pp, &lv_font_montserrat_22, 0);
    lv_obj_center(lbl_pp);
    lv_obj_add_event_cb(btn_play_pause, play_pause_click_cb, LV_EVENT_CLICKED, NULL);

    /* Next button */
    btn_next = lv_button_create(btn_row);
    lv_obj_set_size(btn_next, 64, 64);
    lv_obj_set_style_radius(btn_next, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_bg_opa(btn_next, LV_OPA_70, 0);
    lv_obj_t *lbl_nxt = lv_label_create(btn_next);
    lv_label_set_text(lbl_nxt, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(lbl_nxt, lv_color_white(), 0);
    lv_obj_center(lbl_nxt);
    lv_obj_add_event_cb(btn_next, next_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Idle / active state management ──────────────────────────────────── */

static void dim_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void set_idle_state(bool idle)
{
    if (is_idle == idle) return;
    is_idle = idle;

    lv_opa_t target_opa = idle ? DIM_OPACITY_IDLE : DIM_OPACITY_ACTIVE;

    /* Animate dim overlay opacity */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dim_overlay);
    lv_anim_set_values(&a, lv_obj_get_style_bg_opa(dim_overlay, 0), target_opa);
    lv_anim_set_time(&a, 400);
    lv_anim_set_exec_cb(&a, dim_anim_cb);
    lv_anim_start(&a);

    /* Show/hide UI elements */
    if (idle) {
        lv_obj_add_flag(track_info_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(controls_zone, LV_OBJ_FLAG_HIDDEN);
        /* Throttle refresh rate when idle — but only if we have album art.
         * During loading (pulse animation), keep normal refresh rate. */
        if (has_art) {
            set_refr_period(REFR_PERIOD_IDLE_MS);
        }
    } else {
        lv_obj_remove_flag(track_info_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(controls_zone, LV_OBJ_FLAG_HIDDEN);
        /* Restore normal refresh rate for animations and controls */
        set_refr_period(REFR_PERIOD_ACTIVE_MS);
    }
}

static void idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    set_idle_state(true);
    lv_timer_pause(idle_timer);
}

static void page_touch_cb(lv_event_t *e)
{
    (void)e;
    if (is_idle) {
        /* Tap to show overlay */
        set_idle_state(false);
        /* Start idle timer (skip if timeout is 0 = always visible) */
        uint8_t timeout_s = app_config_get()->spotify_overlay_timeout_s;
        if (timeout_s > 0) {
            lv_timer_set_period(idle_timer, (uint32_t)timeout_s * 1000);
            lv_timer_reset(idle_timer);
            lv_timer_resume(idle_timer);
        }
    } else {
        /* Tap to dismiss overlay */
        set_idle_state(true);
        lv_timer_pause(idle_timer);
    }
}

/* ── Playback control callbacks ──────────────────────────────────────── */

static void play_pause_click_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    spotify_action_t action = is_playing ? SPOTIFY_ACTION_PAUSE : SPOTIFY_ACTION_PLAY;
    xQueueSend(spotify_action_queue, &action, 0); /* Don't block LVGL thread */

    /* Toggle local state immediately for responsive UI */
    is_playing = !is_playing;
    lv_obj_t *pp_label = lv_obj_get_child(btn_play_pause, 0);
    if (pp_label) {
        lv_label_set_text(pp_label, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void prev_click_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    spotify_action_t action = SPOTIFY_ACTION_PREV;
    xQueueSend(spotify_action_queue, &action, 0);
}

static void next_click_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    spotify_action_t action = SPOTIFY_ACTION_NEXT;
    xQueueSend(spotify_action_queue, &action, 0);
}

/* ── Public API: update ──────────────────────────────────────────────── */

void nina_spotify_update(const spotify_playback_t *data)
{
    if (!spotify_page || !data) return;

    is_playing = data->is_playing;

    /* Update track info labels — only if changed, to avoid restarting scroll */
    label_set_text_if_changed(lbl_track_title, data->track_title);
    label_set_text_if_changed(lbl_artist_name, data->artist_name);
    label_set_text_if_changed(lbl_album_name, data->album_name);

    /* Update play/pause icon */
    lv_obj_t *pp_label = lv_obj_get_child(btn_play_pause, 0);
    if (pp_label) {
        lv_label_set_text(pp_label, data->is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    /* Update progress bar and time labels */
    if (data->duration_ms > 0) {
        /* Interpolate progress based on time since fetch */
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t elapsed_since_fetch = now_ms - data->fetched_at_ms;
        int current_progress = data->progress_ms;
        if (data->is_playing && elapsed_since_fetch > 0) {
            current_progress += (int)elapsed_since_fetch;
            if (current_progress > data->duration_ms) {
                current_progress = data->duration_ms;
            }
        }

        int bar_val = (int)((int64_t)current_progress * 1000 / data->duration_ms);
        lv_bar_set_value(bar_progress, bar_val, LV_ANIM_ON);

        /* Elapsed time label */
        char buf[16];
        int elapsed_s = current_progress / 1000;
        snprintf(buf, sizeof(buf), "%d:%02d", elapsed_s / 60, elapsed_s % 60);
        lv_label_set_text(lbl_time_elapsed, buf);

        /* Total time label */
        int total_s = data->duration_ms / 1000;
        snprintf(buf, sizeof(buf), "%d:%02d", total_s / 60, total_s % 60);
        lv_label_set_text(lbl_time_total, buf);
    }
}

/* ── Public API: album art ───────────────────────────────────────────── */

void nina_spotify_set_album_art(const uint8_t *rgb565_data, uint32_t w, uint32_t h,
                                 uint32_t data_size)
{
    if (!img_album_art) return;

    /* Free previous buffer */
    if (current_art_buf) {
        free(current_art_buf);
        current_art_buf = NULL;
    }

    if (!rgb565_data || data_size == 0) {
        /* Don't pass NULL to lv_image_set_src -- hide the widget instead */
        lv_obj_add_flag(img_album_art, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(img_album_art, LV_OBJ_FLAG_HIDDEN);

    /* Take ownership of the buffer */
    current_art_buf = (uint8_t *)rgb565_data;

    /* Set up LVGL image descriptor */
    memset(&art_dsc, 0, sizeof(art_dsc));
    art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    art_dsc.header.w = w;
    art_dsc.header.h = h;
    art_dsc.header.stride = w * 2; /* RGB565 = 2 bytes per pixel */
    art_dsc.data = current_art_buf;
    art_dsc.data_size = data_size;

    lv_image_set_src(img_album_art, &art_dsc);

    /* Hide loading logo now that we have album art */
    if (loading_logo && !has_art) {
        stop_logo_pulse();
        lv_obj_add_flag(loading_logo, LV_OBJ_FLAG_HIDDEN);
        has_art = true;
        /* Now safe to throttle refresh if idle (logo animation no longer needed) */
        if (is_idle) {
            set_refr_period(REFR_PERIOD_IDLE_MS);
        }
    }

    /* Scale to fill 720x720 screen from top-left corner.
     * Pivot is (0,0) so scaling expands right and down, filling the screen.
     * lv_image_set_scale uses 256 = 1.0x. */
    if (w > 0 && w < SCREEN_SIZE) {
        uint32_t scale = (SCREEN_SIZE * 256 + w / 2) / w;  /* round to nearest */
        lv_image_set_scale(img_album_art, scale);
    } else {
        lv_image_set_scale(img_album_art, 256);
    }

    ESP_LOGI(TAG, "Album art set: %lux%lu (%lu bytes)", (unsigned long)w,
             (unsigned long)h, (unsigned long)data_size);
}

/* ── Public API: idle / nothing playing ──────────────────────────────── */

void nina_spotify_set_idle(void)
{
    if (!spotify_page) return;

    /* Clear track info */
    lv_label_set_text(lbl_track_title, "Not Playing");
    lv_label_set_text(lbl_artist_name, "");
    lv_label_set_text(lbl_album_name, "");

    /* Reset progress */
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_label_set_text(lbl_time_elapsed, "0:00");
    lv_label_set_text(lbl_time_total, "0:00");

    /* Update play button to show play icon */
    is_playing = false;
    lv_obj_t *pp_label = lv_obj_get_child(btn_play_pause, 0);
    if (pp_label) {
        lv_label_set_text(pp_label, LV_SYMBOL_PLAY);
    }

    /* Hide album art, show loading logo */
    if (current_art_buf) {
        free(current_art_buf);
        current_art_buf = NULL;
    }
    has_art = false;
    lv_obj_add_flag(img_album_art, LV_OBJ_FLAG_HIDDEN);
    if (loading_logo) {
        lv_obj_remove_flag(loading_logo, LV_OBJ_FLAG_HIDDEN);
        start_logo_pulse();
    }
}

/* ── Public API: theme ───────────────────────────────────────────────── */

void spotify_page_apply_theme(void)
{
    if (!spotify_page || !current_theme) return;

    /* The Spotify page uses mostly fixed dark colors (white text on album art),
     * so theme application is minimal. Apply theme accent to progress bar. */
    if (bar_progress) {
        lv_obj_set_style_bg_color(bar_progress,
            lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    }

    lv_obj_invalidate(spotify_page);
}

/* ── Public API: overlay visibility ──────────────────────────────────── */

bool nina_spotify_is_overlay_visible(void)
{
    return !is_idle;
}

/* ── Public API: show / hide lifecycle ───────────────────────────────── */

void nina_spotify_on_show(void)
{
    if (!spotify_page) return;

    ESP_LOGI(TAG, "Spotify page shown");

    /* Start in idle state (just album art, no overlay) */
    is_idle = false; /* Force transition */
    set_idle_state(true);

    /* Restart loading logo pulse if art hasn't loaded yet */
    if (!has_art && loading_logo) {
        lv_obj_remove_flag(loading_logo, LV_OBJ_FLAG_HIDDEN);
        start_logo_pulse();
    }

    /* If user taps, the touch handler will show overlay and start timer */
}

void nina_spotify_on_hide(void)
{
    if (!spotify_page) return;

    ESP_LOGI(TAG, "Spotify page hidden");

    /* Stop idle timer */
    if (idle_timer) {
        lv_timer_pause(idle_timer);
    }

    /* Restore normal refresh rate when leaving Spotify page */
    set_refr_period(REFR_PERIOD_ACTIVE_MS);
}

void nina_spotify_free_art(void)
{
    if (current_art_buf) {
        if (img_album_art) {
            lv_image_set_src(img_album_art, NULL);
            lv_obj_add_flag(img_album_art, LV_OBJ_FLAG_HIDDEN);
        }
        free(current_art_buf);
        current_art_buf = NULL;
        has_art = false;
        ESP_LOGI(TAG, "Album art buffer freed");
    }

    /* Show loading logo again for next page entry */
    if (loading_logo) {
        lv_obj_remove_flag(loading_logo, LV_OBJ_FLAG_HIDDEN);
        start_logo_pulse();
    }
}
