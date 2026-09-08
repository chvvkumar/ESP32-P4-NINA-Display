/**
 * @file nina_layout_rings_round.c
 * @brief NINA layout 7 (Two rings) on a round panel: the readings-only page.
 *
 * Over the picture this board looks like every other round NINA design, and
 * nina_round_overlay.c draws all of that: the safety crown at twelve, the rim
 * exposure arc and the readings plate across the bottom of the disc. The
 * capture belongs to the spine, which creates it before this create() runs and
 * binds the tap cycle and the long press on it.
 *
 * What Two rings draws when the picture steps aside:
 *
 *   outer ring   the exposure, a 12 px band at r 138 in p->alt.arc_progress_num,
 *                which makes the overlay hide its own rim arc on this page so
 *                the two never say the same thing twice
 *   inner ring   one block per sub (the sub bar's ring form), just inside it
 *   centre       the filter and loop count, the elapsed seconds as the hero,
 *                and the completed count with its integration time
 *   west arm     guiding RMS, star size, star count
 *   east arm     the frame length, the flip countdown, the session limit
 *   top          the rig identity, the sequence name and the running step,
 *                then the target name in the wide middle band
 *   bottom       the power readings on a low chord
 *
 * The top 60 px of the vertical axis is left empty for the overlay's crown: the
 * highest object here is the identity line, 226 px above the centre.
 *
 * The hero seconds arrive through p->alt.elapsed_hook, which the overlay calls
 * after writing its own digits, so both heroes come from one writer and one
 * clock. This file never touches p->alt.elapsed_cb, p->alt.arc_progress or
 * p->alt.cap_img.
 *
 * Geometry is centre relative: every vertical position is an offset from
 * screen_center() taken from the 720 px board, and every row width comes from
 * ui_chord_half() at that offset, so the 800 px panel gets the same composition
 * with wider rows and a larger black margin.
 *
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_layout_alt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "time_parse.h"
#include "ui_round.h"
#include "ui_text_fit.h"

LV_FONT_DECLARE(lv_font_hanken_bold_64);

/* ---- design tokens ------------------------------------------------------ */

/* Rows as offsets from the panel centre, from the board (centre y 360 on a
 * 720 disc). Nothing here is a fraction of a panel width. */
#define RGR_DY_IDENT   (-226)
#define RGR_DY_SEQ     (-185)
#define RGR_DY_SEQCAP  (-160)
#define RGR_DY_RULE1   (-140)
#define RGR_DY_NAME    (-112)

/* The ring pair sits below the panel centre: the three name rows above it need
 * the wide part of the disc, and the crown owns the top of the axis. */
#define RGR_RING_DY      67
#define RGR_R_OUT       138   /* exposure ring, centre line */
#define RGR_W_OUT        12
#define RGR_R_SUB       115   /* sub block ring, centre line */
#define RGR_W_SUB         9

#define RGR_TRACK    0x161616
#define RGR_RULE_FG  0x262626
#define RGR_TARGET_FG 0xf2f2f4
#define RGR_SEQ_FG   0x4fc3f7

/* Rows inside the rings, offsets from the RING centre. The inner ring's clear
 * radius is 110 px, so every row here stays inside the chord at its own depth:
 * 177 px of clear width at the filter row, 134 px at the lowest caption. */
#define RGR_FILTER_REL (-65)
#define RGR_HERO_REL    (-4)
#define RGR_DONE_REL     55
#define RGR_DONEC_REL    79
#define RGR_INNER_W     190

/* The two arms, offsets from the panel centre. Each column runs from the disc
 * inward and stops RGR_ARM_GAP short of the exposure ring's widest point, so
 * the type can never touch the ring: 140 px wide at 720, 187 at 800. */
#define RGR_ARM_V1     (-15)
#define RGR_ARM_C1       18
#define RGR_ARM_R1       36
#define RGR_ARM_V2       69
#define RGR_ARM_C2      102
#define RGR_ARM_R2      120
#define RGR_ARM_V3      153
#define RGR_ARM_C3      186
#define RGR_ARM_EXT     194   /* deepest |dy| an arm reaches, its box included */
#define RGR_ARM_GAP       8
#define RGR_SIDE_PAD      4
#define RGR_ARM_RULE_PAD  8
#define RGR_ARM_MIN      90

/* Row insets, per side. */
#define RGR_ROW_PAD      24
#define RGR_RULE_PAD     30
#define RGR_PWR_PAD      16

/* Power chord: a hairline, then one cell per reading, value over caption. Six
 * cells is the ceiling the API can fill (amps, watts and up to four PWM
 * ports); the cells share the chord, so two readings get wide cells. */
#define RGR_PWR_CELLS     6
#define RGR_PWR_DY      250   /* strip centre */
#define RGR_PWR_H        60
#define RGR_PWR_ROW_Y    12
#define RGR_PWR_EXT     280   /* deepest |dy| the strip reaches */

/* The hero is the 64 px Hanken Grotesk Bold, which is full ASCII and tabular,
 * so the seconds and their "s" share one label and the digits do not walk. The
 * readings use Montserrat because an RMS figure needs a period and an inch
 * mark, which the Hanken subset faces do not carry. */
#define RGR_FONT_IDENT   (&lv_font_montserrat_24)
#define RGR_FONT_SEQ     (&lv_font_montserrat_24)
#define RGR_FONT_CAP     (&lv_font_montserrat_14)
#define RGR_FONT_FILTER  (&lv_font_montserrat_24)
#define RGR_FONT_COUNT   (&lv_font_montserrat_24)
#define RGR_FONT_HERO    (&lv_font_hanken_bold_64)
#define RGR_FONT_DONE    (&lv_font_montserrat_24)
#define RGR_FONT_ARM     (&lv_font_montserrat_36)
#define RGR_FONT_PWR     (&lv_font_montserrat_24)

/* p->alt.lbl_caption[] is a spare-handle array on this board, not a set of
 * captions: two of its slots hold widgets whose text changes with the data. */
enum {
    RGR_SLOT_LIMIT_CAP = 0,   /* the time-limit caption, which names the reason */
    RGR_SLOT_EXP_LEN   = 1,   /* the exposure LENGTH value on the east arm */
    RGR_SLOT_DONE      = 2,   /* the completed count inside the rings */
};

/* ---- forward declarations ----------------------------------------------- */

static bool      rgr_red(void);
static uint32_t  rgr_dim(uint32_t color, int gb);
static uint32_t  rgr_filter_color(const char *filter, int inst, int gb);
static int       rgr_row_w(int ext_dy, int pad);
static int       rgr_arm_x(void);
static int       rgr_arm_w(void);
static int       rgr_name_avail(void);
static void      rgr_show(lv_obj_t *obj, bool show);
static void      rgr_set_text(lv_obj_t *lbl, const char *text);
static void      rgr_set_color(lv_obj_t *obj, uint32_t rgb);
static lv_obj_t *rgr_label(lv_obj_t *parent, const lv_font_t *font,
                           const char *text, lv_text_align_t align);
static lv_obj_t *rgr_box(lv_obj_t *parent, int32_t w, int32_t h, int dy);
static lv_obj_t *rgr_rule(lv_obj_t *parent, int w);
static lv_obj_t *rgr_arm_value(lv_obj_t *parent, bool west, int dy);
static lv_obj_t *rgr_arm_caption(lv_obj_t *parent, bool west, int dy,
                                 const char *text);
static void      rgr_arm_rule(lv_obj_t *parent, bool west, int dy);
static void      rgr_upper(char *buf);
static void      rgr_power_build(dashboard_page_t *p, lv_obj_t *parent);
static void      rgr_power_slot(lv_obj_t *row, int idx, const char *title,
                                const char *value);
static void      rgr_power_write(dashboard_page_t *p, const nina_client_t *d);
static void      rgr_elapsed_hook(dashboard_page_t *p, int secs);
static void      rgr_theme_page(dashboard_page_t *p, int gb);

/* ---- small helpers ------------------------------------------------------ */

static bool rgr_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t rgr_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

/* Filter tone: the configured filter colour, theme text on Red Night, label
 * tone when no filter is known. Already brightness applied. */
static uint32_t rgr_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) return rgr_dim(0x808080, gb);
    if (rgr_red()) return rgr_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return rgr_dim(current_theme->label_color, gb);
}

/* Width of a full-width row measured at the deepest offset its box reaches,
 * which is the narrowest chord it spans, less the row's own inset per side. */
static int rgr_row_w(int ext_dy, int pad)
{
    const int w = 2 * ui_chord_half(ext_dy) - 2 * pad;
    return (w > RGR_ARM_MIN) ? w : RGR_ARM_MIN;
}

/* Left edge of the west arm: the disc at the arm's deepest row, plus the
 * shared side inset. The east arm mirrors it from the other edge. */
static int rgr_arm_x(void)
{
    const int x = screen_center() - ui_chord_half(RGR_ARM_EXT) + RGR_SIDE_PAD;
    return (x > 0) ? x : RGR_SIDE_PAD;
}

/* Arm width: from that edge to RGR_ARM_GAP short of the exposure ring's widest
 * point. Floored, so a hypothetical tiny panel still yields a usable box. */
static int rgr_arm_w(void)
{
    const int w = screen_center() - RGR_R_OUT - RGR_W_OUT / 2 - RGR_ARM_GAP
                  - rgr_arm_x();
    return (w > RGR_ARM_MIN) ? w : RGR_ARM_MIN;
}

/* The name row's fitting width. One function so create() and update() cannot
 * drift: update() re-fits on every new target name and must use the same box.
 * Measured at the row's lower edge, the narrowest chord the box spans. */
static int rgr_name_avail(void)
{
    return rgr_row_w(RGR_DY_NAME - 22, RGR_ROW_PAD);
}

static void rgr_show(lv_obj_t *obj, bool show)
{
    if (!obj) return;
    if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Set-if-changed text for a label that can NEVER ellipsise: either it has no
 * bounded width or its longest real string fits the box it was given. Those
 * labels do not need ui_label_set_text()'s shadow copy, and the shadow table
 * holds 64 entries for the whole binary, so leaving them out keeps room for
 * the rows that really can end in dots (the names, the countdowns, the power
 * cells), where a plain compare would stop matching and repaint every poll. */
static void rgr_set_text(lv_obj_t *lbl, const char *text)
{
    if (!lbl || !text) return;
    if (strcmp(lv_label_get_text(lbl), text) == 0) return;
    lv_label_set_text(lbl, text);
}

/* Set-if-changed text colour. LVGL invalidates on ANY style write and this
 * panel is full refresh, so an unguarded per-poll recolour repaints the whole
 * screen even when nothing moved. Every colour write on this board, the theme
 * pass included, goes through here. */
static void rgr_set_color(lv_obj_t *obj, uint32_t rgb)
{
    if (!obj) return;
    const lv_color_t c = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), c)) return;
    lv_obj_set_style_text_color(obj, c, 0);
}

static lv_obj_t *rgr_label(lv_obj_t *parent, const lv_font_t *font,
                           const char *text, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* Transparent, non-clickable container centred at @p dy, so a tap that is not
 * on one of this page's own reading labels falls through to the capture and
 * cycles the view. */
static lv_obj_t *rgr_box(lv_obj_t *parent, int32_t w, int32_t h, int dy)
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

/* One hairline. Grouping on this board is proximity plus a 1 px rule; there is
 * no box, plate or fill anywhere on it. */
static lv_obj_t *rgr_rule(lv_obj_t *parent, int w)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, w, 1);
    lv_obj_set_style_bg_color(r, lv_color_hex(RGR_RULE_FG), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    return r;
}

static lv_obj_t *rgr_arm_value(lv_obj_t *parent, bool west, int dy)
{
    lv_obj_t *l = rgr_label(parent, RGR_FONT_ARM, "--", LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(l, rgr_arm_w(), lv_font_get_line_height(RGR_FONT_ARM));
    lv_obj_align(l, west ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 west ? rgr_arm_x() : -rgr_arm_x(), dy);
    return l;
}

static lv_obj_t *rgr_arm_caption(lv_obj_t *parent, bool west, int dy,
                                 const char *text)
{
    lv_obj_t *l = rgr_label(parent, RGR_FONT_CAP, text, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(l, rgr_arm_w(), lv_font_get_line_height(RGR_FONT_CAP));
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, west ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 west ? rgr_arm_x() : -rgr_arm_x(), dy);
    return l;
}

static void rgr_arm_rule(lv_obj_t *parent, bool west, int dy)
{
    lv_obj_t *r = rgr_rule(parent, rgr_arm_w() - 2 * RGR_ARM_RULE_PAD);
    lv_obj_align(r, west ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 west ? (rgr_arm_x() + RGR_ARM_RULE_PAD)
                      : -(rgr_arm_x() + RGR_ARM_RULE_PAD), dy);
}

static void rgr_upper(char *buf)
{
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);
    }
}

/* ---- power chord -------------------------------------------------------- */

/* The strip is one container so the whole thing hides with a single flag when
 * the power switch is not connected: child 0 is the hairline, child 1 is the
 * cell row, and each cell is a value over its caption. */
static void rgr_power_build(dashboard_page_t *p, lv_obj_t *parent)
{
    const int w = rgr_row_w(RGR_PWR_EXT, RGR_PWR_PAD);

    p->alt.grp_bottom = rgr_box(parent, w, RGR_PWR_H, RGR_PWR_DY);
    lv_obj_set_layout(p->alt.grp_bottom, LV_LAYOUT_NONE);
    nina_dashboard_bind_tap(p->alt.grp_bottom, NINA_TAP_POWER);

    lv_obj_t *rule = rgr_rule(p->alt.grp_bottom, w);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *row = lv_obj_create(p->alt.grp_bottom);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, w, RGR_PWR_H - RGR_PWR_ROW_Y);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, RGR_PWR_ROW_Y);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    for (int i = 0; i < RGR_PWR_CELLS; i++) {
        lv_obj_t *cell = lv_obj_create(row);
        lv_obj_remove_style_all(cell);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(cell, w / RGR_PWR_CELLS, LV_PCT(100));
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_pad_gap(cell, 1, 0);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *val = rgr_label(cell, RGR_FONT_PWR, "--", LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(val, LV_PCT(100), lv_font_get_line_height(RGR_FONT_PWR));
        lv_obj_t *cap = rgr_label(cell, RGR_FONT_CAP, "", LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(cap, LV_PCT(100), lv_font_get_line_height(RGR_FONT_CAP));
        lv_obj_set_style_text_letter_space(cap, 2, 0);

        lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(p->alt.grp_bottom, LV_OBJ_FLAG_HIDDEN);
}

static void rgr_power_slot(lv_obj_t *row, int idx, const char *title,
                           const char *value)
{
    lv_obj_t *cell = lv_obj_get_child(row, idx);
    if (!cell) return;
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_HIDDEN);
    ui_label_set_text(lv_obj_get_child(cell, 0), value);
    ui_label_set_text(lv_obj_get_child(cell, 1), title);
}

static void rgr_power_write(dashboard_page_t *p, const nina_client_t *d)
{
    lv_obj_t *strip = p->alt.grp_bottom;
    if (!strip) return;
    if (!d->power.switch_connected) {
        rgr_show(strip, false);
        return;
    }
    lv_obj_t *row = lv_obj_get_child(strip, 1);
    if (!row) return;
    rgr_show(strip, true);

    char title[40];
    char value[24];
    int n = 0;

    strncpy(title, d->power.amps_name[0] ? d->power.amps_name : "Amps",
            sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    rgr_upper(title);
    snprintf(value, sizeof(value), "%.2fA", (double)d->power.total_amps);
    rgr_power_slot(row, n++, title, value);

    strncpy(title, d->power.watts_name[0] ? d->power.watts_name : "Watts",
            sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    rgr_upper(title);
    snprintf(value, sizeof(value), "%.1fW", (double)d->power.total_watts);
    rgr_power_slot(row, n++, title, value);

    int ports = d->power.pwm_count;
    if (ports > 4) ports = 4;
    for (int i = 0; i < ports && n < RGR_PWR_CELLS; i++) {
        strncpy(title, d->power.pwm_names[i], sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        rgr_upper(title);
        snprintf(value, sizeof(value), "%.0f%%", (double)d->power.pwm[i]);
        rgr_power_slot(row, n++, title, value);
    }

    for (int i = n; i < RGR_PWR_CELLS; i++) {
        lv_obj_t *cell = lv_obj_get_child(row, i);
        if (cell) lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
    }

    /* Share the chord between however many readings the rig actually has, so
     * two readings get wide cells and six get narrow ones. The count is parked
     * on the row's user data, which nothing else on this page uses, so the
     * widths are only rewritten when the count really changed. */
    const int prev = (int)(intptr_t)lv_obj_get_user_data(row);
    if (prev != n && n > 0) {
        lv_obj_set_user_data(row, (void *)(intptr_t)n);
        const int cell_w = rgr_row_w(RGR_PWR_EXT, RGR_PWR_PAD) / n;
        for (int i = 0; i < RGR_PWR_CELLS; i++) {
            lv_obj_t *cell = lv_obj_get_child(row, i);
            if (cell) lv_obj_set_width(cell, cell_w);
        }
    }
}

/* ---- the elapsed sink --------------------------------------------------- */

/* The hero digits' only writer. The overlay owns p->alt.elapsed_cb, writes its
 * own digits from the 200 ms tick and then calls this with the same value, so
 * the two heroes can never disagree. The 64 px face is full ASCII, so the
 * seconds and their unit share one label and the idle marker needs no second
 * one. */
static void rgr_elapsed_hook(dashboard_page_t *p, int secs)
{
    if (!p || !p->alt.lbl_hero) return;
    char buf[16];
    if (secs < 0) {
        snprintf(buf, sizeof(buf), "--s");
    } else {
        if (secs > 9999) secs = 9999;
        snprintf(buf, sizeof(buf), "%ds", secs);
    }
    rgr_set_text(p->alt.lbl_hero, buf);
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_rings_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent) return;

    p->alt.inst = page_index;
    const int gb = app_config_get()->color_brightness;

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* The capture belongs to the spine and the crown, the rim arc and the plate
     * belong to nina_round_overlay.c. Nothing here creates or touches them. */

    /* 1: the outer ring, this page's exposure progress. Storing it in
     * arc_progress_num is what makes the overlay drop its rim arc here; the
     * spine drives its value over 0..1000 and dims it while data is stale, so
     * this file only sets geometry, track and tone. It sits well inside the
     * rim, so it needs no crown gap. */
    {
        const int side = 2 * RGR_R_OUT + RGR_W_OUT;
        p->alt.arc_progress_num = lv_arc_create(parent);
        lv_obj_set_size(p->alt.arc_progress_num, side, side);
        lv_obj_align(p->alt.arc_progress_num, LV_ALIGN_CENTER, 0, RGR_RING_DY);
        lv_arc_set_rotation(p->alt.arc_progress_num, 270);
        lv_arc_set_bg_angles(p->alt.arc_progress_num, 0, 360);
        lv_arc_set_range(p->alt.arc_progress_num, 0, 1000);
        lv_arc_set_value(p->alt.arc_progress_num, 0);
        lv_obj_remove_style(p->alt.arc_progress_num, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(p->alt.arc_progress_num, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(p->alt.arc_progress_num, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_width(p->alt.arc_progress_num, RGR_W_OUT, LV_PART_MAIN);
        lv_obj_set_style_arc_width(p->alt.arc_progress_num, RGR_W_OUT, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(p->alt.arc_progress_num, false, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(p->alt.arc_progress_num, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(p->alt.arc_progress_num,
                                   lv_color_hex(RGR_TRACK), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(p->alt.arc_progress_num, 0, LV_PART_INDICATOR);
    }

    /* 2: the inner ring, one block per sub. create_ring centres its container
     * on the parent, so it is re-aligned onto the exposure ring's centre. The
     * ring is kept for a one-sub target, the same way Orbit keeps its own. */
    nina_subbar_create_ring(&p->subbar, parent, RGR_R_SUB, RGR_W_SUB, 0);
    if (p->subbar.cont) {
        lv_obj_align(p->subbar.cont, LV_ALIGN_CENTER, 0, RGR_RING_DY);
    }
    p->alt.ring_inner = p->subbar.cont;

    /* The overlay calls this after writing its own digits. */
    p->alt.elapsed_hook = rgr_elapsed_hook;

    /* 3: every text of this page in one transparent full-panel group, so the
     * view switch is a single flag. Not clickable, so a tap between the labels
     * falls through to the capture the spine bound the cycle on. */
    p->alt.grp_mid = rgr_box(parent, screen_size(), screen_size(), 0);
    lv_obj_set_layout(p->alt.grp_mid, LV_LAYOUT_NONE);

    /* 4: the three name rows. The identity line's tone is the spine's, not this
     * file's: nina_dashboard_update_status() paints it green while the rig is
     * connected and red while it is not, on every layout. */
    p->lbl_instance_name = rgr_label(p->alt.grp_mid, RGR_FONT_IDENT, "N.I.N.A.",
                                     LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(p->lbl_instance_name, rgr_row_w(240, RGR_ROW_PAD),
                    lv_font_get_line_height(RGR_FONT_IDENT));
    lv_obj_align(p->lbl_instance_name, LV_ALIGN_CENTER, 0, RGR_DY_IDENT);

    {
        const int row_w = rgr_row_w(200, RGR_ROW_PAD);
        const int col_w = row_w / 2 - 12;
        const int col_x = row_w / 2;

        p->lbl_seq_container = rgr_label(p->alt.grp_mid, RGR_FONT_SEQ, "----",
                                         LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(p->lbl_seq_container, col_w,
                        lv_font_get_line_height(RGR_FONT_SEQ));
        lv_obj_align(p->lbl_seq_container, LV_ALIGN_CENTER, -col_x + col_w / 2,
                     RGR_DY_SEQ);
        nina_dashboard_bind_tap(p->lbl_seq_container, NINA_TAP_SEQUENCE);

        lv_obj_t *cap_seq = rgr_label(p->alt.grp_mid, RGR_FONT_CAP, "sequence",
                                      LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(cap_seq, col_w, lv_font_get_line_height(RGR_FONT_CAP));
        lv_obj_set_style_text_letter_space(cap_seq, 2, 0);
        lv_obj_align(cap_seq, LV_ALIGN_CENTER, -col_x + col_w / 2, RGR_DY_SEQCAP);

        p->alt.lbl_seq_step = rgr_label(p->alt.grp_mid, RGR_FONT_SEQ, "----",
                                        LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(p->alt.lbl_seq_step, col_w,
                        lv_font_get_line_height(RGR_FONT_SEQ));
        lv_obj_align(p->alt.lbl_seq_step, LV_ALIGN_CENTER, col_x - col_w / 2,
                     RGR_DY_SEQ);
        nina_dashboard_bind_tap(p->alt.lbl_seq_step, NINA_TAP_SEQUENCE);

        lv_obj_t *cap_step = rgr_label(p->alt.grp_mid, RGR_FONT_CAP, "step",
                                       LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(cap_step, col_w, lv_font_get_line_height(RGR_FONT_CAP));
        lv_obj_set_style_text_letter_space(cap_step, 2, 0);
        lv_obj_align(cap_step, LV_ALIGN_CENTER, col_x - col_w / 2, RGR_DY_SEQCAP);
    }

    {
        lv_obj_t *rule1 = rgr_rule(p->alt.grp_mid, rgr_row_w(140, RGR_RULE_PAD));
        lv_obj_align(rule1, LV_ALIGN_CENTER, 0, RGR_DY_RULE1);
    }

    p->alt.lbl_target = rgr_label(p->alt.grp_mid, UI_FIT_LADDER_NAME[0], "--",
                                  LV_TEXT_ALIGN_CENTER);
    lv_obj_align(p->alt.lbl_target, LV_ALIGN_CENTER, 0, RGR_DY_NAME);
    ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N,
                 rgr_name_avail());
    nina_dashboard_bind_tap(p->alt.lbl_target, NINA_TAP_CAPTURE);

    /* 5: what lives inside the rings. */
    {
        lv_obj_t *frow = rgr_box(p->alt.grp_mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT,
                                 RGR_RING_DY + RGR_FILTER_REL);
        lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(frow, 8, 0);
        nina_dashboard_bind_tap(frow, NINA_TAP_FILTER);

        p->alt.lbl_filter = rgr_label(frow, RGR_FONT_FILTER, "--",
                                      LV_TEXT_ALIGN_CENTER);
        p->alt.lbl_count = rgr_label(frow, RGR_FONT_COUNT, "",
                                     LV_TEXT_ALIGN_CENTER);

        p->alt.lbl_hero = rgr_label(p->alt.grp_mid, RGR_FONT_HERO, "--s",
                                    LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(p->alt.lbl_hero, RGR_INNER_W);
        /* One line only: with a height LONG_DOT dots a 9999s value instead of
         * wrapping it onto a second line inside the ring. */
        lv_obj_set_height(p->alt.lbl_hero, lv_font_get_line_height(RGR_FONT_HERO));
        lv_obj_align(p->alt.lbl_hero, LV_ALIGN_CENTER, 0,
                     RGR_RING_DY + RGR_HERO_REL);
        nina_dashboard_bind_tap(p->alt.lbl_hero, NINA_TAP_EXPOSURE);

        p->alt.lbl_caption[RGR_SLOT_DONE] =
            rgr_label(p->alt.grp_mid, RGR_FONT_DONE, "--", LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(p->alt.lbl_caption[RGR_SLOT_DONE], RGR_INNER_W,
                        lv_font_get_line_height(RGR_FONT_DONE));
        lv_obj_align(p->alt.lbl_caption[RGR_SLOT_DONE], LV_ALIGN_CENTER, 0,
                     RGR_RING_DY + RGR_DONE_REL);
        nina_dashboard_bind_tap(p->alt.lbl_caption[RGR_SLOT_DONE],
                                NINA_TAP_SEQUENCE);

        lv_obj_t *cap_done = rgr_label(p->alt.grp_mid, RGR_FONT_CAP, "completed",
                                       LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(cap_done, RGR_INNER_W,
                        lv_font_get_line_height(RGR_FONT_CAP));
        lv_obj_set_style_text_letter_space(cap_done, 2, 0);
        lv_obj_align(cap_done, LV_ALIGN_CENTER, 0, RGR_RING_DY + RGR_DONEC_REL);
    }

    /* 6: the west arm, guiding and stars. */
    p->alt.lbl_rms = rgr_arm_value(p->alt.grp_mid, true, RGR_ARM_V1);
    nina_dashboard_bind_tap(p->alt.lbl_rms, NINA_TAP_RMS);
    rgr_arm_caption(p->alt.grp_mid, true, RGR_ARM_C1, "rms");
    rgr_arm_rule(p->alt.grp_mid, true, RGR_ARM_R1);

    p->alt.lbl_hfr = rgr_arm_value(p->alt.grp_mid, true, RGR_ARM_V2);
    nina_dashboard_bind_tap(p->alt.lbl_hfr, NINA_TAP_HFR);
    rgr_arm_caption(p->alt.grp_mid, true, RGR_ARM_C2, "hfr");
    rgr_arm_rule(p->alt.grp_mid, true, RGR_ARM_R2);

    p->alt.lbl_stars = rgr_arm_value(p->alt.grp_mid, true, RGR_ARM_V3);
    nina_dashboard_bind_tap(p->alt.lbl_stars, NINA_TAP_STARS);
    rgr_arm_caption(p->alt.grp_mid, true, RGR_ARM_C3, "stars");

    /* 7: the east arm, the frame length and the two countdowns. */
    p->alt.lbl_caption[RGR_SLOT_EXP_LEN] =
        rgr_arm_value(p->alt.grp_mid, false, RGR_ARM_V1);
    nina_dashboard_bind_tap(p->alt.lbl_caption[RGR_SLOT_EXP_LEN],
                            NINA_TAP_EXPOSURE);
    rgr_arm_caption(p->alt.grp_mid, false, RGR_ARM_C1, "exposure");
    rgr_arm_rule(p->alt.grp_mid, false, RGR_ARM_R1);

    p->alt.lbl_flip = rgr_arm_value(p->alt.grp_mid, false, RGR_ARM_V2);
    nina_dashboard_bind_tap(p->alt.lbl_flip, NINA_TAP_FLIP);
    rgr_arm_caption(p->alt.grp_mid, false, RGR_ARM_C2, "flip in");
    rgr_arm_rule(p->alt.grp_mid, false, RGR_ARM_R2);

    p->alt.lbl_limit = rgr_arm_value(p->alt.grp_mid, false, RGR_ARM_V3);
    nina_dashboard_bind_tap(p->alt.lbl_limit, NINA_TAP_SESSION);
    p->alt.lbl_caption[RGR_SLOT_LIMIT_CAP] =
        rgr_arm_caption(p->alt.grp_mid, false, RGR_ARM_C3, "time limit");

    /* 8: the power readings on a low chord. */
    rgr_power_build(p, p->alt.grp_mid);

    rgr_theme_page(p, gb);
}

/* ---- theme -------------------------------------------------------------- */

static void rgr_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !p->alt.grp_mid || !current_theme) return;

    const bool     red   = rgr_red();
    const uint32_t text  = rgr_dim(current_theme->text_color, gb);
    const uint32_t label = rgr_dim(current_theme->label_color, gb);
    const uint32_t prog  = rgr_dim(red ? current_theme->text_color
                                       : current_theme->progress_color, gb);

    if (p->alt.arc_progress_num) {
        const lv_color_t c = lv_color_hex(prog);
        if (!lv_color_eq(lv_obj_get_style_arc_color(p->alt.arc_progress_num,
                                                    LV_PART_INDICATOR), c)) {
            lv_obj_set_style_arc_color(p->alt.arc_progress_num, c,
                                       LV_PART_INDICATOR);
        }
    }
    nina_subbar_apply_theme(&p->subbar);

    /* The fixed captions carry no local colour, so this one write on their
     * group re-tones all of them; every other child sets its own colour and is
     * unaffected. */
    rgr_set_color(p->alt.grp_mid, label);

    rgr_set_color(p->alt.lbl_target,
                  rgr_dim(red ? current_theme->text_color : RGR_TARGET_FG, gb));
    rgr_set_color(p->lbl_seq_container,
                  rgr_dim(red ? current_theme->header_text_color : RGR_SEQ_FG, gb));
    rgr_set_color(p->alt.lbl_seq_step, text);
    rgr_set_color(p->alt.lbl_hero, text);
    rgr_set_color(p->alt.lbl_count, text);
    rgr_set_color(p->alt.lbl_stars, text);
    rgr_set_color(p->alt.lbl_flip, text);
    rgr_set_color(p->alt.lbl_limit, text);
    rgr_set_color(p->alt.lbl_filter,
                  rgr_filter_color(p->subbar.cached_filter, p->alt.inst, gb));
    rgr_set_color(p->alt.lbl_caption[RGR_SLOT_EXP_LEN],
                  rgr_filter_color(p->subbar.cached_filter, p->alt.inst, gb));
    rgr_set_color(p->alt.lbl_caption[RGR_SLOT_DONE],
                  rgr_filter_color(p->subbar.cached_filter, p->alt.inst, gb));

    /* The RMS and HFR tones follow the live value; the next update() repaints
     * them from the configured thresholds, so this is only a resting colour. */
    rgr_set_color(p->alt.lbl_rms,
                  rgr_dim(red ? current_theme->rms_color
                              : current_theme->label_color, gb));
    rgr_set_color(p->alt.lbl_hfr,
                  rgr_dim(red ? current_theme->hfr_color
                              : current_theme->label_color, gb));

    if (p->alt.grp_bottom) {
        lv_obj_t *row = lv_obj_get_child(p->alt.grp_bottom, 1);
        for (int i = 0; row && i < RGR_PWR_CELLS; i++) {
            lv_obj_t *cell = lv_obj_get_child(row, i);
            if (cell) {
                rgr_set_color(lv_obj_get_child(cell, 0), text);
            }
        }
    }
}

void nina_layout_rings_apply_theme(dashboard_page_t *p)
{
    if (!p) return;
    /* Drops a retained capture that was remapped for the other Red Night
     * state. The overlay does not do this, so the layout still must. */
    nina_layout_image_note_theme_switch(p->alt.inst);
    rgr_theme_page(p, app_config_get()->color_brightness);
}

/* ---- view mode ---------------------------------------------------------- */

/* This page IS the readings-only composition, so it shows in NUMBERS and hides
 * in every other mode. Hidden flags only, on objects create() already built:
 * idempotent, and it never touches p->alt.cap_img, which the spine owns and
 * which has to stay visible to the input system in every mode. */
void nina_layout_rings_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    if (!p) return;

    const bool on = (mode == NINA_VIEW_NUMBERS);

    /* The sub ring's flag has one writer, the sub bar, which also hides it
     * when the container plans a single image. */
    nina_subbar_set_shown(&p->subbar, on);
    rgr_show(p->alt.arc_progress_num, on);
    rgr_show(p->alt.grp_mid, on);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_rings_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb)
{
    if (!p || !d || !p->alt.grp_mid || !current_theme) return;

    p->alt.inst = instance_idx;
    const bool red = rgr_red();

    /* Rig identity. The spine paints its tone from the connection state. */
    if (p->lbl_instance_name) {
        char buf[132];
        if (d->telescope_name[0] != '\0' && d->camera_name[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s | %s", d->telescope_name, d->camera_name);
        } else if (d->telescope_name[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s", d->telescope_name);
        } else if (d->camera_name[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s", d->camera_name);
        } else {
            snprintf(buf, sizeof(buf), "N.I.N.A.");
        }
        ui_label_set_text(p->lbl_instance_name, buf);
    }

    /* Target name, refitted to the same box create() used whenever it moves. */
    if (p->alt.lbl_target) {
        ui_label_set_text(p->alt.lbl_target,
            (d->target_name[0] != '\0') ? d->target_name : "----");
        ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N,
                     rgr_name_avail());
    }

    /* Sequence container and running step. */
    ui_label_set_text(p->lbl_seq_container,
        (d->container_name[0] != '\0') ? d->container_name : "----");
    ui_label_set_text(p->alt.lbl_seq_step,
        (d->container_step[0] != '\0') ? d->container_step : "----");

    /* Filter name and loop count, and the exposure ring in the same colour. */
    {
        const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
        const uint32_t fc = rgr_filter_color(filter, instance_idx, gb);

        rgr_set_text(p->alt.lbl_filter, filter);
        rgr_set_color(p->alt.lbl_filter, fc);

        char buf[32];
        if (d->exposure_iterations > 0) {
            snprintf(buf, sizeof(buf), "x %d / %d", d->exposure_count,
                     d->exposure_iterations);
        } else {
            snprintf(buf, sizeof(buf), "x %d", d->exposure_count);
        }
        rgr_set_text(p->alt.lbl_count, buf);

        if (p->alt.arc_progress_num) {
            const lv_color_t c = lv_color_hex(fc);
            if (!lv_color_eq(lv_obj_get_style_arc_color(p->alt.arc_progress_num,
                                                        LV_PART_INDICATOR), c)) {
                lv_obj_set_style_arc_color(p->alt.arc_progress_num, c,
                                           LV_PART_INDICATOR);
            }
        }
    }

    /* Completed count and the integration time it stands for. */
    {
        char buf[40];
        if (d->exposure_total_count > 0) {
            char dur[16];
            fmt_duration(dur, sizeof(dur),
                         (int32_t)(d->exposure_total_count * d->exposure_total),
                         FMT_DUR_HM_COMPACT);
            snprintf(buf, sizeof(buf), "%d / %s", d->exposure_total_count, dur);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        ui_label_set_text(p->alt.lbl_caption[RGR_SLOT_DONE], buf);
        rgr_set_color(p->alt.lbl_caption[RGR_SLOT_DONE],
                      rgr_filter_color(d->current_filter, instance_idx, gb));
    }

    /* The frame length on the east arm, in the filter colour. Clamped on the
     * float, because the cast of an out-of-range float to int is undefined. */
    {
        char buf[24];
        if (d->exposure_total > 0.0f) {
            float total = d->exposure_total;
            if (total > 99999.0f) total = 99999.0f;
            snprintf(buf, sizeof(buf), "%ds", (int)(total + 0.5f));
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        rgr_set_text(p->alt.lbl_caption[RGR_SLOT_EXP_LEN], buf);
        rgr_set_color(p->alt.lbl_caption[RGR_SLOT_EXP_LEN],
                      rgr_filter_color(d->current_filter, instance_idx, gb));
    }

    /* Guiding RMS, threshold tone. */
    {
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
        rgr_set_text(p->alt.lbl_rms, buf);
        rgr_set_color(p->alt.lbl_rms, rgr_dim(c, gb));
    }

    /* HFR, threshold tone. */
    {
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
        rgr_set_text(p->alt.lbl_hfr, buf);
        rgr_set_color(p->alt.lbl_hfr, rgr_dim(c, gb));
    }

    /* Star count. */
    {
        char buf[16];
        if (d->stars >= 0) {
            snprintf(buf, sizeof(buf), "%d", d->stars);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        rgr_set_text(p->alt.lbl_stars, buf);
    }

    /* Meridian flip. NINA sends "HH:MM", the word "FLIPPING", or nothing;
     * a countdown is reformatted, anything else passes through as it is. */
    {
        char buf[32];
        const char *mf = d->meridian_flip;
        int hh = 0, mm = 0;
        if (mf[0] == '\0' || strcmp(mf, "--") == 0) {
            snprintf(buf, sizeof(buf), "--");
        } else if (strcmp(mf, "FLIPPING") != 0 && sscanf(mf, "%d:%d", &hh, &mm) >= 2) {
            if (hh < 0) hh = 0;
            if (hh > 999) hh = 999;
            if (mm < 0) mm = 0;
            if (mm > 99) mm = 99;
            snprintf(buf, sizeof(buf), "%dh %02dm", hh, mm);
        } else {
            snprintf(buf, sizeof(buf), "%s", mf);
        }
        ui_label_set_text(p->alt.lbl_flip, buf);
    }

    /* Session limit and the condition that binds first. */
    {
        ui_label_set_text(p->alt.lbl_limit,
            (d->target_time_remaining[0] != '\0') ? d->target_time_remaining : "--");

        char cap[24];
        if (d->target_time_reason[0] != '\0') {
            if (d->target_condition_count > 1) {
                snprintf(cap, sizeof(cap), "%s+", d->target_time_reason);
            } else {
                snprintf(cap, sizeof(cap), "%s", d->target_time_reason);
            }
            rgr_upper(cap);
        } else {
            snprintf(cap, sizeof(cap), "TIME LIMIT");
        }
        ui_label_set_text(p->alt.lbl_caption[RGR_SLOT_LIMIT_CAP], cap);
    }

    /* The sub ring: the spine owns its progress and its stale dimming. */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* The power chord. */
    rgr_power_write(p, d);

    /* The hero digits arrive through p->alt.elapsed_hook from the overlay's
     * elapsed writer; only the idle reset lives here, routed through that same
     * single writer. */
    if (d->exposure_total <= 0.0f) rgr_elapsed_hook(p, -1);
}
