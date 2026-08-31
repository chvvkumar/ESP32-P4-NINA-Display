/**
 * @file nina_image_page_round.c
 * @brief Round composition of the image page captions (inscribed board 6) and
 *        of the Moon instance (radial board 7), guideline G1 and C2.
 *
 * GOES class (GOES, Solar, Custom, Radar, Clouds): both caption labels are rim
 * arclabels at Rs - 10, split at six o'clock, each over its own translucent
 * band from ui_arclabel_add_band(). The region name trails to the left of six
 * o'clock; the timestamp leads out to the right. The per-frame HH:MM stamp of
 * the Radar and Cloud Cover loops now re-lays out an arclabel on every
 * playback step, by the user's explicit decision (the earlier Branch B chord
 * label fallback is gone). No chip, no bar background, no rim frame ring.
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
#define IMG_R_CAPTION_INSET   10   /* caption arclabel radius = Rs - 10; the band pads outside it */
#define IMG_R_ARC_INSET       12   /* moon rim arc centre line = Rs - 12 */
#define IMG_ARC_W             16
#define IMG_MOON_TICK_H       16   /* fits between the arc object's own edges */
#define IMG_MOON_TEXT_INSET    6   /* rim text outer edge, inside the age arc */
#define IMG_MOON_TEXT_H       39   /* overpass_27 line height */
#define IMG_MOON_TEXT_GAP     10   /* disc edge to the text's inner edge, text shown */
#define IMG_MOON_TEXT_SPAN   150   /* degrees per rim row; 120 dots the three-item top row */
#define IMG_CAP_BAND_PAD       6   /* band padding beyond the glyph cell, GOES-class captions */
#define IMG_CAP_REGION_START  92   /* region arclabel start angle; trailing ends 2 deg left of six o'clock */
#define IMG_CAP_SPAN          88   /* degrees run by each of the two GOES-class rim captions */
#define IMG_CAP_STAMP_START    0   /* timestamp arclabel start angle; leading begins 2 deg right of six o'clock */

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

    /* GOES class: both captions are rim arclabels split at six o'clock. Angles
     * are lv_arclabel angles (0 = three o'clock, clockwise positive, six
     * o'clock 90), drawn counter-clockwise so the glyphs read left to right
     * along the bottom of the circle. The region name is TRAILING, which ends
     * the run exactly on angle_start, so it finishes 2 degrees left of six
     * o'clock and grows left through the 92..180 span. */
    p->lbl_region = ui_arclabel_create(p->overlay_bar, &lv_font_overpass_27,
                                       rs - IMG_R_CAPTION_INSET,
                                       IMG_CAP_REGION_START, IMG_CAP_SPAN, false,
                                       LV_ARCLABEL_TEXT_ALIGN_TRAILING);
    /* NULL when the widget allocation failed; the styler dereferences. */
    if (p->lbl_region) {
        image_page_caption_style(p->lbl_region);
        ui_arclabel_add_band(p->lbl_region, IMG_CAP_BAND_PAD);
    }
    /* No lv_arclabel_set_text("") here: ui_arclabel_create() already set it. */

    /* The timestamp is LEADING, which starts the run at angle_start +
     * angle_size (88), so it begins 2 degrees right of six o'clock and grows
     * right toward three o'clock. */
    p->lbl_timestamp = ui_arclabel_create(p->overlay_bar, &lv_font_overpass_27,
                                          rs - IMG_R_CAPTION_INSET,
                                          IMG_CAP_STAMP_START, IMG_CAP_SPAN, false,
                                          LV_ARCLABEL_TEXT_ALIGN_LEADING);
    if (p->lbl_timestamp) {
        image_page_caption_style(p->lbl_timestamp);
        ui_arclabel_add_band(p->lbl_timestamp, IMG_CAP_BAND_PAD);
    }
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
