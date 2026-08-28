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
 */
#include "ui_arclabel.h"

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

    /* Full parent, centred. The object is only a canvas: the radius is set
     * explicitly below, so the size never moves a glyph. It is the parent size
     * rather than 2 * radius so a glyph cell straddling the radius cannot be
     * clipped by its own object, and IGNORE_LAYOUT keeps a flex or grid parent
     * from moving it off centre. */
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
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
