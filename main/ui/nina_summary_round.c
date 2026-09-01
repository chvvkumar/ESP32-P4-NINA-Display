/**
 * @file nina_summary_round.c
 * @brief Summary page on a round panel: stacked bands (board "Bands A").
 *
 * Three thin rings sit tight to the glass, one per shown rig, outermost first,
 * each carrying that rig's exposure progress in its filter colour with the sub
 * blocks the subbar draws plus a meridian flip tick. Everything else is text,
 * stacked on the vertical centre line as one band per rig: a short colour line,
 * the rig name in the ring colour, the target as the big line, and one reading
 * row holding RMS, sub count, exposure seconds, filter and HFR.
 *
 * A band is a plain transparent container. It is still the tap target that
 * opens that rig's page, it just draws nothing of its own; the picture is the
 * text and the rings.
 *
 * The page owns the data and the colours; this file creates and places widgets.
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_summary_internal.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_dial.h"
#include "ui_helpers.h"
#include "ui_round.h"
#include "ui_text_fit.h"

LV_FONT_DECLARE(lv_font_montserrat_bold_22);
LV_FONT_DECLARE(lv_font_montserrat_bold_28);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* -- design tokens -------------------------------------------------------- */

#define SR_RING_PITCH     16    /* between ring centre lines */
#define SR_RING_OFF        6    /* outermost ring, offset from the rim radius */
#define SR_RING_W         10
#define SR_TICK_W         18    /* meridian flip tick, stroke width */

/* Band stack. The pitch is the same on both panel sizes: the 800 px panel has
 * 40 px more radius, and it spends all of it on chord width, not on taller
 * bands. Three bands span 2 * 158 + 124 = 440 px, which fits inside the
 * innermost ring on both (2 * 306 = 612 at 720, 2 * 346 = 692 at 800).
 * At 720 the top band's target line gets 470 px and its reading row 524; the
 * bottom band 484 and 424; the centre band 576 (800: 566/612, 578/528, 658). */
#define SR_BAND_PITCH    158
#define SR_BAND_H        112    /* the band's nominal height; each line is cut to its own chord */
#define SR_BAND_BOX      124    /* the object: 12 px taller so the 28 px reading
                                 * row is not clipped by the parent's edge */
#define SR_BAND_EDGE      12    /* keep a band's corners off the inner ring */
#define SR_BAND_MIN_W    120

#define SR_LINE_W        150    /* the colour line at the top of a band */
#define SR_LINE_H          2

#define SR_NAME_DY         8
#define SR_TGT_DY         34
#define SR_ROW_DY         78
#define SR_ROW_GAP        10    /* between the RMS figure and the rest of the row */

/* Target ladder: bold first, then the regular faces below it. */
#define SR_LADDER_TARGET_N 3
static const lv_font_t *const SR_LADDER_TARGET[SR_LADDER_TARGET_N] = {
    &lv_font_montserrat_bold_28, &lv_font_montserrat_24, &lv_font_montserrat_20,
};

/* Reading row ladder. HFR sits at the end of the row, so it is what the dots
 * eat when even the smallest face overflows. */
#define SR_LADDER_ROW_N 3
static const lv_font_t *const SR_LADDER_ROW[SR_LADDER_ROW_N] = {
    &lv_font_montserrat_20, &lv_font_montserrat_18, &lv_font_montserrat_16,
};

/* -- geometry ------------------------------------------------------------- */

/* Inner edge of the innermost ring: every band corner stays inside it. */
static int sr_inner_radius(void) {
    return ui_rim_radius() - SR_RING_OFF - 2 * SR_RING_PITCH - SR_RING_W;
}

/* Half chord of the inner circle at vertical offset dy from the panel centre. */
static int sr_inner_half(int dy) {
    int rc = sr_inner_radius();
    if (dy < 0) dy = -dy;
    if (dy >= rc) return 0;
    return (int)sqrtf((float)(rc * rc - dy * dy));
}

/* Ring radius for the k-th shown rig, outermost first. */
static int sr_ring_radius(int rank) {
    return ui_rim_radius() - SR_RING_OFF - rank * SR_RING_PITCH;
}

/* Band centre offset from the panel centre for the k-th shown rig of n.
 * One rig sits on the centre line, two straddle it, three stack on the pitch. */
static int sr_band_dy(int rank, int n) {
    if (n <= 1) return 0;
    if (n == 2) return (rank == 0) ? -SR_BAND_PITCH / 2 : SR_BAND_PITCH / 2;
    return (rank - 1) * SR_BAND_PITCH;
}

static void sr_arc_set_radius(lv_obj_t *arc, int r, int width) {
    if (!arc) return;
    int side = 2 * r + width;
    lv_obj_set_size(arc, side, side);
    lv_obj_center(arc);
}

/* Move a band to @p dy and cut each text line to the chord at its own far
 * edge. Every child that spans a line's width is re-widened with it. */
static void sr_band_place(summary_card_t *sc, int dy) {
    if (!sc->card) return;
    /* Each line is cut to the chord at its own far edge: for a band above the
     * centre the far edge of a line is its top, below the centre its bottom,
     * and the centre band is widest at the band's own edge either way. */
    const int a      = (dy < 0) ? -dy : dy;
    const int half_h = SR_BAND_H / 2;
    const int tgt_h  = lv_font_get_line_height(SR_LADDER_TARGET[0]);
    const int row_h  = lv_font_get_line_height(&lv_font_hanken_bold_28);
    int tgt_far, row_far;
    if (dy < 0) {
        tgt_far = a + half_h - SR_TGT_DY;
        row_far = a + half_h - SR_ROW_DY;
    } else if (dy > 0) {
        tgt_far = a - half_h + SR_TGT_DY + tgt_h;
        row_far = a - half_h + SR_ROW_DY + row_h;
    } else {
        tgt_far = half_h;
        row_far = half_h;
    }
    int tw = 2 * sr_inner_half(tgt_far) - 2 * SR_BAND_EDGE;
    int rw = 2 * sr_inner_half(row_far) - 2 * SR_BAND_EDGE;
    if (tw < SR_BAND_MIN_W) tw = SR_BAND_MIN_W;
    if (rw < SR_BAND_MIN_W) rw = SR_BAND_MIN_W;
    sc->round_target_w = tw;
    sc->round_row_w    = rw;

    /* The band object is only the tap target, so it takes the wider of the two
     * and may poke past the disc: nothing is drawn there. */
    lv_obj_set_size(sc->card, (tw > rw) ? tw : rw, SR_BAND_BOX);
    lv_obj_align(sc->card, LV_ALIGN_CENTER, 0, dy);

    if (sc->lbl_name)   lv_obj_set_width(sc->lbl_name, tw);
    if (sc->lbl_target) lv_obj_set_width(sc->lbl_target, tw);
    if (sc->lbl_row) {
        lv_obj_t *row = lv_obj_get_parent(sc->lbl_row);
        if (row && row != sc->card) lv_obj_set_width(row, rw);
    }
}

void nina_summary_round_place_rings(summary_card_t *cards, const bool *shown) {
    int n = 0;
    for (int slot = 0; slot < MAX_NINA_INSTANCES; slot++) {
        if (shown[slot]) n++;
    }

    int rank = 0;
    for (int slot = 0; slot < MAX_NINA_INSTANCES; slot++) {
        if (!shown[slot]) continue;
        int r = sr_ring_radius(rank);
        nina_subbar_ring_set_radius(&cards[slot].ring, r);
        sr_arc_set_radius(cards[slot].ring_flip_tick, r, SR_TICK_W);
        sr_band_place(&cards[slot], sr_band_dy(rank, n));
        rank++;
    }
}

/* -- fitting -------------------------------------------------------------- */

void nina_summary_round_fit_target(summary_card_t *sc) {
    if (!sc || !sc->lbl_target) return;
    ui_fit_label(sc->lbl_target, SR_LADDER_TARGET, SR_LADDER_TARGET_N,
                 sc->round_target_w);
}

void nina_summary_round_fit_row(summary_card_t *sc, const char *text) {
    if (!sc || !sc->lbl_row || !text) return;

    /* The RMS figure and the rest of the row are one centred pair, so whatever
     * the figure costs comes off the rest. */
    int32_t rms_w = 0;
    if (sc->lbl_rms_val) {
        lv_point_t sz;
        lv_text_get_size(&sz, lv_label_get_text(sc->lbl_rms_val),
                         &lv_font_hanken_bold_28,
                         lv_obj_get_style_text_letter_space(sc->lbl_rms_val, 0),
                         0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        rms_w = sz.x;
    }
    int avail = sc->round_row_w - (int)rms_w - SR_ROW_GAP;
    if (avail < 60) avail = 60;

    /* Measured here rather than through ui_fit_label(): that one measures with
     * LV_TEXT_FLAG_NONE, which counts every "#RRGGBB " recolour tag in this row
     * as glyphs and would shrink the row to its smallest face on every write.
     * The text comes in as an argument for the same reason the measurement
     * cannot read the label: once LVGL has ellipsised a label it has rewritten
     * the label's own text in place, so measuring that back would pick a bigger
     * face, undot, overflow and dot again on the next poll. */
    int32_t ls = lv_obj_get_style_text_letter_space(sc->lbl_row, 0);
    const lv_font_t *pick = SR_LADDER_ROW[0];
    int32_t w = 0;
    for (int i = 0; i < SR_LADDER_ROW_N; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, text, SR_LADDER_ROW[i], ls, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_RECOLOR);
        pick = SR_LADDER_ROW[i];
        w = sz.x;
        if (w <= avail) break;
    }
    if (lv_obj_get_style_text_font(sc->lbl_row, 0) != pick) {
        lv_obj_set_style_text_font(sc->lbl_row, pick, 0);
    }

    if (w > avail) {
        /* Even the smallest face overflows: bound the box so it dots. LVGL only
         * inserts dots when the height is bounded too. */
        if (lv_label_get_long_mode(sc->lbl_row) != LV_LABEL_LONG_DOT) {
            lv_label_set_long_mode(sc->lbl_row, LV_LABEL_LONG_DOT);
        }
        lv_obj_set_width(sc->lbl_row, avail);
        lv_obj_set_height(sc->lbl_row, lv_font_get_line_height(pick));
    } else {
        /* Content sized while it fits, so the pair stays centred in the band. */
        if (lv_label_get_long_mode(sc->lbl_row) != LV_LABEL_LONG_CLIP) {
            lv_label_set_long_mode(sc->lbl_row, LV_LABEL_LONG_CLIP);
        }
        lv_obj_set_width(sc->lbl_row, LV_SIZE_CONTENT);
        lv_obj_set_height(sc->lbl_row, LV_SIZE_CONTENT);
    }
}

/* -- build ---------------------------------------------------------------- */

static lv_obj_t *sr_plain_label(lv_obj_t *parent, const lv_font_t *font,
                                uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, text ? text : "");
    return l;
}

void nina_summary_round_create_card(summary_card_t *sc, lv_obj_t *parent, int slot) {
    if (!sc || !parent || !current_theme) return;
    memset(sc, 0, sizeof(*sc));
    sc->instance_index = slot;

    sc->cached_name_color        = UINT32_MAX;
    sc->cached_filter_text_color = UINT32_MAX;
    sc->cached_filter_bg_color   = UINT32_MAX;
    sc->cached_filter_bg_opa     = UINT8_MAX;
    sc->cached_target_color      = UINT32_MAX;
    sc->cached_bar_ind_color     = UINT32_MAX;
    sc->cached_bar_bg_color      = UINT32_MAX;
    sc->cached_pct_color         = UINT32_MAX;
    sc->cached_seq_name_color    = UINT32_MAX;
    sc->cached_exp_val_color     = UINT32_MAX;
    sc->cached_seq_step_color    = UINT32_MAX;
    sc->cached_rms_color         = UINT32_MAX;
    sc->cached_hfr_color         = UINT32_MAX;
    sc->cached_flip_color        = UINT32_MAX;
    sc->cached_detail_color      = UINT32_MAX;
    sc->cached_safety_color      = UINT32_MAX;
    sc->cached_tick_color        = UINT32_MAX;

    int gb = app_config_get()->color_brightness;
    uint32_t label_col = app_config_apply_brightness(current_theme->label_color, gb);

    /* 1: this rig's ring. Slot 0 outermost until the ranking moves it. The ring
     * closes at twelve o'clock (gap 0): this board has no safety crown. */
    int r = sr_ring_radius(slot);
    nina_subbar_create_ring(&sc->ring, parent, r, SR_RING_W, 0);
    sc->ring_flip_tick = ui_dial_arc(parent, r, SR_TICK_W, 0, 2);
    lv_obj_set_style_arc_color(sc->ring_flip_tick,
        lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
        LV_PART_MAIN);
    lv_obj_add_flag(sc->ring_flip_tick, LV_OBJ_FLAG_HIDDEN);

    /* 2: the band. Invisible, but still the tap target that opens the rig's
     * page; the picture is the text inside it and the ring outside it. */
    sc->card = lv_obj_create(parent);
    lv_obj_remove_style_all(sc->card);
    lv_obj_set_layout(sc->card, LV_LAYOUT_NONE);
    lv_obj_remove_flag(sc->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(sc->card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sc->card, 0, 0);
    lv_obj_set_style_pad_all(sc->card, 0, 0);
    summary_bind_card_tap(sc->card, slot);

    /* 3: the colour line at the top of the band, in the ring colour. */
    sc->tick = lv_obj_create(sc->card);
    lv_obj_remove_style_all(sc->tick);
    lv_obj_remove_flag(sc->tick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sc->tick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sc->tick, SR_LINE_W, SR_LINE_H);
    lv_obj_align(sc->tick, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(sc->tick, SR_LINE_H / 2, 0);
    lv_obj_set_style_bg_opa(sc->tick, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sc->tick, lv_color_hex(label_col), 0);

    /* 4: the rig name, also in the ring colour. */
    sc->lbl_name = sr_plain_label(sc->card, &lv_font_montserrat_bold_22,
                                  label_col, "N.I.N.A.");
    lv_label_set_long_mode(sc->lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(sc->lbl_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(sc->lbl_name,
                      lv_font_get_line_height(&lv_font_montserrat_bold_22));
    lv_obj_align(sc->lbl_name, LV_ALIGN_TOP_MID, 0, SR_NAME_DY);

    /* 5: the target, the big line, fitted to the band on every write. */
    sc->lbl_target = sr_plain_label(sc->card, SR_LADDER_TARGET[0],
        app_config_apply_brightness(current_theme->target_name_color, gb), "----");
    lv_label_set_long_mode(sc->lbl_target, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(sc->lbl_target, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(sc->lbl_target,
                      lv_font_get_line_height(SR_LADDER_TARGET[0]));
    lv_obj_align(sc->lbl_target, LV_ALIGN_TOP_MID, 0, SR_TGT_DY);

    /* 6: the reading row: the RMS figure and everything else, centred as one
     * pair. The row's own text is composed and recoloured by nina_summary.c. */
    lv_obj_t *row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, SR_ROW_DY);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, SR_ROW_GAP, 0);

    sc->lbl_rms_val = sr_plain_label(row, &lv_font_hanken_bold_28,
        app_config_apply_brightness(current_theme->rms_color, gb), "--");

    sc->lbl_row = sr_plain_label(row, SR_LADDER_ROW[0], label_col, "");
    lv_label_set_recolor(sc->lbl_row, true);

    sr_band_place(sc, sr_band_dy(slot, MAX_NINA_INSTANCES));
}
