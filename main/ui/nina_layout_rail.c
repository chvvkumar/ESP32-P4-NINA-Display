/**
 * @file nina_layout_rail.c
 * @brief NINA layout 5 (Night rail) on a SQUARE panel.
 *
 * A readings page with no picture: nina_layout_uses_capture() is false for
 * this id on the square family, so no capture is fetched, p->alt.cap_img is
 * never created and set_view() has nothing to switch.
 *
 * The composition is one horizontal rail across the lower middle of the panel
 * that carries the exposure fill, with the saved subs as blocks directly under
 * it. Everything else stacks around that line: identity, target name and the
 * sequence row on top, then the hero elapsed seconds flanked by guiding RMS and
 * HFR with the star count under them, and below the blocks one line of four
 * evenly spaced readings (filter, completed, flip, limit) over the power strip.
 *
 * The rail carries the exposure fill ONLY. The board drew a flip mark and a
 * limit end cap on it; both readings moved onto the four item line under the
 * blocks instead, where they are read as figures rather than as positions.
 *
 * Elapsed seconds arrive through p->alt.elapsed_cb, the 200 ms tick's single
 * sink for this page, so nina_subbar_set_elapsed_cb() is deliberately not
 * called: two sinks would double write the same digits.
 *
 * All entry points run with the LVGL display lock held by the caller.
 */

#include "nina_layout_alt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "time_parse.h"
#include "ui_helpers.h"
#include "ui_text_fit.h"

/* The whole builder is square only: on the round family
 * nina_layout_rail_round.c defines the same four entry points and these bodies
 * would be unreferenced. The includes above stay outside the guard so the
 * round build still compiles a well formed translation unit. */
#if !CONFIG_NINA_FAMILY_ROUND

LV_FONT_DECLARE(lv_font_material_safety);
LV_FONT_DECLARE(lv_font_hanken_black_96);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

/* Fonts. Only faces already compiled: built-in Montserrat for every mixed
 * string, Hanken Grotesk for the ticking hero. lv_font_hanken_black_96 carries
 * digits and a colon only, so its unit "s" and the idle "--" live in the full
 * ASCII 28 px Hanken Bold label beside it. */
#define RAIL_FONT_IDENT    (&lv_font_montserrat_24)
#define RAIL_FONT_SEQ      (&lv_font_montserrat_24)
#define RAIL_FONT_CAP      (&lv_font_montserrat_14)
#define RAIL_FONT_STAT     (&lv_font_montserrat_48)   /* RMS and HFR, board 56 */
#define RAIL_FONT_HERO     (&lv_font_hanken_black_96) /* board 112 */
#define RAIL_FONT_UNIT     (&lv_font_hanken_bold_28)
#define RAIL_FONT_STARS    (&lv_font_montserrat_30)
#define RAIL_FONT_ROW      (&lv_font_montserrat_28)
#define RAIL_FONT_PWR      (&lv_font_montserrat_26)

/* Side margin and the row tops, straight from the board (720 reference). The
 * content width follows screen_size(), so a wider square panel keeps the same
 * margins and spends the extra pixels on the rows. */
#define RAIL_PAD            36
#define RAIL_Y_IDENT        22
#define RAIL_Y_NAME         58
#define RAIL_Y_SEQ         114
#define RAIL_H_SEQ          44
#define RAIL_Y_RULE        172
#define RAIL_Y_STAT_CAP    210
#define RAIL_Y_STAT_VAL    228
#define RAIL_Y_HERO        200
#define RAIL_Y_STARS       296
#define RAIL_Y_BAR         430
#define RAIL_H_BAR          22
#define RAIL_Y_BLOCKS      464
#define RAIL_H_BLOCK        14
#define RAIL_Y_ROW         506
#define RAIL_Y_PWR         604
#define RAIL_H_PWR          80
#define RAIL_PWR_PAD_TOP    14

#define RAIL_STAT_W        180   /* RMS and HFR box, clear of the hero digits */
#define RAIL_HERO_GAP        6
#define RAIL_STARS_GAP       8
#define RAIL_CAP_SPACE       2   /* caption letter spacing */

#define RAIL_RULE_COLOR   0x242424
#define RAIL_TRACK_COLOR  0x202020
#define RAIL_PWR_BG       0x161616
#define RAIL_PWR_DIV      0x2a2a2a
#define RAIL_TARGET_FG    0xf2f2f4
#define RAIL_IDENT_FG     0x2ecc71
#define RAIL_SEQ_FG       0x4fc3f7

/* Material Symbols codepoints (UTF-8), the glyphs every NINA page uses. */
#define RAIL_ICON_SAFE    "\xee\xa3\xa8"  /* U+E8E8 verified_user */
#define RAIL_ICON_UNSAFE  "\xef\x80\x92"  /* U+F012 gpp_bad       */
#define RAIL_ICON_UNKNOWN "\xef\x80\x94"  /* U+F014 gpp_maybe     */

/* Power strip: amps, watts and up to four PWM ports. The cells are always all
 * present so the dividers never move; unused ones simply carry no text, the
 * way the board draws its spare slots. */
#define RAIL_PWR_SLOTS       6

/* Captions this page owns. p->alt.lbl_caption[] holds six, this page needs
 * seven plus the power strip's own, so they live in the store below. */
enum {
    RAIL_CAP_RMS = 0,
    RAIL_CAP_HFR,
    RAIL_CAP_STARS,
    RAIL_CAP_FILTER,
    RAIL_CAP_DONE,
    RAIL_CAP_FLIP,
    RAIL_CAP_LIMIT,
    RAIL_CAP_N
};

/* Widgets with no home in dashboard_page_t::alt, kept per instance. create()
 * clears the slot before it fills it, so a page rebuild can never inherit a
 * deleted object, and every read is null checked. */
typedef struct {
    lv_obj_t *lbl_ident;
    lv_obj_t *lbl_container;
    lv_obj_t *rule;
    lv_obj_t *cap[RAIL_CAP_N];
    lv_obj_t *pwr_strip;
    lv_obj_t *pwr_cap[RAIL_PWR_SLOTS];
    lv_obj_t *pwr_val[RAIL_PWR_SLOTS];
} rail_page_t;

static rail_page_t s_rail[MAX_NINA_INSTANCES];

/* ---- forward declarations ----------------------------------------------- */

static rail_page_t *rail_store(int inst);
static bool         rail_red(void);
static uint32_t     rail_dim(uint32_t color, int gb);
static uint32_t     rail_filter_color(const char *filter, int inst, int gb);
static int          rail_content_w(void);
static lv_obj_t    *rail_box(lv_obj_t *parent, int32_t w, int32_t h, int x, int y);
static lv_obj_t    *rail_label(lv_obj_t *parent, const lv_font_t *font,
                               const char *text, lv_text_align_t align);
static lv_obj_t    *rail_cap_label(lv_obj_t *parent, const char *text,
                                   lv_text_align_t align);
static void         rail_set_color(lv_obj_t *l, uint32_t rgb);
static void         rail_upper(char *dst, size_t sz, const char *src,
                               const char *fallback);
static void         rail_elapsed_cb(dashboard_page_t *p, int secs);
static void         rail_theme_page(dashboard_page_t *p, int gb);

/* ---- small helpers ------------------------------------------------------ */

static rail_page_t *rail_store(int inst)
{
    if (inst < 0 || inst >= MAX_NINA_INSTANCES) return NULL;
    return &s_rail[inst];
}

static bool rail_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t rail_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

/* Filter tone: the configured filter colour, theme text on Red Night, label
 * tone when no filter is known. Already brightness applied. */
static uint32_t rail_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) return rail_dim(0x808080, gb);
    if (rail_red()) return rail_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return rail_dim(current_theme->label_color, gb);
}

static int rail_content_w(void)
{
    int w = screen_size() - 2 * RAIL_PAD;
    return (w < 120) ? 120 : w;
}

/* Transparent container placed at an absolute panel coordinate. Not clickable
 * unless a caller binds a tap to it. */
static lv_obj_t *rail_box(lv_obj_t *parent, int32_t w, int32_t h, int x, int y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_gap(o, 0, 0);
    return o;
}

static lv_obj_t *rail_label(lv_obj_t *parent, const lv_font_t *font,
                            const char *text, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static lv_obj_t *rail_cap_label(lv_obj_t *parent, const char *text,
                                lv_text_align_t align)
{
    lv_obj_t *l = rail_label(parent, RAIL_FONT_CAP, text, align);
    lv_obj_set_style_text_letter_space(l, RAIL_CAP_SPACE, 0);
    return l;
}

/* Set-if-changed text colour. LVGL invalidates unconditionally on a style
 * write and this panel is full refresh, so an unguarded per-poll recolour
 * repaints the whole screen every cycle even when nothing moved. The theme
 * path may write unguarded: it runs once per theme change. */
static void rail_set_color(lv_obj_t *l, uint32_t rgb)
{
    if (!l) return;
    const lv_color_t c = lv_color_hex(rgb);
    if (!lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(l, c, 0);
    }
}

/* Copy @p src (or @p fallback when it is empty) into @p dst, upper cased. */
static void rail_upper(char *dst, size_t sz, const char *src,
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

/* Sole writer of the hero digits: the 200 ms tick through p->alt.elapsed_cb,
 * and the idle reset that arrives as -1. The 96 px face has no hyphen glyph,
 * so idle empties the digits and the full ASCII unit label carries the "--". */
static void rail_elapsed_cb(dashboard_page_t *p, int secs)
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

    rail_page_t *w = rail_store(page_index);
    if (!w) return;
    memset(w, 0, sizeof(*w));

    const int gb  = app_config_get()->color_brightness;
    const int cw  = rail_content_w();
    const int right = RAIL_PAD + cw;
    p->alt.inst = page_index;

    /* The page root is the full panel for every non-zero layout, so every
     * coordinate below is an absolute panel pixel. */
    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 1: identity, the scope and the camera on one line. */
    w->lbl_ident = rail_label(parent, RAIL_FONT_IDENT, "N.I.N.A.",
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(w->lbl_ident, cw, lv_font_get_line_height(RAIL_FONT_IDENT));
    lv_obj_set_pos(w->lbl_ident, RAIL_PAD, RAIL_Y_IDENT);

    /* 2: the target name, fitted to the full content width on one line. */
    p->alt.lbl_target = rail_label(parent, &lv_font_montserrat_40, "----",
                                   LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(p->alt.lbl_target, RAIL_PAD, RAIL_Y_NAME);
    ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N, cw);
    nina_dashboard_bind_tap(p->alt.lbl_target, NINA_TAP_CAPTURE);

    /* 3: the sequence row, container on the left, safety shield in the middle,
     * running step on the right. The whole row opens the sequence overlay. */
    {
        lv_obj_t *row = rail_box(parent, cw, RAIL_H_SEQ, RAIL_PAD, RAIL_Y_SEQ);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        nina_dashboard_bind_tap(row, NINA_TAP_SEQUENCE);

        w->lbl_container = rail_label(row, RAIL_FONT_SEQ, "----",
                                      LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(w->lbl_container, (cw - 60) / 2,
                        lv_font_get_line_height(RAIL_FONT_SEQ));

        p->alt.lbl_safety = rail_label(row, &lv_font_material_safety,
                                       RAIL_ICON_UNKNOWN, LV_TEXT_ALIGN_CENTER);

        p->alt.lbl_seq_step = rail_label(row, RAIL_FONT_SEQ, "----",
                                         LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(p->alt.lbl_seq_step, (cw - 60) / 2,
                        lv_font_get_line_height(RAIL_FONT_SEQ));
    }

    /* 4: the hairline that separates the names from the readings. */
    w->rule = rail_box(parent, cw, 1, RAIL_PAD, RAIL_Y_RULE);
    lv_obj_set_style_bg_opa(w->rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(w->rule, lv_color_hex(RAIL_RULE_COLOR), 0);

    /* 5: guiding RMS on the left and HFR on the right, one caption above each
     * figure, flanking the hero. */
    {
        lv_obj_t *box = rail_box(parent, RAIL_STAT_W,
                                 RAIL_Y_STAT_VAL - RAIL_Y_STAT_CAP
                                 + lv_font_get_line_height(RAIL_FONT_STAT),
                                 RAIL_PAD, RAIL_Y_STAT_CAP);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        nina_dashboard_bind_tap(box, NINA_TAP_RMS);
        w->cap[RAIL_CAP_RMS] = rail_cap_label(box, "RMS", LV_TEXT_ALIGN_LEFT);
        p->alt.lbl_rms = rail_label(box, RAIL_FONT_STAT, "--", LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(p->alt.lbl_rms, RAIL_STAT_W,
                        lv_font_get_line_height(RAIL_FONT_STAT));
    }
    {
        lv_obj_t *box = rail_box(parent, RAIL_STAT_W,
                                 RAIL_Y_STAT_VAL - RAIL_Y_STAT_CAP
                                 + lv_font_get_line_height(RAIL_FONT_STAT),
                                 right - RAIL_STAT_W, RAIL_Y_STAT_CAP);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        nina_dashboard_bind_tap(box, NINA_TAP_HFR);
        w->cap[RAIL_CAP_HFR] = rail_cap_label(box, "HFR", LV_TEXT_ALIGN_RIGHT);
        p->alt.lbl_hfr = rail_label(box, RAIL_FONT_STAT, "--", LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(p->alt.lbl_hfr, RAIL_STAT_W,
                        lv_font_get_line_height(RAIL_FONT_STAT));
    }

    /* 6: the hero, interpolated exposure seconds, centred between the two
     * figures. Digits in the 96 px face, unit beside them in a full ASCII
     * 28 px face lifted onto the digits' baseline. */
    {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_gap(row, RAIL_HERO_GAP, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, RAIL_Y_HERO);
        nina_dashboard_bind_tap(row, NINA_TAP_EXPOSURE);

        p->alt.lbl_hero = rail_label(row, RAIL_FONT_HERO, "",
                                     LV_TEXT_ALIGN_CENTER);
        p->alt.lbl_hero_unit = rail_label(row, RAIL_FONT_UNIT, "--",
                                          LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_translate_y(p->alt.lbl_hero_unit,
            RAIL_FONT_UNIT->base_line - RAIL_FONT_HERO->base_line, 0);
    }

    /* 7: the star count, under the hero. */
    {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_gap(row, RAIL_STARS_GAP, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, RAIL_Y_STARS);

        p->alt.lbl_stars = rail_label(row, RAIL_FONT_STARS, "--",
                                      LV_TEXT_ALIGN_CENTER);
        nina_dashboard_bind_tap(p->alt.lbl_stars, NINA_TAP_STARS);
        w->cap[RAIL_CAP_STARS] = rail_cap_label(row, "STARS", LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_translate_y(w->cap[RAIL_CAP_STARS],
            RAIL_FONT_CAP->base_line - RAIL_FONT_STARS->base_line, 0);
        nina_dashboard_bind_tap(w->cap[RAIL_CAP_STARS], NINA_TAP_STARS);
    }

    /* 8: the rail itself. The spine's 200 ms tick owns its value (0..1000) and
     * the stale rule owns its indicator opacity, so neither is written again
     * outside create(). */
    {
        lv_obj_t *bar = lv_bar_create(parent);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, cw, RAIL_H_BAR);
        lv_obj_set_pos(bar, RAIL_PAD, RAIL_Y_BAR);
        lv_bar_set_range(bar, 0, 1000);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(RAIL_TRACK_COLOR),
                                  LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        p->alt.bar_progress = bar;
    }

    /* 9: the saved subs, one block each, directly under the rail. */
    {
        lv_obj_t *host = rail_box(parent, cw, RAIL_H_BLOCK, RAIL_PAD,
                                  RAIL_Y_BLOCKS);
        nina_subbar_create(&p->subbar, host, RAIL_H_BLOCK);
        /* No nina_subbar_set_elapsed_cb here: p->alt.elapsed_cb below is this
         * page's single elapsed sink. */
    }
    p->alt.elapsed_cb = rail_elapsed_cb;

    /* 10: the four reading line under the blocks, evenly spaced across the
     * full content width: filter, completed, flip and limit. */
    {
        const int col = cw / 4;
        const int val_h = lv_font_get_line_height(RAIL_FONT_ROW);
        const int cap_h = lv_font_get_line_height(RAIL_FONT_CAP);
        const int box_h = cap_h + 2 + val_h;
        struct {
            int              cap_idx;
            const char      *cap;
            lv_text_align_t  align;
            lv_flex_align_t  cross;
            nina_tap_target_t tap;
            lv_obj_t       **out;
        } items[4] = {
            { RAIL_CAP_FILTER, "FILTER",    LV_TEXT_ALIGN_LEFT,   LV_FLEX_ALIGN_START,
              NINA_TAP_FILTER,  &p->alt.lbl_filter },
            { RAIL_CAP_DONE,   "COMPLETED", LV_TEXT_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
              NINA_TAP_FILTER,  &p->alt.lbl_count  },
            { RAIL_CAP_FLIP,   "FLIP",      LV_TEXT_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
              NINA_TAP_FLIP,    &p->alt.lbl_flip   },
            { RAIL_CAP_LIMIT,  "TIME LIMIT", LV_TEXT_ALIGN_RIGHT, LV_FLEX_ALIGN_END,
              NINA_TAP_SESSION, &p->alt.lbl_limit  },
        };
        for (int i = 0; i < 4; i++) {
            const int x = (i == 3) ? (right - col) : (RAIL_PAD + i * col);
            lv_obj_t *box = rail_box(parent, col, box_h, x, RAIL_Y_ROW);
            lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, items[i].cross,
                                  items[i].cross);
            lv_obj_set_style_pad_gap(box, 2, 0);
            nina_dashboard_bind_tap(box, items[i].tap);

            w->cap[items[i].cap_idx] = rail_cap_label(box, items[i].cap,
                                                      items[i].align);
            lv_obj_set_size(w->cap[items[i].cap_idx], col, cap_h);
            *items[i].out = rail_label(box, RAIL_FONT_ROW, "--", items[i].align);
            lv_obj_set_size(*items[i].out, col, val_h);
        }
    }

    /* 11: the power strip. Every cell is always present so the dividers never
     * move; the whole strip hides while the switch is not connected. */
    {
        const int cell_w = cw / RAIL_PWR_SLOTS;
        w->pwr_strip = rail_box(parent, cw, RAIL_H_PWR, RAIL_PAD, RAIL_Y_PWR);
        lv_obj_set_style_bg_opa(w->pwr_strip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(w->pwr_strip, lv_color_hex(RAIL_PWR_BG), 0);
        lv_obj_set_style_radius(w->pwr_strip, 6, 0);
        lv_obj_set_style_pad_top(w->pwr_strip, RAIL_PWR_PAD_TOP, 0);
        nina_dashboard_bind_tap(w->pwr_strip, NINA_TAP_POWER);
        lv_obj_set_flex_flow(w->pwr_strip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(w->pwr_strip, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        for (int i = 0; i < RAIL_PWR_SLOTS; i++) {
            lv_obj_t *cell = lv_obj_create(w->pwr_strip);
            lv_obj_remove_style_all(cell);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(cell, cell_w,
                            RAIL_H_PWR - RAIL_PWR_PAD_TOP);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_style_pad_gap(cell, 4, 0);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (i < RAIL_PWR_SLOTS - 1) {
                lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_RIGHT, 0);
                lv_obj_set_style_border_width(cell, 1, 0);
                lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(cell,
                    lv_color_hex(RAIL_PWR_DIV), 0);
            }
            w->pwr_cap[i] = rail_cap_label(cell, "", LV_TEXT_ALIGN_CENTER);
            lv_obj_set_size(w->pwr_cap[i], cell_w - 8,
                            lv_font_get_line_height(RAIL_FONT_CAP));
            w->pwr_val[i] = rail_label(cell, RAIL_FONT_PWR, "",
                                       LV_TEXT_ALIGN_CENTER);
            lv_obj_set_size(w->pwr_val[i], cell_w - 8,
                            lv_font_get_line_height(RAIL_FONT_PWR));
        }
        lv_obj_add_flag(w->pwr_strip, LV_OBJ_FLAG_HIDDEN);
    }

    rail_theme_page(p, gb);
}

/* ---- theme -------------------------------------------------------------- */

static void rail_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !current_theme) return;
    rail_page_t *w = rail_store(p->alt.inst);
    if (!w) return;

    const bool     red   = rail_red();
    const uint32_t text  = rail_dim(current_theme->text_color, gb);
    const uint32_t label = rail_dim(current_theme->label_color, gb);

    /* The identity tone also carries the connection state, so update() writes
     * it every poll; this seeds it for the first frame. */
    if (w->lbl_ident) {
        lv_obj_set_style_text_color(w->lbl_ident, lv_color_hex(label), 0);
    }
    if (w->lbl_container) {
        lv_obj_set_style_text_color(w->lbl_container,
            lv_color_hex(rail_dim(red ? current_theme->header_text_color
                                      : RAIL_SEQ_FG, gb)), 0);
    }
    if (p->alt.lbl_seq_step) {
        lv_obj_set_style_text_color(p->alt.lbl_seq_step, lv_color_hex(text), 0);
    }
    if (p->alt.lbl_target) {
        lv_obj_set_style_text_color(p->alt.lbl_target,
            lv_color_hex(rail_dim(red ? current_theme->text_color
                                      : RAIL_TARGET_FG, gb)), 0);
    }
    if (w->rule) {
        lv_obj_set_style_bg_color(w->rule,
            lv_color_hex(rail_dim(RAIL_RULE_COLOR, gb)), 0);
    }
    for (int i = 0; i < RAIL_CAP_N; i++) {
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
            lv_color_hex(rail_dim(red ? current_theme->rms_color
                                      : current_theme->label_color, gb)), 0);
    }
    if (p->alt.lbl_hfr) {
        lv_obj_set_style_text_color(p->alt.lbl_hfr,
            lv_color_hex(rail_dim(red ? current_theme->hfr_color
                                      : current_theme->label_color, gb)), 0);
    }
    {
        const uint32_t fc = rail_filter_color(p->subbar.cached_filter,
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
                lv_color_hex(rail_dim(RAIL_TRACK_COLOR, gb)), LV_PART_MAIN);
        }
    }
    for (int i = 0; i < RAIL_PWR_SLOTS; i++) {
        if (w->pwr_cap[i]) {
            lv_obj_set_style_text_color(w->pwr_cap[i], lv_color_hex(label), 0);
        }
        if (w->pwr_val[i]) {
            lv_obj_set_style_text_color(w->pwr_val[i], lv_color_hex(text), 0);
        }
    }
    if (w->pwr_strip) {
        lv_obj_set_style_bg_color(w->pwr_strip,
            lv_color_hex(rail_dim(RAIL_PWR_BG, gb)), 0);
    }
}

void nina_layout_rail_apply_theme(dashboard_page_t *p)
{
    if (!p) return;
    rail_theme_page(p, app_config_get()->color_brightness);
}

/* ---- view mode ---------------------------------------------------------- */

/* The square Night rail page has one composition and no picture, so there is
 * nothing to switch. Deliberately empty rather than absent: the spine
 * dispatches to it on both families. */
void nina_layout_rail_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    LV_UNUSED(p);
    LV_UNUSED(mode);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_rail_update(dashboard_page_t *p, const nina_client_t *d,
                             int instance_idx, int gb)
{
    if (!p || !d || !current_theme) return;
    rail_page_t *w = rail_store(instance_idx);
    if (!w) return;

    p->alt.inst = instance_idx;
    const bool red = rail_red();

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
            c = d->connected ? RAIL_IDENT_FG : current_theme->label_color;
        }
        rail_set_color(w->lbl_ident, rail_dim(c, gb));
    }

    /* Target name, refitted whenever the text changes. */
    if (p->alt.lbl_target) {
        ui_label_set_text(p->alt.lbl_target,
            (d->target_name[0] != '\0') ? d->target_name : "----");
        ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME,
                     UI_FIT_LADDER_NAME_N, rail_content_w());
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

    /* Safety shield. */
    if (p->alt.lbl_safety) {
        const char *icon;
        uint32_t icon_color;
        if (!d->safety_connected) {
            icon = RAIL_ICON_UNKNOWN;
            icon_color = red ? current_theme->label_color : 0x999999;
        } else if (d->safety_is_safe) {
            icon = RAIL_ICON_SAFE;
            icon_color = red ? 0x7f1d1d : 0x4CAF50;
        } else {
            icon = RAIL_ICON_UNSAFE;
            icon_color = red ? 0xff0000 : 0xF44336;
        }
        ui_label_set_text(p->alt.lbl_safety, icon);
        rail_set_color(p->alt.lbl_safety, rail_dim(icon_color, gb));
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
        rail_set_color(p->alt.lbl_rms, rail_dim(c, gb));
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
        rail_set_color(p->alt.lbl_hfr, rail_dim(c, gb));
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

    /* The rail's blocks. The spine owns their progress and their theme. */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* Filter name with the loop count, and the completed count with the
     * integration time it stands for. Both take the filter tone. */
    {
        const char *filter = (d->current_filter[0] != '\0')
                           ? d->current_filter : "--";
        const uint32_t fc = rail_filter_color(filter, instance_idx, gb);

        if (p->alt.lbl_filter) {
            char buf[64];
            if (d->exposure_iterations > 0) {
                snprintf(buf, sizeof(buf), "%s x %d/%d", filter,
                         d->exposure_count, d->exposure_iterations);
            } else {
                snprintf(buf, sizeof(buf), "%s", filter);
            }
            ui_label_set_text(p->alt.lbl_filter, buf);
            rail_set_color(p->alt.lbl_filter, fc);
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
            rail_set_color(p->alt.lbl_count, fc);
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
    if (w->cap[RAIL_CAP_LIMIT]) {
        char buf[24];
        if (d->target_time_reason[0] != '\0') {
            char why[20];
            rail_upper(why, sizeof(why), d->target_time_reason, "TIME LIMIT");
            if (d->target_condition_count > 1) {
                snprintf(buf, sizeof(buf), "%s+", why);
            } else {
                snprintf(buf, sizeof(buf), "%s", why);
            }
        } else {
            snprintf(buf, sizeof(buf), "TIME LIMIT");
        }
        ui_label_set_text(w->cap[RAIL_CAP_LIMIT], buf);
    }

    /* Power strip: amps, watts and up to four PWM ports, hidden whole while
     * the switch is not connected. A cell with no reading is hidden along
     * with its own divider (a border on the cell itself), and the remaining
     * cells share the strip width equally. */
    if (w->pwr_strip) {
        const bool sw = d->power.switch_connected;
        if (sw) {
            lv_obj_remove_flag(w->pwr_strip, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(w->pwr_strip, LV_OBJ_FLAG_HIDDEN);
        }
        if (sw) {
            int slot = 0;
            char title[32];
            char value[32];

            rail_upper(title, sizeof(title), d->power.amps_name, "AMPS");
            snprintf(value, sizeof(value), "%.2fA", (double)d->power.total_amps);
            ui_label_set_text(w->pwr_cap[slot], title);
            ui_label_set_text(w->pwr_val[slot], value);
            lv_obj_remove_flag(lv_obj_get_parent(w->pwr_cap[slot]),
                               LV_OBJ_FLAG_HIDDEN);
            slot++;

            rail_upper(title, sizeof(title), d->power.watts_name, "WATTS");
            snprintf(value, sizeof(value), "%.1fW", (double)d->power.total_watts);
            ui_label_set_text(w->pwr_cap[slot], title);
            ui_label_set_text(w->pwr_val[slot], value);
            lv_obj_remove_flag(lv_obj_get_parent(w->pwr_cap[slot]),
                               LV_OBJ_FLAG_HIDDEN);
            slot++;

            for (int i = 0; i < d->power.pwm_count && slot < RAIL_PWR_SLOTS;
                 i++, slot++) {
                rail_upper(title, sizeof(title), d->power.pwm_names[i], "PWM");
                snprintf(value, sizeof(value), "%.0f%%", (double)d->power.pwm[i]);
                ui_label_set_text(w->pwr_cap[slot], title);
                ui_label_set_text(w->pwr_val[slot], value);
                lv_obj_remove_flag(lv_obj_get_parent(w->pwr_cap[slot]),
                                   LV_OBJ_FLAG_HIDDEN);
            }
            const int filled = slot;
            for (; slot < RAIL_PWR_SLOTS; slot++) {
                lv_obj_add_flag(lv_obj_get_parent(w->pwr_cap[slot]),
                                LV_OBJ_FLAG_HIDDEN);
            }

            /* Share the strip width between however many readings the rig
             * actually has, the same recompute-on-count-change the ring
             * layout uses. The count lives on the strip's own user data, so
             * the widths are only rewritten when it really changed. */
            const int prev = (int)(intptr_t)lv_obj_get_user_data(w->pwr_strip);
            if (prev != filled && filled > 0) {
                lv_obj_set_user_data(w->pwr_strip, (void *)(intptr_t)filled);
                const int cell_w = rail_content_w() / filled;
                for (int i = 0; i < RAIL_PWR_SLOTS; i++) {
                    lv_obj_set_width(lv_obj_get_parent(w->pwr_cap[i]), cell_w);
                    /* The two labels were sized to the cell at create time,
                     * so they follow it or the text sits off centre. */
                    lv_obj_set_width(w->pwr_cap[i], cell_w - 8);
                    lv_obj_set_width(w->pwr_val[i], cell_w - 8);
                }
            }
        }
    }

    /* The hero digits arrive through p->alt.elapsed_cb from the 200 ms tick,
     * so only the idle reset lives here, routed through the same writer. */
    if (d->exposure_total <= 0.0f) rail_elapsed_cb(p, -1);
}

#endif  /* !CONFIG_NINA_FAMILY_ROUND */
