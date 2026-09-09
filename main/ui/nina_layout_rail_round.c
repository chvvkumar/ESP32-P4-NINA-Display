/**
 * @file nina_layout_rail_round.c
 * @brief NINA layout 5 (Night rail) on a ROUND panel: the readings-only page.
 *
 * Same picture page every round NINA design is: the spine builds the capture
 * and nina_round_overlay.h/.c draws the safety shield crown at twelve, the
 * exposure rim arc flush with the glass and the readings plate across the
 * bottom. This file owns ONLY Night rail's readings-only page, the one the
 * fourth tap state shows with the picture drawn fully transparent.
 *
 * That page is the board: the rail lies on the horizontal diameter and carries
 * the exposure fill, the saved subs sit as blocks directly under it, and the
 * text pairs stack toward the vertical middle from both caps:
 *
 *   identity   the scope and the camera, centred under the crown
 *   sequence   the running container and the running step
 *   name       the target, fitted to the chord at its own row
 *   readings   guiding RMS on the left, HFR on the right, hero seconds between
 *   rail       exposure fill on the diameter, sub blocks under it
 *   row 1      exposure length, meridian flip, session limit
 *   row 2      filter and loop count, completed and integration, star count
 *   strip      the power cells on a low chord
 *
 * The safety shield is NOT drawn here: the crown at twelve is the safety
 * state. The top 60 px of the vertical axis is left empty for it, so the
 * topmost object reaches y 84 on a 720 disc.
 *
 * Every coordinate is an offset from the panel centre and every row width comes
 * from the rim chord at that offset, so the 800 px panel gets the same
 * composition with wider rows.
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
#include "ui_helpers.h"
#include "ui_round.h"
#include "ui_text_fit.h"

LV_FONT_DECLARE(lv_font_hanken_black_96);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

/* Only faces already compiled. lv_font_hanken_black_96 carries digits and a
 * colon only, so the hero's unit "s" and its idle "--" live in the full ASCII
 * 28 px Hanken Bold label beside it. */
#define RAILR_FONT_IDENT   (&lv_font_montserrat_24)
#define RAILR_FONT_SEQ     (&lv_font_montserrat_24)
#define RAILR_FONT_CAP     (&lv_font_montserrat_14)
#define RAILR_FONT_STAT    (&lv_font_montserrat_48)
#define RAILR_FONT_HERO    (&lv_font_hanken_black_96)
#define RAILR_FONT_UNIT    (&lv_font_hanken_bold_28)
#define RAILR_FONT_ROW     (&lv_font_montserrat_26)
#define RAILR_FONT_PWR     (&lv_font_montserrat_24)

/* Row centres as offsets from the panel centre, from the board (centre y 360
 * on a 720 disc). */
#define RAILR_DY_IDENT    (-261)
#define RAILR_DY_SEQ      (-225)
#define RAILR_DY_NAME     (-159)
#define RAILR_DY_HERO      (-75)
#define RAILR_DY_STAT      (-62)
#define RAILR_DY_BAR          0
#define RAILR_DY_BLOCKS      29
#define RAILR_DY_ROW1        65
#define RAILR_DY_ROW2       113
#define RAILR_DY_PWR        213

/* Widest |dy| each row reaches, half its own box height included. Widths are
 * measured there, so no row can touch the rim. */
#define RAILR_EXT_IDENT     276
#define RAILR_EXT_SEQ       240
#define RAILR_EXT_NAME      180
#define RAILR_EXT_STAT       98
#define RAILR_EXT_BAR        11
#define RAILR_EXT_BLOCKS     36
#define RAILR_EXT_ROW1       80
#define RAILR_EXT_ROW2      128
#define RAILR_EXT_PWR       246

#define RAILR_PAD            20   /* per side, ordinary rows */
#define RAILR_NAME_PAD       24
#define RAILR_STAT_PAD       32
#define RAILR_BAR_PAD        24
#define RAILR_BLOCK_PAD      40
#define RAILR_PWR_PAD         8
#define RAILR_ROW_MIN       120   /* floor, so a tiny panel never yields 0 */

#define RAILR_STAT_W        170   /* RMS and HFR box, clear of the hero digits */
#define RAILR_H_BAR          22
#define RAILR_H_BLOCK        14
#define RAILR_H_PWR          66
#define RAILR_PWR_PAD_TOP     8
#define RAILR_HERO_GAP        6
#define RAILR_ITEM_GAP        6
#define RAILR_SEQ_GAP        20
#define RAILR_CAP_SPACE       2

#define RAILR_TRACK_COLOR  0x202020
#define RAILR_PWR_BG       0x161616
#define RAILR_PWR_DIV      0x2a2a2a
#define RAILR_TARGET_FG    0xf2f2f4
#define RAILR_IDENT_FG     0x2ecc71
#define RAILR_SEQ_FG       0x4fc3f7

#define RAILR_PWR_SLOTS       6

enum {
    RAILR_CAP_RMS = 0,
    RAILR_CAP_HFR,
    RAILR_CAP_EXPOSURE,
    RAILR_CAP_FLIP,
    RAILR_CAP_LIMIT,
    RAILR_CAP_FILTER,
    RAILR_CAP_DONE,
    RAILR_CAP_STARS,
    RAILR_CAP_N
};

/* Widgets with no home in dashboard_page_t::alt, kept per instance. create()
 * clears the slot before it fills it, so a page rebuild can never inherit a
 * deleted object, and every read is null checked. */
typedef struct {
    lv_obj_t *lbl_ident;
    lv_obj_t *lbl_container;
    lv_obj_t *lbl_exposure;
    lv_obj_t *cap[RAILR_CAP_N];
    lv_obj_t *pwr_strip;
    lv_obj_t *pwr_cap[RAILR_PWR_SLOTS];
    lv_obj_t *pwr_val[RAILR_PWR_SLOTS];
} railr_page_t;

static railr_page_t s_railr[MAX_NINA_INSTANCES];

/* ---- forward declarations ----------------------------------------------- */

static railr_page_t *railr_store(int inst);
static bool          railr_red(void);
static uint32_t      railr_dim(uint32_t color, int gb);
static uint32_t      railr_filter_color(const char *filter, int inst, int gb);
static int           railr_row_w(int ext_dy, int pad);
static lv_obj_t     *railr_box(lv_obj_t *parent, int32_t w, int32_t h,
                               int dx, int dy);
static lv_obj_t     *railr_label(lv_obj_t *parent, const lv_font_t *font,
                                 const char *text, lv_text_align_t align);
static lv_obj_t     *railr_cap_label(lv_obj_t *parent, const char *text);
static lv_obj_t     *railr_item(lv_obj_t *parent, int w, int dx, int dy,
                                lv_flex_align_t main_place, bool cap_first,
                                const char *cap_text, lv_obj_t **out_cap);
static void          railr_show(lv_obj_t *obj, bool show);
static void          railr_set_color(lv_obj_t *l, uint32_t rgb);
static void          railr_upper(char *dst, size_t sz, const char *src,
                                 const char *fallback);
static void          railr_elapsed_hook(dashboard_page_t *p, int secs);
static void          railr_theme_page(dashboard_page_t *p, int gb);

/* ---- helpers ------------------------------------------------------------ */

static railr_page_t *railr_store(int inst)
{
    if (inst < 0 || inst >= MAX_NINA_INSTANCES) return NULL;
    return &s_railr[inst];
}

static bool railr_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t railr_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

/* Filter tone: the configured filter colour, theme text on Red Night, label
 * tone when no filter is known. Already brightness applied. */
static uint32_t railr_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) return railr_dim(0x808080, gb);
    if (railr_red()) return railr_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return railr_dim(current_theme->label_color, gb);
}

/* Row width at the widest vertical offset the row reaches, inset by @p pad
 * each side, so no glyph runs into the rim. */
static int railr_row_w(int ext_dy, int pad)
{
    int w = 2 * ui_chord_half(ext_dy) - 2 * pad;
    return (w < RAILR_ROW_MIN) ? RAILR_ROW_MIN : w;
}

/* Transparent, non-clickable container centred at (@p dx, @p dy) from the
 * panel centre, so a tap that is not on one of this page's own reading labels
 * falls through to the capture and cycles the view. */
static lv_obj_t *railr_box(lv_obj_t *parent, int32_t w, int32_t h,
                           int dx, int dy)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_gap(o, 0, 0);
    return o;
}

static lv_obj_t *railr_label(lv_obj_t *parent, const lv_font_t *font,
                             const char *text, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static lv_obj_t *railr_cap_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = railr_label(parent, RAILR_FONT_CAP, text,
                              LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(l, RAILR_CAP_SPACE, 0);
    lv_obj_set_style_translate_y(l,
        RAILR_FONT_CAP->base_line - RAILR_FONT_ROW->base_line, 0);
    return l;
}

/* One reading of a three column row: a caption and a value on one baseline,
 * the pair placed at @p main_place inside a column @p w wide. Returns the
 * value label; the caption comes back through @p out_cap so update() can
 * rewrite it (the session limit names its own binding condition). */
static lv_obj_t *railr_item(lv_obj_t *parent, int w, int dx, int dy,
                            lv_flex_align_t main_place, bool cap_first,
                            const char *cap_text, lv_obj_t **out_cap)
{
    lv_obj_t *row = railr_box(parent, w,
                              lv_font_get_line_height(RAILR_FONT_ROW), dx, dy);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main_place, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(row, RAILR_ITEM_GAP, 0);

    lv_obj_t *cap = NULL;
    lv_obj_t *val = NULL;
    if (cap_first) {
        cap = railr_cap_label(row, cap_text);
        val = railr_label(row, RAILR_FONT_ROW, "--", LV_TEXT_ALIGN_LEFT);
    } else {
        val = railr_label(row, RAILR_FONT_ROW, "--", LV_TEXT_ALIGN_LEFT);
        cap = railr_cap_label(row, cap_text);
    }
    if (out_cap) *out_cap = cap;
    return val;
}

static void railr_show(lv_obj_t *obj, bool show)
{
    if (!obj) return;
    if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Set-if-changed text colour. LVGL invalidates unconditionally on a style
 * write and this panel is full refresh, so an unguarded per-poll recolour
 * repaints the whole screen every cycle even when nothing moved. The theme
 * path may write unguarded: it runs once per theme change. */
static void railr_set_color(lv_obj_t *l, uint32_t rgb)
{
    if (!l) return;
    const lv_color_t c = lv_color_hex(rgb);
    if (!lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(l, c, 0);
    }
}

/* Copy @p src (or @p fallback when it is empty) into @p dst, upper cased. */
static void railr_upper(char *dst, size_t sz, const char *src,
                        const char *fallback)
{
    const char *s = (src && src[0] != '\0') ? src : fallback;
    if (!s) s = "";
    size_t i = 0;
    for (; i + 1 < sz && s[i] != '\0'; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* The readings hero, fed from the overlay's own elapsed writer through
 * p->alt.elapsed_hook, so this page and the overlay's plate can never
 * disagree. -1 is idle: the 96 px face has no hyphen glyph, so the digits go
 * empty and the full ASCII unit label carries the "--". */
static void railr_elapsed_hook(dashboard_page_t *p, int secs)
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

void nina_layout_rail_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent || !current_theme) return;

    railr_page_t *w = railr_store(page_index);
    if (!w) return;
    memset(w, 0, sizeof(*w));

    const int gb = app_config_get()->color_brightness;
    p->alt.inst = page_index;

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* The capture belongs to the spine and the crown, the rim arc and the
     * plate belong to nina_round_overlay.c. Nothing here creates or touches
     * them, and no shield is drawn: the crown IS the safety state.
     *
     * The overlay owns p->alt.elapsed_cb; this page takes the hook it calls. */
    p->alt.elapsed_hook = railr_elapsed_hook;

    /* One full-panel transparent group holds every object this board builds,
     * so the whole page appears and disappears with a single flag. */
    p->alt.grp_mid = railr_box(parent, screen_size(), screen_size(), 0, 0);
    lv_obj_t *g = p->alt.grp_mid;

    /* 1: identity, the scope and the camera on one line under the crown. */
    w->lbl_ident = railr_label(g, RAILR_FONT_IDENT, "N.I.N.A.",
                               LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(w->lbl_ident, railr_row_w(RAILR_EXT_IDENT, RAILR_PAD),
                    lv_font_get_line_height(RAILR_FONT_IDENT));
    lv_obj_align(w->lbl_ident, LV_ALIGN_CENTER, 0, RAILR_DY_IDENT);

    /* 2: the sequence row, the running container beside the running step. */
    {
        const int row_w = railr_row_w(RAILR_EXT_SEQ, RAILR_PAD);
        const int col   = (row_w - RAILR_SEQ_GAP) / 2;
        lv_obj_t *row = railr_box(g, row_w,
                                  lv_font_get_line_height(RAILR_FONT_SEQ),
                                  0, RAILR_DY_SEQ);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, RAILR_SEQ_GAP, 0);
        nina_dashboard_bind_tap(row, NINA_TAP_SEQUENCE);

        w->lbl_container = railr_label(row, RAILR_FONT_SEQ, "----",
                                       LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(w->lbl_container, col,
                        lv_font_get_line_height(RAILR_FONT_SEQ));
        p->alt.lbl_seq_step = railr_label(row, RAILR_FONT_SEQ, "----",
                                          LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(p->alt.lbl_seq_step, col,
                        lv_font_get_line_height(RAILR_FONT_SEQ));
    }

    /* 3: the target name, fitted to the chord at its own row. */
    p->alt.lbl_target = railr_label(g, &lv_font_montserrat_34, "----",
                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_align(p->alt.lbl_target, LV_ALIGN_CENTER, 0, RAILR_DY_NAME);
    ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N,
                 railr_row_w(RAILR_EXT_NAME, RAILR_NAME_PAD));
    nina_dashboard_bind_tap(p->alt.lbl_target, NINA_TAP_CAPTURE);

    /* 4: guiding RMS on the left and HFR on the right, one caption above each
     * figure, flanking the hero. Both boxes hug the chord at their own row, so
     * the wider panel pushes them outward and the hero keeps the middle. */
    {
        const int half = ui_chord_half(RAILR_EXT_STAT);
        const int dx   = half - RAILR_STAT_PAD - RAILR_STAT_W / 2;
        const int h    = lv_font_get_line_height(RAILR_FONT_CAP) + 2
                       + lv_font_get_line_height(RAILR_FONT_STAT);

        lv_obj_t *box = railr_box(g, RAILR_STAT_W, h, -dx, RAILR_DY_STAT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(box, 2, 0);
        nina_dashboard_bind_tap(box, NINA_TAP_RMS);
        w->cap[RAILR_CAP_RMS] = railr_label(box, RAILR_FONT_CAP, "RMS",
                                            LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_text_letter_space(w->cap[RAILR_CAP_RMS],
                                           RAILR_CAP_SPACE, 0);
        p->alt.lbl_rms = railr_label(box, RAILR_FONT_STAT, "--",
                                     LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(p->alt.lbl_rms, RAILR_STAT_W,
                        lv_font_get_line_height(RAILR_FONT_STAT));

        box = railr_box(g, RAILR_STAT_W, h, dx, RAILR_DY_STAT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(box, 2, 0);
        nina_dashboard_bind_tap(box, NINA_TAP_HFR);
        w->cap[RAILR_CAP_HFR] = railr_label(box, RAILR_FONT_CAP, "HFR",
                                            LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_style_text_letter_space(w->cap[RAILR_CAP_HFR],
                                           RAILR_CAP_SPACE, 0);
        p->alt.lbl_hfr = railr_label(box, RAILR_FONT_STAT, "--",
                                     LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(p->alt.lbl_hfr, RAILR_STAT_W,
                        lv_font_get_line_height(RAILR_FONT_STAT));
    }

    /* 5: the hero, interpolated exposure seconds, between the two figures. */
    {
        lv_obj_t *row = railr_box(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT, 0,
                                  RAILR_DY_HERO);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(row, RAILR_HERO_GAP, 0);

        p->alt.lbl_hero = railr_label(row, RAILR_FONT_HERO, "",
                                      LV_TEXT_ALIGN_CENTER);
        p->alt.lbl_hero_unit = railr_label(row, RAILR_FONT_UNIT, "--",
                                           LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_translate_y(p->alt.lbl_hero_unit,
            RAILR_FONT_UNIT->base_line - RAILR_FONT_HERO->base_line, 0);
    }

    /* 6: the rail on the horizontal diameter. The spine's 200 ms tick owns its
     * value (0..1000) and the stale rule owns its indicator opacity, so
     * neither is written again outside create(). */
    {
        lv_obj_t *bar = lv_bar_create(g);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, railr_row_w(RAILR_EXT_BAR, RAILR_BAR_PAD),
                        RAILR_H_BAR);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, RAILR_DY_BAR);
        lv_bar_set_range(bar, 0, 1000);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(RAILR_TRACK_COLOR),
                                  LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        p->alt.bar_progress = bar;
    }

    /* 7: the saved subs, one block each, directly under the rail. The flex row
     * form, not the ring: the ring belongs to the boards that draw one. */
    {
        lv_obj_t *host = railr_box(g, railr_row_w(RAILR_EXT_BLOCKS,
                                                  RAILR_BLOCK_PAD),
                                   RAILR_H_BLOCK, 0, RAILR_DY_BLOCKS);
        nina_subbar_create(&p->subbar, host, RAILR_H_BLOCK);
        /* No nina_subbar_set_elapsed_cb here: the overlay's elapsed_cb, and
         * the hook it calls above, are this page's single elapsed sink. */
    }

    /* 8: row 1, the exposure length, the meridian flip and the session
     * limit. */
    {
        const int row_w = railr_row_w(RAILR_EXT_ROW1, RAILR_PAD);
        const int col   = row_w / 3;
        const int dx    = col;
        w->lbl_exposure = railr_item(g, col, -dx, RAILR_DY_ROW1,
                                     LV_FLEX_ALIGN_START, true, "EXPOSURE",
                                     &w->cap[RAILR_CAP_EXPOSURE]);
        p->alt.lbl_flip = railr_item(g, col, 0, RAILR_DY_ROW1,
                                     LV_FLEX_ALIGN_CENTER, true, "FLIP",
                                     &w->cap[RAILR_CAP_FLIP]);
        p->alt.lbl_limit = railr_item(g, col, dx, RAILR_DY_ROW1,
                                      LV_FLEX_ALIGN_END, true, "TIME LIMIT",
                                      &w->cap[RAILR_CAP_LIMIT]);
        nina_dashboard_bind_tap(lv_obj_get_parent(p->alt.lbl_flip),
                                NINA_TAP_FLIP);
        nina_dashboard_bind_tap(lv_obj_get_parent(p->alt.lbl_limit),
                                NINA_TAP_SESSION);
    }

    /* 9: row 2, the filter with its loop count, the completed count with the
     * integration time it stands for, and the star count. */
    {
        const int row_w = railr_row_w(RAILR_EXT_ROW2, RAILR_PAD);
        const int col   = row_w / 3;
        const int dx    = col;
        p->alt.lbl_filter = railr_item(g, col, -dx, RAILR_DY_ROW2,
                                       LV_FLEX_ALIGN_START, false, "FILTER",
                                       &w->cap[RAILR_CAP_FILTER]);
        p->alt.lbl_count = railr_item(g, col, 0, RAILR_DY_ROW2,
                                      LV_FLEX_ALIGN_CENTER, false, "DONE",
                                      &w->cap[RAILR_CAP_DONE]);
        p->alt.lbl_stars = railr_item(g, col, dx, RAILR_DY_ROW2,
                                      LV_FLEX_ALIGN_END, false, "STARS",
                                      &w->cap[RAILR_CAP_STARS]);
        nina_dashboard_bind_tap(lv_obj_get_parent(p->alt.lbl_filter),
                                NINA_TAP_FILTER);
        nina_dashboard_bind_tap(lv_obj_get_parent(p->alt.lbl_count),
                                NINA_TAP_FILTER);
        nina_dashboard_bind_tap(lv_obj_get_parent(p->alt.lbl_stars),
                                NINA_TAP_STARS);
    }

    /* 10: the power strip on a low chord. Every cell is always present so the
     * dividers never move; the whole strip hides while the switch is not
     * connected. */
    {
        const int strip_w = railr_row_w(RAILR_EXT_PWR, RAILR_PWR_PAD);
        const int cell_w  = strip_w / RAILR_PWR_SLOTS;
        w->pwr_strip = railr_box(g, strip_w, RAILR_H_PWR, 0, RAILR_DY_PWR);
        lv_obj_set_style_bg_opa(w->pwr_strip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(w->pwr_strip, lv_color_hex(RAILR_PWR_BG), 0);
        lv_obj_set_style_radius(w->pwr_strip, 6, 0);
        lv_obj_set_style_pad_top(w->pwr_strip, RAILR_PWR_PAD_TOP, 0);
        nina_dashboard_bind_tap(w->pwr_strip, NINA_TAP_POWER);
        lv_obj_set_flex_flow(w->pwr_strip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(w->pwr_strip, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        for (int i = 0; i < RAILR_PWR_SLOTS; i++) {
            lv_obj_t *cell = lv_obj_create(w->pwr_strip);
            lv_obj_remove_style_all(cell);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(cell, cell_w, RAILR_H_PWR - RAILR_PWR_PAD_TOP);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_style_pad_gap(cell, 2, 0);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (i < RAILR_PWR_SLOTS - 1) {
                lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_RIGHT, 0);
                lv_obj_set_style_border_width(cell, 1, 0);
                lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(cell,
                    lv_color_hex(RAILR_PWR_DIV), 0);
            }
            w->pwr_cap[i] = railr_label(cell, RAILR_FONT_CAP, "",
                                        LV_TEXT_ALIGN_CENTER);
            lv_obj_set_style_text_letter_space(w->pwr_cap[i],
                                               RAILR_CAP_SPACE, 0);
            lv_obj_set_size(w->pwr_cap[i], cell_w - 6,
                            lv_font_get_line_height(RAILR_FONT_CAP));
            w->pwr_val[i] = railr_label(cell, RAILR_FONT_PWR, "",
                                        LV_TEXT_ALIGN_CENTER);
            lv_obj_set_size(w->pwr_val[i], cell_w - 6,
                            lv_font_get_line_height(RAILR_FONT_PWR));
        }
        lv_obj_add_flag(w->pwr_strip, LV_OBJ_FLAG_HIDDEN);
    }

    railr_theme_page(p, gb);
}

/* ---- theme -------------------------------------------------------------- */

static void railr_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !current_theme) return;
    railr_page_t *w = railr_store(p->alt.inst);
    if (!w) return;

    const bool     red   = railr_red();
    const uint32_t text  = railr_dim(current_theme->text_color, gb);
    const uint32_t label = railr_dim(current_theme->label_color, gb);

    /* The identity tone also carries the connection state, so update() writes
     * it every poll; this seeds it for the first frame. */
    if (w->lbl_ident) {
        lv_obj_set_style_text_color(w->lbl_ident, lv_color_hex(label), 0);
    }
    if (w->lbl_container) {
        lv_obj_set_style_text_color(w->lbl_container,
            lv_color_hex(railr_dim(red ? current_theme->header_text_color
                                       : RAILR_SEQ_FG, gb)), 0);
    }
    if (p->alt.lbl_seq_step) {
        lv_obj_set_style_text_color(p->alt.lbl_seq_step, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_target) {
        lv_obj_set_style_text_color(p->alt.lbl_target,
            lv_color_hex(railr_dim(red ? current_theme->text_color
                                       : RAILR_TARGET_FG, gb)), 0);
    }
    for (int i = 0; i < RAILR_CAP_N; i++) {
        if (w->cap[i]) {
            lv_obj_set_style_text_color(w->cap[i], lv_color_hex(label), 0);
        }
    }
    if (p->alt.lbl_hero) {
        lv_obj_set_style_text_color(p->alt.lbl_hero, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_hero_unit) {
        lv_obj_set_style_text_color(p->alt.lbl_hero_unit, lv_color_hex(label), 0);
    }
    if (w->lbl_exposure) {
        lv_obj_set_style_text_color(w->lbl_exposure, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_stars) {
        lv_obj_set_style_text_color(p->alt.lbl_stars, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_flip) {
        lv_obj_set_style_text_color(p->alt.lbl_flip, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_limit) {
        lv_obj_set_style_text_color(p->alt.lbl_limit, lv_color_hex(label), 0);
    }
    /* RMS, HFR, the filter line, the completed line and the rail all take a
     * live tone; the next update() repaints them from the thresholds and the
     * configured filter colours. */
    if (p->alt.lbl_rms) {
        lv_obj_set_style_text_color(p->alt.lbl_rms,
            lv_color_hex(railr_dim(red ? current_theme->rms_color
                                       : current_theme->label_color, gb)), 0);
    }
    if (p->alt.lbl_hfr) {
        lv_obj_set_style_text_color(p->alt.lbl_hfr,
            lv_color_hex(railr_dim(red ? current_theme->hfr_color
                                       : current_theme->label_color, gb)), 0);
    }
    {
        const uint32_t fc = railr_filter_color(p->subbar.cached_filter,
                                               p->alt.inst, gb);
        if (p->alt.lbl_filter) {
            lv_obj_set_style_text_color(p->alt.lbl_filter, lv_color_hex(fc), 0);
        }
        if (p->alt.lbl_count) {
            lv_obj_set_style_text_color(p->alt.lbl_count, lv_color_hex(fc), 0);
        }
        if (p->alt.bar_progress) {
            lv_obj_set_style_bg_color(p->alt.bar_progress, lv_color_hex(fc),
                                      LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(p->alt.bar_progress,
                lv_color_hex(railr_dim(RAILR_TRACK_COLOR, gb)), LV_PART_MAIN);
        }
    }
    for (int i = 0; i < RAILR_PWR_SLOTS; i++) {
        if (w->pwr_cap[i]) {
            lv_obj_set_style_text_color(w->pwr_cap[i], lv_color_hex(label), 0);
        }
        if (w->pwr_val[i]) {
            lv_obj_set_style_text_color(w->pwr_val[i], lv_color_hex(text), 0);
        }
    }
    if (w->pwr_strip) {
        lv_obj_set_style_bg_color(w->pwr_strip,
            lv_color_hex(railr_dim(RAILR_PWR_BG, gb)), 0);
    }
}

void nina_layout_rail_apply_theme(dashboard_page_t *p)
{
    if (!p) return;
    /* Drops a retained capture that was remapped for the other Red Night
     * state. The overlay does not do this, so the layout still must. */
    nina_layout_image_note_theme_switch(p->alt.inst);
    railr_theme_page(p, app_config_get()->color_brightness);
}

/* ---- view mode ---------------------------------------------------------- */

/* This page IS the NUMBERS composition, so it shows in that one state and
 * hides in every other. The capture, the crown and the rim arc are not this
 * module's to touch. Idempotent, creates and deletes nothing. */
void nina_layout_rail_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    if (!p) return;
    railr_show(p->alt.grp_mid, mode == NINA_VIEW_NUMBERS);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_rail_update(dashboard_page_t *p, const nina_client_t *d,
                             int instance_idx, int gb)
{
    if (!p || !d || !current_theme) return;
    railr_page_t *w = railr_store(instance_idx);
    if (!w) return;

    p->alt.inst = instance_idx;
    const bool red = railr_red();

    /* Identity, green while the rig answers. */
    if (w->lbl_ident) {
        char buf[132];
        if (d->telescope_name[0] != '\0' && d->camera_name[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s | %s", d->telescope_name,
                     d->camera_name);
        } else if (d->telescope_name[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s", d->telescope_name);
        } else if (d->camera_name[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s", d->camera_name);
        } else {
            snprintf(buf, sizeof(buf), "N.I.N.A.");
        }
        ui_label_set_text(w->lbl_ident, buf);
        uint32_t c;
        if (red) {
            c = current_theme->header_text_color;
        } else {
            c = d->connected ? RAILR_IDENT_FG : current_theme->label_color;
        }
        railr_set_color(w->lbl_ident, railr_dim(c, gb));
    }

    /* Target name, refitted to the chord whenever the text changes. */
    if (p->alt.lbl_target) {
        ui_label_set_text(p->alt.lbl_target,
            (d->target_name[0] != '\0') ? d->target_name : "----");
        ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME,
                     UI_FIT_LADDER_NAME_N,
                     railr_row_w(RAILR_EXT_NAME, RAILR_NAME_PAD));
    }

    /* Sequence container and running step. */
    if (w->lbl_container) {
        ui_label_set_text(w->lbl_container,
            (d->container_name[0] != '\0') ? d->container_name : "----");
    }
    if (p->alt.lbl_seq_step) {
        ui_label_set_text(p->alt.lbl_seq_step,
            (d->container_step[0] != '\0') ? d->container_step : "----");
    }

    /* Guiding RMS, threshold tone. */
    if (p->alt.lbl_rms) {
        char buf[32];
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
        railr_set_color(p->alt.lbl_rms, railr_dim(c, gb));
    }

    /* HFR, threshold tone. */
    if (p->alt.lbl_hfr) {
        char buf[32];
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
        railr_set_color(p->alt.lbl_hfr, railr_dim(c, gb));
    }

    /* Star count. */
    if (p->alt.lbl_stars) {
        char buf[16];
        if (d->stars >= 0) {
            snprintf(buf, sizeof(buf), "%d", d->stars);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        ui_label_set_text(p->alt.lbl_stars, buf);
    }

    /* The length of one sub, the scale the rail fills against. */
    if (w->lbl_exposure) {
        char buf[16];
        if (d->exposure_total > 0.0f) {
            snprintf(buf, sizeof(buf), "%ds", (int)d->exposure_total);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        ui_label_set_text(w->lbl_exposure, buf);
    }

    /* The rail's blocks. The spine owns their progress and their theme. */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* Filter name with the loop count, and the completed count with the
     * integration time it stands for. Both take the filter tone. */
    {
        const char *filter = (d->current_filter[0] != '\0')
                           ? d->current_filter : "--";
        const uint32_t fc = railr_filter_color(filter, instance_idx, gb);

        if (p->alt.lbl_filter) {
            char buf[64];
            if (d->exposure_iterations > 0) {
                snprintf(buf, sizeof(buf), "%s x %d/%d", filter,
                         d->exposure_count, d->exposure_iterations);
            } else {
                snprintf(buf, sizeof(buf), "%s", filter);
            }
            ui_label_set_text(p->alt.lbl_filter, buf);
            railr_set_color(p->alt.lbl_filter, fc);
        }
        if (p->alt.lbl_count) {
            char buf[40];
            if (d->exposure_total_count > 0) {
                char dur[16];
                int total_secs = (int)((float)d->exposure_total_count
                                       * d->exposure_total);
                fmt_duration(dur, sizeof(dur), total_secs, FMT_DUR_HM_COMPACT);
                snprintf(buf, sizeof(buf), "%d / %s", d->exposure_total_count,
                         dur);
            } else {
                snprintf(buf, sizeof(buf), "--");
            }
            ui_label_set_text(p->alt.lbl_count, buf);
            railr_set_color(p->alt.lbl_count, fc);
        }
        if (p->alt.bar_progress) {
            const lv_color_t c = lv_color_hex(fc);
            if (!lv_color_eq(lv_obj_get_style_bg_color(p->alt.bar_progress,
                                                       LV_PART_INDICATOR), c)) {
                lv_obj_set_style_bg_color(p->alt.bar_progress, c,
                                          LV_PART_INDICATOR);
            }
        }
    }

    /* Meridian flip. "HH:MM" becomes "9h 26m"; a state word such as
     * "FLIPPING" and the unknown "--" pass through as they are. */
    if (p->alt.lbl_flip) {
        char buf[32];
        const char *mf = d->meridian_flip;
        int hh = 0, mm = 0;
        if (mf[0] == '\0' || strcmp(mf, "--") == 0) {
            snprintf(buf, sizeof(buf), "--");
        } else if (strcmp(mf, "FLIPPING") != 0
                   && sscanf(mf, "%d:%d", &hh, &mm) >= 2) {
            snprintf(buf, sizeof(buf), "%dh %02dm", hh, mm);
        } else {
            snprintf(buf, sizeof(buf), "%s", mf);
        }
        ui_label_set_text(p->alt.lbl_flip, buf);
    }

    /* Session limit: the countdown, captioned with whichever condition binds
     * first and a "+" when more than one is active. */
    if (p->alt.lbl_limit) {
        ui_label_set_text(p->alt.lbl_limit,
            (d->target_time_remaining[0] != '\0') ? d->target_time_remaining
                                                  : "--");
    }
    if (w->cap[RAILR_CAP_LIMIT]) {
        char buf[24];
        if (d->target_time_reason[0] != '\0') {
            char why[20];
            railr_upper(why, sizeof(why), d->target_time_reason, "TIME LIMIT");
            if (d->target_condition_count > 1) {
                snprintf(buf, sizeof(buf), "%s+", why);
            } else {
                snprintf(buf, sizeof(buf), "%s", why);
            }
        } else {
            snprintf(buf, sizeof(buf), "TIME LIMIT");
        }
        ui_label_set_text(w->cap[RAILR_CAP_LIMIT], buf);
    }

    /* Power strip: amps, watts and up to four PWM ports, hidden whole while
     * the switch is not connected. A cell with no reading is hidden along
     * with its own divider, and the remaining cells share the strip width
     * equally. */
    if (w->pwr_strip) {
        const bool sw = d->power.switch_connected;
        railr_show(w->pwr_strip, sw);
        if (sw) {
            int slot = 0;
            char title[32];
            char value[32];

            railr_upper(title, sizeof(title), d->power.amps_name, "AMPS");
            snprintf(value, sizeof(value), "%.2fA", (double)d->power.total_amps);
            ui_label_set_text(w->pwr_cap[slot], title);
            ui_label_set_text(w->pwr_val[slot], value);
            railr_show(lv_obj_get_parent(w->pwr_cap[slot]), true);
            slot++;

            railr_upper(title, sizeof(title), d->power.watts_name, "WATTS");
            snprintf(value, sizeof(value), "%.1fW", (double)d->power.total_watts);
            ui_label_set_text(w->pwr_cap[slot], title);
            ui_label_set_text(w->pwr_val[slot], value);
            railr_show(lv_obj_get_parent(w->pwr_cap[slot]), true);
            slot++;

            for (int i = 0; i < d->power.pwm_count && slot < RAILR_PWR_SLOTS;
                 i++, slot++) {
                railr_upper(title, sizeof(title), d->power.pwm_names[i], "PWM");
                snprintf(value, sizeof(value), "%.0f%%", (double)d->power.pwm[i]);
                ui_label_set_text(w->pwr_cap[slot], title);
                ui_label_set_text(w->pwr_val[slot], value);
                railr_show(lv_obj_get_parent(w->pwr_cap[slot]), true);
            }
            const int filled = slot;
            for (; slot < RAILR_PWR_SLOTS; slot++) {
                railr_show(lv_obj_get_parent(w->pwr_cap[slot]), false);
            }

            /* Share the strip width between however many readings the rig
             * actually has; the count lives on the strip's own user data, so
             * the widths are only rewritten when it really changed. */
            const int prev = (int)(intptr_t)lv_obj_get_user_data(w->pwr_strip);
            if (prev != filled && filled > 0) {
                lv_obj_set_user_data(w->pwr_strip, (void *)(intptr_t)filled);
                const int cell_w = railr_row_w(RAILR_EXT_PWR, RAILR_PWR_PAD)
                                  / filled;
                for (int i = 0; i < RAILR_PWR_SLOTS; i++) {
                    lv_obj_set_width(lv_obj_get_parent(w->pwr_cap[i]), cell_w);
                    /* The two labels were sized to the cell at create time,
                     * so they follow it or the text sits off centre. */
                    lv_obj_set_width(w->pwr_cap[i], cell_w - 6);
                    lv_obj_set_width(w->pwr_val[i], cell_w - 6);
                }
            }
        }
    }

    /* The hero digits arrive through p->alt.elapsed_hook from the overlay's
     * elapsed writer, including the idle -1, so nothing writes them here. */
}
