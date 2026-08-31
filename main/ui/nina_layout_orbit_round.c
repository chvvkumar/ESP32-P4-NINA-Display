/**
 * @file nina_layout_orbit_round.c
 * @brief NINA layout 4 (Orbit) on a round panel: the readings-only page.
 *
 * This file owns ONE composition, the one the tap cycle calls NUMBERS. Over the
 * picture, layouts 2 and 4 look the same and nina_round_overlay.c draws all of
 * it: the safety crown at twelve, the rim exposure arc and the readings plate.
 * The capture object itself belongs to the spine, which creates it before this
 * create() runs and binds the tap cycle and the long-press preview on it.
 *
 * What Orbit draws when the picture steps aside:
 *
 *   outer ring   one block per sub, flush with the glass, with the same crown
 *                gap the overlay's rim arc leaves so the shield stays legible
 *   inner ring   the exposure arc, a 30 px band at r 196, in
 *                p->alt.arc_progress_num, which makes the overlay hide its own
 *                rim arc on this page so the two never say the same thing twice
 *   centre       the elapsed seconds as hero digits inside the inner ring, the
 *                filter name above them and the exposure length below
 *   sides        guiding RMS on the left, HFR on the right, outside the ring
 *   bottom       the meridian-flip countdown
 *
 * The hero seconds arrive through p->alt.elapsed_hook, which the overlay calls
 * after writing its own digits, so both heroes come from one writer and one
 * clock. This file never touches p->alt.elapsed_cb, p->alt.arc_progress or
 * p->alt.cap_img.
 *
 * Geometry is centre relative: every vertical position is screen_center() plus
 * an offset taken from the 720 px board, and every row width comes from
 * ui_chord_half() at that offset, so the 800 px panel gets the same composition
 * with wider rows and a larger black margin.
 *
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_layout_alt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_dial.h"
#include "ui_round.h"
#include "ui_text_fit.h"

LV_FONT_DECLARE(lv_font_hanken_black_96);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

/* Outer sub ring, flush with the glass: its centreline sits half a stroke in so
 * the outer edge lands on the panel edge, the same rule the round Dashboard and
 * the overlay's rim arc use. */
#define ORB_W_RIM          14

/* The crown gap. These two numbers and the formula below them are the rule from
 * nina_round_overlay.c (ROV_SHIELD_HALF, ROV_CROWN_CLEAR, rov_gap_deg): the
 * clear space each side of the 40 px shield stays an absolute 14 px on both
 * panel sizes instead of a fixed angle that would open up on the wider disc.
 * The overlay's helper is static, so the rule is restated here; the two radii
 * round to the same angle on both panels (6 degrees at 720, 5 at 800). */
#define ORB_SHIELD_HALF    20
#define ORB_CROWN_CLEAR    14

/* Inner exposure ring, board r 196 / stroke 30. Absolute pixels, not a fraction
 * of the rim, so the 800 panel spends its extra diameter on the margin and on
 * the side readings rather than inflating the hero ring. */
#define ORB_R_INNER       196
#define ORB_W_INNER        30
#define ORB_TRACK_COLOR   0x161b22

/* Insets: side readings from the chord, bottom rows from the chord, and the
 * shared 2 x 24 px inset every round name row uses. */
#define ORB_ROW_PAD        26
#define ORB_SIDE_PAD       24
#define ORB_NAME_PAD       48
#define ORB_NAME_GAP       12    /* name box bottom to the inner ring's top */

/* Target name tone: the board's white, replaced by the theme text colour on
 * Red Night. Same rule as the round Image-forward page. */
#define ORB_NAME_FG       0xf2f2f4

/* Offsets from the panel centre (board 258 / centre / 436 / 316 / 346 / 578 /
 * 638 on a 720 px panel). */
#define ORB_FILTER_DY   (-102)
#define ORB_OF_DY          76
#define ORB_CAP_DY       (-44)
#define ORB_VAL_DY       (-14)
#define ORB_SIDE_W        132
#define ORB_FLIP_DY       222
#define ORB_FLIPC_DY      278

/* Hero digits are the 96 px Hanken Black, whose glyph set is digits and colon
 * only, so its idle state is an empty string and never "--"; the exposure
 * length under it is a full-ASCII face. The readings use Montserrat because an
 * RMS figure needs a period and an inch mark, which the Hanken subset faces do
 * not carry. */
#define ORB_FONT_HERO     (&lv_font_hanken_black_96)
#define ORB_FONT_OF       (&lv_font_hanken_bold_28)
#define ORB_FONT_FILTER   (&lv_font_montserrat_32)
#define ORB_FONT_VALUE    (&lv_font_montserrat_36)
#define ORB_FONT_CAPTION  (&lv_font_montserrat_24)

/* The only caption of this page that needs a handle: it is hidden when the
 * flip value is not a countdown. The other two never change and never move, so
 * they hold no handle at all. */
enum { ORB_CAP_FLIP = 0 };

/* ---- helpers ------------------------------------------------------------ */

static bool      orb_red(void);
static uint32_t  orb_dim(uint32_t color, int gb);
static uint32_t  orb_filter_color(const char *filter, int inst, int gb);
static int       orb_ring_r(void);
static int       orb_crown_gap_deg(void);
static int       orb_name_dy(void);
static int       orb_name_avail(void);
static void      orb_show(lv_obj_t *obj, bool show);
static void      orb_set_text(lv_obj_t *lbl, const char *text);
static void      orb_set_color(lv_obj_t *lbl, uint32_t color);
static void      orb_set_bg_color(lv_obj_t *obj, uint32_t color);
static lv_obj_t *orb_label(lv_obj_t *parent, const lv_font_t *font,
                           const char *text, lv_text_align_t align);
static void      orb_elapsed_hook(dashboard_page_t *p, int secs);
static void      orb_theme_page(dashboard_page_t *p, int gb);

static bool orb_red(void) {
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t orb_dim(uint32_t color, int gb) {
    return app_config_apply_brightness(color, gb);
}

/* Filter tone: the configured filter colour, theme text on Red Night, label
 * tone when no filter is known. Already brightness applied. */
static uint32_t orb_filter_color(const char *filter, int inst, int gb) {
    if (!current_theme) return orb_dim(0x808080, gb);
    if (orb_red()) return orb_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return orb_dim(current_theme->label_color, gb);
}

/* Centre-line radius of the outer sub ring. */
static int orb_ring_r(void) {
    return screen_center() - ORB_W_RIM / 2;
}

/* The FULL angular gap the sub ring leaves at twelve, in degrees: twice the
 * half-angle the overlay computes for its rim arc, because
 * nina_subbar_create_ring() takes the whole gap while lv_arc takes each side. */
static int orb_crown_gap_deg(void) {
    const float r = (float)orb_ring_r();
    if (r <= 0.0f) return 0;
    float s = (float)(ORB_SHIELD_HALF + ORB_CROWN_CLEAR) / r;
    if (s > 1.0f) s = 1.0f;
    return 2 * (int)lroundf(asinf(s) * 180.0f / (float)M_PI);
}

/* Top edge of the target name row. Anchored UPWARD from the inner ring rather
 * than written as a literal, so the box can never grow into the ring: the
 * anchor uses the LARGEST ladder face's line height, and every smaller pick
 * leaves more clearance, not less. It lands at -267 on both panels, which is
 * about 47 px below the crown's shield at 720 and about 87 px at 800 (the
 * shield hangs from the rim, the name is measured from the centre). */
static int orb_name_dy(void) {
    return -(ORB_R_INNER + ORB_W_INNER / 2 + ORB_NAME_GAP
             + lv_font_get_line_height(UI_FIT_LADDER_NAME[0]));
}

/* The name's fitting width: the chord at its top edge, the narrowest point of
 * the box, less the shared 2 x 24 px inset. One function so create() and
 * update() cannot drift, because update() re-fits on every new target name and
 * must use the same box. */
static int orb_name_avail(void) {
    const int w = 2 * ui_chord_half(orb_name_dy()) - ORB_NAME_PAD;
    return (w > ORB_NAME_PAD) ? w : ORB_NAME_PAD;
}

static void orb_show(lv_obj_t *obj, bool show) {
    if (!obj) return;
    if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void orb_set_text(lv_obj_t *lbl, const char *text) {
    if (!lbl || !text) return;
    ui_label_set_text(lbl, text);
}

/* Set-if-changed. LVGL invalidates on ANY style write, moved value or not, and
 * this panel is full refresh, so an unguarded per-poll colour write repaints
 * the whole screen every couple of seconds (review M3). Every text colour this
 * file writes goes through here, the theme path included: the compare costs
 * nothing and one door is easier to keep shut than six. */
static void orb_set_color(lv_obj_t *lbl, uint32_t color) {
    if (!lbl) return;
    const lv_color_t c = lv_color_hex(color);
    if (lv_color_eq(lv_obj_get_style_text_color(lbl, LV_PART_MAIN), c)) return;
    lv_obj_set_style_text_color(lbl, c, 0);
}

/* Same guard for the inner ring's leading-edge cap, a background fill rather
 * than a text colour but with the same full-panel repaint cost. */
static void orb_set_bg_color(lv_obj_t *obj, uint32_t color) {
    if (!obj) return;
    const lv_color_t c = lv_color_hex(color);
    if (lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), c)) return;
    lv_obj_set_style_bg_color(obj, c, 0);
}

static lv_obj_t *orb_label(lv_obj_t *parent, const lv_font_t *font,
                           const char *text, lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* The hero digits' only writer. The overlay owns p->alt.elapsed_cb, writes its
 * own digits from the 200 ms tick and then calls this with the same value, so
 * the two heroes can never disagree. -1 is idle: the 96 px face carries digits
 * only, so it goes empty and the unit label carries the marker instead. */
static void orb_elapsed_hook(dashboard_page_t *p, int secs) {
    if (!p) return;
    if (secs < 0) {
        orb_set_text(p->alt.lbl_hero, "");
        orb_set_text(p->alt.lbl_hero_unit, "--");
        return;
    }
    if (secs > 9999) secs = 9999;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", secs);
    orb_set_text(p->alt.lbl_hero, buf);
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_orbit_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent) return;

    p->alt.inst = page_index;

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 1: the outer ring, one block per sub, flush with the glass and leaving
     * the overlay's crown its gap at twelve. */
    nina_subbar_create_ring(&p->subbar, parent, orb_ring_r(), ORB_W_RIM,
                            orb_crown_gap_deg());
    p->alt.ring_rim = p->subbar.cont;

    /* 2: the inner ring, this page's exposure arc. Storing it in
     * arc_progress_num is what makes the overlay drop its rim arc here; the
     * spine drives its value over 0..1000 and dims it while data is stale, so
     * this file only sets geometry, track and tone. */
    p->alt.arc_progress_num = lv_arc_create(parent);
    lv_obj_set_size(p->alt.arc_progress_num, 2 * ORB_R_INNER + ORB_W_INNER,
                                             2 * ORB_R_INNER + ORB_W_INNER);
    lv_obj_center(p->alt.arc_progress_num);
    lv_arc_set_rotation(p->alt.arc_progress_num, 270);
    lv_arc_set_bg_angles(p->alt.arc_progress_num, 0, 360);
    lv_arc_set_range(p->alt.arc_progress_num, 0, 1000);
    lv_arc_set_value(p->alt.arc_progress_num, 0);
    lv_obj_remove_style(p->alt.arc_progress_num, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(p->alt.arc_progress_num, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(p->alt.arc_progress_num, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(p->alt.arc_progress_num, ORB_W_INNER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->alt.arc_progress_num, ORB_W_INNER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(p->alt.arc_progress_num, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(p->alt.arc_progress_num, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->alt.arc_progress_num,
                               lv_color_hex(ORB_TRACK_COLOR), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(p->alt.arc_progress_num, 0, LV_PART_INDICATOR);

    /* The ring's leading-edge cap. lv_arc only fills whole degrees, several px
     * at this radius, so the cap rides the exact fraction the 200 ms tick
     * computes and the quantised band trails under it. Rotation 270 with bg
     * angles 0..360 means the ring starts at twelve and runs a full lap. Track
     * colour for now, orb_theme_page() at the end of create() tones it. */
    p->alt.cap_progress_num.obj   = ui_dial_cap_create(p->alt.arc_progress_num,
                                                       ORB_W_INNER,
                                                       ORB_TRACK_COLOR);
    p->alt.cap_progress_num.r     = ORB_R_INNER;
    p->alt.cap_progress_num.a0    = 0;
    p->alt.cap_progress_num.sweep = 360;

    /* 3: every text of this page in one transparent full-panel group, so the
     * view switch is a single flag. Not clickable, so a tap between the labels
     * falls through to the capture the spine bound the cycle on. */
    p->alt.grp_mid = lv_obj_create(parent);
    lv_obj_remove_style_all(p->alt.grp_mid);
    lv_obj_remove_flag(p->alt.grp_mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.grp_mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(p->alt.grp_mid, LV_LAYOUT_NONE);
    lv_obj_set_size(p->alt.grp_mid, screen_size(), screen_size());
    lv_obj_center(p->alt.grp_mid);
    lv_obj_set_style_pad_all(p->alt.grp_mid, 0, 0);

    /* Target name, the board's top row. It is the only object of this page on
     * the upper vertical axis and it clears the crown's keep-out by about
     * 47 px at 720; the picture-side name lives in the overlay's plate, which
     * is hidden in this mode. */
    p->alt.lbl_target = orb_label(p->alt.grp_mid, UI_FIT_LADDER_NAME[0], "--",
                                  LV_TEXT_ALIGN_CENTER);
    lv_obj_align(p->alt.lbl_target, LV_ALIGN_TOP_MID, 0,
                 screen_center() + orb_name_dy());
    ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N,
                 orb_name_avail());

    /* Filter name over the hero, exposure length under it, both inside the
     * inner ring. */
    {
        const int inner_w = 2 * ORB_R_INNER - 40;

        p->alt.lbl_filter = orb_label(p->alt.grp_mid, ORB_FONT_FILTER, "",
                                      LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(p->alt.lbl_filter, inner_w);
        lv_obj_align(p->alt.lbl_filter, LV_ALIGN_TOP_MID, 0,
                     screen_center() + ORB_FILTER_DY);
        nina_dashboard_bind_tap(p->alt.lbl_filter, NINA_TAP_FILTER);

        /* The 96 px face reports a line height near 69 px, well inside the
         * ring's 181 px inner radius. */
        p->alt.lbl_hero = orb_label(p->alt.grp_mid, ORB_FONT_HERO, "",
                                    LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(p->alt.lbl_hero, inner_w);
        lv_obj_align(p->alt.lbl_hero, LV_ALIGN_CENTER, 0, 0);

        p->alt.lbl_hero_unit = orb_label(p->alt.grp_mid, ORB_FONT_OF, "--",
                                         LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(p->alt.lbl_hero_unit, inner_w);
        lv_obj_align(p->alt.lbl_hero_unit, LV_ALIGN_TOP_MID, 0,
                     screen_center() + ORB_OF_DY);
    }

    /* RMS and HFR sit outside the ring. The x inset is measured on the chord at
     * the deepest edge of the pair, so the caption above and the value below
     * share one column that stays inside the disc; the 132 px cell keeps the
     * widest figure clear of the ring's 181 px inner edge. The fixed captions
     * carry no handle: they never change text and inherit their tone from the
     * group. */
    {
        const int val_h = lv_font_get_line_height(ORB_FONT_VALUE);
        const int bound = LV_MAX(-ORB_CAP_DY, ORB_VAL_DY + val_h);
        const int x     = screen_center() - ui_chord_half(bound) + ORB_SIDE_PAD;

        lv_obj_t *cap_rms = orb_label(p->alt.grp_mid, ORB_FONT_CAPTION, "RMS",
                                      LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(cap_rms, ORB_SIDE_W);
        lv_obj_align(cap_rms, LV_ALIGN_TOP_LEFT, x, screen_center() + ORB_CAP_DY);

        p->alt.lbl_rms = orb_label(p->alt.grp_mid, ORB_FONT_VALUE, "--",
                                   LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(p->alt.lbl_rms, ORB_SIDE_W);
        lv_obj_align(p->alt.lbl_rms, LV_ALIGN_TOP_LEFT, x,
                     screen_center() + ORB_VAL_DY);
        nina_dashboard_bind_tap(p->alt.lbl_rms, NINA_TAP_RMS);

        lv_obj_t *cap_hfr = orb_label(p->alt.grp_mid, ORB_FONT_CAPTION, "HFR",
                                      LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(cap_hfr, ORB_SIDE_W);
        lv_obj_align(cap_hfr, LV_ALIGN_TOP_RIGHT, -x, screen_center() + ORB_CAP_DY);

        p->alt.lbl_hfr = orb_label(p->alt.grp_mid, ORB_FONT_VALUE, "--",
                                   LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(p->alt.lbl_hfr, ORB_SIDE_W);
        lv_obj_align(p->alt.lbl_hfr, LV_ALIGN_TOP_RIGHT, -x,
                     screen_center() + ORB_VAL_DY);
        nina_dashboard_bind_tap(p->alt.lbl_hfr, NINA_TAP_HFR);
    }

    /* Flip countdown, value over its caption, both below the ring's outer edge
     * (the ring reaches 211 px from the centre, the value starts at 222). */
    {
        const int val_h  = lv_font_get_line_height(ORB_FONT_VALUE);
        const int cap_h  = lv_font_get_line_height(ORB_FONT_CAPTION);
        const int val_w  = 2 * ui_chord_half(ORB_FLIP_DY + val_h) - 2 * ORB_ROW_PAD;
        const int capt_w = 2 * ui_chord_half(ORB_FLIPC_DY + cap_h) - 2 * ORB_ROW_PAD;

        p->alt.lbl_flip = orb_label(p->alt.grp_mid, ORB_FONT_VALUE, "--",
                                    LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(p->alt.lbl_flip, val_w);
        lv_obj_align(p->alt.lbl_flip, LV_ALIGN_TOP_MID, 0,
                     screen_center() + ORB_FLIP_DY);
        nina_dashboard_bind_tap(p->alt.lbl_flip, NINA_TAP_FLIP);

        /* "FLIP IN" is only true over a countdown, so update() hides it when
         * NINA sends a word instead of a time (review L5). That needs a
         * handle, and the caption slot is the honest place for it. */
        p->alt.lbl_caption[ORB_CAP_FLIP] =
            orb_label(p->alt.grp_mid, ORB_FONT_CAPTION, "FLIP IN",
                      LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(p->alt.lbl_caption[ORB_CAP_FLIP], capt_w);
        lv_obj_align(p->alt.lbl_caption[ORB_CAP_FLIP], LV_ALIGN_TOP_MID, 0,
                     screen_center() + ORB_FLIPC_DY);
    }

    /* The overlay calls this after writing its own digits. */
    p->alt.elapsed_hook = orb_elapsed_hook;

    orb_theme_page(p, app_config_get()->color_brightness);
}

/* ---- theme -------------------------------------------------------------- */

static void orb_theme_page(dashboard_page_t *p, int gb) {
    if (!p || !p->alt.grp_mid || !current_theme) return;

    const bool     red   = orb_red();
    const uint32_t text  = orb_dim(current_theme->text_color, gb);
    const uint32_t label = orb_dim(current_theme->label_color, gb);
    const uint32_t prog  = orb_dim(red ? current_theme->text_color
                                       : current_theme->progress_color, gb);

    if (p->alt.arc_progress_num) {
        lv_obj_set_style_arc_color(p->alt.arc_progress_num, lv_color_hex(prog),
                                   LV_PART_INDICATOR);
    }
    orb_set_bg_color(p->alt.cap_progress_num.obj, prog);
    nina_subbar_apply_theme(&p->subbar);

    /* The three fixed captions carry no local colour, so this one write on
     * their group re-tones all of them; every other child sets its own colour
     * and is unaffected. */
    lv_obj_set_style_text_color(p->alt.grp_mid, lv_color_hex(label), 0);

    orb_set_color(p->alt.lbl_target,
                  orb_dim(red ? current_theme->text_color : ORB_NAME_FG, gb));
    orb_set_color(p->alt.lbl_hero, text);
    orb_set_color(p->alt.lbl_hero_unit, label);
    orb_set_color(p->alt.lbl_flip, text);
    orb_set_color(p->alt.lbl_filter,
                  orb_filter_color(p->subbar.cached_filter, p->alt.inst, gb));

    /* RMS and HFR tones depend on the live value; the next update() repaints
     * them, so a theme switch only needs a sane resting colour. */
    orb_set_color(p->alt.lbl_rms,
                  orb_dim(red ? current_theme->rms_color
                              : current_theme->label_color, gb));
    orb_set_color(p->alt.lbl_hfr,
                  orb_dim(red ? current_theme->hfr_color
                              : current_theme->label_color, gb));
}

void nina_layout_orbit_apply_theme(dashboard_page_t *p)
{
    if (!p) return;
    nina_layout_image_note_theme_switch(p->alt.inst);
    orb_theme_page(p, app_config_get()->color_brightness);
}

/* ---- view mode ---------------------------------------------------------- */

/* This page is the readings-only composition, so it shows in NUMBERS and hides
 * in every other mode. Hidden flags only, on objects create() already built:
 * idempotent, and it never touches p->alt.cap_img, which the spine owns and
 * which has to stay visible to the input system in every mode. */
void nina_layout_orbit_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    if (!p) return;

    const bool on = (mode == NINA_VIEW_NUMBERS);

    /* The sub ring's flag has one writer, the sub bar (see hide_single). Orbit
     * keeps its ring for a one-sub target: the segments are its outer rim. */
    nina_subbar_set_shown(&p->subbar, on);
    orb_show(p->alt.arc_progress_num, on);
    orb_show(p->alt.grp_mid, on);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_orbit_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb)
{
    if (!p || !d || !p->alt.grp_mid || !current_theme) return;

    p->alt.inst = instance_idx;
    const bool red = orb_red();

    /* Target name, re-fitted on every change against the same box create()
     * used. */
    if (p->alt.lbl_target) {
        orb_set_text(p->alt.lbl_target,
                     (d->target_name[0] != 0) ? d->target_name : "--");
        ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME,
                     UI_FIT_LADDER_NAME_N, orb_name_avail());
    }

    /* Filter name in its configured colour. */
    {
        const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
        orb_set_text(p->alt.lbl_filter, filter);
        orb_set_color(p->alt.lbl_filter,
                      orb_filter_color(filter, instance_idx, gb));
        /* The inner exposure ring follows the filter colour too; the theme
         * pass only gives it a resting tone. Guarded write: an arc style
         * change invalidates, and the panel is full refresh. */
        if (p->alt.arc_progress_num) {
            lv_color_t fc =
                lv_color_hex(orb_filter_color(filter, instance_idx, gb));
            if (!lv_color_eq(lv_obj_get_style_arc_color(p->alt.arc_progress_num,
                                                        LV_PART_INDICATOR), fc)) {
                lv_obj_set_style_arc_color(p->alt.arc_progress_num, fc,
                                           LV_PART_INDICATOR);
            }
        }
        orb_set_bg_color(p->alt.cap_progress_num.obj,
                         orb_filter_color(filter, instance_idx, gb));
    }

    /* Guiding RMS, threshold tone. */
    {
        char buf[24];
        uint32_t rms_c;
        if (d->guider.rms_total > 0.0f) {
            snprintf(buf, sizeof(buf), "%.2f\"", (double)d->guider.rms_total);
            rms_c = red ? current_theme->rms_color
                        : app_config_get_rms_color(d->guider.rms_total, instance_idx);
        } else {
            snprintf(buf, sizeof(buf), "--");
            rms_c = current_theme->label_color;
        }
        orb_set_text(p->alt.lbl_rms, buf);
        orb_set_color(p->alt.lbl_rms, orb_dim(rms_c, gb));
    }

    /* HFR, threshold tone. */
    {
        char buf[24];
        uint32_t hfr_c;
        if (d->hfr > 0.0f) {
            snprintf(buf, sizeof(buf), "%.2f", (double)d->hfr);
            hfr_c = red ? current_theme->hfr_color
                        : app_config_get_hfr_color(d->hfr, instance_idx);
        } else {
            snprintf(buf, sizeof(buf), "--");
            hfr_c = current_theme->label_color;
        }
        orb_set_text(p->alt.lbl_hfr, buf);
        orb_set_color(p->alt.lbl_hfr, orb_dim(hfr_c, gb));
    }

    /* Meridian flip. The value is free text from NINA: a countdown ("3h 58m"),
     * a state word ("FLIPPING"), or nothing at all. "FLIP IN" reads correctly
     * only over a countdown, so the caption follows the simplest honest test
     * there is, whether the value carries a digit. "FLIP IN FLIPPING" and
     * "FLIP IN --" can no longer happen; the word or the dashes stand alone. */
    {
        const char *flip = (d->meridian_flip[0] != '\0')
                           ? d->meridian_flip : "--";
        orb_set_text(p->alt.lbl_flip, flip);
        orb_show(p->alt.lbl_caption[ORB_CAP_FLIP],
                 strpbrk(flip, "0123456789") != NULL);
    }

    /* Exposure length under the hero digits, and the idle marker the digits
     * cannot spell. Clamped on the float, because the cast of an out-of-range
     * float to int is undefined. */
    {
        char buf[24];
        if (d->exposure_total > 0.0f) {
            float total = d->exposure_total;
            if (total > 99999.0f) total = 99999.0f;
            snprintf(buf, sizeof(buf), "/ %ds", (int)(total + 0.5f));
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        orb_set_text(p->alt.lbl_hero_unit, buf);
    }

    /* Outer sub ring: the spine owns set_progress() and the stale dimming. */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* Elapsed digits: the overlay's tick writes them through the hook; only the
     * idle reset lives here, routed through that same single writer. */
    if (d->exposure_total <= 0.0f) orb_elapsed_hook(p, -1);
}
