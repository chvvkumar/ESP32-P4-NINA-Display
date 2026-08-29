/**
 * @file nina_image_page_round.c
 * @brief Round composition of the image page captions (inscribed board 6) and
 *        of the Moon instance (radial board 7), guideline G1 and C2.
 *
 * GOES class (GOES, Solar, Custom, Radar, Clouds): the two corner-flush caption
 * labels move to the bottom rim. The region name is an arclabel trailing into
 * six o'clock from the left; the timestamp is a plain label on a bottom chord
 * (Branch B of the plan's per-frame stamp ruling: the A2 arclabel measurement
 * has not been run, so the caption that changes once per playback step stays a
 * cheap label). No chip, no bar background, no rim frame ring.
 *
 * Moon: the disc gives ground (432 px, centred 12 px above the panel centre) so
 * the freed annulus can hold the four labels on two chords; illumination stops
 * being text and becomes a rim arc from twelve o'clock with a new-moon origin
 * tick. lbl_timestamp is created and never shown on this instance.
 *
 * This file sets NO colour (spec B.2). Text colour comes from the page through
 * image_page_caption_style(); the arc and its tick are painted by
 * moon_arc_apply_theme() in nina_image_page.c, both at create and on every
 * theme change.
 *
 * Display lock held by the caller.
 */

#include "nina_image_page_internal.h"
#include "ui_round.h"
#include "ui_arclabel.h"
#include "screen_geom.h"
#include "lvgl.h"

extern const lv_font_t lv_font_overpass_27;

/* Rim geometry, absolute pixels at both round widths (the extra diameter at 800
 * goes into chord width, never into type or stroke). */
#define IMG_R_CAPTION_INSET   30   /* caption arclabel radius = Rs - 30 */
#define IMG_R_ARC_INSET       12   /* moon rim arc centre line = Rs - 12 */
#define IMG_ARC_W             16
#define IMG_MOON_DISC_PX     432
#define IMG_MOON_DISC_DY     (-12)
#define IMG_MOON_TOP_W       366   /* review_impl_F1.md M-6: 342 left only 10 px of margin */
#define IMG_MOON_TOP_DY     (-264)
#define IMG_MOON_BOT_DY      232   /* review_impl_F1.md I-2: box width is now the chord at this
                                     * row (see image_page_build_overlay_round), not a literal;
                                     * 232 keeps it 5.5 px clear of the disc bottom (was 244/400) */
#define IMG_MOON_NAME_DY     290
#define IMG_MOON_ROW_H        45   /* 27 px line height 39 + 6 pad, as on square */
#define IMG_MOON_TICK_H       16   /* fits between the arc object's own edges */
#define IMG_CAP_ANGLE_START   90   /* six o'clock in lv_arclabel angles */
#define IMG_CAP_ANGLE_SIZE    70   /* run left along the bottom rim */
#define IMG_STAMP_DY         232   /* GOES class: bottom chord carrying HH:MM */
#define IMG_STAMP_CHORD_PAD   32   /* inset from both chord ends */

/* A transparent full-panel container that never eats a tap. overlay_bar must be
 * full panel on round because ui_arclabel_create() centres its arc on the
 * parent, and it must stay the visibility container because
 * image_page_set_overlay_visible() toggles exactly this object. lv_obj_create()
 * makes an object CLICKABLE by default; leaving that set would swallow the C2
 * tap and the Moon drag gesture over the whole panel. */
static lv_obj_t *round_layer(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_center(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE |
                          LV_OBJ_FLAG_SCROLLABLE |
                          LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                          LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    return o;
}

static lv_obj_t *moon_chord_box(lv_obj_t *parent, int w, int dy)
{
    lv_obj_t *box = round_layer(parent, w, IMG_MOON_ROW_H);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, dy);
    return box;
}

static lv_obj_t *moon_label(lv_obj_t *box, lv_align_t align)
{
    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, &lv_font_overpass_27, 0);
    image_page_caption_style(lbl);
    lv_obj_align(lbl, align, 0, 0);
    lv_label_set_text(lbl, "");
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    return lbl;
}

/* Rise/Set only: the bottom chord can be narrower than "Rise 12:34pm +1" (12h
 * clock with a day suffix, the worst case in fmt_moon_event()), so each label
 * is bounded to half the box and dots instead of running into its neighbour
 * (review_impl_F1.md I-2 / ESCALATION 1 ruling). Width, text align and long
 * mode are set before lv_obj_align so alignment sees the final box, matching
 * the lbl_timestamp pattern below. */
static lv_obj_t *moon_label_capped(lv_obj_t *box, lv_align_t align, int w, lv_text_align_t txt_align)
{
    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, &lv_font_overpass_27, 0);
    image_page_caption_style(lbl);
    lv_obj_set_width(lbl, w);
    lv_obj_set_style_text_align(lbl, txt_align, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, align, 0, 0);
    lv_label_set_text(lbl, "");
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    return lbl;
}

void image_page_build_overlay_round(image_page_t *p, lv_obj_t *page_container)
{
    const int rs = ui_rim_radius();

    p->overlay_bar = round_layer(page_container, screen_size(), screen_size());

    if (p->src == IMG_SRC_MOON) {
        /* The disc shrinks so the annulus can hold the labels. */
        p->fit_px = IMG_MOON_DISC_PX;
        p->fit_dy = IMG_MOON_DISC_DY;

        p->lbl_region = lv_label_create(p->overlay_bar);
        lv_obj_set_style_text_font(p->lbl_region, &lv_font_overpass_27, 0);
        image_page_caption_style(p->lbl_region);
        lv_obj_set_style_text_align(p->lbl_region, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(p->lbl_region, LV_ALIGN_CENTER, 0, IMG_MOON_NAME_DY);
        lv_label_set_text(p->lbl_region, "");

        /* Illumination is the rim arc, so the percentage is never drawn. The
         * handle stays live so every caption call site keeps working with no
         * source test. */
        p->lbl_timestamp = lv_label_create(p->overlay_bar);
        lv_obj_set_style_text_font(p->lbl_timestamp, &lv_font_overpass_27, 0);
        image_page_caption_style(p->lbl_timestamp);
        lv_label_set_text(p->lbl_timestamp, "");
        lv_obj_add_flag(p->lbl_timestamp, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *top = moon_chord_box(page_container, IMG_MOON_TOP_W, IMG_MOON_TOP_DY);
        p->lbl_moon_age  = moon_label(top, LV_ALIGN_LEFT_MID);
        p->lbl_moon_next = moon_label(top, LV_ALIGN_RIGHT_MID);

        int bot_w = ui_chord_at_y(screen_center() + IMG_MOON_BOT_DY + IMG_MOON_ROW_H / 2);
        lv_obj_t *bot = moon_chord_box(page_container, bot_w, IMG_MOON_BOT_DY);
        int bot_cap_w = bot_w / 2 - 8;
        p->lbl_moon_rise = moon_label_capped(bot, LV_ALIGN_LEFT_MID,  bot_cap_w, LV_TEXT_ALIGN_LEFT);
        p->lbl_moon_set  = moon_label_capped(bot, LV_ALIGN_RIGHT_MID, bot_cap_w, LV_TEXT_ALIGN_RIGHT);

        /* Lunar age arc: track full circle, indicator from twelve o'clock
         * clockwise to the lit fraction. lv_arc puts its centre line at
         * (size - arc_width) / 2, so size back-solves from Rs - 12. */
        int arc_sz = 2 * (rs - IMG_R_ARC_INSET) + IMG_ARC_W;
        p->moon_illum_arc = lv_arc_create(page_container);
        lv_obj_remove_style(p->moon_illum_arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(p->moon_illum_arc, LV_OBJ_FLAG_CLICKABLE |
                                              LV_OBJ_FLAG_SCROLLABLE |
                                              LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                                              LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_set_size(p->moon_illum_arc, arc_sz, arc_sz);
        lv_obj_center(p->moon_illum_arc);
        lv_obj_set_style_bg_opa(p->moon_illum_arc, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_width(p->moon_illum_arc, IMG_ARC_W, LV_PART_MAIN);
        lv_obj_set_style_arc_width(p->moon_illum_arc, IMG_ARC_W, LV_PART_INDICATOR);
        lv_arc_set_bg_angles(p->moon_illum_arc, 0, 360);
        lv_arc_set_rotation(p->moon_illum_arc, 270);   /* zero at twelve o'clock */
        lv_arc_set_range(p->moon_illum_arc, 0, 100);
        lv_arc_set_value(p->moon_illum_arc, 0);

        /* New-moon origin tick, a child of the arc so the C2 tap hides both
         * with one flag. 16 px, not 28: the arc object's own edge is at radius
         * rs - 12 + IMG_ARC_W / 2, and a longer tick would be clipped by it. */
        p->moon_illum_tick = lv_obj_create(p->moon_illum_arc);
        lv_obj_remove_style_all(p->moon_illum_tick);
        lv_obj_set_size(p->moon_illum_tick, 3, IMG_MOON_TICK_H);
        lv_obj_set_style_bg_opa(p->moon_illum_tick, LV_OPA_COVER, 0);
        lv_obj_remove_flag(p->moon_illum_tick, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(p->moon_illum_tick, LV_ALIGN_CENTER, 0, -(rs - IMG_R_ARC_INSET));
        return;
    }

    /* GOES class: the region name is a rim arclabel trailing into six o'clock
     * from the left. Angles are lv_arclabel angles (0 = three o'clock, clockwise
     * positive, six o'clock 90), drawn counter-clockwise so the glyphs read left
     * to right along the bottom of the circle; TRAILING ends the run exactly on
     * angle_start, so the caption finishes at six o'clock and grows left into
     * the 90..160 span. */
    p->lbl_region = ui_arclabel_create(p->overlay_bar, &lv_font_overpass_27,
                                       rs - IMG_R_CAPTION_INSET,
                                       IMG_CAP_ANGLE_START, IMG_CAP_ANGLE_SIZE, false,
                                       LV_ARCLABEL_TEXT_ALIGN_TRAILING);
    /* NULL when the widget allocation failed; the styler dereferences. */
    if (p->lbl_region) image_page_caption_style(p->lbl_region);
    /* No lv_arclabel_set_text("") here: ui_arclabel_create() already set it. */

    /* Branch B: the per-frame HH:MM stamp of the Radar and Cloud Cover loops
     * changes on every playback step, and the A2 arclabel redraw measurement has
     * not been run, so this caption stays a plain label on a bottom chord for
     * all five GOES-class sources (one code path, no per-source test). */
    p->lbl_timestamp = lv_label_create(p->overlay_bar);
    lv_obj_set_style_text_font(p->lbl_timestamp, &lv_font_overpass_27, 0);
    image_page_caption_style(p->lbl_timestamp);
    lv_obj_set_width(p->lbl_timestamp,
                     ui_chord_at_y(screen_center() + IMG_STAMP_DY + 20) - IMG_STAMP_CHORD_PAD);
    lv_obj_set_style_text_align(p->lbl_timestamp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(p->lbl_timestamp, LV_ALIGN_CENTER, 0, IMG_STAMP_DY);
    lv_label_set_text(p->lbl_timestamp, "");
}
