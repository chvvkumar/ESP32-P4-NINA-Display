/**
 * @file nina_summary.c
 * @brief Summary page — adaptive cards showing connected NINA instances at a glance.
 *
 * Displays one card per CONNECTED NINA instance with: instance name, filter badge,
 * target name, progress bar with percentage, sequence info (1-card mode),
 * and key stats (RMS, HFR, time to flip).
 *
 * Cards expand to fill the available 688×688 area using flex_grow.
 * 3-tier font scaling adapts to 1, 2, or 3 visible cards.
 * An empty-state message is shown when no instances are connected.
 */

#include "nina_summary.h"
#include "nina_dashboard_internal.h"
#include "nina_dashboard.h"
#include "nina_connection.h"
#include "nina_nav_arbiter.h"
#include "app_config.h"
#include "themes.h"
#include "nina_empty_state.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "esp_timer.h"

/* ── Change-detection helpers ──────────────────────────────────────── */
/* Set label text only if it actually changed (avoids marking objects dirty) */
static inline void set_label_if_changed(lv_obj_t *label, const char *text) {
    const char *cur = lv_label_get_text(label);
    if (strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

/* Set label text (printf-style) only if it actually changed */
#define SET_LABEL_FMT_IF_CHANGED(label, bufsize, fmt, ...) do { \
    char _buf[bufsize]; \
    snprintf(_buf, sizeof(_buf), fmt, __VA_ARGS__); \
    const char *_cur = lv_label_get_text(label); \
    if (strcmp(_cur, _buf) != 0) lv_label_set_text(label, _buf); \
} while (0)

/* Set bar value only if it actually changed */
static inline void set_bar_if_changed(lv_obj_t *bar, int32_t val, lv_anim_enable_t anim) {
    if (lv_bar_get_value(bar) != val) lv_bar_set_value(bar, val, anim);
}

/* ── Bar animation (mirrors the monotonic-timer exposure model of the arc) ─ */
/* Bar-scaled copies of the arc's exposure-model constants (see
 * nina_dashboard_internal.h: ARC_RANGE/ARC_TIMER_MS/ARC_TRANSITION_MS/
 * ARC_GAP_GRACE_S). The bar range is 0-100 instead of 0-3600. */
#define BAR_RANGE           100
#define BAR_TIMER_MS        200
#define BAR_TRANSITION_MS   300
#define BAR_GAP_GRACE_S     60

static void bar_anim_exec(void *obj, int32_t v) {
    lv_bar_set_value((lv_obj_t *)obj, v, LV_ANIM_OFF);
}

/* Set text color only if cached value differs (avoids LVGL dirty-marking) */
static inline void set_text_color_cached(lv_obj_t *obj, uint32_t *cached, uint32_t color) {
    if (*cached != color) {
        *cached = color;
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    }
}

/* Set bg color only if cached value differs */
static inline void set_bg_color_cached(lv_obj_t *obj, uint32_t *cached, uint32_t color, lv_style_selector_t sel) {
    if (*cached != color) {
        *cached = color;
        lv_obj_set_style_bg_color(obj, lv_color_hex(color), sel);
    }
}

/* Set bg opacity only if cached value differs */
static inline void set_bg_opa_cached(lv_obj_t *obj, lv_opa_t *cached, lv_opa_t opa, lv_style_selector_t sel) {
    if (*cached != opa) {
        *cached = opa;
        lv_obj_set_style_bg_opa(obj, opa, sel);
    }
}

/* ── Safety icon glyphs (Material Symbols, UTF-8) ─────────────────── */
#define ICON_VERIFIED_USER  "\xee\xa3\xa8"  /* U+E8E8 — shield + check */
#define ICON_GPP_BAD        "\xef\x80\x92"  /* U+F012 — shield + X     */
#define ICON_GPP_MAYBE      "\xef\x80\x94"  /* U+F014 — shield + ?     */

/* ── Layout Constants ──────────────────────────────────────────────── */
#define CARD_RADIUS      20
#define CARD_GAP         16
#define STAT_COLS         3

/* ── 3-tier font/layout presets ───────────────────────────────────── */
typedef struct {
    const lv_font_t *name;
    const lv_font_t *filter;
    const lv_font_t *target;
    const lv_font_t *stat_label;
    const lv_font_t *stat_value;
    const lv_font_t *pct_label;
    const lv_font_t *seq_label;
    const lv_font_t *seq_value;
    int bar_height;
    int card_pad;
    int card_row_gap;
    int stat_pad_row;
} card_layout_preset_t;

static const card_layout_preset_t layout_presets[3] = {
    /* 1 card visible — maximum size */
    {
        .name       = &lv_font_montserrat_36,
        .filter     = &lv_font_montserrat_28,
        .target     = &lv_font_montserrat_48,
        .stat_label = &lv_font_montserrat_22,
        .stat_value = &lv_font_montserrat_48,
        .pct_label  = &lv_font_montserrat_28,
        .seq_label  = &lv_font_montserrat_18,
        .seq_value  = &lv_font_montserrat_28,
        .bar_height = 20,
        .card_pad   = 28,
        .card_row_gap = 16,
        .stat_pad_row = 12,
    },
    /* 2 cards visible — medium */
    {
        .name       = &lv_font_montserrat_28,
        .filter     = &lv_font_montserrat_24,
        .target     = &lv_font_montserrat_32,
        .stat_label = &lv_font_montserrat_18,
        .stat_value = &lv_font_montserrat_36,
        .pct_label  = &lv_font_montserrat_22,
        .seq_label  = &lv_font_montserrat_14,
        .seq_value  = &lv_font_montserrat_22,
        .bar_height = 12,
        .card_pad   = 22,
        .card_row_gap = 10,
        .stat_pad_row = 8,
    },
    /* 3 cards visible — compact */
    {
        .name       = &lv_font_montserrat_20,
        .filter     = &lv_font_montserrat_18,
        .target     = &lv_font_montserrat_24,
        .stat_label = &lv_font_montserrat_14,
        .stat_value = &lv_font_montserrat_28,
        .pct_label  = &lv_font_montserrat_18,
        .seq_label  = &lv_font_montserrat_12,
        .seq_value  = &lv_font_montserrat_14,
        .bar_height = 6,
        .card_pad   = 16,
        .card_row_gap = 6,
        .stat_pad_row = 4,
    },
};

/* ── Per-card widget references ────────────────────────────────────── */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *lbl_name;
    lv_obj_t *lbl_filter;
    lv_obj_t *filter_box;
    lv_obj_t *lbl_target;
    lv_obj_t *bar_progress;
    lv_obj_t *lbl_pct;          /* progress percentage label */
    lv_obj_t *seq_row;          /* sequence info row (visible in 1-2 card mode) */
    lv_obj_t *lbl_seq_title;    /* "SEQUENCE" label */
    lv_obj_t *lbl_seq_name;
    lv_obj_t *lbl_exp_title;    /* "EXPOSURES" label */
    lv_obj_t *lbl_exp_val;      /* exposure count "X / Y" */
    lv_obj_t *lbl_step_title;   /* "STEP" label */
    lv_obj_t *lbl_seq_step;
    lv_obj_t *stats_row;
    lv_obj_t *lbl_rms_label;
    lv_obj_t *lbl_rms_val;
    lv_obj_t *lbl_hfr_label;
    lv_obj_t *lbl_hfr_val;
    lv_obj_t *lbl_flip_label;
    lv_obj_t *lbl_flip_val;
    lv_obj_t *detail_row;       /* exposure detail line (visible in 1-card mode) */
    lv_obj_t *lbl_detail;
    lv_obj_t *lbl_safety;       /* safety monitor icon (floating, bottom-left) */
    int instance_index;         /* which NINA instance this card represents */
    /* Bar exposure-model state (mirrors dashboard_page_t's arc fields) */
    bool bar_completing;        /* true while snap-to-full animation is in flight */
    int64_t exp_anchor_us;      /* monotonic esp_timer anchor (us); 0 = no active exposure */
    float   exp_anchor_elapsed; /* elapsed seconds at the anchor moment */
    bool    cached_is_exposing; /* last-seen is_exposing (edge detection) */
    float   cached_total;       /* cached exposure_total (seconds) */
    int64_t cached_end_epoch;   /* cached exposure_end_epoch (Unix seconds) */
    int64_t gap_start_epoch;    /* inter-exposure gap grace start (Unix seconds) */
    /* Cached style values — only call lv_obj_set_style_* when changed to avoid
     * unnecessary LVGL invalidations that trigger expensive full redraws. */
    uint32_t cached_name_color;
    uint32_t cached_filter_text_color;
    uint32_t cached_filter_bg_color;
    lv_opa_t cached_filter_bg_opa;
    uint32_t cached_target_color;
    uint32_t cached_bar_ind_color;
    uint32_t cached_bar_bg_color;
    uint32_t cached_pct_color;
    uint32_t cached_seq_name_color;
    uint32_t cached_exp_val_color;
    uint32_t cached_seq_step_color;
    uint32_t cached_rms_color;
    uint32_t cached_hfr_color;
    uint32_t cached_flip_color;
    uint32_t cached_detail_color;
    uint32_t cached_safety_color;
} summary_card_t;

/* ── Module state ──────────────────────────────────────────────────── */
static lv_obj_t *sum_page = NULL;
static summary_card_t cards[MAX_NINA_INSTANCES];
static int card_count = 0;
static int prev_visible_count = -1;

/* ── Bar exposure model (scaled copy of the dashboard arc model) ─────── */
static void bar_start_exposure_anim(summary_card_t *sc);

/* Clear a card's exposure anchor and reset its progress bar to empty. Mirrors
 * update_disconnected_state's arc reset (nina_dashboard_update.c): used when a
 * card is hidden/skipped for unavailability so a stale anchor cannot keep the
 * interp timer driving a hidden bar. */
static void bar_reset_exposure_state(summary_card_t *sc) {
    if (!sc->bar_progress) return;
    lv_anim_delete(sc->bar_progress, bar_anim_exec);
    sc->bar_completing     = false;
    sc->exp_anchor_us      = 0;
    sc->exp_anchor_elapsed = 0;
    sc->cached_is_exposing = false;
    sc->cached_total       = 0;
    sc->cached_end_epoch   = 0;
    sc->gap_start_epoch    = 0;
    set_bar_if_changed(sc->bar_progress, 0, LV_ANIM_OFF);
    set_label_if_changed(sc->lbl_pct, "");
}

/* Scaled copy of arc_start_exposure_anim (nina_dashboard_update.c). Drives one
 * long linear anim toward (BAR_RANGE-1) over the monotonic remaining time so
 * the finished edge is the only thing that fills to 100. */
static void bar_start_exposure_anim(summary_card_t *sc) {
    if (sc->exp_anchor_us == 0 || sc->cached_total <= 0 || !sc->cached_is_exposing) return;

    float since_anchor_s = (float)(esp_timer_get_time() - sc->exp_anchor_us) / 1e6f;
    float remaining_s = sc->cached_total - (sc->exp_anchor_elapsed + since_anchor_s);
    if (remaining_s <= 0.1f) return;   /* <=100ms: completion edge will fill it */

    int current = lv_bar_get_value(sc->bar_progress);
    int remaining_ms = (int)(remaining_s * 1000.0f);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sc->bar_progress);
    lv_anim_set_values(&a, current, BAR_RANGE - 1);
    lv_anim_set_time(&a, remaining_ms);
    lv_anim_set_exec_cb(&a, bar_anim_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

/* Scaled copy of arc_interp_timer_cb. Runs continuously (created at page build)
 * and iterates all cards. Runs inside lv_timer_handler, which holds the display
 * lock, so no extra locking is required (same as the dashboard arc timer). */
static void summary_bar_interp_cb(lv_timer_t *timer) {
    (void)timer;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        summary_card_t *sc = &cards[i];
        if (!sc->bar_progress || sc->bar_completing) continue;
        if (sc->exp_anchor_us == 0 || sc->cached_total <= 0) continue;
        if (!sc->cached_is_exposing) continue;

        /* Monotonic elapsed: esp_timer never skews or goes backward. */
        float elapsed = sc->exp_anchor_elapsed +
                        (float)(esp_timer_get_time() - sc->exp_anchor_us) / 1e6f;

        /* Backward-only wall correction. Difference epoch seconds in int64
         * first (they exceed float's 24-bit precision), then cast the small
         * result to float for the P4 single-precision FPU. */
        int64_t remaining_wall_ms = (sc->cached_end_epoch - (int64_t)time(NULL)) * 1000;
        float elapsed_wall = sc->cached_total - (float)remaining_wall_ms / 1000.0f;

        if (elapsed_wall < elapsed - 1.0f) {
            sc->exp_anchor_us = esp_timer_get_time();
            sc->exp_anchor_elapsed = (elapsed_wall > 0.0f) ? elapsed_wall : 0.0f;
            bar_start_exposure_anim(sc);
            continue;
        }

        /* The long linear anim is the smooth source of truth; only restart it if
         * none is running and time remains. */
        lv_anim_t *existing = lv_anim_get(sc->bar_progress, bar_anim_exec);
        if (!existing && elapsed < sc->cached_total) {
            bar_start_exposure_anim(sc);
        }

        /* Refresh the percent label from the live interpolated bar value. */
        int live_pct = lv_bar_get_value(sc->bar_progress);
        SET_LABEL_FMT_IF_CHANGED(sc->lbl_pct, 8, "%d%%", live_pct);
    }
}

/* Empty state widget (shared component — Plan 01) */
static lv_obj_t *empty_cont = NULL;

/* Glass card style — semi-transparent with subtle border */
static lv_style_t style_glass_card;
static bool styles_initialized = false;

/* ── Helpers ───────────────────────────────────────────────────────── */

static void summary_card_click_cb(lv_event_t *e) {
    int instance_index = (int)(intptr_t)lv_event_get_user_data(e);
    /* Pure-offset map to the ABSOLUTE page index (or -1 for a bad index).
     * Availability is a separate query — only navigate to available slots. */
    int page = nina_dashboard_instance_to_page(instance_index);
    if (page >= 0 && instance_index >= 0 && instance_index < MAX_NINA_INSTANCES
        && nina_slot_available[instance_index]) {
        nina_dashboard_show_page_animated(page, 0, 0);
        nav_arbiter_submit_user(page, esp_timer_get_time() / 1000, -1);
    }
}

static uint32_t darken_color_summary(uint32_t color, int pct) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    r = (uint8_t)(r * (100 - pct) / 100);
    g = (uint8_t)(g * (100 - pct) / 100);
    b = (uint8_t)(b * (100 - pct) / 100);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void init_glass_styles(void) {
    if (styles_initialized) return;
    styles_initialized = true;

    lv_style_init(&style_glass_card);
    /* Geometry set in apply_glass_theme() based on widget_style */
}

static void apply_glass_theme(void) {
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;
    int ws = app_config_get()->widget_style;

    lv_style_reset(&style_glass_card);
    lv_style_init(&style_glass_card);
    lv_style_set_pad_all(&style_glass_card, 16);

    switch (ws) {
    case 1: /* Subtle Border */
        lv_style_set_bg_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_bg, gb)));
        lv_style_set_bg_opa(&style_glass_card, LV_OPA_COVER);
        lv_style_set_radius(&style_glass_card, 12);
        lv_style_set_border_width(&style_glass_card, 1);
        lv_style_set_border_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)));
        lv_style_set_border_opa(&style_glass_card, LV_OPA_COVER);
        break;
    case 2: /* Tech Wireframe */
        lv_style_set_bg_opa(&style_glass_card, LV_OPA_TRANSP);
        lv_style_set_radius(&style_glass_card, 0);
        lv_style_set_border_width(&style_glass_card, 1);
        lv_style_set_border_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)));
        lv_style_set_border_opa(&style_glass_card, LV_OPA_COVER);
        break;
    case 3: /* Soft Inset */
        lv_style_set_bg_color(&style_glass_card,
            lv_color_hex(darken_color_summary(current_theme->bento_bg, 30)));
        lv_style_set_bg_opa(&style_glass_card, LV_OPA_COVER);
        lv_style_set_radius(&style_glass_card, 12);
        lv_style_set_border_width(&style_glass_card, 1);
        lv_style_set_border_side(&style_glass_card, LV_BORDER_SIDE_TOP);
        lv_style_set_border_color(&style_glass_card, lv_color_hex(0x000000));
        lv_style_set_border_opa(&style_glass_card, LV_OPA_COVER);
        break;
    case 4: /* Frosted Glass */
        lv_style_set_bg_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_bg, gb)));
        lv_style_set_bg_opa(&style_glass_card, LV_OPA_40);
        lv_style_set_radius(&style_glass_card, CARD_RADIUS);
        lv_style_set_border_width(&style_glass_card, 1);
        lv_style_set_border_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)));
        lv_style_set_border_opa(&style_glass_card, LV_OPA_20);
        break;
    case 5: /* Accent Bar */
        lv_style_set_bg_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_bg, gb)));
        lv_style_set_bg_opa(&style_glass_card, LV_OPA_COVER);
        lv_style_set_radius(&style_glass_card, 12);
        lv_style_set_border_width(&style_glass_card, 4);
        lv_style_set_border_side(&style_glass_card, LV_BORDER_SIDE_LEFT);
        lv_style_set_border_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)));
        lv_style_set_border_opa(&style_glass_card, LV_OPA_COVER);
        break;
    case 6: /* Chamfered */
        lv_style_set_bg_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_bg, gb)));
        lv_style_set_bg_opa(&style_glass_card, LV_OPA_COVER);
        lv_style_set_radius(&style_glass_card, 0);
        lv_style_set_border_width(&style_glass_card, 0);
        break;
    default: /* 0 = Default */
        lv_style_set_bg_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_bg, gb)));
        lv_style_set_bg_opa(&style_glass_card, LV_OPA_COVER);
        lv_style_set_radius(&style_glass_card, CARD_RADIUS);
        lv_style_set_border_width(&style_glass_card, 1);
        lv_style_set_border_color(&style_glass_card,
            lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)));
        lv_style_set_border_opa(&style_glass_card, LV_OPA_30);
        break;
    }
}

/**
 * @brief Build a single stat column (label above, value below).
 */
static void create_stat_block(lv_obj_t *parent,
                              const char *label_text,
                              lv_obj_t **out_label,
                              lv_obj_t **out_value)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(block, 4, 0);

    /* Label (small, dim) */
    lv_obj_t *lbl = lv_label_create(block);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    /* Value (large) */
    lv_obj_t *val = lv_label_create(block);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);

    *out_label = lbl;
    *out_value = val;
}

/**
 * @brief Build one glassmorphism instance card with all widgets.
 *        Some widgets (seq_row, detail_row) are hidden by default
 *        and shown only when visible_count is low enough.
 */
static void create_card(summary_card_t *sc, lv_obj_t *parent, int instance_index) {
    memset(sc, 0, sizeof(summary_card_t));
    sc->instance_index = instance_index;

    /* Invalidate all cached colors so the first update always applies */
    sc->cached_name_color       = UINT32_MAX;
    sc->cached_filter_text_color = UINT32_MAX;
    sc->cached_filter_bg_color  = UINT32_MAX;
    sc->cached_filter_bg_opa    = UINT8_MAX;
    sc->cached_target_color     = UINT32_MAX;
    sc->cached_bar_ind_color    = UINT32_MAX;
    sc->cached_bar_bg_color     = UINT32_MAX;
    sc->cached_pct_color        = UINT32_MAX;
    sc->cached_seq_name_color   = UINT32_MAX;
    sc->cached_exp_val_color    = UINT32_MAX;
    sc->cached_seq_step_color   = UINT32_MAX;
    sc->cached_rms_color        = UINT32_MAX;
    sc->cached_hfr_color        = UINT32_MAX;
    sc->cached_flip_color       = UINT32_MAX;
    sc->cached_detail_color     = UINT32_MAX;
    sc->cached_safety_color     = UINT32_MAX;

    /* ── Card container ── */
    sc->card = lv_obj_create(parent);
    lv_obj_remove_style_all(sc->card);
    lv_obj_add_style(sc->card, &style_glass_card, 0);
    ui_styles_set_widget_draw_cbs(sc->card);
    lv_obj_set_width(sc->card, LV_PCT(100));
    lv_obj_set_flex_grow(sc->card, 1);
    lv_obj_set_flex_flow(sc->card, LV_FLEX_FLOW_COLUMN);
    /* START packs header/target/bar at top; stats_row with flex_grow=1
     * absorbs remaining vertical space, centering stats in the middle */
    lv_obj_set_flex_align(sc->card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(sc->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(sc->card, 16, 0);
    lv_obj_set_style_pad_row(sc->card, 6, 0);

    /* Make card clickable for navigation */
    lv_obj_add_flag(sc->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sc->card, summary_card_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)instance_index);

    /* ── Instance name (full width, truncated with dots) ── */
    sc->lbl_name = lv_label_create(sc->card);
    lv_obj_set_width(sc->lbl_name, LV_PCT(100));
    lv_label_set_long_mode(sc->lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sc->lbl_name, &lv_font_montserrat_20, 0);
    {
        const char *url = app_config_get_instance_url(instance_index);
        char host[64] = {0};
        extract_host_from_url(url, host, sizeof(host));
        lv_label_set_text(sc->lbl_name, host[0] ? host : "N.I.N.A.");
    }

    /* ── Target name ── */
    sc->lbl_target = lv_label_create(sc->card);
    lv_obj_set_width(sc->lbl_target, LV_PCT(100));
    lv_obj_set_style_text_align(sc->lbl_target, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(sc->lbl_target, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_24, 0);
    lv_label_set_text(sc->lbl_target, "----");

    /* ── Progress bar row: filter | bar | pct ── */
    lv_obj_t *bar_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(bar_row);
    lv_obj_set_width(bar_row, LV_PCT(100));
    lv_obj_set_height(bar_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar_row, 10, 0);

    /* Filter badge (left of progress bar) */
    sc->filter_box = lv_obj_create(bar_row);
    lv_obj_remove_style_all(sc->filter_box);
    lv_obj_set_size(sc->filter_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sc->filter_box, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(sc->filter_box, lv_color_black(), 0);
    lv_obj_set_style_radius(sc->filter_box, 6, 0);
    lv_obj_set_style_pad_hor(sc->filter_box, 8, 0);
    lv_obj_set_style_pad_ver(sc->filter_box, 4, 0);

    sc->lbl_filter = lv_label_create(sc->filter_box);
    lv_obj_set_style_text_font(sc->lbl_filter, &lv_font_montserrat_14, 0);
    lv_label_set_text(sc->lbl_filter, "--");

    sc->bar_progress = lv_bar_create(bar_row);
    lv_obj_set_flex_grow(sc->bar_progress, 1);
    lv_obj_set_height(sc->bar_progress, 6);
    lv_bar_set_range(sc->bar_progress, 0, 100);
    lv_bar_set_value(sc->bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(sc->bar_progress, 3, 0);
    lv_obj_set_style_radius(sc->bar_progress, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sc->bar_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(sc->bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);

    sc->lbl_pct = lv_label_create(bar_row);
    lv_obj_set_style_text_font(sc->lbl_pct, &lv_font_montserrat_14, 0);
    lv_label_set_text(sc->lbl_pct, "");

    /* ── Sequence info row (shown in 1-2 card mode) ── */
    /* Three equal-width columns: SEQUENCE | EXPOSURES | STEP
     * Each column has flex_grow=1 so they split width evenly,
     * keeping the center column centered regardless of content. */
    sc->seq_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(sc->seq_row);
    lv_obj_set_width(sc->seq_row, LV_PCT(100));
    lv_obj_set_height(sc->seq_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc->seq_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sc->seq_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN); /* hidden by default */

    /* Left column: SEQUENCE */
    lv_obj_t *seq_left = lv_obj_create(sc->seq_row);
    lv_obj_remove_style_all(seq_left);
    lv_obj_clear_flag(seq_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(seq_left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(seq_left, 1);
    lv_obj_set_flex_flow(seq_left, LV_FLEX_FLOW_COLUMN);

    sc->lbl_seq_title = lv_label_create(seq_left);
    lv_label_set_text(sc->lbl_seq_title, "SEQUENCE");
    lv_obj_set_style_text_font(sc->lbl_seq_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sc->lbl_seq_title, 1, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(sc->lbl_seq_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    sc->lbl_seq_name = lv_label_create(seq_left);
    lv_obj_set_style_text_font(sc->lbl_seq_name, &lv_font_montserrat_18, 0);
    lv_label_set_text(sc->lbl_seq_name, "----");

    /* Center column: EXPOSURES */
    lv_obj_t *seq_center = lv_obj_create(sc->seq_row);
    lv_obj_remove_style_all(seq_center);
    lv_obj_clear_flag(seq_center, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(seq_center, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(seq_center, 1);
    lv_obj_set_flex_flow(seq_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(seq_center, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    sc->lbl_exp_title = lv_label_create(seq_center);
    lv_label_set_text(sc->lbl_exp_title, "COMPLETED");
    lv_obj_set_style_text_font(sc->lbl_exp_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sc->lbl_exp_title, 1, 0);
    lv_obj_set_style_text_align(sc->lbl_exp_title, LV_TEXT_ALIGN_CENTER, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(sc->lbl_exp_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    sc->lbl_exp_val = lv_label_create(seq_center);
    lv_obj_set_style_text_font(sc->lbl_exp_val, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(sc->lbl_exp_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(sc->lbl_exp_val, "-- / --");

    /* Right column: STEP */
    lv_obj_t *seq_right = lv_obj_create(sc->seq_row);
    lv_obj_remove_style_all(seq_right);
    lv_obj_clear_flag(seq_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(seq_right, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(seq_right, 1);
    lv_obj_set_flex_flow(seq_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(seq_right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    sc->lbl_step_title = lv_label_create(seq_right);
    lv_label_set_text(sc->lbl_step_title, "STEP");
    lv_obj_set_style_text_font(sc->lbl_step_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sc->lbl_step_title, 1, 0);
    lv_obj_set_style_text_align(sc->lbl_step_title, LV_TEXT_ALIGN_RIGHT, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(sc->lbl_step_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    sc->lbl_seq_step = lv_label_create(seq_right);
    lv_obj_set_style_text_font(sc->lbl_seq_step, &lv_font_montserrat_18, 0);
    lv_label_set_text(sc->lbl_seq_step, "----");

    /* ── Stats row: RMS | HFR | FLIP ── */
    sc->stats_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(sc->stats_row);
    lv_obj_set_width(sc->stats_row, LV_PCT(100));
    lv_obj_set_height(sc->stats_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc->stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(sc->stats_row, 1);
    lv_obj_set_flex_align(sc->stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Safety icon column (first in stats row) */
    {
        extern const lv_font_t lv_font_material_safety;
        lv_obj_t *safety_block = lv_obj_create(sc->stats_row);
        lv_obj_remove_style_all(safety_block);
        lv_obj_set_size(safety_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(safety_block, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(safety_block, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        sc->lbl_safety = lv_label_create(safety_block);
        lv_obj_set_style_text_font(sc->lbl_safety, &lv_font_material_safety, 0);
        uint32_t safety_unknown_color = theme_is_red_night(current_theme)
            ? current_theme->label_color : 0x999999;
        lv_obj_set_style_text_color(sc->lbl_safety, lv_color_hex(safety_unknown_color), 0);
        lv_label_set_text(sc->lbl_safety, ICON_GPP_MAYBE);
        lv_obj_add_flag(sc->lbl_safety, LV_OBJ_FLAG_HIDDEN);
    }

    create_stat_block(sc->stats_row, "RMS",  &sc->lbl_rms_label,  &sc->lbl_rms_val);
    create_stat_block(sc->stats_row, "HFR",  &sc->lbl_hfr_label,  &sc->lbl_hfr_val);
    create_stat_block(sc->stats_row, "FLIP", &sc->lbl_flip_label, &sc->lbl_flip_val);

    /* ── Exposure detail line (shown in 1-card mode only) ── */
    sc->detail_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(sc->detail_row);
    lv_obj_set_width(sc->detail_row, LV_PCT(100));
    lv_obj_set_height(sc->detail_row, LV_SIZE_CONTENT);
    lv_obj_add_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN); /* hidden by default */

    sc->lbl_detail = lv_label_create(sc->detail_row);
    lv_obj_set_width(sc->lbl_detail, LV_PCT(100));
    lv_obj_set_style_text_font(sc->lbl_detail, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(sc->lbl_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(sc->lbl_detail, "");
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(sc->lbl_detail,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

}

/**
 * @brief Create the empty-state container (shown when no instances connected).
 *
 * Uses the shared nina_empty_state component (Plan 01) for a branded,
 * icon-led presentation (IDLE-02).
 */
static void create_empty_state(lv_obj_t *parent) {
    empty_cont = nina_empty_state_create(parent,
                                         ICON_CLOUD_OFF,
                                         "No N.I.N.A. Instances Connected",
                                         "N.I.N.A. may not be running",
                                         0);
    if (empty_cont) {
        /* Take the container out of sum_page's START-aligned flex flow
         * (which otherwise pins it to the top) and center it on the page. */
        lv_obj_add_flag(empty_cont, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(empty_cont, LV_ALIGN_CENTER, 0, 0);
    }
}

/* ── Page Creation ─────────────────────────────────────────────────── */

lv_obj_t *summary_page_create(lv_obj_t *parent) {
    init_glass_styles();
    apply_glass_theme();

    sum_page = lv_obj_create(parent);
    lv_obj_remove_style_all(sum_page);
    lv_obj_set_size(sum_page, SCREEN_SIZE - 2 * OUTER_PADDING,
                              SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_flex_flow(sum_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sum_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sum_page, CARD_GAP, 0);
    lv_obj_clear_flag(sum_page, LV_OBJ_FLAG_SCROLLABLE);

    /* Build one card per fixed-identity slot (full reserved band width).
     * card_count is always MAX_NINA_INSTANCES so cards[i] == instance i. */
    card_count = MAX_NINA_INSTANCES;

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        create_card(&cards[i], sum_page, i);
        /* All cards start hidden; summary_page_rebuild / summary_page_update
         * control visibility based on nina_slot_available[] and connectivity. */
        lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
    }

    /* Single shared interpolation timer driving all cards' progress bars
     * (mirrors the per-page arc timer in nina_dashboard.c). Runs continuously;
     * each tick it advances only cards with an active exposure anchor. */
    lv_timer_create(summary_bar_interp_cb, BAR_TIMER_MS, NULL);

    /* Empty state — shown when no instances are connected */
    create_empty_state(sum_page);

    prev_visible_count = -1;

    return sum_page;
}

/**
 * @brief Re-evaluate card visibility for the current nina_slot_available[] set.
 *
 * Must be called under the LVGL display lock. Hides cards for unavailable slots
 * and resets prev_visible_count so the next summary_page_update forces a full
 * layout pass. Task 1.4 (nina_dashboard_rebuild_slot) calls this after a slot
 * is created or destroyed.
 */
void summary_page_rebuild(void) {
    if (!sum_page) return;

    /* Hide cards for unavailable slots; leave available slots for
     * summary_page_update to show/hide based on live connection state. */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (!nina_slot_available[i]) {
            lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            /* Drop the exposure anchor so a re-enabled card starts clean and no
             * stale anchor keeps the interp timer driving a hidden bar. */
            lv_anim_delete(cards[i].bar_progress, bar_anim_exec);
            cards[i].bar_completing     = false;
            cards[i].exp_anchor_us      = 0;
            cards[i].exp_anchor_elapsed = 0;
            cards[i].cached_is_exposing = false;
            cards[i].cached_total       = 0;
            cards[i].cached_end_epoch   = 0;
            cards[i].gap_start_epoch    = 0;
        }
    }

    /* Force the next summary_page_update call to redo layout presets */
    prev_visible_count = -1;
}

/* ── Layout Update ─────────────────────────────────────────────────── */

static void update_card_layout(summary_card_t *sc, int visible_count) {
    int idx = (visible_count <= 1) ? 0 : (visible_count == 2) ? 1 : 2;
    const card_layout_preset_t *p = &layout_presets[idx];

    /* Card flex strategy: 1-card uses SPACE_BETWEEN to pin header at top
     * and detail at bottom, spreading items across the full height.
     * 2-3 cards pack to top with stats_row absorbing remaining space. */
    if (visible_count <= 1) {
        lv_obj_set_flex_align(sc->card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_flex_grow(sc->stats_row, 0);
    } else {
        lv_obj_set_flex_align(sc->card, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_flex_grow(sc->stats_row, 1);
    }

    /* Card padding and spacing */
    lv_obj_set_style_pad_all(sc->card, p->card_pad, 0);
    lv_obj_set_style_pad_row(sc->card, p->card_row_gap, 0);

    /* Instance name */
    lv_obj_set_style_text_font(sc->lbl_name, p->name, 0);

    /* Filter badge */
    lv_obj_set_style_text_font(sc->lbl_filter, p->filter, 0);

    /* Target name */
    lv_obj_set_style_text_font(sc->lbl_target, p->target, 0);

    /* Progress bar height */
    lv_obj_set_height(sc->bar_progress, p->bar_height);
    lv_obj_set_style_radius(sc->bar_progress, p->bar_height / 2, 0);
    lv_obj_set_style_radius(sc->bar_progress, p->bar_height / 2, LV_PART_INDICATOR);

    /* Progress percentage label */
    lv_obj_set_style_text_font(sc->lbl_pct, p->pct_label, 0);

    /* Sequence row: shown in 1-2 card mode, hidden in 3-card mode */
    if (visible_count <= 2) {
        lv_obj_clear_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(sc->lbl_seq_title, p->seq_label, 0);
        lv_obj_set_style_text_font(sc->lbl_seq_name, p->seq_value, 0);
        lv_obj_set_style_text_font(sc->lbl_exp_title, p->seq_label, 0);
        lv_obj_set_style_text_font(sc->lbl_exp_val, p->seq_value, 0);
        lv_obj_set_style_text_font(sc->lbl_step_title, p->seq_label, 0);
        lv_obj_set_style_text_font(sc->lbl_seq_step, p->seq_value, 0);
    } else {
        lv_obj_add_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* Stats (Labels and Values) */
    lv_obj_t *labels[] = { sc->lbl_rms_label, sc->lbl_hfr_label, sc->lbl_flip_label };
    lv_obj_t *values[] = { sc->lbl_rms_val, sc->lbl_hfr_val, sc->lbl_flip_val };

    for (int i = 0; i < 3; i++) {
        if (labels[i]) {
            lv_obj_set_style_text_font(labels[i], p->stat_label, 0);
            /* Update stat block padding */
            lv_obj_t *block = lv_obj_get_parent(labels[i]);
            if (block) lv_obj_set_style_pad_row(block, p->stat_pad_row, 0);
        }
        if (values[i]) lv_obj_set_style_text_font(values[i], p->stat_value, 0);
    }

    /* Detail row: shown only in 1-card mode */
    if (visible_count <= 1) {
        lv_obj_clear_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(sc->lbl_detail, p->pct_label, 0);
    } else {
        lv_obj_add_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Card Transition Animations ────────────────────────────────────── */

#define ANIM_DURATION_MS  1000

static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void anim_translate_y_cb(void *obj, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

static void anim_translate_y_done_cb(lv_anim_t *a) {
    lv_obj_set_style_translate_y((lv_obj_t *)a->var, 0, 0);
}

/**
 * @brief Fade-in + slide-up entrance for a newly appearing card.
 */
static void animate_card_in(summary_card_t *sc) {
    lv_obj_set_style_opa(sc->card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(sc->card, 40, 0);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, sc->card);
    lv_anim_set_values(&a_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_opa, ANIM_DURATION_MS);
    lv_anim_set_exec_cb(&a_opa, anim_opa_cb);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_start(&a_opa);

    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, sc->card);
    lv_anim_set_values(&a_y, 40, 0);
    lv_anim_set_duration(&a_y, ANIM_DURATION_MS);
    lv_anim_set_exec_cb(&a_y, anim_translate_y_cb);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_out);
    lv_anim_start(&a_y);
}

/**
 * @brief Smoothly glide a card from its old position to its new position.
 * Uses translate_y to offset from the flex-assigned position, then animate to 0.
 */
static void animate_card_move(summary_card_t *sc, int32_t dy) {
    lv_obj_set_style_translate_y(sc->card, dy, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sc->card);
    lv_anim_set_values(&a, dy, 0);
    lv_anim_set_duration(&a, ANIM_DURATION_MS);
    lv_anim_set_exec_cb(&a, anim_translate_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, anim_translate_y_done_cb);
    lv_anim_start(&a);
}

/* ── Data Update ───────────────────────────────────────────────────── */

void summary_page_update(const nina_client_t *instances, int count) {
    if (!sum_page) return;

    int gb = app_config_get()->color_brightness;

    /* Count visible cards using the SAME predicate as the per-card show/hide
     * loop below (slot-available AND within count AND connected). Using the
     * raw nina_connection_connected_count() here would ignore slot
     * availability and could desync the empty-state test and the layout tier
     * from what the cards actually show. */
    int visible = 0;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (nina_slot_available[i] && i < count && nina_connection_is_connected(i)) visible++;
    }

    /* Empty state: show message when nothing is visible */
    if (visible == 0) {
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
        }
        nina_empty_state_show(empty_cont);
        prev_visible_count = 0;
        return;
    }

    /* Hide empty state, show cards */
    nina_empty_state_hide(empty_cont);

    bool layout_changed = (visible != prev_visible_count);

    if (layout_changed) {
        /* ── FLIP animation: First, Last, Invert, Play ───────────── */

        /* FIRST: snapshot current visibility, Y positions and heights */
        bool was_visible[MAX_NINA_INSTANCES];
        int32_t old_y[MAX_NINA_INSTANCES];
        int32_t old_h[MAX_NINA_INSTANCES];
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            was_visible[i] = !lv_obj_has_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            old_y[i] = was_visible[i] ? lv_obj_get_y(cards[i].card) : 0;
            old_h[i] = was_visible[i] ? lv_obj_get_height(cards[i].card) : 0;
        }

        /* LAST: apply show/hide and layout preset changes.
         * Only available slots (nina_slot_available[i]) can ever be shown;
         * unavailable slots are always kept hidden regardless of connection. */
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            if (nina_slot_available[i] && i < count && nina_connection_is_connected(i)) {
                lv_obj_clear_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
                update_card_layout(&cards[i], visible);
            } else {
                lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            }
        }

        /* Force synchronous layout so new positions are available */
        lv_obj_update_layout(sum_page);

        /* INVERT + PLAY: animate transitions */
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            if (!nina_slot_available[i] || i >= count || !nina_connection_is_connected(i)) continue;
            int32_t new_y = lv_obj_get_y(cards[i].card);
            int32_t new_h = lv_obj_get_height(cards[i].card);
            if (!was_visible[i]) {
                /* New card — fade in + slide up */
                animate_card_in(&cards[i]);
            } else {
                /* Existing card — animate position if moved */
                if (old_y[i] != new_y) {
                    animate_card_move(&cards[i], old_y[i] - new_y);
                }
                /* Crossfade if height changed (flex handles actual resize) */
                if (old_h[i] != new_h && old_h[i] > 0) {
                    lv_anim_delete(cards[i].card, anim_opa_cb);
                    lv_obj_set_style_opa(cards[i].card, LV_OPA_40, 0);
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, cards[i].card);
                    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
                    lv_anim_set_duration(&a, ANIM_DURATION_MS);
                    lv_anim_set_exec_cb(&a, anim_opa_cb);
                    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                    lv_anim_start(&a);
                }
            }
        }
    } else {
        /* No layout change — just ensure correct visibility.
         * Unavailable slots stay hidden regardless of connection state. */
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            if (nina_slot_available[i] && i < count && nina_connection_is_connected(i)) {
                lv_obj_clear_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    prev_visible_count = visible;

    /* ── Update card data ────────────────────────────────────────── */
    for (int i = 0; i < MAX_NINA_INSTANCES && i < count; i++) {
        summary_card_t *sc = &cards[i];
        const nina_client_t *d = &instances[i];

        if (!nina_slot_available[i] || !nina_connection_is_connected(i)) {
            /* Card is hidden this cycle — drop any stale exposure anchor so the
             * interp timer doesn't keep driving an off-screen bar. */
            bar_reset_exposure_state(sc);
            continue;
        }

        /* Instance name — telescope + camera, fallback to profile, then host */
        if (d->telescope_name[0] && d->camera_name[0]) {
            char combined[128];
            snprintf(combined, sizeof(combined), "%s | %s", d->telescope_name, d->camera_name);
            set_label_if_changed(sc->lbl_name, combined);
        } else if (d->telescope_name[0]) {
            set_label_if_changed(sc->lbl_name, d->telescope_name);
        } else if (d->camera_name[0]) {
            set_label_if_changed(sc->lbl_name, d->camera_name);
        } else if (d->profile_name[0]) {
            set_label_if_changed(sc->lbl_name, d->profile_name);
        } else {
            const char *url = app_config_get_instance_url(i);
            char host[64] = {0};
            extract_host_from_url(url, host, sizeof(host));
            set_label_if_changed(sc->lbl_name, host[0] ? host : "N.I.N.A.");
        }

        /* Name color: theme header color (always connected at this point) */
        if (current_theme) {
            uint32_t name_color = app_config_apply_brightness(
                current_theme->header_text_color, gb);
            set_text_color_cached(sc->lbl_name, &sc->cached_name_color, name_color);
        }

        /* Filter */
        if (d->current_filter[0]) {
            set_label_if_changed(sc->lbl_filter, d->current_filter);
            uint32_t fc = theme_is_red_night(current_theme)
                ? 0 : app_config_get_filter_color(d->current_filter, i);
            if (fc != 0) {
                uint32_t dimmed_fc = app_config_apply_brightness(fc, gb);
                set_text_color_cached(sc->lbl_filter, &sc->cached_filter_text_color, dimmed_fc);
                set_bg_color_cached(sc->filter_box, &sc->cached_filter_bg_color, dimmed_fc, 0);
                set_bg_opa_cached(sc->filter_box, &sc->cached_filter_bg_opa, LV_OPA_20, 0);
            } else if (current_theme) {
                set_text_color_cached(sc->lbl_filter, &sc->cached_filter_text_color,
                    app_config_apply_brightness(current_theme->filter_text_color, gb));
                set_bg_color_cached(sc->filter_box, &sc->cached_filter_bg_color,
                    app_config_apply_brightness(current_theme->bento_border, gb), 0);
                set_bg_opa_cached(sc->filter_box, &sc->cached_filter_bg_opa, LV_OPA_50, 0);
            }
        } else {
            set_label_if_changed(sc->lbl_filter, "--");
            if (current_theme) {
                set_text_color_cached(sc->lbl_filter, &sc->cached_filter_text_color,
                    app_config_apply_brightness(current_theme->label_color, gb));
            }
            set_bg_color_cached(sc->filter_box, &sc->cached_filter_bg_color,
                current_theme ? current_theme->bento_bg : 0x000000, 0);
            set_bg_opa_cached(sc->filter_box, &sc->cached_filter_bg_opa, LV_OPA_50, 0);
        }

        /* Target name */
        if (d->target_name[0]) {
            set_label_if_changed(sc->lbl_target, d->target_name);
        } else {
            set_label_if_changed(sc->lbl_target, "Idle");
        }

        if (current_theme) {
            uint32_t tgt_color = app_config_apply_brightness(
                current_theme->target_name_color, gb);
            set_text_color_cached(sc->lbl_target, &sc->cached_target_color, tgt_color);
        }

        /* Progress bar + percentage — monotonic-timer exposure model scaled to
         * 0-100 (ports update_exposure_arc from nina_dashboard_update.c). Smooth
         * progress between polls is driven by summary_bar_interp_cb; this block
         * handles seeding, re-anchoring, the finished edge, and the gap/idle
         * reset. */
        {
            time_t now = time(NULL);

            /* Detect the is_exposing true->false edge BEFORE updating the cached
             * flag; NINA drops is_exposing at sub end and the poll usually sees
             * that before remaining hits zero, so this edge drives the snap. */
            bool finished_edge = (sc->cached_is_exposing && !d->is_exposing
                                  && sc->exp_anchor_us != 0);
            sc->cached_is_exposing = d->is_exposing;

            if (finished_edge) {
                /* Snap the bar to full for a polished completion; the gap logic
                 * below then holds/fades it before the next sub. */
                lv_anim_delete(sc->bar_progress, bar_anim_exec);
                sc->bar_completing = true;
                sc->exp_anchor_us = 0;
                int cur_fill = lv_bar_get_value(sc->bar_progress);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, sc->bar_progress);
                lv_anim_set_values(&a, cur_fill, BAR_RANGE);
                lv_anim_set_time(&a, BAR_TRANSITION_MS);
                lv_anim_set_exec_cb(&a, bar_anim_exec);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_start(&a);
                SET_LABEL_FMT_IF_CHANGED(sc->lbl_pct, 8, "%d%%", 100);
            }

            if (d->exposure_total > 0 && d->exposure_end_epoch > 0 && d->is_exposing) {
                /* Camera is actively exposing. */
                sc->gap_start_epoch = 0;

                /* Detect new exposure by end_epoch change, or exposing with no
                 * anchor. Computed against the OLD cached_* values, so this must
                 * run BEFORE the cache assignments below. */
                bool new_exposure = (d->exposure_end_epoch != sc->cached_end_epoch
                                     && d->exposure_end_epoch > (int64_t)now)
                                    || (sc->exp_anchor_us == 0);
                bool same_exposure = (sc->exp_anchor_us != 0
                                      && d->exposure_end_epoch == sc->cached_end_epoch);
                bool total_changed = (same_exposure && sc->cached_total > 0.0f
                                      && fabsf(sc->cached_total - d->exposure_total) > 1.0f);

                sc->cached_end_epoch = d->exposure_end_epoch;
                sc->cached_total = d->exposure_total;

                if (new_exposure) {
                    /* Anchor the monotonic clock; seed elapsed with a ONE-TIME
                     * wall estimate (detection can land mid-sub). Difference the
                     * epochs in int64 first, then cast to float for the P4 FPU. */
                    int64_t remaining_seed_ms = (d->exposure_end_epoch - (int64_t)now) * 1000;
                    float seed = d->exposure_total - (float)remaining_seed_ms / 1000.0f;
                    if (seed < 0.0f) seed = 0.0f;
                    if (seed > d->exposure_total) seed = d->exposure_total;

                    sc->exp_anchor_us = esp_timer_get_time();
                    sc->exp_anchor_elapsed = seed;
                    sc->bar_completing = false;

                    lv_anim_delete(sc->bar_progress, bar_anim_exec);
                    int seed_val = (int)((seed * (float)BAR_RANGE) / d->exposure_total);
                    if (seed_val < 0) seed_val = 0;
                    if (seed_val > BAR_RANGE - 1) seed_val = BAR_RANGE - 1;
                    lv_bar_set_value(sc->bar_progress, seed_val, LV_ANIM_OFF);
                    SET_LABEL_FMT_IF_CHANGED(sc->lbl_pct, 8, "%d%%", seed_val);

                    bar_start_exposure_anim(sc);
                } else if (total_changed) {
                    /* Same ongoing sub, exposure_total corrected. Re-anchor and
                     * smoothly animate the one-time correction; do NOT call
                     * bar_start_exposure_anim (it would delete this correction
                     * anim). The interp timer restarts the long anim afterward. */
                    int64_t remaining_seed_ms = (d->exposure_end_epoch - (int64_t)now) * 1000;
                    float seed = d->exposure_total - (float)remaining_seed_ms / 1000.0f;
                    if (seed < 0.0f) seed = 0.0f;
                    if (seed > d->exposure_total) seed = d->exposure_total;

                    sc->exp_anchor_us = esp_timer_get_time();
                    sc->exp_anchor_elapsed = seed;

                    int target_val = (int)((seed * (float)BAR_RANGE) / d->exposure_total);
                    if (target_val < 0) target_val = 0;
                    if (target_val > BAR_RANGE - 1) target_val = BAR_RANGE - 1;
                    int cur_val = lv_bar_get_value(sc->bar_progress);

                    lv_anim_delete(sc->bar_progress, bar_anim_exec);
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, sc->bar_progress);
                    lv_anim_set_values(&a, cur_val, target_val);
                    lv_anim_set_time(&a, BAR_TRANSITION_MS);
                    lv_anim_set_exec_cb(&a, bar_anim_exec);
                    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                    lv_anim_start(&a);
                }
                /* Normal progress updates are handled by summary_bar_interp_cb. */
            } else {
                /* No active exposure — inter-exposure gap or idle state. */
                if (sc->cached_end_epoch > 0 && sc->cached_total > 0) {
                    /* Was recently exposing — hold bar position during the gap. */
                    bool camera_idle = (strcmp(d->status, "Idle") == 0
                                     || strcmp(d->status, "NoState") == 0
                                     || strcmp(d->status, "OFFLINE") == 0);

                    if (sc->gap_start_epoch == 0) {
                        sc->gap_start_epoch = (int64_t)now;
                    }

                    int64_t gap_duration = (int64_t)now - sc->gap_start_epoch;
                    if (camera_idle || gap_duration > BAR_GAP_GRACE_S) {
                        /* Grace expired — transition to idle. */
                        sc->cached_end_epoch = 0;
                        sc->cached_total = 0;
                        sc->gap_start_epoch = 0;
                        sc->exp_anchor_us = 0;
                        sc->exp_anchor_elapsed = 0;
                        sc->bar_completing = false;
                        lv_anim_delete(sc->bar_progress, bar_anim_exec);

                        lv_anim_t a;
                        lv_anim_init(&a);
                        lv_anim_set_var(&a, sc->bar_progress);
                        lv_anim_set_values(&a, lv_bar_get_value(sc->bar_progress), 0);
                        lv_anim_set_time(&a, 500);
                        lv_anim_set_exec_cb(&a, bar_anim_exec);
                        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                        lv_anim_start(&a);

                        set_label_if_changed(sc->lbl_pct, "");
                    }
                    /* else: within grace — hold, do nothing. */
                } else {
                    /* Genuinely idle — no recent exposure data. */
                    sc->gap_start_epoch = 0;
                    sc->exp_anchor_us = 0;
                    sc->exp_anchor_elapsed = 0;
                    lv_anim_delete(sc->bar_progress, bar_anim_exec);
                    set_bar_if_changed(sc->bar_progress, 0, LV_ANIM_OFF);
                    set_label_if_changed(sc->lbl_pct, "");
                }
            }
        }

        /* Progress bar color: filter color or theme progress */
        {
            uint32_t bar_col = 0;
            if (!theme_is_red_night(current_theme) && d->current_filter[0]) {
                bar_col = app_config_get_filter_color(d->current_filter, i);
            }
            if (bar_col == 0 && current_theme) {
                bar_col = current_theme->progress_color;
            }
            if (bar_col != 0) {
                set_bg_color_cached(sc->bar_progress, &sc->cached_bar_ind_color,
                    app_config_apply_brightness(bar_col, gb), LV_PART_INDICATOR);
            }
            if (current_theme) {
                set_bg_color_cached(sc->bar_progress, &sc->cached_bar_bg_color,
                    current_theme->bento_border, 0);
            }
        }

        /* Percentage label color */
        if (current_theme) {
            set_text_color_cached(sc->lbl_pct, &sc->cached_pct_color,
                app_config_apply_brightness(current_theme->text_color, gb));
        }

        /* Sequence info (visible in 1-2 card mode) */
        if (visible <= 2 && !lv_obj_has_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN)) {
            if (d->container_name[0]) {
                set_label_if_changed(sc->lbl_seq_name, d->container_name);
            } else {
                set_label_if_changed(sc->lbl_seq_name, "----");
            }
            if (d->container_step[0]) {
                set_label_if_changed(sc->lbl_seq_step, d->container_step);
            } else {
                set_label_if_changed(sc->lbl_seq_step, "----");
            }

            /* Completed filter exposures + integration time */
            if (d->exposure_total_count > 0) {
                int total_secs = (int)(d->exposure_total_count * d->exposure_total);
                int h = total_secs / 3600;
                int m = (total_secs % 3600) / 60;
                if (h > 0) {
                    SET_LABEL_FMT_IF_CHANGED(sc->lbl_exp_val, 32, "%d / %dh %02dm",
                        d->exposure_total_count, h, m);
                } else {
                    SET_LABEL_FMT_IF_CHANGED(sc->lbl_exp_val, 32, "%d / %dm",
                        d->exposure_total_count, m);
                }
            } else if (d->exposure_iterations > 0) {
                SET_LABEL_FMT_IF_CHANGED(sc->lbl_exp_val, 32, "%d / %d",
                    d->exposure_count, d->exposure_iterations);
            } else {
                set_label_if_changed(sc->lbl_exp_val, "--");
            }

            if (current_theme) {
                set_text_color_cached(sc->lbl_seq_name, &sc->cached_seq_name_color,
                    app_config_apply_brightness(current_theme->header_text_color, gb));
                /* Use filter color for completed count when available */
                uint32_t exp_color = current_theme->text_color;
                if (d->exposure_total_count > 0 && !theme_is_red_night(current_theme) &&
                    d->current_filter[0] && strcmp(d->current_filter, "--") != 0) {
                    uint32_t fc = app_config_get_filter_color(d->current_filter, i);
                    if (fc != 0) exp_color = fc;
                }
                set_text_color_cached(sc->lbl_exp_val, &sc->cached_exp_val_color,
                    app_config_apply_brightness(exp_color, gb));
                set_text_color_cached(sc->lbl_seq_step, &sc->cached_seq_step_color,
                    app_config_apply_brightness(current_theme->text_color, gb));
            }
        }

        /* RMS */
        if (d->guider.rms_total > 0.001f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f\"", d->guider.rms_total);
            set_label_if_changed(sc->lbl_rms_val, buf);
            uint32_t rms_col = theme_is_red_night(current_theme)
                ? 0 : app_config_get_rms_color(d->guider.rms_total, i);
            if (rms_col != 0) {
                set_text_color_cached(sc->lbl_rms_val, &sc->cached_rms_color,
                    app_config_apply_brightness(rms_col, gb));
            } else if (current_theme) {
                set_text_color_cached(sc->lbl_rms_val, &sc->cached_rms_color,
                    app_config_apply_brightness(current_theme->rms_color, gb));
            }
        } else {
            set_label_if_changed(sc->lbl_rms_val, "--");
            if (current_theme) {
                set_text_color_cached(sc->lbl_rms_val, &sc->cached_rms_color,
                    app_config_apply_brightness(current_theme->text_color, gb));
            }
        }

        /* HFR */
        if (d->hfr > 0.001f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", d->hfr);
            set_label_if_changed(sc->lbl_hfr_val, buf);
            uint32_t hfr_col = theme_is_red_night(current_theme)
                ? 0 : app_config_get_hfr_color(d->hfr, i);
            if (hfr_col != 0) {
                set_text_color_cached(sc->lbl_hfr_val, &sc->cached_hfr_color,
                    app_config_apply_brightness(hfr_col, gb));
            } else if (current_theme) {
                set_text_color_cached(sc->lbl_hfr_val, &sc->cached_hfr_color,
                    app_config_apply_brightness(current_theme->hfr_color, gb));
            }
        } else {
            set_label_if_changed(sc->lbl_hfr_val, "--");
            if (current_theme) {
                set_text_color_cached(sc->lbl_hfr_val, &sc->cached_hfr_color,
                    app_config_apply_brightness(current_theme->text_color, gb));
            }
        }

        /* Time to flip — format "HH:MM:SS" as "Xh XXm" */
        if (d->meridian_flip[0] && strcmp(d->meridian_flip, "--") != 0
            && strcmp(d->meridian_flip, "FLIPPING") != 0) {
            int hh = 0, mm = 0;
            if (sscanf(d->meridian_flip, "%d:%d", &hh, &mm) >= 2) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%dh %02dm", hh, mm);
                set_label_if_changed(sc->lbl_flip_val, buf);
            } else {
                set_label_if_changed(sc->lbl_flip_val, d->meridian_flip);
            }
        } else if (d->meridian_flip[0]) {
            set_label_if_changed(sc->lbl_flip_val, d->meridian_flip);
        } else {
            set_label_if_changed(sc->lbl_flip_val, "--");
        }
        if (current_theme) {
            set_text_color_cached(sc->lbl_flip_val, &sc->cached_flip_color,
                app_config_apply_brightness(current_theme->text_color, gb));
        }

        /* Exposure detail line (1-card mode only) */
        if (visible <= 1 &&
            !lv_obj_has_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN)) {
            char detail[128] = "";
            int len = 0;

            if (d->exposure_total > 0) {
                len += snprintf(detail + len, sizeof(detail) - len,
                    "%ds %s", (int)d->exposure_total,
                    d->current_filter[0] ? d->current_filter : "");
            }
            if (d->exposure_total_count > 0 && len > 0) {
                int tsecs = (int)(d->exposure_total_count * d->exposure_total);
                int th = tsecs / 3600;
                int tm = (tsecs % 3600) / 60;
                if (th > 0) {
                    len += snprintf(detail + len, sizeof(detail) - len,
                        "  |  %d / %dh %02dm", d->exposure_total_count, th, tm);
                } else {
                    len += snprintf(detail + len, sizeof(detail) - len,
                        "  |  %d / %dm", d->exposure_total_count, tm);
                }
            } else if (d->exposure_iterations > 0 && len > 0) {
                len += snprintf(detail + len, sizeof(detail) - len,
                    "  |  %d / %d exp",
                    d->exposure_count, d->exposure_iterations);
            }
            if (d->stars > 0 && len > 0) {
                len += snprintf(detail + len, sizeof(detail) - len,
                    "  |  %d stars", d->stars);
            }
            if (d->target_time_remaining[0] && len > 0) {
                if (d->target_time_reason[0] &&
                    strcmp(d->target_time_reason, "TIME LIMIT") != 0) {
                    // Show constraint-specific label: "sets in 2:30" / "dawn in 1:45"
                    char reason_lower[16];
                    strlcpy(reason_lower, d->target_time_reason, sizeof(reason_lower));
                    for (int c = 0; reason_lower[c]; c++)
                        if (reason_lower[c] >= 'A' && reason_lower[c] <= 'Z')
                            reason_lower[c] += 32;
                    len += snprintf(detail + len, sizeof(detail) - len,
                        "  |  %s %s", reason_lower, d->target_time_remaining);
                } else {
                    len += snprintf(detail + len, sizeof(detail) - len,
                        "  |  %s left", d->target_time_remaining);
                }
            }

            set_label_if_changed(sc->lbl_detail, detail);

            if (current_theme) {
                set_text_color_cached(sc->lbl_detail, &sc->cached_detail_color,
                    app_config_apply_brightness(current_theme->text_color, gb));
            }
        }

        /* Safety monitor icon */
        if (sc->lbl_safety) {
            lv_obj_clear_flag(sc->lbl_safety, LV_OBJ_FLAG_HIDDEN);
            if (d->safety_connected) {
                if (d->safety_is_safe) {
                    set_label_if_changed(sc->lbl_safety, ICON_VERIFIED_USER);
                    set_text_color_cached(sc->lbl_safety, &sc->cached_safety_color,
                        theme_is_red_night(current_theme) ? 0x7f1d1d : 0x4CAF50);
                } else {
                    set_label_if_changed(sc->lbl_safety, ICON_GPP_BAD);
                    set_text_color_cached(sc->lbl_safety, &sc->cached_safety_color,
                        theme_is_red_night(current_theme) ? 0xff0000 : 0xF44336);
                }
            } else {
                set_label_if_changed(sc->lbl_safety, ICON_GPP_MAYBE);
                set_text_color_cached(sc->lbl_safety, &sc->cached_safety_color,
                    theme_is_red_night(current_theme) ? current_theme->label_color : 0x999999);
            }
        }
    }
}

/* ── Theme Application ─────────────────────────────────────────────── */

void summary_page_apply_theme(void) {
    if (!sum_page || !current_theme) return;

    int gb = app_config_get()->color_brightness;

    /* Update glass card style */
    apply_glass_theme();
    lv_obj_report_style_change(&style_glass_card);

    /* Invalidate all cached colors so the next update re-applies everything */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        summary_card_t *sc = &cards[i];
        sc->cached_name_color       = UINT32_MAX;
        sc->cached_filter_text_color = UINT32_MAX;
        sc->cached_filter_bg_color  = UINT32_MAX;
        sc->cached_filter_bg_opa    = UINT8_MAX;
        sc->cached_target_color     = UINT32_MAX;
        sc->cached_bar_ind_color    = UINT32_MAX;
        sc->cached_bar_bg_color     = UINT32_MAX;
        sc->cached_pct_color        = UINT32_MAX;
        sc->cached_seq_name_color   = UINT32_MAX;
        sc->cached_exp_val_color    = UINT32_MAX;
        sc->cached_seq_step_color   = UINT32_MAX;
        sc->cached_rms_color        = UINT32_MAX;
        sc->cached_hfr_color        = UINT32_MAX;
        sc->cached_flip_color       = UINT32_MAX;
        sc->cached_detail_color     = UINT32_MAX;
        sc->cached_safety_color     = UINT32_MAX;
    }

    /* Update per-card widgets */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        summary_card_t *sc = &cards[i];

        /* Stat labels + sequence title labels */
        lv_obj_t *labels[] = {
            sc->lbl_rms_label, sc->lbl_hfr_label, sc->lbl_flip_label,
            sc->lbl_seq_title, sc->lbl_exp_title, sc->lbl_step_title
        };
        for (int j = 0; j < 6; j++) {
            if (labels[j]) {
                lv_obj_set_style_text_color(labels[j],
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->label_color, gb)), 0);
            }
        }

        /* Bar background */
        if (sc->bar_progress) {
            lv_obj_set_style_bg_color(sc->bar_progress,
                lv_color_hex(current_theme->bento_border), 0);
        }
    }

    /* Update empty state theme via shared component */
    nina_empty_state_apply_theme(empty_cont, current_theme, gb);

    lv_obj_invalidate(sum_page);
}
