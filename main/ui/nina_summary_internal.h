#pragma once

/**
 * @file nina_summary_internal.h
 * @brief Layout seam for the Summary page.
 *
 * nina_summary.c owns the data, the interpolation timer, the formatting and
 * every colour; a card builder creates and places widgets and writes the
 * handles below. A builder may leave any handle NULL and the update path null
 * checks it, which is how the round card drops the numbers its rings already
 * tell. Only included by nina_summary.c and nina_summary_round.c.
 */

#include "lvgl.h"

#include "nina_subbar.h"

/* Bullseye bearings on the round card, degrees clockwise from twelve o'clock
 * (radial.html board 3). Single owner shared by the builder
 * (nina_summary_round.c) and the update path (nina_summary.c); the round
 * Dashboard uses the same pair under its own name, so a dot's direction means
 * the same thing on both boards. */
#define SR_BEARING_RMS  305
#define SR_BEARING_HFR  125

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
    /* NINA-domain clock pair copied from nina_client_t while the data lock is
     * held (summary update path); read lock-free by summary_bar_interp_cb.
     * cached_nina_epoch == 0 -> unknown, fall back to time(NULL). */
    int64_t cached_nina_epoch;   /* NINA-PC UTC epoch at capture (HTTP Date) */
    int64_t cached_nina_mono_us; /* esp_timer_get_time() at capture */
    /* Cached style values: only call lv_obj_set_style_* when changed to avoid
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
    /* Safety state colour. The square card spends it on lbl_safety, the round
     * card on ring_crown; exactly one of the two handles exists per card, so
     * the two writers never collide. */
    uint32_t cached_safety_color;

    /* Round shape handles (radial board 3). NULL on the square card; the
     * update path drives each one only when it is set. */
    nina_subbar_t ring;            /* this rig's concentric sub-block ring */
    lv_obj_t *ring_crown;          /* safety crown at twelve o'clock */
    lv_obj_t *ring_flip_tick;      /* meridian flip tick on the same ring */
    lv_obj_t *rms_bull;
    lv_obj_t *rms_dot;
    lv_obj_t *hfr_bull;
    lv_obj_t *hfr_dot;
} summary_card_t;

/* The shipped glass card style, owned and re-themed by nina_summary.c. */
extern lv_style_t style_glass_card;

/** @brief Make a card clickable and route its tap to that rig's page. */
void summary_bind_card_tap(lv_obj_t *card, int instance_index);

/**
 * @brief Build one round card plus its ring set (round family only).
 *
 * Defined in nina_summary_round.c. @p slot is the fixed instance identity,
 * 0..MAX_NINA_INSTANCES-1, which also picks the ring radius and the card's
 * chord-derived width. LVGL lock held by caller.
 */
void nina_summary_round_create_card(summary_card_t *sc, lv_obj_t *parent, int slot);

/**
 * @brief Rank the shown rigs outward (round family only).
 *
 * Online rigs take the outermost rings in slot order: with rigs 1 and 2
 * offline, rig 3 sits on the rim ring instead of two pitches in. Called once
 * per summary update after visibility is settled; @p shown is indexed by slot.
 */
void nina_summary_round_place_rings(summary_card_t *cards, const bool *shown);
