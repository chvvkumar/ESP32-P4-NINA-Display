/**
 * @file octoprint_layout_glass_round.c
 * @brief OctoPrint layout 2 "Immersive image", round family (radial batch 2,
 *        board 5), guideline G1.
 *
 * Geometry only: no colour is set here beyond the two flat scrims, which take
 * OCTO_COL_BG like their square siblings. This file never reads
 * octoprint_data_t.
 *
 * The picture IS the page, cover-cropped edge to edge, and the 8 px linear
 * track that ran above the square metrics row becomes the RIM: a 10 px arc
 * flush with the panel edge, with a state crown of the same stroke at twelve
 * o'clock. That buys back the whole bottom of the circle for the readings the
 * narrowing chord was about to squeeze out.
 *
 *   page   hero host, cover, the only child of the page
 *   layer  flat page dim + flat cap from centre + 126 to the bottom
 *   rim    progress arc (arc_completion, retargeted from the bar) + crown
 *   cap    four caption-over-value cells on the 600 px chord, the layer row,
 *          the percent + unit under it, then the state on a bottom arclabel
 *          between the percent and the ring -- no plate under any of them
 *
 * Dropped from the square layout, per the board: the connection chip (the crown
 * carries link health as colour and the fault strip still speaks), "COMPLETE",
 * and the fifth metric cell (nozzle and bed share one merged "TEMPS" reading,
 * w->lbl_temps, and neither octo_temp_el_t is built).
 */

#include "nina_octoprint_internal.h"
#include "ui_arclabel.h"
#include "ui_dial.h"
#include "ui_round.h"

/* Rim ring and crown. The ring is flush with the panel edge: its centreline is
 * screen_center() - GR_RIM_W/2, so its OUTER edge lands exactly on the glass and
 * no ground shows between the ring and the picture behind it. The crown shares
 * the ring's centreline AND its stroke -- a wider crown read as a second, fatter
 * arc at twelve o'clock. The crown is NOT progress: it is the fixed 40 degree
 * state/health band the update path paints, and the progress track stops short
 * of it on both sides. */
#define GR_RIM_W        10
#define GR_CROWN_W      GR_RIM_W
#define GR_CROWN_DEG    40

/* Rim centreline: outer edge on the panel edge. */
static inline int gr_rim_r(void) { return screen_center() - GR_RIM_W / 2; }

/* State text on the BOTTOM rim, in the band between the percent hero and the
 * ring. ui_arclabel_bottom() takes the OUTER edge of the glyph cell, so this is
 * the ring's inner edge less a hairline; the cell grows inward from there. */
#define GR_STATE_H       30     /* lv_font_montserrat_28.line_height */
#define GR_STATE_GAP     11     /* percent row bottom to the state cell */
static inline int gr_state_r(void) { return screen_center() - GR_RIM_W - 6; }

/* Cap and text bands, as offsets from the panel centre. The cells row is placed
 * where the rim's half chord equals the cells' half width (outer bottom corners
 * on the rim), the LAYER row a fixed gap under it, and the percent hero last,
 * under LAYER, with the state text on the bottom rim under that. Those are
 * 123 / 201 / 250 at 720 and 163 / 241 / 310 at 800. The percent used to sit at
 * the panel centre, over the picture, and the state on the TOP rim; the four
 * rows now read as one bottom stack and the picture keeps its middle. */
#define GR_CELL_H       64      /* caption over value, as octo_w_row builds it */
#define GR_CELL_GAP     14      /* cells bottom to LAYER row top */
#define GR_CELL_CHORD  600
#define GR_CELL_W      150
#define GR_LAYER_H      30      /* lv_font_montserrat_28.line_height */
#define GR_PCT_GAP       6      /* LAYER row bottom to percent row top */
#define GR_PCT_H        66      /* lv_font_montserrat_64.line_height */

/* Height of everything under the cells row: gap, LAYER, gap, percent. */
#define GR_BELOW_CELLS  (GR_CELL_GAP + GR_LAYER_H + GR_PCT_GAP + GR_PCT_H)

static int gr_cell_dy(void)
{
    const int rs = ui_rim_radius(), h = GR_CELL_CHORD / 2;
    int dy = (int)sqrtf((float)(rs * rs - h * h)) - GR_CELL_H;
    /* The chord rule alone puts the cells as low as the rim allows, which is
     * fine while nothing follows them. Four rows do now, and on the 800 panel
     * the chord answer (191) drops the last of them under the state text. Cap
     * the row so the percent's bottom still clears the state cell: 123 at 720
     * (the chord answer, unchanged) and 163 at 800. */
    const int dy_max = gr_state_r() - GR_STATE_H - GR_STATE_GAP
                       - GR_CELL_H - GR_BELOW_CELLS;
    return (dy < dy_max) ? dy : dy_max;
}
#define GR_CAP_DY      gr_cell_dy()
#define GR_CELL_DY     gr_cell_dy()
#define GR_LAYER_DY    (gr_cell_dy() + GR_CELL_H + GR_CELL_GAP)
/* Percent is the LAST row, under LAYER, and is aligned by its CENTRE, so the
 * half line height is added on top of the row's top edge. At 720 the row bottom
 * lands at dy 303 and at 800 at dy 343, both 11 px clear of the state cell above
 * the ring; the widest the number gets ("100" plus the unit, about 180 px)
 * clears the chord at either size. */
#define GR_PCT_DY      (GR_LAYER_DY + GR_LAYER_H + GR_PCT_GAP + GR_PCT_H / 2)

#define GR_DIM_OPA     LV_OPA_40
#define GR_CAP_OPA     LV_OPA_60

/** Flat fill in the page ground colour: no gradient anywhere over the picture
 *  (a tall two-stop ramp quantises into visible bands in RGB565). */
static lv_obj_t *flat_scrim(lv_obj_t *parent, lv_opa_t opa)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s, opa, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(octo_color(OCTO_COL_BG)), 0);
    return s;
}

/** One caption-over-value cell, centred on @p cx. */
static lv_obj_t *cap_cell(lv_obj_t *parent, const char *caption, int cx, int y,
                          lv_obj_t **out_value)
{
    lv_obj_t *cell = octo_w_row(parent, false, 4);
    lv_obj_set_size(cell, GR_CELL_W, LV_SIZE_CONTENT);
    lv_obj_set_pos(cell, cx - GR_CELL_W / 2, y);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = octo_w_label(cell, caption, &lv_font_montserrat_28,
                                 &octo_style_label);
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *val = octo_w_label(cell, "--", &lv_font_montserrat_28,
                                 &octo_style_value);
    lv_obj_set_width(val, LV_PCT(100));
    lv_label_set_long_mode(val, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    if (out_value) {
        *out_value = val;
    }
    return cell;
}

/* -- ground -------------------------------------------------------------- */

static void build_ground(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_t *host = octo_w_image_hero(page, w);
    lv_obj_set_size(host, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(host, 0, 0);
    lv_obj_set_style_radius(host, 0, 0);
    lv_obj_set_style_border_width(host, 0, 0);

    if (w->img_hero) {
        /* CENTER, not CONTAIN: image_cover means the spine already staged the
         * frame at cover size, so the host clips the middle of it and no
         * software transform runs per redraw. */
        lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_CENTER);
    }
    if (w->img_placeholder) {
        /* octo_w_image_hero() builds it at Montserrat 16, under the round floor. */
        lv_obj_set_style_text_font(w->img_placeholder, &lv_font_montserrat_28, 0);
        lv_obj_align(w->img_placeholder, LV_ALIGN_CENTER, 0, -40);
    }
}

/* -- rim ----------------------------------------------------------------- */

static void build_rim(lv_obj_t *layer, octoprint_widgets_t *w)
{
    int r = gr_rim_r();

    /* The progress ring IS arc_completion, so the update path drives it with no
     * new code: only the geometry differs from the square arc. */
    lv_obj_t *arc = octo_w_progress_arc(layer, 2 * r + GR_RIM_W, GR_RIM_W, w);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_rotation(arc, 270 + GR_CROWN_DEG / 2);
    lv_arc_set_bg_angles(arc, 0, 360 - GR_CROWN_DEG);
    lv_obj_center(arc);

    /* Crown: a fixed span at twelve o'clock, painted with the state colour by
     * the update path. ui_dial_arc() is the same fixed-arc-segment helper the
     * NINA dashboard's safety crown and countdown tick use; unlike the
     * hand-rolled version this replaces, it sets arc_rounded false, so the
     * crown's flat ends stop overlapping the ring's rounded ones by a few px
     * at 250 and 290 degrees (review_impl_D12.md M-3). */
    lv_obj_t *crown = ui_dial_arc(layer, r, GR_CROWN_W,
                                  -GR_CROWN_DEG / 2, GR_CROWN_DEG / 2);
    w->rim_crown = crown;

    /* G1: the state is caption-class text and changes far less than once a
     * minute, so it goes on the rim. BOTTOM rim, under the percent hero and
     * inside the ring, so the whole reading stack -- cells, LAYER, percent,
     * state -- reads top to bottom in one place instead of straddling the
     * panel. lbl_state stays NULL on this layout. */
    w->rim_state_arclabel = ui_arclabel_bottom(layer, &lv_font_montserrat_28,
                                               gr_state_r());
}

/* -- readings ------------------------------------------------------------ */

static void build_hero_percent(lv_obj_t *layer, octoprint_widgets_t *w)
{
    lv_obj_t *row = octo_w_row(layer, true, 8);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, GR_PCT_DY);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* The board sets the hero in Hanken 96. The value is rendered "61.8" and
     * lv_font_hanken_black_96 carries 0x30-0x3A only (no full stop), so the
     * largest full-ASCII face on the page is used instead, which is the same
     * substitution octoprint_layout_glass.c already made for Playfair. */
    w->lbl_pct = octo_w_label(row, "--", &lv_font_montserrat_64, &octo_style_value);
    w->lbl_pct_unit = octo_w_label(row, "%", &lv_font_montserrat_40,
                                   &octo_style_accent);
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, 8, 0);
}

static void build_cap(lv_obj_t *layer, octoprint_widgets_t *w)
{
    int cy = screen_center();

    lv_obj_t *dim = flat_scrim(layer, GR_DIM_OPA);
    lv_obj_set_size(dim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(dim, 0, 0);

    lv_obj_t *cap = flat_scrim(layer, GR_CAP_OPA);
    lv_obj_set_size(cap, screen_size(), screen_size() - (cy + GR_CAP_DY));
    lv_obj_set_pos(cap, 0, cy + GR_CAP_DY);

    /* Four cells on the 600 px chord: ELAPSED, REMAIN, FINISH, TEMPS. Nozzle
     * and bed share the last cell (board 5): five cells at a 28 px caption put
     * the outer two past the bezel on every chord inside the cap. */
    int x0 = cy - GR_CELL_CHORD / 2;
    int y  = cy + GR_CELL_DY;

    cap_cell(layer, "ELAPSED", x0 + GR_CELL_W / 2,     y, &w->lbl_elapsed);
    cap_cell(layer, "REMAIN",  x0 + GR_CELL_W * 3 / 2, y, &w->lbl_remaining);
    w->finish_cell = cap_cell(layer, "FINISH", x0 + GR_CELL_W * 5 / 2, y,
                              &w->lbl_finish);

    /* TEMPS: ONE merged reading, exactly as the board draws it ("215 / 60"
     * with the degree sign). Two OCTO_TEMP_COMPACT elements format "%.1f" plus
     * the degree sign each, about 84 + 67 px plus the gap, which is 161 px in a
     * 150 px cell and runs past the rim at this height. w->lbl_temps is the
     * page-side merged label; the two octo_temp_el_t elements stay unbuilt, so
     * the update path skips them. */
    lv_obj_t *temps = octo_w_row(layer, false, 4);
    lv_obj_set_size(temps, GR_CELL_W, LV_SIZE_CONTENT);
    lv_obj_set_pos(temps, x0 + GR_CELL_W * 3, y);
    lv_obj_set_flex_align(temps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *tcap = octo_w_label(temps, "TEMPS", &lv_font_montserrat_28,
                                  &octo_style_label);
    lv_obj_set_width(tcap, LV_PCT(100));
    lv_obj_set_style_text_align(tcap, LV_TEXT_ALIGN_CENTER, 0);

    w->lbl_temps = octo_w_label(temps, "--", &lv_font_montserrat_28,
                                &octo_style_value);
    lv_obj_set_width(w->lbl_temps, LV_PCT(100));
    lv_label_set_long_mode(w->lbl_temps, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(w->lbl_temps, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_layer_row(lv_obj_t *layer, octoprint_widgets_t *w)
{
    w->layer_cell = octo_w_row(layer, true, 10);
    lv_obj_set_size(w->layer_cell, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_pos(w->layer_cell, 0, screen_center() + GR_LAYER_DY);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    octo_w_label(w->layer_cell, "LAYER", &lv_font_montserrat_28, &octo_style_label);
    w->lbl_layer_cur = octo_w_label(w->layer_cell, "--", &lv_font_montserrat_28,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(w->layer_cell, "/ --", &lv_font_montserrat_28,
                                      &octo_style_label);
}

static void build_error_slot(lv_obj_t *layer, octoprint_widgets_t *w)
{
    octo_w_status_strip(layer, w);
    if (!w->error_strip) {
        return;
    }
    lv_obj_set_style_bg_opa(w->error_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w->error_strip, 0, 0);
    lv_obj_set_size(w->error_strip, GR_CELL_CHORD, 36);
    lv_obj_set_pos(w->error_strip, screen_center() - GR_CELL_CHORD / 2,
                   screen_center() - 150);
    lv_obj_set_flex_align(w->error_strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (w->lbl_error) {
        /* octo_w_chip() builds it at Montserrat 12, under the round floor. */
        lv_obj_set_style_text_font(w->lbl_error, &lv_font_montserrat_28, 0);
        /* error_text can run to 63 chars; clip it to the strip's own chord
         * instead of letting an unclipped LV_SIZE_CONTENT label run past the
         * panel (review_impl_D12.md M-2). */
        lv_obj_set_width(w->lbl_error, GR_CELL_CHORD);
        lv_label_set_long_mode(w->lbl_error, LV_LABEL_LONG_DOT);
    }
}

/* -- entry point --------------------------------------------------------- */

static void glass_round_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    lv_obj_set_layout(page, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(page, 0, 0);

    build_ground(page, w);

    lv_obj_t *layer = octo_w_overlay_layer(page, w);

    build_cap(layer, w);            /* scrims first: everything draws over them */
    build_rim(layer, w);
    build_hero_percent(layer, w);
    build_layer_row(layer, w);
    build_error_slot(layer, w);
}

const octoprint_layout_ops_t octoprint_layout_glass_round = {
    .name        = "Immersive image",
    .full_bleed  = true,
    .image_cover = true,
    .build       = glass_round_build,
};
