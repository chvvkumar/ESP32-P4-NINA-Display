/**
 * @file nina_adsb_round.c
 * @brief ADS-B page, round family: widget construction and placement only.
 *
 * Sky Dome  inscribed batch 2 board 5
 * Radar Scope  radial batch 2 board 9 (task D4)
 * Board  inscribed batch 2 board 7 (task D5)
 *
 * Three views, one widget set, exactly as on square: nina_adsb.c shows and
 * hides per mode and owns every string, every colour and all the maths. This
 * file sets no colour and no text; apply_colors() paints everything before the
 * first recompute().
 *
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_adsb_internal.h"
#include "ui_round.h"
#include "ui_arclabel.h"
#include "display_defs.h"

LV_FONT_DECLARE(lv_font_montserrat_64);

/* Sky Dome, board 5. The disc is the panel edge, so the cardinals pull in far
 * enough to clear the two 72 px chord caps: N and S sit 106 px inside the rim,
 * E and W 56 px, which reads deliberate and is the compromise the board
 * records. On the Scope (board 9) all four sit at the shipped 24 px inset. */
#define ADSBR_SKY_CARD_V   106
#define ADSBR_SKY_CARD_H    56
#define ADSBR_SCOPE_CARD    24
#define ADSBR_RING_INSET    34   /* the outermost number clears the bezel */
#define ADSBR_CAP_H         72
#define ADSBR_TAG_W        184
#define ADSBR_TAG_H         76   /* two 28 px lines: the 27 px floor grew it */

/* Radar Scope, board 9. Chord blocks, as offsets from the panel centre
 * (720: 164 / 552 / 596). The rim carries within/tracked as a 7 px arc
 * over the 3 px ring, and the range label and CONTACTS caption are arclabels on
 * the two quiet diagonals. */
#define ADSBR_SC_RING_W       7
#define ADSBR_SC_VAL_DY   (-196)
#define ADSBR_SC_CALL_DY    192
#define ADSBR_SC_FIG_DY     236
#define ADSBR_SC_BLK_W      200
#define ADSBR_SC_FIG_W      330
#define ADSBR_SC_RATE_W     170
#define ADSBR_SC_LBL_R      235   /* contact labels never cross this radius */
#define ADSBR_SC_ARC_K       12   /* arclabel baseline: Rs - 12, outside the blocks */

/* -- local primitives ---------------------------------------------------- */

static lv_obj_t *r_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, "");
    return l;
}

/** Translucent chord cap over the disc. The panel clips it to a segment. */
static lv_obj_t *r_cap(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, screen_size(), h);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void hide(lv_obj_t *o)
{
    if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* -- Sky Dome ------------------------------------------------------------ */

static void build_sky(lv_obj_t *root, lv_obj_t *content, const adsb_slots_t *s)
{
    /* Cardinals and ring numbers. Both families build these; nina_adsb.c
     * positions them every rotation from adsb_geom_t. */
    for (int i = 0; i < 4; i++) {
        s->lbl_card[i] = r_label(content, &lv_font_montserrat_28);
    }
    for (int i = 0; i < 3; i++) {
        s->lbl_ring[i] = r_label(content, &lv_font_montserrat_28);
    }

    /* Three boxed tags: one object each, so the declutter pass moves one thing
     * and the text never lands straight on a glyph. 28 over 28 clears the
     * floor, which is what took the block from 60 px tall to 76. */
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        lv_obj_t *box = lv_obj_create(content);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, ADSBR_TAG_W, ADSBR_TAG_H);
        lv_obj_set_style_bg_opa(box, LV_OPA_80, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_opa(box, LV_OPA_60, 0);
        lv_obj_set_style_radius(box, 3, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s->tag_box[i] = box;

        s->tag_l1[i] = r_label(box, &lv_font_montserrat_28);
        lv_obj_set_pos(s->tag_l1[i], 10, 2);
        s->tag_l2[i] = r_label(box, &lv_font_montserrat_28);
        lv_obj_set_pos(s->tag_l2[i], 10, 40);
        hide(box);
    }

    /* Two chord caps carrying the mount pointing and the status line, both
     * centred: the board drops the "ADS-B" title (the page says which page it
     * is) and lbl_title stays NULL. */
    /* Both labels sit toward the disc, not the cap centre: the round glass
     * chord is narrower near the panel edge, so pushing the ink one row in
     * from the outer edge of each cap buys back the width the chord took
     * away (review_impl_D3 I-1). */
    *s->hdr = r_cap(root, 0, ADSBR_CAP_H);
    *s->lbl_mount = r_label(*s->hdr, &lv_font_montserrat_28);
    lv_obj_set_width(*s->lbl_mount, screen_size());
    lv_obj_set_style_text_align(*s->lbl_mount, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(*s->lbl_mount, LV_ALIGN_BOTTOM_MID, 0, -4);

    *s->strip = r_cap(root, screen_size() - ADSBR_CAP_H, ADSBR_CAP_H);
    *s->lbl_strip = r_label(*s->strip, &lv_font_montserrat_28);
    lv_obj_set_width(*s->lbl_strip, screen_size());
    lv_obj_set_style_text_align(*s->lbl_strip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(*s->lbl_strip, LV_ALIGN_TOP_MID, 0, 4);
}

/* -- Radar Scope --------------------------------------------------------- */

/** Fixed-width right-aligned label at (x, y): Montserrat digits are
 *  proportional, so a free-width number walks its neighbours on every poll. */
static lv_obj_t *r_num(lv_obj_t *parent, const lv_font_t *font, int x, int y, int w)
{
    lv_obj_t *l = r_label(parent, font);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void build_scope(lv_obj_t *root, lv_obj_t *content, const adsb_slots_t *s,
                        adsb_geom_t *g)
{
    int cx = screen_center();
    int rs = ui_rim_radius();

    /* The CONTACTS corner block, as a ring: a 7 px arc over the rim from twelve
     * o'clock, swept by within / tracked. The page drives it with
     * lv_arc_set_value over the same 0..1000 range the progress widgets use. */
    lv_obj_t *ring = lv_arc_create(content);
    lv_obj_set_size(ring, 2 * rs + ADSBR_SC_RING_W + 1, 2 * rs + ADSBR_SC_RING_W + 1);
    lv_obj_center(ring);
    lv_arc_set_rotation(ring, 270);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_range(ring, 0, 1000);
    lv_arc_set_value(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, ADSBR_SC_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_INDICATOR);
    *s->sc_contacts_ring = ring;

    /* G1: the range label and the CONTACTS caption change at most once a poll
     * and once a config change, so both go on the rim. The range label sits on
     * the NNW diagonal, which is the one radius no tag box wants; the caption
     * sits on the NNE diagonal over its own chord block.
     *
     * Rs - 12 puts the glyph body between about r 304 and r 330, INSIDE the
     * 7 px contacts arc (338.5..345.5) and OUTSIDE the count block below it.
     * At Rs - 40 the caption's body reached down to r 276 and crossed the
     * right-aligned "12 / 34". */
    *s->sc_rim_label = ui_arclabel_create(content, &lv_font_montserrat_28,
                                          rs - ADSBR_SC_ARC_K, 225 - 30, 60,
                                          true, LV_ARCLABEL_TEXT_ALIGN_CENTER);
    *s->sc_contacts_arclabel = ui_arclabel_create(content, &lv_font_montserrat_28,
                                                  rs - ADSBR_SC_ARC_K, 315 - 30, 60,
                                                  true, LV_ARCLABEL_TEXT_ALIGN_CENTER);

    /* Upper-right chord: the count, right edge at cx + 200, one line lower than
     * the first draft so it clears the caption's arc. */
    *s->sc_within = r_num(content, &lv_font_montserrat_36,
                          cx, cx + ADSBR_SC_VAL_DY, ADSBR_SC_BLK_W);

    /* Lower-left chord: callsign over one merged figures line. */
    *s->sc_call = r_label(content, &lv_font_montserrat_40);
    lv_obj_set_pos(*s->sc_call, cx - 226, cx + ADSBR_SC_CALL_DY);
    lv_label_set_long_mode(*s->sc_call, LV_LABEL_LONG_DOT);
    lv_obj_set_width(*s->sc_call, ADSBR_SC_BLK_W);
    lv_obj_set_height(*s->sc_call, lv_font_get_line_height(&lv_font_montserrat_40));

    *s->sc_alt = r_label(content, &lv_font_montserrat_28);
    lv_obj_set_pos(*s->sc_alt, cx - 192, cx + ADSBR_SC_FIG_DY);
    lv_obj_set_width(*s->sc_alt, ADSBR_SC_FIG_W);
    /* r_label() leaves the default LV_LABEL_LONG_WRAP: the merged figures line
     * is about 335 px for a five-digit altitude and would wrap onto a second
     * line over the S cardinal. Clip and pin the height to one line. */
    lv_label_set_long_mode(*s->sc_alt, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(*s->sc_alt, lv_font_get_line_height(&lv_font_montserrat_28));

    /* Lower-right chord: the message rate. sc_dist stays NULL, which is what
     * makes the page merge the distance into the figures line. */
    *s->sc_rate = r_num(content, &lv_font_montserrat_28,
                        cx + 68, cx + ADSBR_SC_CALL_DY, ADSBR_SC_RATE_W);
    *s->sc_cue  = r_num(root, &lv_font_montserrat_28,
                        cx + 68, cx + ADSBR_SC_CALL_DY - 40, ADSBR_SC_RATE_W);
    lv_obj_clear_flag(*s->sc_cue, LV_OBJ_FLAG_CLICKABLE);

    /* The three chord blocks are screen area the contact labels used to be free
     * to use, so the declutter pass must treat them like placed tags. Each one
     * is the label's own box, grown by a few pixels. */
    g->no_go[0] = (lv_area_t){ cx, cx + ADSBR_SC_VAL_DY - 4,
                               cx + ADSBR_SC_BLK_W, cx + ADSBR_SC_VAL_DY + 48 };
    g->no_go[1] = (lv_area_t){ cx - 226, cx + ADSBR_SC_CALL_DY,
                               cx - 192 + ADSBR_SC_FIG_W, cx + ADSBR_SC_FIG_DY + 36 };
    g->no_go[2] = (lv_area_t){ cx + 68,  cx + ADSBR_SC_CALL_DY - 40,
                               cx + 68 + ADSBR_SC_RATE_W, cx + ADSBR_SC_CALL_DY + 36 };
    g->no_go_n  = 3;
    g->scope_lbl_r = ADSBR_SC_LBL_R;
}

/* -- Board --------------------------------------------------------------- */

/* Board, board 7. Rows are cut to the chord at their own narrow edge, so the
 * stack tapers into a lens; the rail keeps ONE extent on every row so the five
 * dots share one distance scale and can be compared down the column. */
#define ADSBR_BD_LEAD_DY   (-174)
#define ADSBR_BD_SUB_DY     (-82)
#define ADSBR_BD_ROW_Y0     (-38)
#define ADSBR_BD_ROW_DY       62
#define ADSBR_BD_ROW_H        52
#define ADSBR_BD_ROW_PAD      20   /* pulled off each side of the row's chord */
#define ADSBR_BD_RAIL_X0      (-8) /* from the centre */
#define ADSBR_BD_RAIL_X1     168
#define ADSBR_BD_DOT_D        14
/* The legend clears BOTH the last row and the rim. The last row ends at
 * cx + 262 (Y0 -38 + 4 * 62 + H 52), so the planned 246 put the label's top
 * 16 px inside it; 266 sits 4 px below it. The right edge comes back from
 * cx + 158 to cx + 150 for the rim: at 720 the box's bottom-right corner is
 * then at r 337 against Rs 342, where 158 would have been at 341. */
#define ADSBR_BD_LEG_DY      266
#define ADSBR_BD_LEG_X       150   /* right edge, from the centre */
#define ADSBR_BD_LEG_W       150

/** Half chord of the rim circle at the row's narrow edge, which is whichever
 *  edge is farther from the equator. */
static int row_half(int dy_top, int h)
{
    int a = (dy_top < 0) ? -dy_top : dy_top;
    int b = dy_top + h;
    if (b < 0) {
        b = -b;
    }
    return ui_chord_half((b > a) ? b : a);
}

static void build_board(lv_obj_t *content, const adsb_slots_t *s, adsb_geom_t *g)
{
    int cx = screen_center();

    lv_obj_t *board = lv_obj_create(content);
    lv_obj_remove_style_all(board);
    lv_obj_set_size(board, screen_size(), screen_size());
    lv_obj_set_pos(board, 0, 0);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    *s->board = board;

    /* Lead block on the widest chord it spans. The amber eyebrow and the whole
     * nine-field detail card are not built: every string in them is below the
     * 27 px floor. The lead is marked by row 0's own COL_LEAD_BG / COL_LEAD_BRD
     * instead, which the page paints in apply_colors(). */
    int lead_x = cx - ui_chord_half(ADSBR_BD_LEAD_DY) + 22;
    *s->lbl_glance = r_label(board, &lv_font_montserrat_64);
    lv_obj_set_pos(*s->lbl_glance, lead_x, cx + ADSBR_BD_LEAD_DY);

    *s->lbl_gsub = r_label(board, &lv_font_montserrat_28);
    lv_obj_set_pos(*s->lbl_gsub, lead_x + 4, cx + ADSBR_BD_SUB_DY);
    lv_label_set_long_mode(*s->lbl_gsub, LV_LABEL_LONG_DOT);
    /* Width to the rim at the line's BOTTOM edge, not at its top: a 620 px run
     * from x 92 reaches 712, and the rim at that line is at 692. */
    lv_obj_set_width(*s->lbl_gsub,
                     cx + ui_chord_half(ADSBR_BD_SUB_DY + 28) - 22 - (lead_x + 4));
    lv_obj_set_height(*s->lbl_gsub, lv_font_get_line_height(&lv_font_montserrat_28));

    for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
        int dy = ADSBR_BD_ROW_Y0 + i * ADSBR_BD_ROW_DY;
        int w  = 2 * row_half(dy, ADSBR_BD_ROW_H) - 2 * ADSBR_BD_ROW_PAD;
        int x  = cx - w / 2;

        lv_obj_t *p = lv_obj_create(board);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, w, ADSBR_BD_ROW_H);
        lv_obj_set_pos(p, x, cx + dy);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(p, 1, 0);
        lv_obj_set_style_radius(p, 12, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s->row_panel[i] = p;
        /* Born hidden, as the square builder does: recompute() shows the rows
         * it has contacts for (review D5 M-6). */
        hide(p);

        s->row_call[i] = r_label(p, &lv_font_montserrat_28);
        lv_obj_set_pos(s->row_call[i], 18, 8);
        lv_label_set_long_mode(s->row_call[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s->row_call[i], (cx + ADSBR_BD_RAIL_X0) - x - 30);
        lv_obj_set_height(s->row_call[i],
                          lv_font_get_line_height(&lv_font_montserrat_28));

        /* The four dropped columns as one rail. Rail and dot are children of
         * the row panel, so a row that hides takes them with it. */
        lv_obj_t *rail = lv_obj_create(p);
        lv_obj_remove_style_all(rail);
        lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(rail, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(rail, ADSBR_BD_RAIL_X1 - ADSBR_BD_RAIL_X0, 2);
        lv_obj_set_pos(rail, (cx + ADSBR_BD_RAIL_X0) - x, ADSBR_BD_ROW_H / 2 - 1);
        lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
        s->row_rail[i] = rail;

        lv_obj_t *dot = lv_obj_create(p);
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, ADSBR_BD_DOT_D, ADSBR_BD_DOT_D);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        s->row_dot[i] = dot;
    }

    /* One legend number carries the rail scale for all five rows, at the rail's
     * far end and below the last row. */
    *s->lbl_legend = r_num(board, &lv_font_montserrat_28,
                           cx + ADSBR_BD_LEG_X - ADSBR_BD_LEG_W,
                           cx + ADSBR_BD_LEG_DY, ADSBR_BD_LEG_W);

    g->board_marks = true;
}

/* -- entry point --------------------------------------------------------- */

void adsb_round_build(lv_obj_t *root, lv_obj_t *content,
                      const adsb_slots_t *s, adsb_geom_t *g)
{
    /* Defaults first: build_scope() and build_board() write into g. */
    *g = (adsb_geom_t){
        .card_off_v = { ADSBR_SKY_CARD_V, ADSBR_SCOPE_CARD },
        .card_off_h = { ADSBR_SKY_CARD_H, ADSBR_SCOPE_CARD },
        .rim_w      = { 2, 3 },
        .ring_inset = ADSBR_RING_INSET,
        .ring_lbl_w = 100,   /* the 28 px ring-number face needs more no-go
                              * width than the square 22 px face's 84 */
        .tag_h      = ADSBR_TAG_H,
        .tag_font1  = &lv_font_montserrat_28,
        .tag_font2  = &lv_font_montserrat_28,
        .tag_l1_y   = 2,
        .tag_l2_y   = 40,
        .scrim_top  = { ADSBR_CAP_H, 0 },
        .scrim_bot  = { ADSBR_CAP_H, 0 },
        .short_caps = true,  /* the square sentence overflows the round chord */
    };

    /* CREATION ORDER IS LOAD BEARING. LVGL draws children in creation order, so
     * the Board container and its five opaque row panels must be created BEFORE
     * the transparent draw host, or they paint over the heading arrows the draw
     * callback puts on the rails. Everything the Sky and the Scope add after the
     * host must stay after it, so their tag boxes and labels sit above the
     * contact glyphs exactly as on square.
     *
     *   build_board()
     *   draw host
     *   build_sky()
     *   build_scope()
     */

    build_board(content, s, g);

    lv_obj_t *disc = lv_obj_create(content);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, screen_size(), screen_size());
    lv_obj_set_pos(disc, 0, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    *s->disc = disc;

    build_sky(root, content, s);

    build_scope(root, content, s, g);
}
