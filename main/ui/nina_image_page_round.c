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
#include "app_config.h"
#include <stdatomic.h>
#include "screen_geom.h"
#include "lvgl.h"

extern const lv_font_t lv_font_overpass_27;

/* Rim geometry, absolute pixels at both round widths (the extra diameter at 800
 * goes into chord width, never into type or stroke). */
#define IMG_R_CAPTION_INSET   30   /* caption arclabel radius = Rs - 30 */
#define IMG_R_ARC_INSET       12   /* moon rim arc centre line = Rs - 12 */
#define IMG_ARC_W             16
#define IMG_MOON_TICK_H       16   /* fits between the arc object's own edges */
#define IMG_MOON_TEXT_INSET    6   /* rim text outer edge, inside the age arc */
#define IMG_MOON_TEXT_H       39   /* overpass_27 line height */
#define IMG_MOON_TEXT_GAP     10   /* disc edge to the text's inner edge, text shown */
#define IMG_MOON_TEXT_SPAN   150   /* degrees per rim row; 120 dots the three-item top row */
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

void image_page_build_overlay_round(image_page_t *p, lv_obj_t *page_container)
{
    const int rs = ui_rim_radius();

    p->overlay_bar = round_layer(page_container, screen_size(), screen_size());

    if (p->src == IMG_SRC_MOON) {
        /* The disc is the whole canvas; image_page_moon_ortho() sizes the
         * sphere inside it, so the starfield reaches the glass. */
        p->fit_px = 0;
        p->fit_dy = 0;
        atomic_store(&p->moon_overlay_on, false);

        /* The caption call sites write lbl_region and lbl_timestamp; on round
         * both stay hidden (the name rides the top rim row, the illumination
         * is the arc). */
        p->lbl_region = lv_label_create(p->overlay_bar);
        lv_obj_set_style_text_font(p->lbl_region, &lv_font_overpass_27, 0);
        image_page_caption_style(p->lbl_region);
        lv_label_set_text(p->lbl_region, "");
        lv_obj_add_flag(p->lbl_region, LV_OBJ_FLAG_HIDDEN);
        p->lbl_timestamp = lv_label_create(p->overlay_bar);
        lv_obj_set_style_text_font(p->lbl_timestamp, &lv_font_overpass_27, 0);
        image_page_caption_style(p->lbl_timestamp);
        lv_label_set_text(p->lbl_timestamp, "");
        lv_obj_add_flag(p->lbl_timestamp, LV_OBJ_FLAG_HIDDEN);

        /* Two rim rows just inside the age arc, children of overlay_bar so the
         * C2 tap hides them with it. Text comes from update_moon_corner_labels. */
        int r_txt = rs - IMG_R_ARC_INSET - IMG_ARC_W / 2 - IMG_MOON_TEXT_INSET;
        /* 150 degrees, not the wrapper's 120: the top row carries the phase
         * name, the age and the next phase ("Waning Gibbous / Age 16.1d / New
         * in 13d", 39 glyphs), which the shorter span dotted at 800. Same
         * angle conventions as ui_arclabel_top()/_bottom(). */
        p->moon_arc_top = ui_arclabel_create(p->overlay_bar, &lv_font_overpass_27, r_txt,
                                             270 - IMG_MOON_TEXT_SPAN / 2, IMG_MOON_TEXT_SPAN,
                                             true, LV_ARCLABEL_TEXT_ALIGN_CENTER);
        if (p->moon_arc_top) image_page_caption_style(p->moon_arc_top);
        p->moon_arc_bot = ui_arclabel_create(p->overlay_bar, &lv_font_overpass_27, r_txt,
                                             90 - IMG_MOON_TEXT_SPAN / 2, IMG_MOON_TEXT_SPAN,
                                             false, LV_ARCLABEL_TEXT_ALIGN_CENTER);
        if (p->moon_arc_bot) image_page_caption_style(p->moon_arc_bot);

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

/* Disc diameter from moon_round_size_pct (percent of the rim diameter) while
 * the text is hidden; with the text shown it is also capped so the disc clears
 * the rim rows. Returned as the renderer's half-extent over a full-panel canvas,
 * so the starfield fills the glass whatever the disc size. */
float image_page_moon_ortho(const image_page_t *p)
{
    const int rs = ui_rim_radius();
    int pct = app_config_get()->moon_round_size_pct;
    if (pct < 50 || pct > 150) pct = 100;
    int disc = 2 * rs * pct / 100;
    if (atomic_load(&p->moon_overlay_on)) {
        int r_txt = rs - IMG_R_ARC_INSET - IMG_ARC_W / 2 - IMG_MOON_TEXT_INSET;
        int cap = 2 * (r_txt - IMG_MOON_TEXT_H - IMG_MOON_TEXT_GAP);
        if (disc > cap) disc = cap;
    }
    if (disc < 100) disc = 100;
    return (float)screen_size() / (float)disc;
}
