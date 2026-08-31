/**
 * @file ui_arclabel.c
 * @brief Rim text wrapper. Round family only.
 *
 * The widget's own conventions, read from
 * managed_components/lvgl__lvgl/src/widgets/arclabel/lv_arclabel.c:
 *
 * - Angles: 0 is three o'clock and they grow clockwise (the draw loop takes
 *   sin() for y, and y grows downward). Twelve o'clock is 270, six o'clock 90.
 * - The arc centre is the object's CONTENT box centre, so a full-parent object
 *   with no padding puts the arc on the parent's centre and the widget's own
 *   centre offsets stay at 0.
 * - The circle point is the glyph BASELINE MIDPOINT: lv_draw_letter() sets the
 *   pivot to {adv_w/2, ascent} and lv_draw_unit_draw_letter() subtracts it
 *   again from the letter box.
 * - Direction decides which way the glyph body grows from that baseline:
 *   clockwise rotates by curr_angle + 90 (body grows outward), counter-
 *   clockwise by curr_angle - 90 (body grows inward).
 * - text_align_v then shifts the baseline circle by the ascent (LEADING) or by
 *   minus the base line (TRAILING), with the sign flipped for counter-
 *   clockwise. So LEADING with clockwise and TRAILING with counter-clockwise
 *   both land the outer edge of the glyph cell exactly on `radius`, which is
 *   what every caller wants: text inside the rim, never crossing it. The
 *   wrapper picks it from the direction so no caller has to know this.
 *
 * ui_arclabel_add_band() gives a caller a translucent band directly behind
 *   the glyph run, refitted to the visible text's angular window on every
 *   ui_arclabel_set_text() call. It is a plain lv_arc sibling of the
 *   arclabel, not a style on the arclabel object itself: the arclabel is
 *   sized to the whole panel (see ui_arclabel_create() above), so a
 *   background style on it would tint the entire disc rather than just the
 *   strip under the text.
 */
#include "ui_arclabel.h"
#include "screen_geom.h"

/* Angular span both rim conventions use. 120 degrees at radius 312 is about
 * 653 px of arc, which holds a caption at 27 px with room for the ellipsis. */
#define UI_ARCLABEL_SPAN_DEG 120

lv_obj_t *ui_arclabel_create(lv_obj_t *parent, const lv_font_t *font, int radius,
                             int angle_start_deg, int angle_size_deg, bool dir_cw,
                             lv_arclabel_text_align_t halign)
{
    if (!parent || radius <= 0 || angle_size_deg <= 0) return NULL;

    lv_obj_t *o = lv_arclabel_create(parent);
    if (!o) return NULL;

    /* No theme card behind rim text (guideline C1) and no padding, so the
     * content box centre the widget measures from is the object centre. */
    lv_obj_remove_style_all(o);
    /* The strip above leaves the text colour at the LVGL default, which is
     * black and invisible on every dark theme. White is the readable default;
     * every caller sets its own colour straight after and overwrites this. */
    lv_obj_set_style_text_color(o, lv_color_white(), 0);

    /* Full panel, centred. The object is only a canvas: the radius is set
     * explicitly below, so the size never moves a glyph. It is sized to the
     * panel (not a parent-relative percent, and not 2 * radius) so a padded
     * ancestor cannot shrink it and clip a glyph cell straddling the radius;
     * IGNORE_LAYOUT keeps a flex or grid parent from moving it off centre. */
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(o, screen_size(), screen_size());
    lv_obj_center(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    if (font) lv_obj_set_style_text_font(o, font, 0);

    lv_arclabel_set_radius(o, (uint32_t)radius);
    lv_arclabel_set_angle_start(o, angle_start_deg);
    lv_arclabel_set_angle_size(o, angle_size_deg);
    lv_arclabel_set_dir(o, dir_cw ? LV_ARCLABEL_DIR_CLOCKWISE
                                  : LV_ARCLABEL_DIR_COUNTER_CLOCKWISE);
    lv_arclabel_set_text_horizontal_align(o, halign);
    /* See the file comment: this pairing is what keeps the glyphs inside the
     * radius for both directions. */
    lv_arclabel_set_text_vertical_align(o, dir_cw ? LV_ARCLABEL_TEXT_ALIGN_LEADING
                                                  : LV_ARCLABEL_TEXT_ALIGN_TRAILING);
    /* A caption that outgrows its span ends in dots rather than wrapping round
     * the rim into the opposite caption. */
    lv_arclabel_set_overflow(o, LV_ARCLABEL_OVERFLOW_ELLIPSIS);
    /* The constructor installs a static default string; start empty so a page
     * that has no data yet shows nothing. */
    lv_arclabel_set_text(o, "");
    return o;
}

lv_obj_t *ui_arclabel_top(lv_obj_t *parent, const lv_font_t *font, int radius)
{
    /* Twelve o'clock is 270. Centre a 120 degree span on it and run clockwise,
     * which is left to right across the top of the panel. */
    return ui_arclabel_create(parent, font, radius,
                              270 - UI_ARCLABEL_SPAN_DEG / 2, UI_ARCLABEL_SPAN_DEG,
                              true, LV_ARCLABEL_TEXT_ALIGN_CENTER);
}

lv_obj_t *ui_arclabel_bottom(lv_obj_t *parent, const lv_font_t *font, int radius)
{
    /* Six o'clock is 90. Counter-clockwise means the first glyph is drawn at
     * angle_start + angle_size (150, bottom left) and the last at angle_start
     * (30, bottom right), so the text reads left to right along the bottom rim
     * and is upright rather than upside down. */
    return ui_arclabel_create(parent, font, radius,
                              90 - UI_ARCLABEL_SPAN_DEG / 2, UI_ARCLABEL_SPAN_DEG,
                              false, LV_ARCLABEL_TEXT_ALIGN_CENTER);
}

lv_obj_t *ui_arclabel_add_band(lv_obj_t *arclabel, int pad_px)
{
    if (!arclabel) return NULL;

    lv_obj_t *parent = lv_obj_get_parent(arclabel);
    lv_obj_t *band = lv_arc_create(parent);
    if (!band) return NULL;

    const int r = (int)lv_arclabel_get_radius(arclabel);
    const lv_font_t *font = lv_obj_get_style_text_font(arclabel, LV_PART_MAIN);
    const int lh = lv_font_get_line_height(font);
    const int w = lh + 2 * pad_px;
    const int rc = r + pad_px - w / 2;
    const int size = 2 * rc + w;

    lv_obj_remove_style_all(band);
    lv_obj_set_style_arc_color(band, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(band, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_width(band, w, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(band, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(band, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(band, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, 0);

    lv_obj_set_size(band, size, size);
    lv_obj_center(band);

    lv_arc_set_value(band, 0);
    lv_arc_set_rotation(band, 0);

    lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(band, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);

    /* Band is created after arclabel, so it starts on top; drop it directly
     * beneath so the glyph run draws over it. */
    lv_obj_move_to_index(band, lv_obj_get_index(arclabel));

    /* From here on, arclabel's user_data belongs to this wrapper. */
    lv_obj_set_user_data(arclabel, band);

    return band;
}

void ui_arclabel_set_text(lv_obj_t *arclabel, const char *text)
{
    if (!arclabel) return;

    lv_arclabel_set_text(arclabel, text ? text : "");

    lv_obj_t *band = (lv_obj_t *)lv_obj_get_user_data(arclabel);
    if (!band) return;

    if (!text || text[0] == '\0') {
        lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const float t = (float)lv_arclabel_get_text_angle(arclabel);
    const float s = (float)lv_arclabel_get_angle_start(arclabel);
    const float z = (float)lv_arclabel_get_angle_size(arclabel);
    const lv_arclabel_dir_t dir = lv_arclabel_get_dir(arclabel);
    const lv_arclabel_text_align_t halign = lv_arclabel_get_text_horizontal_align(arclabel);

    float a0 = 0.0f;
    float a1 = 0.0f;

    if (halign == LV_ARCLABEL_TEXT_ALIGN_LEADING) {
        if (dir == LV_ARCLABEL_DIR_CLOCKWISE) {
            a0 = s;
            a1 = s + t;
        } else {
            a1 = s + z;
            a0 = s + z - t;
        }
    } else if (halign == LV_ARCLABEL_TEXT_ALIGN_TRAILING) {
        if (dir == LV_ARCLABEL_DIR_CLOCKWISE) {
            a1 = s + z;
            a0 = s + z - t;
        } else {
            a0 = s;
            a1 = s + t;
        }
    } else {
        /* CENTER (and the widget's DEFAULT, which no caller here uses). */
        const float c = s + z / 2.0f;
        a0 = c - t / 2.0f;
        a1 = c + t / 2.0f;
    }

    /* Recompute the centreline radius from the band's own current style
     * rather than storing it: pad_px is read back out of the arc width. */
    const int r = (int)lv_arclabel_get_radius(arclabel);
    const int arc_w = (int)lv_obj_get_style_arc_width(band, LV_PART_MAIN);
    const int lh = lv_font_get_line_height(lv_obj_get_style_text_font(arclabel, LV_PART_MAIN));
    const int pad = (arc_w - lh) / 2;
    const float rc = (float)r + (float)pad - (float)arc_w / 2.0f;

    /* arc_rounded already extends the drawn band by half its width past each
     * window end (the cap is a semicircle of diameter arc_w), so the window
     * itself is grown by pad MINUS that cap radius: the visible band then ends
     * pad px past the last glyph, not pad + arc_w / 2. That figure is negative
     * for every pad below arc_w / 2, which shrinks the window; a text shorter
     * than the cap (a "--" placeholder) would invert it and lv_arc would draw
     * the complementary near-full circle, so the window is floored at half a
     * degree, which the two caps render as one round pill. */
    const float ext_deg = (float)(pad - arc_w / 2) * 180.0f / (3.14159265f * rc);
    a0 -= ext_deg;
    a1 += ext_deg;
    if (a1 - a0 < 0.5f) {
        const float mid = (a0 + a1) / 2.0f;
        a0 = mid - 0.25f;
        a1 = mid + 0.25f;
    }

    while (a0 < 0.0f) a0 += 360.0f;
    while (a0 >= 360.0f) a0 -= 360.0f;
    while (a1 < 0.0f) a1 += 360.0f;
    while (a1 >= 360.0f) a1 -= 360.0f;

    lv_arc_set_bg_angles(band, a0, a1);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_HIDDEN);
}
