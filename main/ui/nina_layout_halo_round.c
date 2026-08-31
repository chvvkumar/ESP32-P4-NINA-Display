/**
 * @file nina_layout_halo_round.c
 * @brief NINA layout 2 (Halo) on a round panel: the readings-only page.
 *
 * Everything Halo used to draw over the picture now belongs to the shared
 * round overlay (nina_round_overlay.h/.c): the safety shield crown at twelve,
 * the exposure rim arc flush with the glass, and the readings plate across the
 * bottom of the disc. The spine creates the capture image before calling this
 * file and binds the tap cycle and the long press on it.
 *
 * What is left here is Halo's own READINGS-ONLY page, the one the fourth tap
 * state shows with the picture drawn fully transparent:
 *
 *   inner ring   one tick per sub (nina_subbar ring form), inside the overlay's
 *                rim arc, with the same crown gap so the ticks stop short of
 *                the shield
 *   step         the running sequence instruction, alone and centred
 *   name         the target, fitted to the chord at its own row
 *   hero         the interpolated exposure seconds, digits plus the unit
 *   readings     RMS | filter | HFR, three even columns under 24 px captions
 *   edge row     the meridian-flip countdown and the binding session limit
 *
 * The top 60 px of the vertical axis is left empty for the overlay's crown; the
 * topmost object here reaches y 176 on a 720 panel.
 *
 * Every coordinate is an offset from the panel centre and every row width comes
 * from the chord at that offset, so the 800 px panel gets the same composition
 * with wider rows.
 *
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_layout_alt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_round.h"
#include "ui_text_fit.h"

LV_FONT_DECLARE(lv_font_hanken_black_96);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

/* The sub tick ring sits inside the overlay's rim arc, whose inner edge is
 * screen_center() - 12 (a 12 px stroke on a centre line at screen_center() - 6).
 * A 27 px centre line with an 8 px stroke leaves an 11 px band between them. */
#define HALO_RING_OFF       27   /* ring centre line, in from the glass */
#define HALO_W_SUB           8   /* ring stroke */
#define HALO_TEXT_GAP       10   /* ring inner edge to the text field */

/* The crown gap, the same rule nina_round_overlay.c uses for its rim arc:
 * an absolute clearance each side of the 40 px shield rather than a fixed
 * angle, so the ticks stop the same distance short of the shield on both panel
 * sizes. Evaluated at THIS ring's radius, which is smaller than the arc's, so
 * the tick gap is a little wider in degrees and never runs under the crown. */
#define HALO_SHIELD_HALF    20
#define HALO_CROWN_CLEAR    14

/* Row centres as offsets from the panel centre, from the board's "No capture"
 * panel (centre y 360 on a 720 disc). */
#define HALO_DY_STEP      (-168)
#define HALO_DY_NAME      (-114)
#define HALO_DY_HERO       (-18)
#define HALO_DY_CAPS        (73)
#define HALO_DY_VALS       (113)
#define HALO_DY_EDGE       (168)

/* Widest |dy| each row reaches, half its own box height included. Row widths
 * are measured there, so no row can touch the ring at its ends. */
#define HALO_EXT_STEP      186
#define HALO_EXT_NAME      136
#define HALO_EXT_CAPS       87
#define HALO_EXT_VALS      134
#define HALO_EXT_EDGE      186

#define HALO_ROW_PAD        20   /* per side, ordinary rows */
#define HALO_NAME_PAD       24   /* per side, the name row (2 x 24 inset) */
#define HALO_ROW_MIN       120   /* floor, so a tiny panel never yields 0 */
#define HALO_HERO_GAP        6   /* digits to unit */

#define HALO_TARGET_FG    0xf2f2f4

/* The hero digits are the 96 px Hanken Black face, whose glyph set is digits
 * and colon only: its unit "s" and the idle "--" live in the 28 px Hanken Bold
 * label beside it, which is full ASCII. */
#define HALO_FONT_STEP    (&lv_font_montserrat_28)
#define HALO_FONT_HERO    (&lv_font_hanken_black_96)
#define HALO_FONT_UNIT    (&lv_font_hanken_bold_28)
#define HALO_FONT_CAP     (&lv_font_montserrat_24)
#define HALO_FONT_VAL     (&lv_font_montserrat_36)
#define HALO_FONT_EDGE    (&lv_font_montserrat_28)

/* ---- geometry ----------------------------------------------------------- */

static inline int halo_r_sub(void)  { return screen_center() - HALO_RING_OFF; }
static inline int halo_r_text(void)
{
    return halo_r_sub() - HALO_W_SUB / 2 - HALO_TEXT_GAP;
}

/* The TOTAL crown gap at the tick ring's radius, in degrees. asinf gives the
 * half angle, the same quantity nina_round_overlay.c feeds to its rim arc as
 * bg_angles(g, 360 - g); nina_subbar_create_ring() wants the whole gap, so this
 * returns twice that (360 / pi rather than 180 / pi). */
static int halo_gap_deg(void)
{
    const float r = (float)halo_r_sub();
    if (r <= 0.0f) return 0;
    float s = (float)(HALO_SHIELD_HALF + HALO_CROWN_CLEAR) / r;
    if (s > 1.0f) s = 1.0f;
    return (int)lroundf(asinf(s) * 360.0f / (float)M_PI);
}

/* Half chord of the text field (inside the tick ring) at offset dy. */
static int halo_half(int dy)
{
    int r = halo_r_text();
    if (dy < 0) dy = -dy;
    if (dy >= r) return 0;
    return (int)sqrtf((float)(r * r - dy * dy));
}

static int halo_row_w(int ext_dy, int pad)
{
    int w = 2 * halo_half(ext_dy) - 2 * pad;
    return (w < HALO_ROW_MIN) ? HALO_ROW_MIN : w;
}

/* ---- helpers ------------------------------------------------------------ */

static bool      halo_red(void);
static uint32_t  halo_dim(uint32_t color, int gb);
static uint32_t  halo_filter_color(const char *filter, int inst, int gb);
static lv_obj_t *halo_box(lv_obj_t *parent, int32_t w, int32_t h, int dy);
static lv_obj_t *halo_label(lv_obj_t *parent, const lv_font_t *font,
                            const char *text);
static lv_obj_t *halo_col(lv_obj_t *row, const lv_font_t *font, int w,
                          const char *text);
static void      halo_show(lv_obj_t *obj, bool show);
static void      halo_set_color(lv_obj_t *l, uint32_t rgb);
static void      halo_elapsed_hook(dashboard_page_t *p, int secs);
static void      halo_theme_page(dashboard_page_t *p, int gb);

static bool halo_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t halo_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

/* Filter tone: the configured filter colour, theme text on Red Night, label
 * tone when no filter is known. Already brightness applied. */
static uint32_t halo_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) return halo_dim(0x808080, gb);
    if (halo_red()) return halo_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return halo_dim(current_theme->label_color, gb);
}

/* Transparent, non-clickable container centred at @p dy, so a tap that is not
 * on one of this page's own reading labels falls through to the capture and
 * cycles the view. */
static lv_obj_t *halo_box(lv_obj_t *parent, int32_t w, int32_t h, int dy)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, dy);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_gap(o, 0, 0);
    return o;
}

static lv_obj_t *halo_label(lv_obj_t *parent, const lv_font_t *font,
                            const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* One column of an evenly split row. LONG_DOT needs a bounded box in both
 * axes: an unbounded height never dots, it grows. */
static lv_obj_t *halo_col(lv_obj_t *row, const lv_font_t *font, int w,
                          const char *text)
{
    lv_obj_t *l = halo_label(row, font, text);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    return l;
}

static void halo_show(lv_obj_t *obj, bool show)
{
    if (!obj) return;
    if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Set-if-changed text colour. LVGL invalidates unconditionally on a style
 * write and this panel is full refresh, so an unguarded per-poll recolour
 * repaints the whole screen every cycle even when the value has not moved.
 * The theme path may write unguarded: it runs once per theme change. */
static void halo_set_color(lv_obj_t *l, uint32_t rgb)
{
    if (!l) return;
    const lv_color_t c = lv_color_hex(rgb);
    if (!lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(l, c, 0);
    }
}

/* The readings hero, fed from the overlay's own elapsed writer through
 * p->alt.elapsed_hook, so this page and the overlay's plate can never disagree.
 * -1 is idle: the 96 px face has no '-' glyph, so the digits go empty and the
 * full-ASCII unit label carries the "--". */
static void halo_elapsed_hook(dashboard_page_t *p, int secs)
{
    if (!p || !p->alt.lbl_hero || !p->alt.lbl_hero_unit) return;
    if (secs < 0) {
        ui_label_set_text(p->alt.lbl_hero, "");
        ui_label_set_text(p->alt.lbl_hero_unit, "--");
        return;
    }
    if (secs > 9999) secs = 9999;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", secs);
    ui_label_set_text(p->alt.lbl_hero, buf);
    ui_label_set_text(p->alt.lbl_hero_unit, "s");
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_halo_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent || !current_theme) return;

    const int gb = app_config_get()->color_brightness;
    p->alt.inst = page_index;

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* The capture belongs to the spine and the crown, the rim arc and the plate
     * belong to nina_round_overlay.c. Nothing here creates or touches them. */

    /* 1: the inner ring of sub ticks, one block per sub, with the crown gap
     * left open at twelve. p->alt.arc_progress_num stays NULL, so the overlay
     * keeps showing its own rim arc on this page and the exposure progress is
     * still on screen. */
    nina_subbar_create_ring(&p->subbar, parent, halo_r_sub(), HALO_W_SUB,
                            halo_gap_deg());
    p->subbar.hide_single = true;   /* one sub: the rim arc is enough */
    p->alt.ring_inner = p->subbar.cont;

    /* The overlay owns p->alt.elapsed_cb; this page takes the hook it calls. */
    p->alt.elapsed_hook = halo_elapsed_hook;

    /* 2: the readings stack. One full-panel transparent group, so the whole
     * page appears and disappears with a single flag in set_view(). */
    p->alt.grp_mid = halo_box(parent, screen_size(), screen_size(), 0);

    /* 2a: the sequence step, alone and centred now that the shield is the
     * overlay's crown. */
    p->alt.lbl_seq_step = halo_label(p->alt.grp_mid, HALO_FONT_STEP, "--");
    lv_obj_set_size(p->alt.lbl_seq_step, halo_row_w(HALO_EXT_STEP, HALO_ROW_PAD),
                    lv_font_get_line_height(HALO_FONT_STEP));
    lv_obj_align(p->alt.lbl_seq_step, LV_ALIGN_CENTER, 0, HALO_DY_STEP);
    nina_dashboard_bind_tap(p->alt.lbl_seq_step, NINA_TAP_SEQUENCE);

    /* 2b: the target name, fitted to the chord at its own offset. */
    p->alt.lbl_target = halo_label(p->alt.grp_mid, &lv_font_montserrat_34, "--");
    lv_obj_align(p->alt.lbl_target, LV_ALIGN_CENTER, 0, HALO_DY_NAME);
    ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N,
                 halo_row_w(HALO_EXT_NAME, HALO_NAME_PAD));

    /* 2c: the hero, elapsed seconds. Digits in the 96 px face, unit beside them
     * in a full-ASCII 28 px face, bottom aligned with the unit lifted onto the
     * digits' baseline. */
    {
        lv_obj_t *row = halo_box(p->alt.grp_mid, LV_SIZE_CONTENT,
                                 LV_SIZE_CONTENT, HALO_DY_HERO);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(row, HALO_HERO_GAP, 0);

        p->alt.lbl_hero = halo_label(row, HALO_FONT_HERO, "");
        p->alt.lbl_hero_unit = halo_label(row, HALO_FONT_UNIT, "--");
        lv_obj_set_style_translate_y(p->alt.lbl_hero_unit,
            HALO_FONT_UNIT->base_line - HALO_FONT_HERO->base_line, 0);
    }

    /* 2d: captions and values, three even columns each, so RMS, the filter and
     * HFR line up under their own headings. Both rows take the NARROWER of the
     * two chords they span, or the columns would not align. */
    {
        const int w = LV_MIN(halo_row_w(HALO_EXT_CAPS, HALO_ROW_PAD),
                             halo_row_w(HALO_EXT_VALS, HALO_ROW_PAD));
        const int col = w / 3;

        lv_obj_t *caps = halo_box(p->alt.grp_mid, w,
                                  lv_font_get_line_height(HALO_FONT_CAP),
                                  HALO_DY_CAPS);
        lv_obj_set_flex_flow(caps, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(caps, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        p->alt.lbl_caption[0] = halo_col(caps, HALO_FONT_CAP, col, "RMS");
        p->alt.lbl_caption[1] = halo_col(caps, HALO_FONT_CAP, col, "");
        p->alt.lbl_caption[2] = halo_col(caps, HALO_FONT_CAP, col, "HFR");
        for (int i = 0; i < 3; i++) {
            lv_obj_set_style_text_letter_space(p->alt.lbl_caption[i], 2, 0);
        }

        p->alt.row_vals = halo_box(p->alt.grp_mid, w,
                                   lv_font_get_line_height(HALO_FONT_VAL),
                                   HALO_DY_VALS);
        lv_obj_set_flex_flow(p->alt.row_vals, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(p->alt.row_vals, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        p->alt.lbl_rms    = halo_col(p->alt.row_vals, HALO_FONT_VAL, col, "--");
        p->alt.lbl_filter = halo_col(p->alt.row_vals, HALO_FONT_VAL, col, "--");
        p->alt.lbl_hfr    = halo_col(p->alt.row_vals, HALO_FONT_VAL, col, "--");
        nina_dashboard_bind_tap(p->alt.lbl_rms, NINA_TAP_RMS);
        nina_dashboard_bind_tap(p->alt.lbl_filter, NINA_TAP_FILTER);
        nina_dashboard_bind_tap(p->alt.lbl_hfr, NINA_TAP_HFR);
    }

    /* 2e: the flip and session-limit row on the bottom rim. Uneven split, 40 to
     * 60: the flip column's longest real string is "FLIP 12h 34m" at about
     * 177 px in Montserrat 28, while the limit column pastes the reason onto
     * the countdown and reaches "TIME LIMIT 12h 34m" at about 280 px (both
     * measured with lv_text_get_size). An even split is 239 px per column on
     * the 4C and ellipsises the limit; at 40 / 60 the 4C's 478 px row gives
     * 191 and 287, so both fit, and the 3.4C is wider still. The two widths sum
     * to the row width exactly, so SPACE_BETWEEN leaves no gap. */
    {
        const int w         = halo_row_w(HALO_EXT_EDGE, HALO_ROW_PAD);
        const int col_flip  = w * 2 / 5;
        const int col_limit = w - col_flip;

        lv_obj_t *edge = halo_box(p->alt.grp_mid, w,
                                  lv_font_get_line_height(HALO_FONT_EDGE),
                                  HALO_DY_EDGE);
        lv_obj_set_flex_flow(edge, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(edge, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        p->alt.lbl_flip  = halo_col(edge, HALO_FONT_EDGE, col_flip, "FLIP --");
        p->alt.lbl_limit = halo_col(edge, HALO_FONT_EDGE, col_limit, "--");
        nina_dashboard_bind_tap(p->alt.lbl_flip, NINA_TAP_FLIP);
        nina_dashboard_bind_tap(p->alt.lbl_limit, NINA_TAP_SESSION);
    }

    halo_theme_page(p, gb);
}

/* ---- theme -------------------------------------------------------------- */

static void halo_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !current_theme) return;

    const bool red = halo_red();
    const uint32_t text  = halo_dim(current_theme->text_color, gb);
    const uint32_t label = halo_dim(current_theme->label_color, gb);

    if (p->alt.lbl_seq_step) {
        lv_obj_set_style_text_color(p->alt.lbl_seq_step,
            lv_color_hex(halo_dim(current_theme->header_text_color, gb)), 0);
    }
    if (p->alt.lbl_target) {
        lv_obj_set_style_text_color(p->alt.lbl_target,
            lv_color_hex(halo_dim(red ? current_theme->text_color
                                      : HALO_TARGET_FG, gb)), 0);
    }
    if (p->alt.lbl_hero) {
        lv_obj_set_style_text_color(p->alt.lbl_hero, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_hero_unit) {
        lv_obj_set_style_text_color(p->alt.lbl_hero_unit, lv_color_hex(label), 0);
    }
    for (int i = 0; i < 3; i++) {
        if (p->alt.lbl_caption[i]) {
            lv_obj_set_style_text_color(p->alt.lbl_caption[i],
                                        lv_color_hex(label), 0);
        }
    }
    if (p->alt.lbl_flip) {
        lv_obj_set_style_text_color(p->alt.lbl_flip, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_limit) {
        lv_obj_set_style_text_color(p->alt.lbl_limit, lv_color_hex(text), 0);
    }
    /* The RMS, HFR and filter tones depend on live values; the next update()
     * repaints them from the configured thresholds and filter colours. */
    if (p->alt.lbl_rms) {
        lv_obj_set_style_text_color(p->alt.lbl_rms,
            lv_color_hex(halo_dim(red ? current_theme->rms_color
                                      : current_theme->label_color, gb)), 0);
    }
    if (p->alt.lbl_hfr) {
        lv_obj_set_style_text_color(p->alt.lbl_hfr,
            lv_color_hex(halo_dim(red ? current_theme->hfr_color
                                      : current_theme->label_color, gb)), 0);
    }
    if (p->alt.lbl_filter) {
        lv_obj_set_style_text_color(p->alt.lbl_filter,
            lv_color_hex(halo_filter_color(p->subbar.cached_filter,
                                           p->alt.inst, gb)), 0);
    }
}

void nina_layout_halo_apply_theme(dashboard_page_t *p)
{
    if (!p) return;
    /* Drops a retained capture that was remapped for the other Red Night
     * state. The overlay does not do this, so the layout still must. */
    nina_layout_image_note_theme_switch(p->alt.inst);
    halo_theme_page(p, app_config_get()->color_brightness);
}

/* ---- view mode ---------------------------------------------------------- */

/* This page IS the NUMBERS composition, so it shows in that one state and
 * hides in every other. The capture, the crown and the rim arc are not this
 * module's to touch. Idempotent, creates and deletes nothing. */
void nina_layout_halo_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    if (!p) return;

    const bool numbers = (mode == NINA_VIEW_NUMBERS);
    halo_show(p->alt.grp_mid, numbers);
    /* The ring's flag has one writer: the sub bar, which also hides it for a
     * one-sub target (hide_single), so this goes through it. */
    nina_subbar_set_shown(&p->subbar, numbers);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_halo_update(dashboard_page_t *p, const nina_client_t *d,
                             int instance_idx, int gb)
{
    if (!p || !d || !current_theme) return;

    p->alt.inst = instance_idx;
    const bool red = halo_red();

    /* Sequence step. */
    if (p->alt.lbl_seq_step) {
        ui_label_set_text(p->alt.lbl_seq_step,
            (d->container_step[0] != '\0') ? d->container_step : "--");
    }

    /* Target name, refitted to the chord whenever the text changes. */
    if (p->alt.lbl_target) {
        ui_label_set_text(p->alt.lbl_target,
            (d->target_name[0] != '\0') ? d->target_name : "--");
        ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N,
                     halo_row_w(HALO_EXT_NAME, HALO_NAME_PAD));
    }

    /* Sub tick ring. The spine owns its progress, its stale dim and its theme. */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* Guiding RMS, threshold tone. */
    if (p->alt.lbl_rms) {
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
        ui_label_set_text(p->alt.lbl_rms, buf);
        halo_set_color(p->alt.lbl_rms, halo_dim(c, gb));
    }

    /* HFR, threshold tone. */
    if (p->alt.lbl_hfr) {
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
        ui_label_set_text(p->alt.lbl_hfr, buf);
        halo_set_color(p->alt.lbl_hfr, halo_dim(c, gb));
    }

    /* Filter name in its configured colour. */
    if (p->alt.lbl_filter) {
        const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
        ui_label_set_text(p->alt.lbl_filter, filter);
        halo_set_color(p->alt.lbl_filter,
                       halo_filter_color(filter, instance_idx, gb));
    }

    /* Meridian flip. The field is free text: a countdown, "--" when unknown,
     * or a state word such as "FLIPPING" that already reads as a sentence. */
    if (p->alt.lbl_flip) {
        char buf[64];
        const char *mf = d->meridian_flip;
        if (mf[0] == '\0' || strcmp(mf, "--") == 0) {
            snprintf(buf, sizeof(buf), "FLIP --");
        } else if (strcmp(mf, "FLIPPING") == 0) {
            snprintf(buf, sizeof(buf), "FLIPPING");
        } else {
            snprintf(buf, sizeof(buf), "FLIP %s", mf);
        }
        ui_label_set_text(p->alt.lbl_flip, buf);
    }

    /* Session limit: whichever condition binds first, with its own label. */
    if (p->alt.lbl_limit) {
        char buf[64];
        const char *rem = (d->target_time_remaining[0] != '\0')
                        ? d->target_time_remaining : "--";
        const char *why = (d->target_time_reason[0] != '\0')
                        ? d->target_time_reason : "LIMIT";
        snprintf(buf, sizeof(buf), "%s %s", why, rem);
        ui_label_set_text(p->alt.lbl_limit, buf);
    }

    /* The hero digits arrive through p->alt.elapsed_hook from the overlay's
     * elapsed writer, including the idle -1, so nothing writes them here. */
}
