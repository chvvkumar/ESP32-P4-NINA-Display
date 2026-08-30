/**
 * @file nina_round_overlay.c
 * @brief The picture overlay shared by the round capture layouts.
 *
 * See nina_round_overlay.h for what it owns. Geometry is written in the design
 * numbers for a 720 px panel but derived from screen_center(), ui_rim_radius()
 * and ui_chord_half(), so the 800 px panel gets the same composition with the
 * extra diameter spent on chord width; the stack keeps its fonts and is
 * measured UP from the rim rather than down from the top.
 *
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_round_overlay.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_dial.h"
#include "ui_round.h"
#include "ui_text_fit.h"

LV_FONT_DECLARE(lv_font_material_safety);
LV_FONT_DECLARE(lv_font_hanken_bold_64);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

#define ROV_ARC_W          12    /* rim arc stroke */
#define ROV_TRACK_COLOR    0x1a1d21
#define ROV_SHIELD_HALF    20    /* half the 40 px shield cell */
#define ROV_CROWN_CLEAR    14    /* clear space each side of the shield */

#define ROV_PLATE_H       178    /* rim upward to the top of the plate */
#define ROV_PLATE_OPA     LV_OPA_30
#define ROV_PLATE_COLOR   0x000000

#define ROV_ROW1_DY         6    /* target name, from the plate top */
#define ROV_ROW2_DY        44    /* RMS | hero | HFR, just under the name row */
#define ROV_ROW3_DY       122    /* filter + counter */

#define ROV_NAME_PAD       48    /* chord at the name row minus this */
#define ROV_VALS_PAD       60    /* chord at the value row minus this */
#define ROV_META_GAP       18
#define ROV_HERO_GAP        4    /* digits to the "s" */
#define ROV_CAP_GAP         6    /* caption to its value, on one baseline */

#define ROV_NAME_FG        0xf2f2f4

#define ROV_FONT_CAP      (&lv_font_montserrat_24)
#define ROV_FONT_VAL      (&lv_font_hanken_bold_28)
#define ROV_FONT_HERO     (&lv_font_hanken_bold_64)
#define ROV_FONT_FILTER   (&lv_font_montserrat_28)

/* The filter name shrinks one step before it starts dotting, so a two-word
* name reads whole where it can. Both faces are full ASCII. */
#define ROV_LADDER_FILTER_N 2
static const lv_font_t *const ROV_LADDER_FILTER[ROV_LADDER_FILTER_N] = {
    &lv_font_montserrat_28, &lv_font_montserrat_24,
};

/* Material Symbols codepoints (UTF-8), the same glyphs every other page uses. */
#define ROV_ICON_SAFE     "\xee\xa3\xa8"  /* U+E8E8 verified_user */
#define ROV_ICON_UNSAFE   "\xef\x80\x92"  /* U+F012 gpp_bad       */
#define ROV_ICON_UNKNOWN  "\xef\x80\x94"  /* U+F014 gpp_maybe     */

/* ---- geometry ----------------------------------------------------------- */

/* Centre-line radius of the rim arc: half a stroke in from the glass, so the
 * arc's outer edge lands on the panel edge. */
static inline int rov_arc_r(void)
{
    return screen_center() - ROV_ARC_W / 2;
}

/* Half the angular gap the crown needs, in degrees. asinf keeps the clearance
 * an absolute 14 px on both panel sizes instead of a fixed angle that would
 * open up on the wider disc. */
static inline int rov_gap_deg(void)
{
    const float r = (float)rov_arc_r();
    if (r <= 0.0f) return 0;
    float s = (float)(ROV_SHIELD_HALF + ROV_CROWN_CLEAR) / r;
    if (s > 1.0f) s = 1.0f;
    return (int)lroundf(asinf(s) * 180.0f / (float)M_PI);
}

/* Top edge of the readings plate, measured up from the bottom of the disc. */
static inline int rov_plate_top(void)
{
    return screen_center() + ui_rim_radius() - ROV_PLATE_H;
}

/* Usable width on a row whose vertical centre is at absolute y. */
static inline int rov_row_avail(int y_centre, int pad)
{
    int w = 2 * ui_chord_half(y_centre - screen_center()) - pad;
    return (w > 60) ? w : 60;
}

/* The name row's width. One function so create() and update() cannot drift:
 * update() re-fits on every new target name and must use the same box. */
static inline int rov_name_avail(void)
{
    const int h = lv_font_get_line_height(UI_FIT_LADDER_NAME_28[0]);
    return rov_row_avail(rov_plate_top() + ROV_ROW1_DY + h / 2, ROV_NAME_PAD);
}

/* ---- helpers ------------------------------------------------------------ */

static bool      rov_red(void);
static uint32_t  rov_dim(uint32_t color, int gb);
static uint32_t  rov_filter_color(const char *filter, int inst, int gb);
static lv_obj_t *rov_group(lv_obj_t *parent, lv_flex_flow_t flow);
static lv_obj_t *rov_label(lv_obj_t *parent, const lv_font_t *font, const char *text);
static void      rov_show(lv_obj_t *obj, bool on);
static void      rov_set_color(lv_obj_t *obj, uint32_t rgb);
static void      rov_set_bg_color(lv_obj_t *obj, lv_color_t c);
static void      rov_elapsed_cb(dashboard_page_t *p, int secs);
static void      rov_theme_page(dashboard_page_t *p, int gb);

static bool rov_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t rov_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

/* Filter tone: the configured colour, theme text under Red Night, label tone
 * when no filter is known. Already brightness applied. */
static uint32_t rov_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) return rov_dim(0x808080, gb);
    if (rov_red()) return rov_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return rov_dim(current_theme->label_color, gb);
}

/* Transparent content-sized group that only lays its children out. */
static lv_obj_t *rov_group(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_style_pad_gap(g, 0, 0);
    lv_obj_set_flex_flow(g, flow);
    return g;
}

static lv_obj_t *rov_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static void rov_show(lv_obj_t *obj, bool on)
{
    if (!obj) return;
    if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Every colour write in this file goes through here. lv_obj_set_local_style_prop
 * has no same-value early out: it always refreshes the style, which invalidates
 * the object, and every invalidation on this panel is a full 720x720 redraw. The
 * update path runs once per poll and most of its colours never change, so an
 * unguarded write would repaint the whole frame every two seconds. */
static void rov_set_color(lv_obj_t *obj, uint32_t rgb)
{
    if (!obj) return;
    lv_color_t c = lv_color_hex(rgb);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), c)) {
        lv_obj_set_style_text_color(obj, c, 0);
    }
}

/* Same guard for the rim arc's leading-edge cap, which is a background fill
 * rather than a text colour but carries the same repaint cost. */
static void rov_set_bg_color(lv_obj_t *obj, lv_color_t c)
{
    if (!obj) return;
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, 0), c)) {
        lv_obj_set_style_bg_color(obj, c, 0);
    }
}

/* Sole writer of the overlay's hero digits, and the one place the layout's own
 * readings-only hero is fed from, so both show the same second. */
static void rov_elapsed_cb(dashboard_page_t *p, int secs)
{
    if (!p) return;

    if (p->alt.ov.lbl_hero) {
        char buf[8];
        if (secs < 0) {
            buf[0] = '\0';
        } else {
            if (secs > 9999) secs = 9999;
            snprintf(buf, sizeof(buf), "%d", secs);
        }
        ui_label_set_text(p->alt.ov.lbl_hero, buf);
    }
    /* The unit carries the idle marker, so the digit cell only ever holds
     * digits and a layout hero in a digits-only face can mirror it verbatim. */
    if (p->alt.ov.lbl_hero_unit) {
        ui_label_set_text(p->alt.ov.lbl_hero_unit, (secs < 0) ? "--" : "s");
    }

    if (p->alt.elapsed_hook) p->alt.elapsed_hook(p, secs);
}

/* ---- create ------------------------------------------------------------- */

void nina_round_overlay_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent) return;

    p->alt.inst = page_index;
    memset(&p->alt.ov, 0, sizeof(p->alt.ov));

    const int gb = app_config_get()->color_brightness;

    /* 1: the rim exposure arc, flush with the glass, with an equal gap either
     * side of the crown. Rotation 270 puts zero degrees at twelve o'clock, so
     * the gap is symmetric about the shield. The spine owns the value (range
     * 0..1000 from the 200 ms tick) and the stale dimming. */
    {
        const int r = rov_arc_r();
        const int g = rov_gap_deg();
        lv_obj_t *arc = lv_arc_create(parent);
        lv_obj_set_size(arc, 2 * r + ROV_ARC_W, 2 * r + ROV_ARC_W);
        lv_obj_center(arc);
        lv_arc_set_rotation(arc, 270);
        lv_arc_set_bg_angles(arc, g, 360 - g);
        lv_arc_set_range(arc, 0, 1000);
        lv_arc_set_value(arc, 0);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_width(arc, ROV_ARC_W, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, ROV_ARC_W, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(ROV_TRACK_COLOR), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(arc, 0, LV_PART_INDICATOR);
        p->alt.arc_progress = arc;

        /* The leading edge rides on top of the band: lv_arc only fills whole
         * degrees, about 7 px out here, so without the cap a 300 s exposure
         * visibly jumps once a second. Track colour for now, rov_theme_page()
         * at the end of create() gives it the filter tone. */
        p->alt.cap_progress.obj   = ui_dial_cap_create(arc, ROV_ARC_W,
                                                       ROV_TRACK_COLOR);
        p->alt.cap_progress.r     = r;
        p->alt.cap_progress.a0    = g;
        p->alt.cap_progress.sweep = 360 - 2 * g;
    }

    /* 2: the crown. Centred at twelve with the top of the glyph on the rim
     * inset, which is the gap the arc leaves for it. */
    p->alt.ov.crown = rov_label(parent, &lv_font_material_safety, ROV_ICON_UNKNOWN);
    lv_obj_align(p->alt.ov.crown, LV_ALIGN_TOP_MID, 0,
                 screen_center() - ui_rim_radius());

    /* 3: the plate. Flat, no radius: its top edge is the only edge that shows,
     * the rest runs off the disc. */
    const int plate_top = rov_plate_top();
    p->alt.ov.plate = lv_obj_create(parent);
    lv_obj_remove_style_all(p->alt.ov.plate);
    lv_obj_remove_flag(p->alt.ov.plate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.ov.plate, LV_OBJ_FLAG_CLICKABLE);
    /* Down to the glass, not to the design rim: the rim sits a few px inside
     * the panel edge and a plate stopping there leaves a sliver of unplated
     * picture on the bottom chord. */
    lv_obj_set_size(p->alt.ov.plate, screen_size(), screen_size() - plate_top);
    lv_obj_align(p->alt.ov.plate, LV_ALIGN_TOP_MID, 0, plate_top);
    lv_obj_set_style_bg_opa(p->alt.ov.plate, ROV_PLATE_OPA, 0);
    lv_obj_set_style_bg_color(p->alt.ov.plate, lv_color_hex(ROV_PLATE_COLOR), 0);

    /* 4: row 1, the target name. ui_fit_label picks the largest face that fits
     * the chord at this row and bounds the box so a long name ellipsises
     * instead of wrapping into the values below. */
    {
        p->alt.ov.lbl_name = rov_label(parent, UI_FIT_LADDER_NAME_28[0], "--");
        lv_obj_set_style_text_align(p->alt.ov.lbl_name, LV_TEXT_ALIGN_CENTER, 0);
        ui_fit_label(p->alt.ov.lbl_name, UI_FIT_LADDER_NAME_28,
                     UI_FIT_LADDER_NAME_28_N, rov_name_avail());
        lv_obj_align(p->alt.ov.lbl_name, LV_ALIGN_TOP_MID, 0,
                     plate_top + ROV_ROW1_DY);
        nina_dashboard_bind_tap(p->alt.ov.lbl_name, NINA_TAP_SEQUENCE);
    }

    /* 5: row 2, RMS | hero seconds | HFR. One row as tall as the hero face,
     * children bottom aligned (LVGL has no baseline), pushed apart so the
     * seconds stay on the vertical axis. */
    {
        const int row_h = lv_font_get_line_height(ROV_FONT_HERO);
        /* The children are bottom aligned, so the ends of the row sit on its
         * BOTTOM edge, where the circle is narrower than at its centre line;
         * size the row from the chord there or the RMS and HFR values land on
         * the rim arc (seen on the 800 panel). */
        const int avail = rov_row_avail(plate_top + ROV_ROW2_DY + row_h,
                                        ROV_VALS_PAD);
        p->alt.ov.row_vals = lv_obj_create(parent);
        lv_obj_remove_style_all(p->alt.ov.row_vals);
        lv_obj_remove_flag(p->alt.ov.row_vals, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(p->alt.ov.row_vals, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(p->alt.ov.row_vals, avail, row_h);
        lv_obj_align(p->alt.ov.row_vals, LV_ALIGN_TOP_MID, 0,
                     plate_top + ROV_ROW2_DY);
        lv_obj_set_style_pad_all(p->alt.ov.row_vals, 0, 0);
        lv_obj_set_style_pad_gap(p->alt.ov.row_vals, 0, 0);
        lv_obj_set_flex_flow(p->alt.ov.row_vals, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(p->alt.ov.row_vals, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

        /* All five pieces sit on ONE baseline, each caption inline before its
         * value, which is what panel A of the board shows. LVGL has no baseline
         * align, so every cell bottom-aligns its children (cross END) and each
         * smaller face is lifted by the difference between its base_line and
         * the face it has to sit level with. */
        const int cap_lift = ROV_FONT_CAP->base_line - ROV_FONT_VAL->base_line;

        /* Left: caption, then value. */
        lv_obj_t *cell = rov_group(p->alt.ov.row_vals, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(cell, ROV_CAP_GAP, 0);
        p->alt.ov.lbl_rms_cap = rov_label(cell, ROV_FONT_CAP, "RMS");
        lv_obj_set_style_translate_y(p->alt.ov.lbl_rms_cap, cap_lift, 0);
        p->alt.ov.lbl_rms     = rov_label(cell, ROV_FONT_VAL, "--");
        nina_dashboard_bind_tap(cell, NINA_TAP_RMS);

        /* Centre: the seconds, with the unit on the digits own baseline. */
        lv_obj_t *hero = rov_group(p->alt.ov.row_vals, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(hero, ROV_HERO_GAP, 0);
        p->alt.ov.lbl_hero      = rov_label(hero, ROV_FONT_HERO, "");
        p->alt.ov.lbl_hero_unit = rov_label(hero, ROV_FONT_VAL, "--");
        lv_obj_set_style_translate_y(p->alt.ov.lbl_hero_unit,
            ROV_FONT_VAL->base_line - ROV_FONT_HERO->base_line, 0);
        nina_dashboard_bind_tap(hero, NINA_TAP_EXPOSURE);

        /* Right: caption, then value. */
        cell = rov_group(p->alt.ov.row_vals, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(cell, ROV_CAP_GAP, 0);
        p->alt.ov.lbl_hfr_cap = rov_label(cell, ROV_FONT_CAP, "HFR");
        lv_obj_set_style_translate_y(p->alt.ov.lbl_hfr_cap, cap_lift, 0);
        p->alt.ov.lbl_hfr     = rov_label(cell, ROV_FONT_VAL, "--");
        nina_dashboard_bind_tap(cell, NINA_TAP_HFR);
    }

    /* 6: row 3, the filter name and the sub counter. Bounded to the chord at
     * the row BOTTOM edge, the narrowest point the row spans, so a long filter
     * name trims instead of running under the rim arc. The counter is the
     * reading that must stay whole, so the filter label is the one that yields:
     * it gets whatever the widest counter leaves. */
    {
        const int row_h = lv_font_get_line_height(ROV_FONT_FILTER);
        const int row_w = rov_row_avail(plate_top + ROV_ROW3_DY + row_h,
                                        ROV_VALS_PAD);
        lv_point_t cnt = { 0, 0 };
        lv_text_get_size(&cnt, "9999 / 9999", ROV_FONT_VAL, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int filter_w = row_w - ROV_META_GAP - (int)cnt.x;
        if (filter_w < 60) filter_w = 60;

        p->alt.ov.row_meta = rov_group(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(p->alt.ov.row_meta, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(p->alt.ov.row_meta, ROV_META_GAP, 0);
        lv_obj_align(p->alt.ov.row_meta, LV_ALIGN_TOP_MID, 0,
                     plate_top + ROV_ROW3_DY);

        /* Right aligned inside its box, so a short name still sits against the
         * gap and the pair reads as one centred group. */
        p->alt.ov.lbl_filter = rov_label(p->alt.ov.row_meta, ROV_FONT_FILTER, "--");
        lv_obj_set_style_text_align(p->alt.ov.lbl_filter, LV_TEXT_ALIGN_RIGHT, 0);
        ui_fit_label(p->alt.ov.lbl_filter, ROV_LADDER_FILTER,
                     ROV_LADDER_FILTER_N, filter_w);

        p->alt.ov.lbl_count  = rov_label(p->alt.ov.row_meta, ROV_FONT_VAL, "--");
        nina_dashboard_bind_tap(p->alt.ov.lbl_filter, NINA_TAP_FILTER);
        nina_dashboard_bind_tap(p->alt.ov.lbl_count, NINA_TAP_SEQUENCE);
    }

    /* The overlay owns the interpolated seconds and forwards them to whatever
     * hook the layout registered for its own readings-only hero. */
    p->alt.elapsed_cb = rov_elapsed_cb;

    rov_theme_page(p, gb);
}

/* ---- theme -------------------------------------------------------------- */

static void rov_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !current_theme) return;

    const bool red = rov_red();
    const uint32_t text   = rov_dim(current_theme->text_color, gb);
    const uint32_t label  = rov_dim(current_theme->label_color, gb);
    const uint32_t filter = rov_filter_color(p->subbar.cached_filter,
                                             p->alt.inst, gb);

    if (p->alt.arc_progress) {
        lv_obj_set_style_arc_color(p->alt.arc_progress, lv_color_hex(filter),
                                   LV_PART_INDICATOR);
    }
    rov_set_bg_color(p->alt.cap_progress.obj, lv_color_hex(filter));
    rov_set_color(p->alt.ov.lbl_name,
                  rov_dim(red ? current_theme->text_color : ROV_NAME_FG, gb));
    rov_set_color(p->alt.ov.lbl_rms_cap, label);
    rov_set_color(p->alt.ov.lbl_hfr_cap, label);
    rov_set_color(p->alt.ov.lbl_hero, text);
    rov_set_color(p->alt.ov.lbl_hero_unit, label);
    rov_set_color(p->alt.ov.lbl_count, text);
    rov_set_color(p->alt.ov.lbl_filter, filter);
    /* The RMS and HFR tones follow the live values; the next update() repaints
     * them from the configured thresholds. */
    rov_set_color(p->alt.ov.lbl_rms,
                  rov_dim(red ? current_theme->rms_color
                              : current_theme->label_color, gb));
    rov_set_color(p->alt.ov.lbl_hfr,
                  rov_dim(red ? current_theme->hfr_color
                              : current_theme->label_color, gb));
}

void nina_round_overlay_apply_theme(dashboard_page_t *p)
{
    if (!p) return;
    rov_theme_page(p, app_config_get()->color_brightness);
}

/* ---- view mode ---------------------------------------------------------- */

void nina_round_overlay_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    if (!p) return;

    const bool picture = (mode == NINA_VIEW_PICTURE);
    const bool numbers = (mode == NINA_VIEW_NUMBERS);
    const bool stack   = (mode == NINA_VIEW_FULL);

    rov_show(p->alt.ov.crown, !picture);

    /* NUMBERS keeps the rim arc unless the layout has an exposure ring of its
     * own on that page, in which case two rings would say the same thing. The
     * round Dashboard always has one (p->arc_exposure, its rim ring), so it
     * qualifies by layout id rather than by arc_progress_num. */
    const bool layout_has_ring = (p->alt.arc_progress_num != NULL) || (p->layout == 0);
    rov_show(p->alt.arc_progress, !picture && !(numbers && layout_has_ring));

    rov_show(p->alt.ov.plate, stack);
    rov_show(p->alt.ov.lbl_name, stack);
    rov_show(p->alt.ov.row_vals, stack);
    rov_show(p->alt.ov.row_meta, stack);
}

/* ---- update ------------------------------------------------------------- */

void nina_round_overlay_update(dashboard_page_t *p, const nina_client_t *d,
                               int instance_idx, int gb)
{
    if (!p || !d || !current_theme) return;

    p->alt.inst = instance_idx;
    const bool red = rov_red();

    /* Crown. */
    if (p->alt.ov.crown) {
        const char *icon;
        uint32_t icon_color;
        if (!d->safety_connected) {
            icon = ROV_ICON_UNKNOWN;
            icon_color = red ? current_theme->label_color : 0x999999;
        } else if (d->safety_is_safe) {
            icon = ROV_ICON_SAFE;
            icon_color = red ? 0x7f1d1d : 0x4CAF50;
        } else {
            icon = ROV_ICON_UNSAFE;
            icon_color = red ? 0xff0000 : 0xF44336;
        }
        ui_label_set_text(p->alt.ov.crown, icon);
        rov_set_color(p->alt.ov.crown, rov_dim(icon_color, gb));
    }

    /* Target name: re-fit, because a longer name may need a smaller face. */
    if (p->alt.ov.lbl_name) {
        ui_label_set_text(p->alt.ov.lbl_name,
                          (d->target_name[0] != '\0') ? d->target_name : "--");
        ui_fit_label(p->alt.ov.lbl_name, UI_FIT_LADDER_NAME_28,
                     UI_FIT_LADDER_NAME_28_N, rov_name_avail());
    }

    /* TOTAL RMS, threshold tone. */
    if (p->alt.ov.lbl_rms) {
        char buf[24];
        uint32_t c;
        if (d->guider.rms_total > 0.0f) {
            snprintf(buf, sizeof(buf), "%.2f\"", (double)d->guider.rms_total);
            c = red ? current_theme->rms_color
                    : app_config_get_rms_color(d->guider.rms_total, instance_idx);
        } else {
            snprintf(buf, sizeof(buf), "--");
            c = current_theme->label_color;
        }
        ui_label_set_text(p->alt.ov.lbl_rms, buf);
        rov_set_color(p->alt.ov.lbl_rms, rov_dim(c, gb));
    }

    /* HFR, threshold tone. */
    if (p->alt.ov.lbl_hfr) {
        char buf[24];
        uint32_t c;
        if (d->hfr > 0.0f) {
            snprintf(buf, sizeof(buf), "%.2f", (double)d->hfr);
            c = red ? current_theme->hfr_color
                    : app_config_get_hfr_color(d->hfr, instance_idx);
        } else {
            snprintf(buf, sizeof(buf), "--");
            c = current_theme->label_color;
        }
        ui_label_set_text(p->alt.ov.lbl_hfr, buf);
        rov_set_color(p->alt.ov.lbl_hfr, rov_dim(c, gb));
    }

    /* Filter name in its configured colour, and the rim arc follows it. The
     * theme pass colours the arc from the sub bar's cached filter, which is
     * empty until a poll lands, so the live poll is the writer that matters;
     * guard the write or the style change repaints the full panel every
     * cycle. */
    {
        const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
        const uint32_t frgb = rov_filter_color(filter, instance_idx, gb);
        lv_color_t fc = lv_color_hex(frgb);
        if (p->alt.ov.lbl_filter) {
            ui_label_set_text(p->alt.ov.lbl_filter, filter);
            rov_set_color(p->alt.ov.lbl_filter, frgb);
        }
        if (p->alt.arc_progress &&
            !lv_color_eq(lv_obj_get_style_arc_color(p->alt.arc_progress,
                                                    LV_PART_INDICATOR), fc)) {
            lv_obj_set_style_arc_color(p->alt.arc_progress, fc,
                                       LV_PART_INDICATOR);
        }
        rov_set_bg_color(p->alt.cap_progress.obj, fc);
    }

    /* Sub counter "done / target". */
    if (p->alt.ov.lbl_count) {
        char buf[32];
        int target = d->exposure_iterations;
        int done   = d->exposure_count;
        if (done < 0) done = 0;
        if (target > 0 && done > target) done = target;
        if (target > 0) snprintf(buf, sizeof(buf), "%d / %d", done, target);
        else            snprintf(buf, sizeof(buf), "%d", done);
        ui_label_set_text(p->alt.ov.lbl_count, buf);
    }

    /* The tick writes the seconds; only the idle reset lives here, routed
     * through the same single writer. */
    if (d->exposure_total <= 0.0f) rov_elapsed_cb(p, -1);
}
