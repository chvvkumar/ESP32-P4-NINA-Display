/**
 * @file octoprint_layout_bento_round.c
 * @brief OctoPrint layout 0 "Grid", round family (inscribed batch 2, board 1).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from octo_color() or a
 * shared style, so all nine themes work without a change here. This file never
 * reads octoprint_data_t and never sets a live value.
 *
 * The square Grid is a flex column of four full-width bands. On a circle a band
 * cannot be LV_PCT(100), so the four bands keep their order and their card
 * language and are cut to the CHORD at their own narrow edge: the stack swells
 * at the equator and pinches at the poles. Absolute placement throughout
 * (LV_LAYOUT_NONE), band tops expressed as offsets from screen_center() so the
 * 800 px panel keeps the same distances and spends its extra diameter on chord
 * width.
 *
 *   band A  state line, centred                       centre - 256, h  48
 *   band B  progress arc + face stack | image hero    centre - 196, h 306
 *   band C  two temperature halves, bar + tick        centre + 122, h  92
 *   band D  three time values, no captions            centre + 226, h  66
 *
 * Deliberate omissions vs the square Grid, all from the board: no header strip
 * (the connection state has no chord left, and the fault strip moves under the
 * state line), no "PROGRESS" caption, no "COMPLETE" sub-label, no temperature
 * names, no time-tile captions. The 27 px floor forbids re-setting any of them.
 */

#include "nina_octoprint_internal.h"
#include "ui_round.h"

/* Band tops as offsets from the panel centre (720: 104 / 164 / 482 / 586). */
#define BR_A_DY      (-256)
#define BR_A_H          48
#define BR_B_DY      (-196)
#define BR_B_H         306
#define BR_C_DY        122
#define BR_C_H          92
#define BR_D_DY        226
#define BR_D_H          66

#define BR_MARGIN        4      /* pulled off each chord so the card clears the rim */
#define BR_ARC_SIZE    232
#define BR_ARC_W        22
#define BR_HERO_W      266
#define BR_HERO_H      274
#define BR_LAYER_W     156

/**
 * Width of a band whose top edge is @p dy_top from the centre and which is
 * @p h tall: the chord at whichever edge is FARTHER from the equator, less a
 * uniform margin. On the 800 panel ui_chord_half() returns a larger value and
 * every band grows; nothing else moves.
 */
static int band_w(int dy_top, int h)
{
    int dy_bot = dy_top + h;
    int narrow = (dy_top < 0) ? -dy_top : dy_top;
    int other  = (dy_bot < 0) ? -dy_bot : dy_bot;
    if (other > narrow) {
        narrow = other;
    }
    return 2 * ui_chord_half(narrow) - BR_MARGIN;
}

/** Card sized to its band's chord and centred horizontally on the page. */
static lv_obj_t *band_card(lv_obj_t *page, int dy_top, int h)
{
    lv_obj_t *card = octo_w_card(page);
    int w = band_w(dy_top, h);
    lv_obj_set_size(card, w, h);
    lv_obj_set_layout(card, LV_LAYOUT_NONE);
    lv_obj_set_pos(card, screen_center() - w / 2, screen_center() + dy_top);
    return card;
}

/** 1 px rule in the border colour, absolutely placed on @p parent. */
static void band_rule(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
}

/* -- band A: state ------------------------------------------------------- */

static void build_state_band(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *card = band_card(page, BR_A_DY, BR_A_H);

    lv_obj_t *line = octo_w_state_line(card, w);
    lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(line);
    if (w->state_dot) {
        lv_obj_set_size(w->state_dot, 12, 12);
    }
    if (w->lbl_state) {
        lv_obj_set_style_text_font(w->lbl_state, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_letter_space(w->lbl_state, 2, 0);
    }
}

/**
 * Fault slot, born hidden. It gets its own row on the page, above band A, at
 * y centre - 296, rather than sharing band A's 48 px card with the centred
 * state line: the strip is absolutely placed, so when it shows it must not
 * land on the state text. Unplated, like the square header's, and at 28 px,
 * because octo_w_chip() builds its label at Montserrat 12 and the round floor
 * is 27.
 */
static void build_fault_slot(lv_obj_t *page, octoprint_widgets_t *w)
{
    octo_w_status_strip(page, w);
    if (!w->error_strip) {
        return;
    }
    int wdt = band_w(BR_B_DY, BR_B_H);
    lv_obj_set_style_bg_opa(w->error_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w->error_strip, 0, 0);
    lv_obj_set_style_radius(w->error_strip, 0, 0);
    lv_obj_set_style_pad_all(w->error_strip, 0, 0);
    lv_obj_set_size(w->error_strip, wdt, 34);
    lv_obj_set_pos(w->error_strip, screen_center() - wdt / 2,
                   screen_center() + BR_A_DY - 40);
    lv_obj_set_flex_align(w->error_strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (w->lbl_error) {
        lv_obj_set_style_text_font(w->lbl_error, &lv_font_montserrat_28, 0);
        /* error_text can run to 63 chars; clip it to the strip's own chord
         * instead of letting an unclipped LV_SIZE_CONTENT label run past the
         * rim (review_impl_D12.md M-2). */
        lv_obj_set_width(w->lbl_error, wdt);
        lv_label_set_long_mode(w->lbl_error, LV_LABEL_LONG_DOT);
    }
    /* No dot: the state line stays the page's one accent element. */
    if (w->error_dot) {
        lv_obj_add_flag(w->error_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -- band B: arc face + image ------------------------------------------- */

static void build_arc_face(lv_obj_t *arc, octoprint_widgets_t *w)
{
    lv_obj_t *ctr = octo_w_row(arc, false, 0);
    lv_obj_set_size(ctr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(ctr);
    lv_obj_set_flex_align(ctr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *pct_row = octo_w_row(ctr, true, 2);
    lv_obj_set_size(pct_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(pct_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_pct = octo_w_label(pct_row, "--", &lv_font_montserrat_bold_56,
                              &octo_style_value);
    w->lbl_pct_unit = octo_w_label(pct_row, "%", &lv_font_montserrat_28,
                                   &octo_style_label);

    /* No "COMPLETE" sub-label: the board drops it and the arc says it. */

    w->layer_cell = octo_w_row(ctr, false, 0);
    lv_obj_set_size(w->layer_cell, BR_LAYER_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(w->layer_cell, 12, 0);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *rule = lv_obj_create(w->layer_cell);
    lv_obj_remove_style_all(rule);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);

    lv_obj_t *lay = octo_w_row(w->layer_cell, true, 8);
    lv_obj_set_size(lay, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(lay, 10, 0);
    lv_obj_set_flex_align(lay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    w->lbl_layer_cur = octo_w_label(lay, "--", &lv_font_montserrat_28,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(lay, "/ --", &lv_font_montserrat_28,
                                      &octo_style_label);
}

static void build_hero_band(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *card = band_card(page, BR_B_DY, BR_B_H);
    int cw = (int)lv_obj_get_style_width(card, LV_PART_MAIN);

    /* Arc cell left, picture right, both centred vertically in the band. */
    lv_obj_t *arc = octo_w_progress_arc(card, BR_ARC_SIZE, BR_ARC_W, w);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(arc, cw / 4 - BR_ARC_SIZE / 2, (BR_B_H - BR_ARC_SIZE) / 2);
    build_arc_face(arc, w);

    lv_obj_t *img = octo_w_image_hero(card, w);
    lv_obj_set_size(img, BR_HERO_W, BR_HERO_H);
    lv_obj_set_pos(img, cw * 3 / 4 - BR_HERO_W / 2, (BR_B_H - BR_HERO_H) / 2);
    lv_obj_set_style_radius(img, 12, 0);
    if (w->img_placeholder) {
        /* octo_w_image_hero() builds it at Montserrat 16, under the round floor. */
        lv_obj_set_style_text_font(w->img_placeholder, &lv_font_montserrat_28, 0);
    }
}

/* -- band C: temperatures ------------------------------------------------ */

static void build_temp_band(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *card = band_card(page, BR_C_DY, BR_C_H);
    int cw = (int)lv_obj_get_style_width(card, LV_PART_MAIN);
    int half = (cw - 64) / 2;

    band_rule(card, cw / 2, 18, 1, BR_C_H - 36);

    for (int i = 0; i < 2; i++) {
        bool hot = (i == 0);
        octo_temp_el_t *t = hot ? &w->nozzle : &w->bed;

        lv_obj_t *el = octo_w_temp(card, hot ? "NOZZLE" : "BED",
                                   OCTO_TEMP_BAR_GRADIENT, hot, t);
        lv_obj_set_size(el, half, LV_SIZE_CONTENT);
        lv_obj_set_pos(el, hot ? 20 : (cw / 2 + 24), 18);
        lv_obj_set_style_pad_row(el, 10, 0);

        /* No caption: colour and side name the tool (board 1, 27 px floor). */
        if (t->lbl_name) {
            lv_obj_add_flag(t->lbl_name, LV_OBJ_FLAG_HIDDEN);
        }
        if (t->lbl_value) {
            lv_obj_set_style_text_font(t->lbl_value, &lv_font_montserrat_28, 0);
        }
        if (t->bar) {
            lv_obj_set_height(t->bar, 14);
            lv_obj_set_style_radius(t->bar, 7, 0);
            lv_obj_set_style_radius(t->bar, 7, LV_PART_INDICATOR);
        }
        /* The board flattens the heat gradient; apply_styles() re-applies it on
         * every build and theme pass, so the gradient is KEPT and only the
         * target tick is widened from 2 to 3 px, which is the part that carries
         * the overshoot read at this size. */
        if (t->tick) {
            lv_obj_set_size(t->tick, 3, LV_PCT(100));
        }
    }
}

/* -- band D: times ------------------------------------------------------- */

static void build_time_band(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *card = band_card(page, BR_D_DY, BR_D_H);
    int cw = (int)lv_obj_get_style_width(card, LV_PART_MAIN);
    int cell = (cw - 16) / 3;      /* 8 px of side pad, not 20: see the note */

    band_rule(card, 8 + cell, 14, 1, BR_D_H - 28);
    band_rule(card, 8 + 2 * cell, 14, 1, BR_D_H - 28);

    lv_obj_t *out[3] = { NULL, NULL, NULL };
    lv_obj_t *tiles[3] = { NULL, NULL, NULL };
    for (int i = 0; i < 3; i++) {
        /* octo_w_time_tile() maps a NULL caption to "" through octo_w_label(),
         * so the tile still gets an empty Montserrat 14 label about 16 px tall.
         * Hide child 0 so the tile is a value alone, as the board draws it. */
        tiles[i] = octo_w_time_tile(card, NULL, &lv_font_montserrat_28, &out[i]);
        lv_obj_set_size(tiles[i], cell, LV_SIZE_CONTENT);
        lv_obj_set_pos(tiles[i], 8 + i * cell, 18);
        lv_obj_set_flex_align(tiles[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t *cap = lv_obj_get_child(tiles[i], 0);
        if (cap && cap != out[i]) {
            lv_obj_add_flag(cap, LV_OBJ_FLAG_HIDDEN);
        }
        /* fmt_duration() emits "%dh %02dm", so "12h 05m" is about 120 px at
         * Montserrat 28 against a 112 px cell at 720. Pin the width and clip
         * rather than let LV_SIZE_CONTENT run into the divider and the next
         * cell. At 800 the chord is 434 px and the value fits whole. */
        if (out[i]) {
            lv_obj_set_width(out[i], cell);
            lv_label_set_long_mode(out[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(out[i], LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    w->lbl_elapsed   = out[0];
    w->lbl_remaining = out[1];
    w->lbl_finish    = out[2];
    /* The third tile hides wholesale when DisplayLayerProgress is absent, so
     * finish_cell is the TILE, not the value label inside it. */
    w->finish_cell   = tiles[2];
}

/* -- entry point --------------------------------------------------------- */

static void bento_round_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_layout(page, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(page, 0, 0);

    build_state_band(page, w);
    build_fault_slot(page, w);
    build_hero_band(page, w);
    build_temp_band(page, w);
    build_time_band(page, w);
}

const octoprint_layout_ops_t octoprint_layout_bento_round = {
    .name        = "Grid",
    .full_bleed  = true,    /* chord widths are panel coordinates, not 688 ones */
    .image_cover = false,   /* the hero is a framed 266 x 274 cell: CONTAIN */
    .build       = bento_round_build,
};
