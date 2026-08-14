/**
 * @file octoprint_layout_typo.c
 * @brief OctoPrint layout 3 — "Large numerals" (mockup v5 rev 2, typographic).
 *
 * Geometry only. Typography is the interface: no cards, no pills, no fills, no
 * gradients, no progress track. The page is one right-hand type axis over one
 * block of negative space, and the giant Playfair completion numeral IS the
 * progress widget.
 *
 * Structure (640 x 668 content area):
 *   head    brand left | state + connection right-justified
 *           rule
 *   error   fixed-height slot, right-aligned, recolours in place
 *   hero    right-justified Playfair 228 percentage + "%"
 *           right-justified "COMPLETE  LAYER n / N" under it
 *           rule
 *   lower   left column: ELAPSED / REMAINING / FINISH AT stacked vertically,
 *           hairline, then text-only NOZZLE and BED lines.
 *           right: bare bordered image frame, bottom-aligned with the
 *           temperature lines.
 *
 * Vertical budget, in LVGL line heights (not nominal font sizes: montserrat
 * 14/24/32/48 measure 16/27/35/52, playfair 228 measures 191):
 *
 *   head 18 + rule 13 + error 30 + hero 209 + caption 30 + rule 21
 *   + lower margin 16 + lower content 280  =  617 of 668, ~51 px spare.
 *
 * The spare sits under the BED line as bottom margin. The previous version
 * grew the lower row to fill the box and let the 306 px image frame set the
 * row height: the image, not the type, was the tallest item, so the bottom-
 * aligned text stack was pinned to the very last pixel of the content box
 * (BED clipped off-screen) while every pixel of slack piled up as a dead band
 * above ELAPSED. The row now hugs its content and the image is sized under
 * the text stack, so the stack sets the height and the slack lands at the
 * bottom where it belongs.
 *
 * Deviations from the mockup, all forced by the seam:
 *
 * 1. The mockup splits the numeral into a 228 px integer part and a 90 px accent
 *    fraction. The update path writes one "%.1f" string into one label, and a
 *    layout may not read live data, so it is a single 228 px run.
 * 2. No webcam peek chip / source tag: the image hero is a bare frame.
 * 3. The frame is 268 px, not the mockup's 306: at 306 it out-measures the
 *    temperature stack and overruns the 668 px content box.
 *
 * The Playfair faces cover only 0x30-0x3A and 0xB0 — no '.', which the update
 * path always writes. A per-layout font copy with a Montserrat fallback supplies
 * the decimal point; see typo_pct_font().
 */

#include "nina_octoprint_internal.h"

LV_FONT_DECLARE(lv_font_playfair_228);

#define TYPO_PAD_H       24    /* 688 - 2*24 = 640 content width */
#define TYPO_PAD_TOP     12
#define TYPO_PAD_BOT     8
#define TYPO_LEFT_W      300   /* time stack + temperature column */
#define TYPO_IMG         268   /* bare square frame; under the ~280 px text
                                * stack so the type sets the row height. The
                                * mockup's 306 does not fit the 668 px box. */
#define TYPO_HERO_LSP    (-4)  /* the mockup tracks the numeral tight */
#define TYPO_ERR_H       20    /* fixed so a fault never reflows the page */
#define TYPO_TEMP_NAME_W 88    /* keeps both readings on one left edge */
#define TYPO_STACK_GAP   14    /* between the three stacked time cells */

/* Playfair 228 with a Montserrat fallback so the decimal point renders.
 * Not update-path state: it is a font, written once per build with identical
 * values, and the labels only ever hold its address. */
static lv_font_t s_pct_font;

static const lv_font_t *typo_pct_font(void);
static lv_obj_t *typo_rule(lv_obj_t *parent, int margin_top);
static void typo_flatten_chip(lv_obj_t *chip);
static void typo_style_temp(octo_temp_el_t *t, int margin_top);
static void build_head(lv_obj_t *page, octoprint_widgets_t *w);
static void build_hero(lv_obj_t *page, octoprint_widgets_t *w);
static void build_lower(lv_obj_t *page, octoprint_widgets_t *w);

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

/** Text-only temperature line: name in a fixed-width column so both readings
 *  share one left edge, reading beside it. No bar, no underline, no fill. */
static void typo_style_temp(octo_temp_el_t *t, int margin_top)
{
    if (!t || !t->root || !t->lbl_value) {
        return;
    }
    lv_obj_set_width(t->root, LV_PCT(100));
    lv_obj_set_height(t->root, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(t->root, margin_top, 0);

    /* The factory puts name and value in a SPACE_BETWEEN row under the root;
     * this layout wants them packed left against a fixed name column. */
    lv_obj_t *line = lv_obj_get_parent(t->lbl_value);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    if (t->lbl_name) {
        lv_obj_set_width(t->lbl_name, TYPO_TEMP_NAME_W);
        lv_obj_set_style_text_letter_space(t->lbl_name, 2, 0);
    }
    lv_obj_set_style_text_font(t->lbl_value, &lv_font_montserrat_24, 0);
}

/* ── Head: brand, state, connection word, error slot ──────────────────── */

static void build_head(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *head = octo_w_row(page, true, 0);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *brand = octo_w_label(head, "OCTOPRINT", &lv_font_montserrat_12,
                                   &octo_style_label);
    lv_obj_set_style_text_letter_space(brand, 5, 0);

    /* State and connection right-justified on the header baseline. */
    lv_obj_t *right = octo_w_row(head, true, 14);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    octo_w_state_line(right, w);
    /* No dots anywhere in this design: the accent-coloured state word is the
     * indicator. The handle stays live, the update path just recolours nothing
     * visible. */
    if (w->state_dot) {
        lv_obj_add_flag(w->state_dot, LV_OBJ_FLAG_HIDDEN);
    }

    octo_w_conn_chip(right, w);
    typo_flatten_chip(w->conn_chip);
    if (w->conn_dot) {
        lv_obj_add_flag(w->conn_dot, LV_OBJ_FLAG_HIDDEN);
    }
    if (w->lbl_conn) {
        lv_obj_set_style_text_letter_space(w->lbl_conn, 2, 0);
    }

    typo_rule(page, 12);

    /* Fixed-height slot on the right axis: reads "No faults" in the muted label
     * colour and turns alert-red in place when a fault is reported. Nothing
     * reflows. */
    octo_w_status_strip(page, w);
    if (w->error_strip) {
        typo_flatten_chip(w->error_strip);
        lv_obj_set_size(w->error_strip, LV_PCT(100), TYPO_ERR_H);
        lv_obj_set_style_margin_top(w->error_strip, 10, 0);
        lv_obj_set_flex_align(w->error_strip, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    if (w->error_dot) {
        lv_obj_add_flag(w->error_dot, LV_OBJ_FLAG_HIDDEN);
    }
    if (w->lbl_error) {
        lv_obj_set_style_text_letter_space(w->lbl_error, 2, 0);
    }
}

/* ── Hero: the giant right-justified numeral ──────────────────────────── */

static void build_hero(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(page, true, 10);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(row, 18, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    w->lbl_pct = octo_w_label(row, "--", typo_pct_font(), &octo_style_value);
    lv_obj_set_style_text_letter_space(w->lbl_pct, TYPO_HERO_LSP, 0);

    /* Playfair has no '%'; a muted Montserrat cut supplies it, lifted off the
     * bottom so it sits near the giant baseline rather than under it. */
    w->lbl_pct_unit = octo_w_label(row, "%", &lv_font_montserrat_48,
                                   &octo_style_label);
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, 12, 0);

    /* Caption line, right-justified on the same edge as the numeral. */
    lv_obj_t *cap = octo_w_row(page, true, 24);
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_height(cap, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(cap, 8, 0);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    w->lbl_pct_sub = octo_w_caption(cap, "COMPLETE");
    lv_obj_set_style_text_letter_space(w->lbl_pct_sub, 2, 0);

    /* Layer rides the caption line and hides wholesale without DLP. */
    w->layer_cell = octo_w_row(cap, true, 7);
    lv_obj_set_size(w->layer_cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *layer_cap = octo_w_caption(w->layer_cell, "LAYER");
    lv_obj_set_style_text_letter_space(layer_cap, 2, 0);
    w->lbl_layer_cur   = octo_w_label(w->layer_cell, "--", &lv_font_montserrat_20,
                                      &octo_style_value);
    w->lbl_layer_total = octo_w_label(w->layer_cell, "/ --", &lv_font_montserrat_14,
                                      &octo_style_label);
}

/* ── Lower zone: time stack + temperatures left, image frame right ────── */

static void build_lower(lv_obj_t *page, octoprint_widgets_t *w)
{
    typo_rule(page, 20);

    lv_obj_t *row = octo_w_row(page, true, 0);
    lv_obj_set_width(row, LV_PCT(100));
    /* Content height, not flex-grow: growing it to the bottom of the page put
     * every spare pixel above ELAPSED and pinned BED to the last pixel of the
     * content box. Hugging the content leaves the spare as bottom margin. */
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(row, 16, 0);
    /* Cross-END bottom-aligns the image frame with the last temperature line. */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *left = octo_w_row(row, false, 0);
    lv_obj_set_size(left, TYPO_LEFT_W, LV_SIZE_CONTENT);

    lv_obj_t *elapsed = octo_w_time_tile(left, "ELAPSED", &lv_font_montserrat_32,
                                         &w->lbl_elapsed);
    lv_obj_set_style_margin_bottom(elapsed, TYPO_STACK_GAP, 0);

    lv_obj_t *remaining = octo_w_time_tile(left, "REMAINING", &lv_font_montserrat_32,
                                           &w->lbl_remaining);
    lv_obj_set_style_margin_bottom(remaining, TYPO_STACK_GAP, 0);

    /* DLP-sourced: the whole cell is the hide target. */
    w->finish_cell = octo_w_time_tile(left, "FINISH AT", &lv_font_montserrat_32,
                                      &w->lbl_finish);
    lv_obj_set_style_margin_bottom(w->finish_cell, TYPO_STACK_GAP, 0);

    /* Second hairline separates the time stack from the temperatures. */
    typo_rule(left, 0);

    octo_w_temp(left, "NOZZLE", OCTO_TEMP_TEXT_ONLY, true, &w->nozzle);
    typo_style_temp(&w->nozzle, 14);

    octo_w_temp(left, "BED", OCTO_TEMP_TEXT_ONLY, false, &w->bed);
    typo_style_temp(&w->bed, 10);

    /* Bare bordered square, not a rounded card: the only box on the page. */
    lv_obj_t *img = octo_w_image_hero(row, w);
    lv_obj_set_size(img, TYPO_IMG, TYPO_IMG);
    lv_obj_set_style_radius(img, 0, 0);
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
    build_lower(page, w);
}

const octoprint_layout_ops_t octoprint_layout_typo = {
    .name  = "Large numerals",
    .build = typo_build,
};
