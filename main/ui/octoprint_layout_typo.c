/**
 * @file octoprint_layout_typo.c
 * @brief OctoPrint layout 3 — "Large numerals" (mockup v5, typographic minimal).
 *
 * Geometry only. Typography is the interface here: no cards, no fills, no
 * gradients. A giant Playfair completion numeral is the hero, a single hairline
 * track carries the progress bar, and everything else is flat type separated by
 * 1 px rules. One restrained accent (the theme's progress colour) does all the
 * emphasis.
 *
 * Structure (640 x 668 content area):
 *   head     brand | state + connection word          rule
 *   file     ellipsised job name
 *   error    fixed 20 px slot, recolours in place
 *   hero     Playfair 228 percentage + "%"  /  COMPLETE + LAYER n / N
 *   track    4 px hairline progress bar, PROGRESS | FIRMWARE M73 caption
 *            rule
 *   metrics  ELAPSED | REMAINING | FINISH AT, three equal typographic cells
 *            rule
 *   foot     nozzle + bed underline-fill temperatures | 198 px image frame
 *
 * Deviations from the mockup, both forced by the seam and worth knowing:
 *
 * 1. No webcam peek chip / THUMB-CAM affordance (instructed). The image is one
 *    area whose tag label names whichever source the config selected.
 * 2. The mockup marks the firmware M73 value as a tick ON the progress track.
 *    The update path exposes no positioning hook for such a tick (it only
 *    shows/hides `m73_row` and writes `lbl_m73`), so M73 is rendered as the
 *    right-hand track caption instead. Nothing here reads live data, by design.
 *
 * The Playfair faces cover only 0x30-0x3A and 0xB0 — no '.', which the update
 * path always writes ("%.1f"). A per-layout font copy with a Montserrat
 * fallback supplies the decimal point; see typo_pct_font().
 */

#include "nina_octoprint_internal.h"

LV_FONT_DECLARE(lv_font_playfair_228);

#define TYPO_PAD_H     24    /* 688 - 2*24 = 640 content width  */
#define TYPO_PAD_TOP   12
#define TYPO_PAD_BOT   8
#define TYPO_IMG_W     198   /* image frame; temps get the remaining 418 */
#define TYPO_HERO_LSP  (-4)  /* mockup tracks the giant numeral tight */
#define TYPO_TEMP_H    38
#define TYPO_ERR_H     20    /* fixed so a fault never reflows the page */

/* Playfair 228 with a Montserrat fallback so the decimal point renders.
 * Not update-path state: it is a font, written once per build with identical
 * values, and the labels only ever hold its address. */
static lv_font_t s_pct_font;

static const lv_font_t *typo_pct_font(void);
static lv_obj_t *typo_rule(lv_obj_t *parent, int margin_top);
static void typo_flatten_chip(lv_obj_t *chip);
static void typo_style_temp(octo_temp_el_t *t);
static void build_head(lv_obj_t *page, octoprint_widgets_t *w);
static void build_hero(lv_obj_t *page, octoprint_widgets_t *w);
static void build_track(lv_obj_t *page, octoprint_widgets_t *w);
static void build_metrics(lv_obj_t *page, octoprint_widgets_t *w);
static void build_foot(lv_obj_t *page, octoprint_widgets_t *w);

/* ── Helpers ──────────────────────────────────────────────────────────── */

static const lv_font_t *typo_pct_font(void)
{
    s_pct_font = lv_font_playfair_228;
    s_pct_font.fallback = &lv_font_montserrat_64;
    return &s_pct_font;
}

/** Full-width hairline. Built from the shared card style so the nine themes
 *  re-colour it for free via lv_obj_report_style_change(). */
static lv_obj_t *typo_rule(lv_obj_t *parent, int margin_top)
{
    lv_obj_t *rule = octo_w_card(parent);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_set_style_margin_top(rule, margin_top, 0);
    return rule;
}

/** Strip a chip back to bare type: this layout has no pills, only words.
 *  apply_theme only rewrites chip bg/border *colours*, so a transparent,
 *  border-less chip stays flat across theme changes. */
static void typo_flatten_chip(lv_obj_t *chip)
{
    if (!chip) {
        return;
    }
    lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_radius(chip, 0, 0);
    lv_obj_set_style_pad_hor(chip, 0, 0);
    lv_obj_set_style_pad_ver(chip, 0, 0);
}

/** Recast the horizontal temperature element as "number, then an understated
 *  underline that fills from the cold baseline". The value string already
 *  carries actual and target, so the end-of-scale caption is dropped. */
static void typo_style_temp(octo_temp_el_t *t)
{
    if (!t || !t->root) {
        return;
    }
    lv_obj_set_flex_align(t->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    if (t->lbl_name) {
        lv_obj_set_width(t->lbl_name, 62);
        lv_obj_set_style_margin_bottom(t->lbl_name, 5, 0);
    }
    if (t->lbl_value) {
        lv_obj_set_style_text_font(t->lbl_value, &lv_font_montserrat_24, 0);
        lv_obj_set_width(t->lbl_value, 176);
    }
    if (t->bar) {
        lv_obj_set_height(t->bar, 4);
        lv_obj_set_style_radius(t->bar, 0, 0);
        lv_obj_set_style_radius(t->bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_margin_bottom(t->bar, 9, 0);
    }
    if (t->lbl_scale) {
        lv_obj_add_flag(t->lbl_scale, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Head: brand, state, connection word, file name, error slot ───────── */

static void build_head(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *head = octo_w_row(page, true, 0);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *brand = octo_w_label(head, "OCTOPRINT", &lv_font_montserrat_12,
                                   &octo_style_label);
    lv_obj_set_style_text_letter_space(brand, 5, 0);

    lv_obj_t *right = octo_w_row(head, true, 16);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    octo_w_state_line(right, w);
    if (w->state_dot) {
        lv_obj_set_size(w->state_dot, 6, 6);
    }

    /* Connection as a tiny caps word, not a pill. */
    octo_w_conn_chip(right, w);
    typo_flatten_chip(w->conn_chip);
    if (w->lbl_conn) {
        lv_obj_set_style_text_letter_space(w->lbl_conn, 2, 0);
    }

    typo_rule(page, 10);

    octo_w_file_label(page, w);
    if (w->lbl_file) {
        lv_obj_set_style_margin_top(w->lbl_file, 12, 0);
    }

    /* Fixed-height slot: reads "No faults" in the muted label colour and
     * recolours in place when the update path reports one. Nothing moves. */
    octo_w_status_strip(page, w);
    if (w->error_strip) {
        typo_flatten_chip(w->error_strip);
        lv_obj_set_size(w->error_strip, LV_PCT(100), TYPO_ERR_H);
        lv_obj_set_style_margin_top(w->error_strip, 6, 0);
    }
    if (w->lbl_error) {
        lv_obj_set_style_text_letter_space(w->lbl_error, 2, 0);
    }
}

/* ── Hero: the giant numeral ──────────────────────────────────────────── */

static void build_hero(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, 12);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(row, 6, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* Worst case "100.0" is ~500 px of the 640 px column, so the numeral owns a
     * full-width band and the image sits in the foot rather than beside it. */
    w->lbl_pct = octo_w_label(row, "--", typo_pct_font(), &octo_style_value);
    lv_obj_set_style_text_letter_space(w->lbl_pct, TYPO_HERO_LSP, 0);

    /* Playfair has no '%'; the sign is a Montserrat cut, sitting slightly high
     * against the giant baseline, which reads as a deliberate superscript. */
    w->lbl_pct_unit = octo_w_label(row, "%", &lv_font_montserrat_48,
                                   &octo_style_label);

    lv_obj_t *cap = octo_w_row(page, true, 18);
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_height(cap, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(cap, 2, 0);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    w->lbl_pct_sub = octo_w_caption(cap, "COMPLETE");

    /* Layer rides the hero caption line and hides wholesale without DLP. */
    w->layer_cell = octo_w_row(cap, true, 7);
    lv_obj_set_size(w->layer_cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    octo_w_caption(w->layer_cell, "LAYER");
    w->lbl_layer_cur = octo_w_label(w->layer_cell, "--", &lv_font_montserrat_20,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(w->layer_cell, "/ --", &lv_font_montserrat_14,
                                      &octo_style_label);
}

/* ── Track: one hairline, one caption line ────────────────────────────── */

static void build_track(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* The card supplies the themed hairline; the bar inside is transparent
     * except for its accent indicator, so the track survives apply_theme
     * (which rewrites the bar's own background to the page ground). */
    lv_obj_t *track = octo_w_card(page);
    lv_obj_set_size(track, LV_PCT(100), 4);
    lv_obj_set_style_radius(track, 0, 0);
    lv_obj_set_style_margin_top(track, 18, 0);

    octo_w_progress_bar(track, w);
    if (w->bar_progress) {
        lv_obj_set_size(w->bar_progress, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_radius(w->bar_progress, 0, 0);
        lv_obj_set_style_radius(w->bar_progress, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(w->bar_progress, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(w->bar_progress, 0, 0);
    }

    lv_obj_t *cap = octo_w_row(page, true, 0);
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_height(cap, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(cap, 8, 0);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    octo_w_caption(cap, "PROGRESS");

    w->m73_row = octo_w_row(cap, true, 8);
    lv_obj_set_size(w->m73_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->m73_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    octo_w_caption(w->m73_row, "FIRMWARE");
    w->lbl_m73 = octo_w_label(w->m73_row, "M73 --", &lv_font_montserrat_14,
                              &octo_style_value);
}

/* ── Metrics: three equal typographic cells on a rule ─────────────────── */

static void build_metrics(lv_obj_t *page, octoprint_widgets_t *w)
{
    typo_rule(page, 16);

    lv_obj_t *row = octo_w_row(page, true, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(row, 12, 0);

    lv_obj_t *elapsed = octo_w_time_tile(row, "ELAPSED", &lv_font_montserrat_28,
                                         &w->lbl_elapsed);
    lv_obj_set_flex_grow(elapsed, 1);

    lv_obj_t *remaining = octo_w_time_tile(row, "REMAINING", &lv_font_montserrat_28,
                                           &w->lbl_remaining);
    lv_obj_set_flex_grow(remaining, 1);

    /* DLP-sourced: the whole cell is the hide target. */
    w->finish_cell = octo_w_time_tile(row, "FINISH AT", &lv_font_montserrat_28,
                                      &w->lbl_finish);
    lv_obj_set_flex_grow(w->finish_cell, 1);
}

/* ── Foot: temperatures and the image frame ───────────────────────────── */

static void build_foot(lv_obj_t *page, octoprint_widgets_t *w)
{
    typo_rule(page, 14);

    lv_obj_t *row = octo_w_row(page, true, 24);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_margin_top(row, 14, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *temps = octo_w_row(row, false, 22);
    lv_obj_set_height(temps, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(temps, 1);

    lv_obj_t *nozzle = octo_w_temp(temps, "NOZZLE", false, true, &w->nozzle);
    lv_obj_set_size(nozzle, LV_PCT(100), TYPO_TEMP_H);
    typo_style_temp(&w->nozzle);

    lv_obj_t *bed = octo_w_temp(temps, "BED", false, false, &w->bed);
    lv_obj_set_size(bed, LV_PCT(100), TYPO_TEMP_H);
    typo_style_temp(&w->bed);

    /* Square frame, not a rounded card: the only bordered box on the page. */
    lv_obj_t *img = octo_w_image_hero(row, w);
    lv_obj_set_size(img, TYPO_IMG_W, LV_PCT(100));
    lv_obj_set_style_radius(img, 0, 0);
    if (w->lbl_img_tag) {
        lv_obj_set_style_text_font(w->lbl_img_tag, &lv_font_montserrat_12, 0);
        lv_obj_align(w->lbl_img_tag, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    }
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void typo_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 0, 0);      /* spacing is per-block margin */
    lv_obj_set_style_pad_hor(page, TYPO_PAD_H, 0);
    lv_obj_set_style_pad_top(page, TYPO_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(page, TYPO_PAD_BOT, 0);

    build_head(page, w);
    build_hero(page, w);
    build_track(page, w);
    build_metrics(page, w);
    build_foot(page, w);
}

const octoprint_layout_ops_t octoprint_layout_typo = {
    .name  = "Large numerals",
    .build = typo_build,
};
