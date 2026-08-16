/**
 * @file octoprint_layout_overlay.c
 * @brief OctoPrint layout 5 — "Floating overlay" (mockup 2A).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from an octo_color() token or
 * a shared style, so all nine themes work without a change here. This file never
 * reads octoprint_data_t and never calls app_config_get().
 *
 * The webcam/preview frame IS the page: it fills the whole 720 x 720 box edge
 * to edge, with no bands anywhere. The picture is scaled ONCE in the spine to
 * cover that box and its excess is cropped (a 4:3 webcam is staged 960x720 and
 * shows its middle 720 px), which is what octoprint_layout_ops_t::image_cover
 * below asks for. The text floats ON the picture over a TRANSLUCENT TINT: three
 * containers, each welded to a screen edge, fill with OCTO_COL_BG at
 * OV_TINT_OPA (90 of 255, ~35 %) — enough ground to read against a bright or
 * busy frame, sheer enough that the picture still reads through. Two sit in the
 * top corners and one full-width group runs along the bottom, carrying the
 * completion story and ending in an edge-to-edge progress track.
 *
 * LEGIBILITY. The translucent tinted panels are what carry the text: every
 * reading sits on one, so the type always has its own ground rather than the
 * bare picture. The error strip and the connection chip keep their own darker
 * scrim on top of that, because they carry warning text that must never be a
 * judgement call.
 *
 * READINGS TOGGLE. Everything except the image ground is parented to
 * w->overlay_layer (octo_w_overlay_layer()), a full-page transparent
 * LV_LAYOUT_NONE container, so the spine hides or shows the whole readout as
 * ONE object. Every position here is absolute, so nothing reflows when it comes
 * back.
 *
 * Structure (720 x 720 full-bleed page, absolute placement, LV_LAYOUT_NONE).
 * Two children of the page, in z-order: the hero, then the overlay layer that
 * carries every reading:
 *   page
 *     0,0        image ground     full-bleed hero, cover-cropped, no radius/border
 *     overlay_layer (full-page, transparent, hidden/shown as one)
 *       0,0        top-left panel   STATUS | rule | TIME ELAPSED | rule | LAYER
 *       right,0    top-right panel  NOZZLE  BED  (OCTO_TEMP_COMPACT, SIZE_CONTENT)
 *       0,78       error strip  28  left-anchored, absolute, hidden unless faulted
 *       0,112      conn chip        left-anchored, absolute, hidden when healthy
 *       bottom     bottom panel     pct(56) + "%"   <-->   ETA + time(56, content width)
 *       bottom-0   track        12  flush 0..720, square ends, no border
 *
 * Top-row horizontal budget at 720 px (worst-case samples, kerning ignored, so
 * a few px conservative). Both corner panels are SIZE_CONTENT and anchored to
 * their own corner, so they only meet if the sum runs past 720:
 *   left   16 pad + 173 status ("OPERATIONAL" bold_22 + 2)
 *          + 25 (12 gap + 1 rule + 12 gap) + 117 elapsed ("TIME ELAPSED" m14 + 12)
 *          + 25 + 134 layer ("9999" 56 + 6 gap + "/ 9999" 70 + 2) + 16 pad = 506
 *   right  16 + 68 NOZZLE + 24 gap + 73 BED + 16 = 197
 *   sum 703, 17 px clear. That is why OV_TOP_GAP is 12 and not 16: at 16 the
 *   four gaps cost 16 more and the two panels touch (719).
 * The LAYER tile carries its own leading rule inside w->layer_cell, so hiding
 * the cell (no DisplayLayerProgress) takes the rule with it and the panel
 * closes up to STATUS | TIME ELAPSED with nothing dangling.
 *
 * Bottom-row budget: 16 pad + 153 pct ("100.0" bold_56 + 2) + 4 + 27 "%"
 *   <spread>  35 "ETA" (m16 + letter space) + 10 + time + 16 pad. The finish
 *   cell is CONTENT-DRIVEN: the time label is its natural width and the cell is
 *   anchored to the row's right edge, so "ETA" rides 10 px left of the digits
 *   whether they read "10:27" (~160 px) or the worst-case 12-hour "12:56 PM"
 *   (~250 px, total 511 of 720). The row's height is the taller of the two
 *   56 px faces, 59 px, unchanged from when the finish time was 28 px (the
 *   percent already set it).
 *
 * This layout sets octoprint_layout_ops_t::full_bleed, so nina_octoprint.c hands
 * it the whole 720 x 720 screen with the dashboard's 16 px outer padding negated.
 */

#include "nina_octoprint_internal.h"

/* ── Geometry ─────────────────────────────────────────────────────────── */

#define OV_PAD_H        16
#define OV_PAD_V        12
#define OV_GAP           6

/* Panel tint. Not a card: no border, no shadow of its own, just enough ground
 * under the type to survive a bright frame. 90 of 255 is ~35 %. */
#define OV_TINT_OPA     90
#define OV_TINT_RADIUS   8

/* Height of the top-left group, derived from the generated font metrics rather
 * than measured on screen, for the tallest of its cells (elapsed and layer tie):
 *   pad_top OV_PAD_V 12 + caption (montserrat_14, line_height 16) + 2 gap
 *   + value (montserrat_22, line_height 24) + pad_bottom OV_PAD_V 12   =   66
 * rounded up to 72 for slack. Only the error strip and the connection chip
 * below depend on it, and both are absolutely placed, so a few pixels of error
 * costs a slightly wider gap and never an overlap. */
#define OV_TOP_H        72
#define OV_ERR_H        28
#define OV_ERR_Y       (OV_TOP_H + OV_GAP)
#define OV_CONN_Y      (OV_ERR_Y + OV_ERR_H + OV_GAP)

#define OV_DIV_W         1
#define OV_DIV_H        40
#define OV_TOP_GAP      12   /* between the blocks inside the top-left group; 16
                              * puts the two top panels in contact (see header) */
#define OV_TEMP_GAP     24
#define OV_BAR_H        12
#define OV_ETA_GAP      10   /* between "ETA" and the 56 px time */

/* Baseline lift for the small type sitting beside a 56 px face. The row packs
 * on its bottom EDGE, but the eye wants a shared BASELINE, so each small label
 * is raised by the difference in font base_line (descender depth), read from
 * the generated font tables:
 *   bold_56 base_line 11, bold_28 base_line 5       -> 11 - 5 = 6  (the "%")
 *   bold_56 base_line 11, montserrat_16 base_line 3 -> 11 - 3 = 8  ("ETA") */
#define OV_LIFT_UNIT     6
#define OV_LIFT_ETA      8

/* Fixed-width samples. Every live value is clipped to the width of its widest
 * plausible reading, so a digit change can never reflow a neighbour. */
#define OV_S_STATE      "OPERATIONAL"
#define OV_S_ELAPSED    "00:00:00"
#define OV_S_TEMP       "999.9\xC2\xB0"
#define OV_S_PCT        "100.0"
#define OV_S_LAYER_TOT  "/ 9999"

/* ── Primitives ───────────────────────────────────────────────────────── */

/**
 * Give @p panel the shared translucent ground: OCTO_COL_BG at OV_TINT_OPA, no
 * border. @p radius is OV_TINT_RADIUS for the two corner panels and 0 for the
 * bottom one, which is edge-to-edge and would only round into the screen edge.
 */
static void tint_panel(lv_obj_t *panel, int32_t radius)
{
    lv_obj_set_style_bg_color(panel, lv_color_hex(octo_color(OCTO_COL_BG)), 0);
    lv_obj_set_style_bg_opa(panel, OV_TINT_OPA, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, radius, 0);
}

/**
 * Pin @p lbl to the pixel width of @p sample in @p font and clip anything
 * longer. Letter spacing is zeroed first: octo_text_width() measures the bare
 * glyph run while the shared label style adds 2 px per character, which would
 * otherwise push a full-width reading past the measured box.
 */
static void fixed_label(lv_obj_t *lbl, const lv_font_t *font,
                        const char *sample, lv_text_align_t align)
{
    if (!lbl) {
        return;
    }
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_letter_space(lbl, 0, 0);
    lv_obj_set_width(lbl, octo_text_width(font, sample) + 2);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl, align, 0);
}

/**
 * Shared column width for a caption/value pair that right-aligns as one block:
 * the wider of the two, so their right edges land on the same pixel whichever
 * is longer. The caption measurement is topped up because octo_text_width()
 * measures at letter_space 0 while these captions are tracked out by 1 px per
 * character. The 2 px slack is added to the SHARED width, so both members keep
 * their right edges on one pixel.
 */
static int32_t pair_width(const lv_font_t *cap_font, const char *cap_text,
                          const lv_font_t *val_font, const char *val_sample)
{
    int32_t cap_w = octo_text_width(cap_font, cap_text) + 8;
    int32_t val_w = octo_text_width(val_font, val_sample);
    return ((cap_w > val_w) ? cap_w : val_w) + 2;
}

/**
 * Micro caption at the sandwich scale: 14 px (16 for "ETA" in the bottom bar),
 * letter space 1, natural width. Never LV_PCT: a percentage-width child is
 * excluded from a SIZE_CONTENT parent's width, so it would wrap and grow the
 * row instead of sizing it.
 */
static lv_obj_t *caption(lv_obj_t *parent, const char *text,
                         const lv_font_t *font)
{
    lv_obj_t *cap = octo_w_caption(parent, text);
    lv_obj_set_style_text_font(cap, font, 0);
    lv_obj_set_style_text_letter_space(cap, 1, 0);
    return cap;
}

/* ── Image ground ─────────────────────────────────────────────────────── */

static void build_ground(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* The hero fills the page. Its host is a card, so the no-image state is a
     * deliberate dark ground with a centred label rather than a hole in the UI;
     * radius and border are dropped so it truly bleeds to the edges. */
    lv_obj_t *host = octo_w_image_hero(page, w);
    lv_obj_set_size(host, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(host, 0, 0);
    lv_obj_set_style_radius(host, 0, 0);
    lv_obj_set_style_border_width(host, 0, 0);

    if (w->img_hero) {
        /* CENTER, never LVGL's CONTAIN or COVER: both of those are software
         * transforms re-run on every redraw. This layout sets
         * octoprint_layout_ops_t::image_cover, so stage_image() has already
         * scaled the frame ONCE to cover this exact box (a 4:3 webcam becomes
         * 960x720), and CENTER shows its middle 720x720 at scale 1:1 while the
         * hero clips the overhang. CONTAIN here would DOWNscale that
         * cover-sized copy back into the box -- a transform, and the bands
         * back. */
        lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_CENTER);
    }
    if (w->img_placeholder) {
        lv_obj_set_style_text_font(w->img_placeholder, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_letter_space(w->img_placeholder, 3, 0);
        lv_obj_center(w->img_placeholder);
    }
}

/* ── Top-left group: state + elapsed + layer ──────────────────────────── */

/** Vertical hairline in OCTO_COL_BORDER, OV_DIV_W x OV_DIV_H. */
static lv_obj_t *hairline(lv_obj_t *parent)
{
    lv_obj_t *rule = octo_w_row(parent, false, 0);
    lv_obj_set_size(rule, OV_DIV_W, OV_DIV_H);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
    return rule;
}

static void build_top_left(lv_obj_t *parent, octoprint_widgets_t *w)
{
    /* Tinted container welded flush into the corner, sized tight to its content;
     * the tint is what separates the type from the picture behind it. */
    lv_obj_t *p = octo_w_row(parent, true, 0);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
    tint_panel(p, OV_TINT_RADIUS);
    lv_obj_set_style_pad_hor(p, OV_PAD_H, 0);
    lv_obj_set_style_pad_ver(p, OV_PAD_V, 0);
    lv_obj_set_style_pad_column(p, OV_TOP_GAP, 0);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Left half of the screen: caption and value alike are left-justified. */
    lv_obj_t *status = octo_w_row(p, false, 2);
    lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    caption(status, "STATUS", &lv_font_montserrat_14);

    /* The state-line factory gives a dot plus the label. This mockup has no dot
     * anywhere, so it is hidden rather than omitted: the update path recolours
     * it every cycle and must still find a live object. */
    lv_obj_t *line = octo_w_state_line(status, w);
    lv_obj_set_style_pad_all(line, 0, 0);
    if (w->state_dot) {
        lv_obj_add_flag(w->state_dot, LV_OBJ_FLAG_HIDDEN);
    }
    fixed_label(w->lbl_state, &lv_font_montserrat_bold_22, OV_S_STATE,
                LV_TEXT_ALIGN_LEFT);
    if (w->lbl_state) {
        /* The one label here that is NOT clipped: state_text() passes the
         * printer's own words through ("Printing from SD", "Offline after
         * error"), which run past the OV_S_STATE box, and an ellipsis reads as
         * a truncation while a clip reads as a broken word. */
        lv_label_set_long_mode(w->lbl_state, LV_LABEL_LONG_MODE_DOTS);
    }

    hairline(p);

    lv_obj_t *time_block = octo_w_row(p, false, 2);
    lv_obj_set_size(time_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(time_block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    caption(time_block, "TIME ELAPSED", &lv_font_montserrat_14);
    w->lbl_elapsed = octo_w_label(time_block, "--", &lv_font_montserrat_22,
                                  &octo_style_value);
    fixed_label(w->lbl_elapsed, &lv_font_montserrat_22, OV_S_ELAPSED,
                LV_TEXT_ALIGN_LEFT);

    /* LAYER tile, same caption/value dress as TIME ELAPSED. The cell is a ROW
     * holding its own leading hairline plus the caption/value column, so the
     * update path's single HIDDEN flag (no DisplayLayerProgress) removes the
     * rule along with the numbers and the panel closes up cleanly. */
    w->layer_cell = octo_w_row(p, true, OV_TOP_GAP);
    lv_obj_set_size(w->layer_cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    hairline(w->layer_cell);

    lv_obj_t *layer_block = octo_w_row(w->layer_cell, false, 2);
    lv_obj_set_size(layer_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(layer_block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    caption(layer_block, "LAYER", &lv_font_montserrat_14);

    /* The two live numbers the update path writes as "245" and "/ 267". The
     * current layer is left at its natural width: the caption above is
     * left-justified, so the digits must start under its first letter and any
     * slack has to fall at the tile's right end, where nothing lives (this cell
     * is the last in the panel, so a digit-count change moves no neighbour). */
    lv_obj_t *nums = octo_w_row(layer_block, true, 6);
    lv_obj_set_size(nums, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    w->lbl_layer_cur = octo_w_label(nums, "--", &lv_font_montserrat_22,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(nums, "/ --", &lv_font_montserrat_22,
                                      &octo_style_value);
    fixed_label(w->lbl_layer_total, &lv_font_montserrat_22, OV_S_LAYER_TOT,
                LV_TEXT_ALIGN_LEFT);
}

/* ── Top-right group: temperatures ────────────────────────────────────── */

/**
 * COMPACT temperature element, right-justified. Caption and value are frozen at
 * ONE shared width — the wider of the two — so the pair reads as a single
 * right-aligned column and neither a long caption nor "215.0°" can nudge its
 * neighbour.
 */
static void temp_cell(lv_obj_t *parent, const char *name, bool hot,
                      octo_temp_el_t *t)
{
    octo_w_temp(parent, name, OCTO_TEMP_COMPACT, hot, t);
    if (!t->lbl_name || !t->lbl_value) {
        return;
    }
    int32_t width = pair_width(&lv_font_montserrat_14, name,
                               &lv_font_montserrat_bold_22, OV_S_TEMP);

    fixed_label(t->lbl_value, &lv_font_montserrat_bold_22, OV_S_TEMP,
                LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(t->lbl_value, width);

    lv_obj_set_style_text_font(t->lbl_name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(t->lbl_name, 1, 0);
    lv_obj_set_width(t->lbl_name, width);
    lv_label_set_long_mode(t->lbl_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(t->lbl_name, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_top_right(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *p = octo_w_row(parent, true, 0);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(p, LV_ALIGN_TOP_RIGHT, 0, 0);
    tint_panel(p, OV_TINT_RADIUS);
    lv_obj_set_style_pad_hor(p, OV_PAD_H, 0);
    lv_obj_set_style_pad_ver(p, OV_PAD_V, 0);
    lv_obj_set_style_pad_column(p, OV_TEMP_GAP, 0);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    temp_cell(p, "NOZZLE", true, &w->nozzle);
    temp_cell(p, "BED", false, &w->bed);
}

/* ── Fault + connection slots ─────────────────────────────────────────── */

static void build_status_slots(lv_obj_t *parent, octoprint_widgets_t *w)
{
    /* Both are absolutely placed under the top-left group and both spend most
     * of their life hidden (the strip from birth, the chip from the first
     * healthy poll), so showing either reflows nothing. These two carry a
     * DARKER scrim than the panel tint: they carry warning text, and a warning
     * must not depend on what the webcam happens to see. */
    octo_w_status_strip(parent, w);
    if (w->error_strip) {
        /* Flat scrim rather than the factory's card fill: this one floats on
         * the image, where a bordered chip reads as a stray control. */
        lv_obj_set_style_bg_opa(w->error_strip, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(w->error_strip,
                                  lv_color_hex(octo_color(OCTO_COL_BG)), 0);
        lv_obj_set_style_border_width(w->error_strip, 0, 0);
        lv_obj_set_style_radius(w->error_strip, 6, 0);
        lv_obj_set_size(w->error_strip, LV_SIZE_CONTENT, OV_ERR_H);
        lv_obj_set_style_pad_left(w->error_strip, OV_PAD_H, 0);
        lv_obj_set_style_pad_right(w->error_strip, OV_PAD_H, 0);
        lv_obj_set_pos(w->error_strip, 0, OV_ERR_Y);
    }

    octo_w_conn_chip(parent, w);
    if (w->conn_chip) {
        /* Same left inset as the strip, applied as padding rather than as x, so
         * the two grounds share one left edge instead of the chip stepping in
         * by its own padding. */
        lv_obj_set_style_pad_left(w->conn_chip, OV_PAD_H, 0);
        lv_obj_set_style_pad_right(w->conn_chip, OV_PAD_H, 0);
        lv_obj_set_pos(w->conn_chip, 0, OV_CONN_Y);
    }
}

/* ── Bottom group ─────────────────────────────────────────────────────── */

static void build_bottom(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *p = octo_w_row(parent, false, 0);
    lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    /* Radius 0: this panel runs edge to edge and along the bottom of the screen,
     * so rounded corners would only cut into the screen edge. */
    tint_panel(p, 0);
    /* No horizontal padding on the container itself: the track below has to run
     * the full 720, so the 16 px text inset lives on the content row instead. */
    lv_obj_set_style_pad_top(p, 10, 0);
    lv_obj_set_style_pad_bottom(p, 0, 0);
    lv_obj_set_style_pad_row(p, 8, 0);

    lv_obj_t *row = octo_w_row(p, true, 0);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, OV_PAD_H, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* -- left: percent and its unit -------------------------------------- */
    lv_obj_t *left = octo_w_row(row, true, 4);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* RIGHT inside its fixed "100.0" box, the one place this layout departs from
     * "left half, left justified": the digits have to stay welded to their unit,
     * and "61.8" floating away from the "%" reads as a bug. The box itself never
     * moves, so nothing else on the row shifts. */
    w->lbl_pct = octo_w_label(left, "--", &lv_font_montserrat_bold_56,
                              &octo_style_value);
    fixed_label(w->lbl_pct, &lv_font_montserrat_bold_56, OV_S_PCT,
                LV_TEXT_ALIGN_RIGHT);

    w->lbl_pct_unit = octo_w_label(left, "%", &lv_font_montserrat_bold_28,
                                   &octo_style_accent);
    /* The unit rides the digits' baseline, not the row's bottom edge. */
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, OV_LIFT_UNIT, 0);

    /* -- right: "ETA" + finish time at the percent's own 56 px face ------- */
    /* DisplayLayerProgress only, so caption and value hide together. The
     * caption sits to the LEFT of the time on its baseline, exactly as the "%"
     * sits beside the percentage. The time is its NATURAL width, not a
     * worst-case box: the cell is the row's right-hand end and packs from its
     * own right edge, so a change of digits grows the cell leftward and "ETA"
     * stays welded OV_ETA_GAP px off the first digit. */
    w->finish_cell = octo_w_row(row, true, OV_ETA_GAP);
    lv_obj_set_size(w->finish_cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->finish_cell, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_t *eta_cap = caption(w->finish_cell, "ETA", &lv_font_montserrat_16);
    lv_obj_set_style_margin_bottom(eta_cap, OV_LIFT_ETA, 0);
    w->lbl_finish = octo_w_label(w->finish_cell, "--", &lv_font_montserrat_bold_56,
                                 &octo_style_value);
    lv_obj_set_style_text_letter_space(w->lbl_finish, 0, 0);
    lv_obj_set_width(w->lbl_finish, LV_SIZE_CONTENT);

    /* -- track: flush 0..720 along the very bottom edge ------------------ */
    octo_w_progress_bar(p, w);
    if (w->bar_progress) {
        lv_obj_set_width(w->bar_progress, LV_PCT(100));
        lv_obj_set_height(w->bar_progress, OV_BAR_H);
        lv_obj_set_style_radius(w->bar_progress, 0, 0);
        lv_obj_set_style_radius(w->bar_progress, 0, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(w->bar_progress, 0, 0);
        /* Track: the text colour held right down, so it reads as a groove in
         * the image rather than as a second colour competing with the fill. */
        lv_obj_set_style_bg_color(w->bar_progress,
                                  lv_color_hex(octo_color(OCTO_COL_TEXT)), 0);
        lv_obj_set_style_bg_opa(w->bar_progress, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(w->bar_progress,
                                  lv_color_hex(octo_color(OCTO_COL_ACCENT)),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(w->bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    }
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void overlay_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Absolute placement: the text floats over the image ground, which no flex
     * or grid flow can express. Ground first, readings after, so the z-order
     * follows creation order without a single lv_obj_move_foreground(). */
    lv_obj_set_layout(page, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(page, 0, 0);

    /* The hero stays a child of the page; EVERYTHING else goes on the overlay
     * layer, so the spine can hide the whole readout with one HIDDEN flag. All
     * of it is absolutely placed, so the layer changes no geometry. */
    build_ground(page, w);
    lv_obj_t *layer = octo_w_overlay_layer(page, w);
    build_top_left(layer, w);
    build_top_right(layer, w);
    build_status_slots(layer, w);
    build_bottom(layer, w);
}

const octoprint_layout_ops_t octoprint_layout_overlay = {
    .name        = "Floating overlay",
    .full_bleed  = true,   /* the image IS the page: 720x720, no outer inset */
    .image_cover = true,   /* ...and it fills it: staged cover-sized, cropped */
    .build       = overlay_build,
};
