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
    *s->hdr = r_cap(root, 0, ADSBR_CAP_H);
    *s->lbl_mount = r_label(*s->hdr, &lv_font_montserrat_28);
    lv_obj_set_width(*s->lbl_mount, screen_size());
    lv_obj_set_style_text_align(*s->lbl_mount, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(*s->lbl_mount, LV_ALIGN_CENTER, 0, 0);

    *s->strip = r_cap(root, screen_size() - ADSBR_CAP_H, ADSBR_CAP_H);
    *s->lbl_strip = r_label(*s->strip, &lv_font_montserrat_28);
    lv_obj_set_width(*s->lbl_strip, screen_size());
    lv_obj_set_style_text_align(*s->lbl_strip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(*s->lbl_strip, LV_ALIGN_CENTER, 0, 0);
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
        .tag_h      = ADSBR_TAG_H,
        .tag_font1  = &lv_font_montserrat_28,
        .tag_font2  = &lv_font_montserrat_28,
        .tag_l1_y   = 2,
        .tag_l2_y   = 40,
        .scrim_top  = ADSBR_CAP_H,
        .scrim_bot  = ADSBR_CAP_H,
    };

    /* CREATION ORDER IS LOAD BEARING. LVGL draws children in creation order, so
     * the Board container and its five opaque row panels must be created BEFORE
     * the transparent draw host, or they paint over the heading arrows the draw
     * callback puts on the rails. Everything the Sky and the Scope add after the
     * host must stay after it, so their tag boxes and labels sit above the
     * contact glyphs exactly as on square.
     *
     *   build_board()   <- task D5 inserts its call HERE, above the host
     *   draw host
     *   build_sky()
     *   build_scope()   <- task D4 inserts its call HERE
     */

    lv_obj_t *disc = lv_obj_create(content);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, screen_size(), screen_size());
    lv_obj_set_pos(disc, 0, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    *s->disc = disc;

    build_sky(root, content, s);
}
