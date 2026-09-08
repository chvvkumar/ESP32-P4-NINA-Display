/**
 * @file nina_layout_rings.c
 * @brief Layout 7 (Two rings) on a SQUARE panel: a readings page, no picture.
 *
 * Two concentric rings sit in the middle of the panel. The outer one fills as
 * the exposure runs (p->alt.arc_progress, driven by the spine's 200 ms tick);
 * the inner one is cut into one block per sub (the sub bar's ring form). The
 * seconds, the filter and the completed count live inside them, so the loudest
 * shape on the panel answers "how far into this frame" and "how far into this
 * filter" without a word.
 *
 * Everything else is a cross around the rings: the rig's name and its target
 * centred across the top; the safety shield centred below the ring, above the
 * power rule; the sequence name, the guiding RMS and the
 * HFR down the left column; the running step, the exposure length and the star
 * count down the right column; the power readings on a quiet strip along the
 * bottom. The two data fields of a column are placed as a PAIR whose midpoint
 * is the ring's centre line, so the gap between them is symmetric about it,
 * and the sequence and step names sit a fixed gap above them, in the width the
 * ring's own curve opens beside its upper half. The flip countdown and the
 * session limit sit lower, in the band between the ring's outer edge and the
 * power strip: flip toward the lower left, the limit toward the lower right,
 * each width-limited to the free space beside the ring so the text can never
 * run under it. Those two, and only those two, carry their NAME ABOVE the
 * number, because a countdown reads as nonsense until you know what it counts.
 * Every other reading is the number first and its name underneath, split by
 * 1 px hairlines instead of boxes.
 *
 * This board draws NO picture: nina_layout_uses_capture() is false for layout 7
 * on the square family, so the spine never creates p->alt.cap_img, never fetches
 * a capture, and set_view() below is deliberately empty (there is no view cycle
 * on the square family).
 *
 * The elapsed seconds have ONE writer: p->alt.elapsed_cb, registered in
 * create() and called by the 200 ms tick with -1 for idle. The sub bar's own
 * elapsed callback is never registered, so the two can never disagree.
 *
 * Family split: this builder is the SQUARE one and is compiled out of the round
 * binary, where nina_layout_rings_round.c defines the same four entry points.
 *
 * All entry points run with the LVGL display lock held by the caller.
 */

#include "nina_layout_alt.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "time_parse.h"
#include "ui_text_fit.h"

#if !CONFIG_NINA_FAMILY_ROUND

LV_FONT_DECLARE(lv_font_material_safety);
LV_FONT_DECLARE(lv_font_hanken_black_96);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

/* Rows are measured from the top edge of the panel, columns from its side
 * edges: fixed insets, never a fraction of a hardcoded 720. Everything around
 * the rings is derived from screen_center(), screen_size() and the live font
 * line heights, so the figures below are what that arithmetic produces on the
 * 720 px square panel, not numbers typed in:
 *
 *   container top    (rule 1)        120
 *   container bottom (power rule)    600     = 720 - RG_PWR_BOTTOM - RG_PWR_H
 *   ring outer radius (RG_R_OUT)     170 px  UNCHANGED; outer edge 177 px
 *   ring centre y                    360     was 310: the midpoint of the two
 *                                            rules, so the ring spans y
 *                                            183..537 and clears both of them
 *                                            by 63 px
 *
 * The four side rows span the WHOLE container: row 1 starts RG_ROW_PAD under
 * the top rule, row 4 ends RG_ROW_PAD above the bottom one, and the centres of
 * rows 2 and 3 sit evenly spaced between those two centres. A field is its
 * value line, RG_CAP_GAP and the 16 px caption line: 47 px for the 26 px name
 * row, 62 px for the three 44 px value rows.
 *
 *   row 1  sequence / step   y 130..177  centre 153  width 327
 *   row 2  RMS / exposure    y 257..319  centre 288  width 150
 *   row 3  HFR / stars       y 393..455  centre 424  width 150
 *   row 4  flip / time limit y 528..590  centre 559  width 256
 *   column hairline                  360     the ring's centre line, in the
 *                                            gap between rows 2 and 3
 *
 * Ring clearance, centre (360, 360), outer edge r = 177:
 *   row 1 ends at y 177, which is 183 px from the ring's centre and so 6 px
 *         above its topmost pixel: no overlap at any x, and rg_free_w() gives
 *         the whole half panel, 360 - 24 - 0 - 9 = 327 px.
 *   rows 2 and 3 are RG_ARM_GAP inside the ring's widest point by
 *         construction: 24 + rg_arm_w()(150) = 174 against the ring's leftmost
 *         pixel at 360 - 177 = 183, so 9 px clear at every y they cover.
 *   row 4 comes nearest the ring at its TOP edge, y 528, dy 168: the half
 *         chord there is sqrtf(177*177 - 168*168) = 55 px, so the row is cut to
 *         360 - 40 - 55 - 9 = 256 px, ending at x 296 against a ring edge at
 *         x 304.
 *
 * The ring is NOT resized. The widest side string ("Smart Exposure" at
 * Montserrat 26, about 200 px) would want a column of 200 px, which puts the
 * outer radius at screen_center() - RG_MARGIN - 200 - RG_ARM_GAP = 111 px, far
 * smaller than the 170 px this board already draws. So the ring keeps its size
 * and the two names are fitted instead, over a 26-then-24 ladder
 * (rg_seq_ladder), into whatever rg_free_w() leaves at row 1's lower edge. */
#define RG_MARGIN         40
#define RG_Y_IDENT        34
#define RG_Y_TARGET       66
#define RG_Y_RULE1       120

#define RG_R_OUT         170   /* exposure ring, centre line */
#define RG_W_OUT          14
#define RG_R_SUB         142   /* sub block ring, centre line */
#define RG_W_SUB          10

#define RG_TRACK      0x161616
#define RG_RULE_FG    0x262626
#define RG_TARGET_FG  0xf2f2f4
#define RG_SEQ_FG     0x4fc3f7

/* Rows inside the rings, offsets from the RING centre. The inner ring's clear
 * radius is 137 px, so each row is kept well inside the chord at its own
 * depth: the filter row spans 216 px of clear width, the completed row 210. */
#define RG_FILTER_REL   (-84)
#define RG_HERO_REL      (-8)
#define RG_DONE_REL       70
#define RG_DONEC_REL      98
#define RG_INNER_W       210   /* box width for the rows inside the rings */
#define RG_HERO_GAP        6

/* The two columns. A column is inset from its own screen edge and stops
 * RG_ARM_GAP short of the exposure ring's widest point, so the type can never
 * touch the ring. The rows are no longer typed in: rg_row_cy() spreads four
 * evenly spaced row centres over the container, from row 1 hugging the top rule
 * to row 4 hugging the power strip's hairline, all derived from the live line
 * heights. */
#define RG_ARM_X          24
#define RG_ARM_GAP         9
#define RG_ARM_RULE_PAD   10
#define RG_CAP_GAP         2   /* value to its caption, inside a stacked field */
#define RG_ROW_PAD        10   /* a rule to the nearest edge of the row by it */

/* Power strip: a hairline, then one cell per reading, value over caption.
 * Six cells is the ceiling the API can fill (amps, watts and up to four PWM
 * ports); the cells share the strip width, so two readings get wide cells. */
#define RG_PWR_CELLS       6
#define RG_PWR_H          72
#define RG_PWR_BOTTOM     48
#define RG_PWR_ROW_Y      16

/* Numbers are Hanken Grotesk Bold, whose digits are all one width, so a
 * ticking value does not walk. The hero face carries digits and the colon
 * only: its "s" and its idle "--" live in the full-ASCII 28 px label beside
 * it. Montserrat has no 44 px face, so the arms take the 40 px one. */
#define RG_FONT_IDENT   (&lv_font_montserrat_24)
#define RG_FONT_SEQ     (&lv_font_montserrat_26)
#define RG_FONT_CAP     (&lv_font_montserrat_14)
#define RG_FONT_FILTER  (&lv_font_montserrat_26)
#define RG_FONT_COUNT   (&lv_font_hanken_bold_28)
#define RG_FONT_HERO    (&lv_font_hanken_black_96)
#define RG_FONT_UNIT    (&lv_font_hanken_bold_28)
#define RG_FONT_DONE    (&lv_font_hanken_bold_28)
#define RG_FONT_ARM     (&lv_font_montserrat_40)
#define RG_FONT_PWR     (&lv_font_montserrat_30)

/* The sequence and step names are the only rows on this board whose face is
 * chosen at runtime: the 26 px face, one step down to 24, then dots. Two faces
 * only, so the two names can never disagree by more than one step. */
#define RG_SEQ_LADDER_N 2
static const lv_font_t *const rg_seq_ladder[RG_SEQ_LADDER_N] = {
    &lv_font_montserrat_26, &lv_font_montserrat_24,
};

/* Material Symbols codepoints (UTF-8), the same glyphs every other board uses. */
#define RG_ICON_SAFE      "\xee\xa3\xa8"  /* U+E8E8 verified_user */
#define RG_ICON_UNSAFE    "\xef\x80\x92"  /* U+F012 gpp_bad       */
#define RG_ICON_UNKNOWN   "\xef\x80\x94"  /* U+F014 gpp_maybe     */

/* p->alt.lbl_caption[] is a spare-handle array on this board, not a set of
 * captions: two of its slots hold widgets whose text changes with the data. */
enum {
    RG_SLOT_LIMIT_CAP = 0,   /* the time-limit caption, which names the reason */
    RG_SLOT_EXP_LEN   = 1,   /* the exposure LENGTH value on the right arm */
    RG_SLOT_DONE      = 2,   /* the completed count inside the rings */
};

/* ---- forward declarations ----------------------------------------------- */

static bool      rg_red(void);
static uint32_t  rg_dim(uint32_t color, int gb);
static uint32_t  rg_filter_color(const char *filter, int inst, int gb);
static int       rg_arm_w(void);
static int       rg_bot_y(void);
static int       rg_band_top(void);
static int       rg_ring_cy(void);
static int       rg_ring_dy(void);
static int       rg_free_w(int y_edge, int inset);
static int       rg_fld_h(const lv_font_t *font);
static int       rg_row_cy(int row);
static int       rg_arm_top(int row);
static int       rg_seq_top(void);
static int       rg_seq_w(void);
static void      rg_show(lv_obj_t *obj, bool show);
static void      rg_set_text(lv_obj_t *lbl, const char *text);
static void      rg_set_color(lv_obj_t *obj, uint32_t rgb);
static lv_obj_t *rg_label(lv_obj_t *parent, const lv_font_t *font,
                          const char *text, lv_text_align_t align);
static lv_obj_t *rg_box(lv_obj_t *parent, int32_t w, int32_t h, int dy);
static lv_obj_t *rg_rule(lv_obj_t *parent, int w);
static lv_obj_t *rg_stack(lv_obj_t *parent, bool west, int w, int dy_top,
                          const lv_font_t *font, const char *cap_text,
                          lv_text_align_t align, int inset);
static void      rg_arm_rule(lv_obj_t *parent, bool west, int dy);
static void      rg_upper(char *buf);
static void      rg_power_build(dashboard_page_t *p, lv_obj_t *parent, int cw);
static void      rg_power_slot(lv_obj_t *row, int idx, const char *title,
                               const char *value);
static void      rg_power_write(dashboard_page_t *p, const nina_client_t *d);
static void      rg_elapsed_cb(dashboard_page_t *p, int secs);
static void      rg_theme_page(dashboard_page_t *p, int gb);

/* ---- small helpers ------------------------------------------------------ */

static bool rg_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t rg_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

/* Filter tone: the configured filter colour, theme text on Red Night, label
 * tone when no filter is known. Already brightness applied. */
static uint32_t rg_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) return rg_dim(0x808080, gb);
    if (rg_red()) return rg_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return rg_dim(current_theme->label_color, gb);
}

/* Width of one arm column: from its own screen inset to RG_ARM_GAP short of
 * the exposure ring's widest point. 150 px on a 720 panel. */
static int rg_arm_w(void)
{
    const int w = screen_center() - RG_R_OUT - RG_W_OUT / 2 - RG_ARM_GAP - RG_ARM_X;
    return (w > 80) ? w : 80;
}

/* The hairline the power strip starts with, which is the container's bottom
 * edge: 720 - RG_PWR_BOTTOM(48) - RG_PWR_H(72) = 600 on a 720 panel. */
static int rg_bot_y(void)
{
    return screen_size() - RG_PWR_BOTTOM - RG_PWR_H;
}

/* Top of the lowest text block above the power strip, which is the flip and
 * time-limit CAPTION row now that those two read caption over value: row 4,
 * whose own bottom sits RG_ROW_PAD above that hairline. 528 on a 720 panel. */
static int rg_band_top(void)
{
    return rg_row_cy(3) - rg_fld_h(RG_FONT_ARM) / 2;
}

/* The ring centre: the midpoint of the container, halfway between the rule
 * under the target name and the power strip's hairline. On a 720 panel:
 * (120 + 600) / 2 = 360, so the ring (radius 170, outer edge 177) spans y
 * 183..537, clears both rules by 63 px and keeps the size it has always had. */
static int rg_ring_cy(void)
{
    return (RG_Y_RULE1 + rg_bot_y()) / 2;
}

static int rg_ring_dy(void)
{
    return rg_ring_cy() - screen_center();
}

/* Free width beside the ring for a label whose edge NEAREST the ring's centre
 * line sits at absolute y @p y_edge, measured in from @p inset on its own
 * screen side and mirrored, since the ring is horizontally centred. On a 720
 * panel r = RG_R_OUT(170) + RG_W_OUT/2(7) = 177 about a centre at y 360: the
 * flip and limit captions at y 528 have half = sqrtf(177*177 - 168*168) =~ 55,
 * so they get 360 - 40 - 55 - 9 = 256 px, which "FLIPPING" and "24h 00m" at
 * Montserrat 40 clear easily; row 1's lower edge at y 177 is 183 px from the
 * centre, past the ring altogether, so half is 0 and it gets
 * 360 - 24 - 0 - 9 = 327 px. Every label here stays LV_LABEL_LONG_DOT (set by
 * rg_label()) as the last resort. */
static int rg_free_w(int y_edge, int inset)
{
    const int cy = rg_ring_cy();
    const int r  = RG_R_OUT + RG_W_OUT / 2;
    int dy = y_edge - cy;
    if (dy < 0) dy = -dy;
    float half = 0.0f;
    if (dy < r) half = sqrtf((float)(r * r - dy * dy));
    const int w = screen_center() - inset - (int)half - RG_ARM_GAP;
    return (w > 80) ? w : 80;
}

/* Height of one stacked field: a value line, the gap, its caption line. */
static int rg_fld_h(const lv_font_t *font)
{
    return lv_font_get_line_height(font) + RG_CAP_GAP
         + lv_font_get_line_height(RG_FONT_CAP);
}

/* Absolute centre y of side row @p row, 0 (the name row) to 3 (the countdown
 * row). Row 0 hugs the container's top edge, RG_ROW_PAD under the rule below
 * the target name; row 3 hugs its bottom edge, RG_ROW_PAD above the power
 * strip's hairline; the two in between are evenly spaced by centre. On a 720
 * panel: 153, 288, 424, 559. */
static int rg_row_cy(int row)
{
    const int c0 = RG_Y_RULE1 + RG_ROW_PAD + rg_fld_h(RG_FONT_SEQ) / 2;
    const int c3 = rg_bot_y() - RG_ROW_PAD - rg_fld_h(RG_FONT_ARM) / 2;
    if (row <= 0) return c0;
    if (row >= 3) return c3;
    return c0 + ((c3 - c0) * row + 1) / 3;
}

/* Top of data field @p row (1 or 2) of a column, as an offset from the PANEL
 * centre, which is what rg_stack() aligns by. */
static int rg_arm_top(int row)
{
    return rg_row_cy(row) - rg_fld_h(RG_FONT_ARM) / 2 - screen_center();
}

/* Top of the sequence/step field, row 0, in the same offset form. */
static int rg_seq_top(void)
{
    return rg_row_cy(0) - rg_fld_h(RG_FONT_SEQ) / 2 - screen_center();
}

/* Width the sequence and step names get: the free space beside the ring at the
 * LOWER edge of their field, the edge nearest the ring's centre line. */
static int rg_seq_w(void)
{
    /* Row 1 is edge aligned at RG_MARGIN like the identity line and the two
     * lower corners, so its free width is measured from that inset. */
    return rg_free_w(rg_row_cy(0) + rg_fld_h(RG_FONT_SEQ) / 2, RG_MARGIN);
}

static void rg_show(lv_obj_t *obj, bool show)
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
static void rg_set_text(lv_obj_t *lbl, const char *text)
{
    if (!lbl || !text) return;
    if (strcmp(lv_label_get_text(lbl), text) == 0) return;
    lv_label_set_text(lbl, text);
}

/* Set-if-changed text colour. LVGL invalidates on ANY style write and this
 * panel is full refresh, so an unguarded per-poll recolour repaints the whole
 * screen even when nothing moved. Every colour write on this board, the theme
 * pass included, goes through here. */
static void rg_set_color(lv_obj_t *obj, uint32_t rgb)
{
    if (!obj) return;
    const lv_color_t c = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), c)) return;
    lv_obj_set_style_text_color(obj, c, 0);
}

static lv_obj_t *rg_label(lv_obj_t *parent, const lv_font_t *font,
                          const char *text, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* Transparent, non-clickable container centred at @p dy from the panel centre. */
static lv_obj_t *rg_box(lv_obj_t *parent, int32_t w, int32_t h, int dy)
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
static lv_obj_t *rg_rule(lv_obj_t *parent, int w)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, w, 1);
    lv_obj_set_style_bg_color(r, lv_color_hex(RG_RULE_FG), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    return r;
}

/* One stacked column field: a value line over its caption, both centred in a
 * @p w px column inset RG_ARM_X from its own screen edge. @p dy_top is the TOP
 * of the value line as an offset from the panel centre; the alignments below
 * are by mid, so each line's own half-height is added back. Returns the value
 * label, the only one of the two the caller ever writes to. */
static lv_obj_t *rg_stack(lv_obj_t *parent, bool west, int w, int dy_top,
                          const lv_font_t *font, const char *cap_text,
                          lv_text_align_t align, int inset)
{
    const int vh = lv_font_get_line_height(font);
    const int ch = lv_font_get_line_height(RG_FONT_CAP);
    const lv_align_t side = west ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID;
    const int x = west ? inset : -inset;

    lv_obj_t *val = rg_label(parent, font, "--", align);
    lv_obj_set_size(val, w, vh);
    lv_obj_align(val, side, x, dy_top + vh / 2);

    lv_obj_t *cap = rg_label(parent, RG_FONT_CAP, cap_text, align);
    lv_obj_set_size(cap, w, ch);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_align(cap, side, x, dy_top + vh + RG_CAP_GAP + ch / 2);
    return val;
}

static void rg_arm_rule(lv_obj_t *parent, bool west, int dy)
{
    lv_obj_t *r = rg_rule(parent, rg_arm_w() - 2 * RG_ARM_RULE_PAD);
    lv_obj_align(r, west ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 west ? (RG_ARM_X + RG_ARM_RULE_PAD)
                      : -(RG_ARM_X + RG_ARM_RULE_PAD), dy);
}

static void rg_upper(char *buf)
{
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);
    }
}

/* ---- power strip -------------------------------------------------------- */

/* The strip is one container so the whole thing hides with a single flag when
 * the power switch is not connected: child 0 is the hairline, child 1 is the
 * cell row, and each cell is a value over its caption. */
static void rg_power_build(dashboard_page_t *p, lv_obj_t *parent, int cw)
{
    p->alt.grp_bottom = lv_obj_create(parent);
    lv_obj_remove_style_all(p->alt.grp_bottom);
    lv_obj_remove_flag(p->alt.grp_bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.grp_bottom, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(p->alt.grp_bottom, LV_LAYOUT_NONE);
    lv_obj_set_size(p->alt.grp_bottom, cw, RG_PWR_H);
    lv_obj_align(p->alt.grp_bottom, LV_ALIGN_BOTTOM_MID, 0, -RG_PWR_BOTTOM);
    lv_obj_set_style_pad_all(p->alt.grp_bottom, 0, 0);

    lv_obj_t *rule = rg_rule(p->alt.grp_bottom, cw);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *row = lv_obj_create(p->alt.grp_bottom);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, cw, RG_PWR_H - RG_PWR_ROW_Y);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, RG_PWR_ROW_Y);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    for (int i = 0; i < RG_PWR_CELLS; i++) {
        lv_obj_t *cell = lv_obj_create(row);
        lv_obj_remove_style_all(cell);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(cell, cw / RG_PWR_CELLS, LV_PCT(100));
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_pad_gap(cell, 2, 0);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *val = rg_label(cell, RG_FONT_PWR, "--", LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(val, LV_PCT(100), lv_font_get_line_height(RG_FONT_PWR));
        lv_obj_t *cap = rg_label(cell, RG_FONT_CAP, "", LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(cap, LV_PCT(100), lv_font_get_line_height(RG_FONT_CAP));
        lv_obj_set_style_text_letter_space(cap, 2, 0);

        lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(p->alt.grp_bottom, LV_OBJ_FLAG_HIDDEN);
}

static void rg_power_slot(lv_obj_t *row, int idx, const char *title,
                          const char *value)
{
    lv_obj_t *cell = lv_obj_get_child(row, idx);
    if (!cell) return;
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_HIDDEN);
    ui_label_set_text(lv_obj_get_child(cell, 0), value);
    ui_label_set_text(lv_obj_get_child(cell, 1), title);
}

static void rg_power_write(dashboard_page_t *p, const nina_client_t *d)
{
    lv_obj_t *strip = p->alt.grp_bottom;
    if (!strip) return;
    if (!d->power.switch_connected) {
        rg_show(strip, false);
        return;
    }
    lv_obj_t *row = lv_obj_get_child(strip, 1);
    if (!row) return;
    rg_show(strip, true);

    char title[40];
    char value[24];
    int n = 0;

    strncpy(title, d->power.amps_name[0] ? d->power.amps_name : "Amps",
            sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    rg_upper(title);
    snprintf(value, sizeof(value), "%.2fA", (double)d->power.total_amps);
    rg_power_slot(row, n++, title, value);

    strncpy(title, d->power.watts_name[0] ? d->power.watts_name : "Watts",
            sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    rg_upper(title);
    snprintf(value, sizeof(value), "%.1fW", (double)d->power.total_watts);
    rg_power_slot(row, n++, title, value);

    int ports = d->power.pwm_count;
    if (ports > 4) ports = 4;
    for (int i = 0; i < ports && n < RG_PWR_CELLS; i++) {
        strncpy(title, d->power.pwm_names[i], sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        rg_upper(title);
        snprintf(value, sizeof(value), "%.0f%%", (double)d->power.pwm[i]);
        rg_power_slot(row, n++, title, value);
    }

    for (int i = n; i < RG_PWR_CELLS; i++) {
        lv_obj_t *cell = lv_obj_get_child(row, i);
        if (cell) lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
    }

    /* Share the strip between however many readings the rig actually has, so
     * two readings get wide cells and six get narrow ones. The count is parked
     * on the row's user data, which nothing else on this page uses, so the
     * widths are only rewritten when the count really changed. */
    const int prev = (int)(intptr_t)lv_obj_get_user_data(row);
    if (prev != n && n > 0) {
        lv_obj_set_user_data(row, (void *)(intptr_t)n);
        const int cell_w = (screen_size() - 2 * RG_MARGIN) / n;
        for (int i = 0; i < RG_PWR_CELLS; i++) {
            lv_obj_t *cell = lv_obj_get_child(row, i);
            if (cell) lv_obj_set_width(cell, cell_w);
        }
    }
}

/* ---- the elapsed sink --------------------------------------------------- */

/* The hero digits' only writer: the spine's 200 ms tick through
 * p->alt.elapsed_cb, and the idle reset in update(), which passes -1. The
 * 96 px face carries digits and the colon only, so idle empties it and the
 * full-ASCII unit label beside it carries the marker. */
static void rg_elapsed_cb(dashboard_page_t *p, int secs)
{
    if (!p || !p->alt.lbl_hero || !p->alt.lbl_hero_unit) return;
    if (secs < 0) {
        rg_set_text(p->alt.lbl_hero, "");
        rg_set_text(p->alt.lbl_hero_unit, "--");
        return;
    }
    if (secs > 9999) secs = 9999;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", secs);
    rg_set_text(p->alt.lbl_hero, buf);
    rg_set_text(p->alt.lbl_hero_unit, "s");
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_rings_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent) return;

    p->alt.inst = page_index;
    const int gb = app_config_get()->color_brightness;
    const int cw = screen_size() - 2 * RG_MARGIN;
    const int ring_dy = rg_ring_dy();

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 1: the top block. The identity line's tone is the spine's, not this
     * file's: nina_dashboard_update_status() paints it green while the rig is
     * connected and red while it is not, on every layout. Both top lines are
     * centred across the content width; the safety shield lives below the
     * ring, in the band above the power rule. */
    const int ident_h  = lv_font_get_line_height(RG_FONT_IDENT);
    const int safety_h = lv_font_get_line_height(&lv_font_material_safety);

    /* Identity and target name are both centred across the content width
     * (user, 2026-09-08); the shield no longer shares the identity line. */
    p->lbl_instance_name = rg_label(parent, RG_FONT_IDENT, "N.I.N.A.",
                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(p->lbl_instance_name, cw, ident_h);
    lv_obj_align(p->lbl_instance_name, LV_ALIGN_TOP_MID, 0, RG_Y_IDENT);

    /* The safety shield sits centred in the band between the ring's bottom
     * edge and the power rule, between the flip and time-limit corners. On a
     * 720 panel: ring bottom 360 + 177 = 537, power rule 600, band centre 568. */
    {
        const int power_rule_y = screen_size() - RG_PWR_BOTTOM - RG_PWR_H;
        const int ring_bottom  = screen_center() + ring_dy + RG_R_OUT + RG_W_OUT / 2;
        const int band_cy      = (ring_bottom + power_rule_y) / 2;
        p->alt.lbl_safety = rg_label(parent, &lv_font_material_safety,
                                     RG_ICON_UNKNOWN, LV_TEXT_ALIGN_CENTER);
        lv_obj_align(p->alt.lbl_safety, LV_ALIGN_TOP_MID, 0,
                     band_cy - safety_h / 2);
    }

    p->alt.lbl_target = rg_label(parent, UI_FIT_LADDER_NAME[0], "--",
                                 LV_TEXT_ALIGN_CENTER);
    lv_obj_align(p->alt.lbl_target, LV_ALIGN_TOP_LEFT, RG_MARGIN, RG_Y_TARGET);
    ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME, UI_FIT_LADDER_NAME_N, cw);
    nina_dashboard_bind_tap(p->alt.lbl_target, NINA_TAP_SEQUENCE);

    lv_obj_t *rule1 = rg_rule(parent, cw);
    lv_obj_align(rule1, LV_ALIGN_TOP_LEFT, RG_MARGIN, RG_Y_RULE1);

    /* 2: the outer ring, this page's exposure progress. The spine's 200 ms tick
     * owns its value over 0..1000 and dims it while the data is stale, so this
     * file only sets geometry, track and tone. */
    {
        const int side = 2 * RG_R_OUT + RG_W_OUT;
        p->alt.arc_progress = lv_arc_create(parent);
        lv_obj_set_size(p->alt.arc_progress, side, side);
        lv_obj_align(p->alt.arc_progress, LV_ALIGN_CENTER, 0, ring_dy);
        lv_arc_set_rotation(p->alt.arc_progress, 270);
        lv_arc_set_bg_angles(p->alt.arc_progress, 0, 360);
        lv_arc_set_range(p->alt.arc_progress, 0, 1000);
        lv_arc_set_value(p->alt.arc_progress, 0);
        lv_obj_remove_style(p->alt.arc_progress, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(p->alt.arc_progress, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(p->alt.arc_progress, LV_OPA_TRANSP, 0);
        lv_obj_set_style_arc_width(p->alt.arc_progress, RG_W_OUT, LV_PART_MAIN);
        lv_obj_set_style_arc_width(p->alt.arc_progress, RG_W_OUT, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(p->alt.arc_progress, false, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(p->alt.arc_progress, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(p->alt.arc_progress, lv_color_hex(RG_TRACK),
                                   LV_PART_MAIN);
        lv_obj_set_style_shadow_width(p->alt.arc_progress, 0, LV_PART_INDICATOR);
    }

    /* 3: the inner ring, one block per sub. create_ring centres its container
     * on the parent, so it is re-aligned onto the exposure ring's centre. */
    nina_subbar_create_ring(&p->subbar, parent, RG_R_SUB, RG_W_SUB, 0);
    if (p->subbar.cont) {
        lv_obj_align(p->subbar.cont, LV_ALIGN_CENTER, 0, ring_dy);
    }
    p->alt.ring_inner = p->subbar.cont;

    /* 4: what lives inside the rings. Filter and loop count on one row, the
     * hero seconds under it, the completed count and its caption below. */
    {
        lv_obj_t *frow = rg_box(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT,
                                ring_dy + RG_FILTER_REL);
        lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(frow, 10, 0);
        nina_dashboard_bind_tap(frow, NINA_TAP_FILTER);

        p->alt.lbl_filter = rg_label(frow, RG_FONT_FILTER, "--",
                                     LV_TEXT_ALIGN_CENTER);
        p->alt.lbl_count = rg_label(frow, RG_FONT_COUNT, "", LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_translate_y(p->alt.lbl_count,
            RG_FONT_COUNT->base_line - RG_FONT_FILTER->base_line, 0);

        lv_obj_t *hrow = rg_box(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT,
                                ring_dy + RG_HERO_REL);
        lv_obj_set_flex_flow(hrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_gap(hrow, RG_HERO_GAP, 0);
        nina_dashboard_bind_tap(hrow, NINA_TAP_EXPOSURE);

        p->alt.lbl_hero = rg_label(hrow, RG_FONT_HERO, "", LV_TEXT_ALIGN_CENTER);
        p->alt.lbl_hero_unit = rg_label(hrow, RG_FONT_UNIT, "--",
                                        LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_translate_y(p->alt.lbl_hero_unit,
            RG_FONT_UNIT->base_line - RG_FONT_HERO->base_line, 0);

        p->alt.lbl_caption[RG_SLOT_DONE] =
            rg_label(parent, RG_FONT_DONE, "--", LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(p->alt.lbl_caption[RG_SLOT_DONE], RG_INNER_W,
                        lv_font_get_line_height(RG_FONT_DONE));
        lv_obj_align(p->alt.lbl_caption[RG_SLOT_DONE], LV_ALIGN_CENTER, 0,
                     ring_dy + RG_DONE_REL);
        nina_dashboard_bind_tap(p->alt.lbl_caption[RG_SLOT_DONE], NINA_TAP_SEQUENCE);

        lv_obj_t *cap_done = rg_label(parent, RG_FONT_CAP, "completed",
                                      LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(cap_done, RG_INNER_W, lv_font_get_line_height(RG_FONT_CAP));
        lv_obj_set_style_text_letter_space(cap_done, 2, 0);
        lv_obj_align(cap_done, LV_ALIGN_CENTER, 0, ring_dy + RG_DONEC_REL);
    }

    /* 5: the two columns beside the ring. Top down on the west: the sequence
     * name, the guiding RMS, the HFR. On the east: the running step, the frame
     * length, the star count. Those three and the countdown row below them are
     * four evenly spaced rows spanning the whole container (rg_row_cy()); the
     * hairline still sits on the ring's centre line, which falls in the gap
     * between the two data rows. The name row is clear of the ring entirely, so
     * its curve opens that column out to 327 px. */
    {
        const int arm_w = rg_arm_w();
        const int seq_w = rg_seq_w();
        const int top_s = rg_seq_top();
        const int top_1 = rg_arm_top(1);
        const int top_2 = rg_arm_top(2);

        p->lbl_seq_container = rg_stack(parent, true, seq_w, top_s,
                                        RG_FONT_SEQ, "sequence",
                                LV_TEXT_ALIGN_LEFT, RG_MARGIN);
        ui_fit_label(p->lbl_seq_container, rg_seq_ladder, RG_SEQ_LADDER_N, seq_w);
        nina_dashboard_bind_tap(p->lbl_seq_container, NINA_TAP_SEQUENCE);

        p->alt.lbl_rms = rg_stack(parent, true, arm_w, top_1, RG_FONT_ARM, "rms",
                                LV_TEXT_ALIGN_CENTER, RG_ARM_X);
        nina_dashboard_bind_tap(p->alt.lbl_rms, NINA_TAP_RMS);
        rg_arm_rule(parent, true, ring_dy);

        p->alt.lbl_hfr = rg_stack(parent, true, arm_w, top_2, RG_FONT_ARM, "hfr",
                                LV_TEXT_ALIGN_CENTER, RG_ARM_X);
        nina_dashboard_bind_tap(p->alt.lbl_hfr, NINA_TAP_HFR);

        p->alt.lbl_seq_step = rg_stack(parent, false, seq_w, top_s,
                                       RG_FONT_SEQ, "step",
                                LV_TEXT_ALIGN_RIGHT, RG_MARGIN);
        ui_fit_label(p->alt.lbl_seq_step, rg_seq_ladder, RG_SEQ_LADDER_N, seq_w);
        nina_dashboard_bind_tap(p->alt.lbl_seq_step, NINA_TAP_SEQUENCE);

        p->alt.lbl_caption[RG_SLOT_EXP_LEN] =
            rg_stack(parent, false, arm_w, top_1, RG_FONT_ARM, "exposure",
                                LV_TEXT_ALIGN_CENTER, RG_ARM_X);
        nina_dashboard_bind_tap(p->alt.lbl_caption[RG_SLOT_EXP_LEN],
                                NINA_TAP_EXPOSURE);
        rg_arm_rule(parent, false, ring_dy);

        p->alt.lbl_stars = rg_stack(parent, false, arm_w, top_2, RG_FONT_ARM,
                                    "stars",
                                LV_TEXT_ALIGN_CENTER, RG_ARM_X);
    }

    /* 6: the lower band, below both columns: the flip countdown toward the
     * lower left, the session limit toward the lower right, sharing one
     * computed width so neither can run under the ring. These two alone read
     * NAME then number, so the countdown is legible before it is read. See
     * rg_band_top() and rg_free_w() for the on-panel arithmetic. */
    {
        const int value_h = lv_font_get_line_height(RG_FONT_ARM);
        const int cap_h   = lv_font_get_line_height(RG_FONT_CAP);
        const int y_cap   = rg_band_top();
        const int y_val   = y_cap + cap_h + RG_CAP_GAP;
        const int low_w   = rg_free_w(y_cap, RG_MARGIN);

        lv_obj_t *cap_flip = rg_label(parent, RG_FONT_CAP, "flip in",
                                      LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(cap_flip, low_w, cap_h);
        lv_obj_set_style_text_letter_space(cap_flip, 2, 0);
        lv_obj_align(cap_flip, LV_ALIGN_TOP_LEFT, RG_MARGIN, y_cap);

        p->alt.lbl_flip = rg_label(parent, RG_FONT_ARM, "--", LV_TEXT_ALIGN_LEFT);
        lv_obj_set_size(p->alt.lbl_flip, low_w, value_h);
        lv_obj_align(p->alt.lbl_flip, LV_ALIGN_TOP_LEFT, RG_MARGIN, y_val);
        nina_dashboard_bind_tap(p->alt.lbl_flip, NINA_TAP_FLIP);

        p->alt.lbl_caption[RG_SLOT_LIMIT_CAP] =
            rg_label(parent, RG_FONT_CAP, "time limit", LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(p->alt.lbl_caption[RG_SLOT_LIMIT_CAP], low_w, cap_h);
        lv_obj_set_style_text_letter_space(p->alt.lbl_caption[RG_SLOT_LIMIT_CAP],
                                           2, 0);
        lv_obj_align(p->alt.lbl_caption[RG_SLOT_LIMIT_CAP], LV_ALIGN_TOP_RIGHT,
                     -RG_MARGIN, y_cap);

        p->alt.lbl_limit = rg_label(parent, RG_FONT_ARM, "--", LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_size(p->alt.lbl_limit, low_w, value_h);
        lv_obj_align(p->alt.lbl_limit, LV_ALIGN_TOP_RIGHT, -RG_MARGIN, y_val);
        nina_dashboard_bind_tap(p->alt.lbl_limit, NINA_TAP_SESSION);
    }

    /* 7: the power strip along the bottom. */
    rg_power_build(p, parent, cw);

    /* The 200 ms tick's sole sink for this page. The sub bar's own elapsed
     * callback is deliberately never registered: one sink per page. */
    p->alt.elapsed_cb = rg_elapsed_cb;

    rg_theme_page(p, gb);
}

/* ---- theme -------------------------------------------------------------- */

static void rg_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !current_theme) return;

    const bool     red   = rg_red();
    const uint32_t text  = rg_dim(current_theme->text_color, gb);
    const uint32_t label = rg_dim(current_theme->label_color, gb);
    const uint32_t prog  = rg_dim(red ? current_theme->text_color
                                      : current_theme->progress_color, gb);

    if (p->alt.arc_progress) {
        const lv_color_t c = lv_color_hex(prog);
        if (!lv_color_eq(lv_obj_get_style_arc_color(p->alt.arc_progress,
                                                    LV_PART_INDICATOR), c)) {
            lv_obj_set_style_arc_color(p->alt.arc_progress, c, LV_PART_INDICATOR);
        }
    }
    nina_subbar_apply_theme(&p->subbar);

    rg_set_color(p->alt.lbl_target,
                 rg_dim(red ? current_theme->text_color : RG_TARGET_FG, gb));
    rg_set_color(p->lbl_seq_container,
                 rg_dim(red ? current_theme->header_text_color : RG_SEQ_FG, gb));
    rg_set_color(p->alt.lbl_seq_step, text);
    rg_set_color(p->alt.lbl_hero, text);
    rg_set_color(p->alt.lbl_hero_unit, label);
    rg_set_color(p->alt.lbl_count, text);
    rg_set_color(p->alt.lbl_stars, text);
    rg_set_color(p->alt.lbl_flip, text);
    rg_set_color(p->alt.lbl_limit, text);
    rg_set_color(p->alt.lbl_caption[RG_SLOT_EXP_LEN],
                 rg_filter_color(p->subbar.cached_filter, p->alt.inst, gb));
    rg_set_color(p->alt.lbl_caption[RG_SLOT_DONE],
                 rg_filter_color(p->subbar.cached_filter, p->alt.inst, gb));
    rg_set_color(p->alt.lbl_filter,
                 rg_filter_color(p->subbar.cached_filter, p->alt.inst, gb));

    /* The RMS and HFR tones follow the live value; the next update() repaints
     * them from the configured thresholds, so this is only a resting colour. */
    rg_set_color(p->alt.lbl_rms,
                 rg_dim(red ? current_theme->rms_color
                            : current_theme->label_color, gb));
    rg_set_color(p->alt.lbl_hfr,
                 rg_dim(red ? current_theme->hfr_color
                            : current_theme->label_color, gb));

    /* The fixed captions carry no colour of their own, so this one write on
     * the page root re-tones every one of them. The spine's own children (the
     * stale badge, the offline overlay) each set their own colour, so none of
     * them inherits this. */
    rg_set_color(p->page, label);
    if (p->alt.grp_bottom) {
        lv_obj_t *row = lv_obj_get_child(p->alt.grp_bottom, 1);
        for (int i = 0; row && i < RG_PWR_CELLS; i++) {
            lv_obj_t *cell = lv_obj_get_child(row, i);
            if (cell) rg_set_color(lv_obj_get_child(cell, 0), text);
        }
    }
}

void nina_layout_rings_apply_theme(dashboard_page_t *p)
{
    if (!p) return;
    rg_theme_page(p, app_config_get()->color_brightness);
}

/* ---- view mode ---------------------------------------------------------- */

/* The square board has one composition and no picture behind it, so there is
 * nothing to cycle. Deliberately empty rather than absent: the spine dispatches
 * to this on both families. */
void nina_layout_rings_set_view(dashboard_page_t *p, nina_view_mode_t mode)
{
    LV_UNUSED(p);
    LV_UNUSED(mode);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_rings_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb)
{
    if (!p || !d || !current_theme) return;

    p->alt.inst = instance_idx;
    const bool red = rg_red();

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
                     screen_size() - 2 * RG_MARGIN);
    }

    /* Sequence container and running step, each refitted to the same column
     * width create() used, for the same reason the target name is: the face is
     * picked from the text, so a name that arrives after the first fit would
     * otherwise keep the previous name's face and dot instead of stepping down
     * to the 24 px one. */
    {
        const int seq_w = rg_seq_w();
        ui_label_set_text(p->lbl_seq_container,
            (d->container_name[0] != '\0') ? d->container_name : "----");
        ui_fit_label(p->lbl_seq_container, rg_seq_ladder, RG_SEQ_LADDER_N, seq_w);
        ui_label_set_text(p->alt.lbl_seq_step,
            (d->container_step[0] != '\0') ? d->container_step : "----");
        ui_fit_label(p->alt.lbl_seq_step, rg_seq_ladder, RG_SEQ_LADDER_N, seq_w);
    }

    /* Safety shield. */
    if (p->alt.lbl_safety) {
        const char *icon;
        uint32_t icon_color;
        if (!d->safety_connected) {
            icon = RG_ICON_UNKNOWN;
            icon_color = red ? current_theme->label_color : 0x999999;
        } else if (d->safety_is_safe) {
            icon = RG_ICON_SAFE;
            icon_color = red ? 0x7f1d1d : 0x4caf50;
        } else {
            icon = RG_ICON_UNSAFE;
            icon_color = red ? 0xff0000 : 0xf44336;
        }
        rg_set_text(p->alt.lbl_safety, icon);
        rg_set_color(p->alt.lbl_safety, rg_dim(icon_color, gb));
    }

    /* Filter name and loop count, and the exposure ring in the same colour. */
    {
        const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
        const uint32_t fc = rg_filter_color(filter, instance_idx, gb);

        rg_set_text(p->alt.lbl_filter, filter);
        rg_set_color(p->alt.lbl_filter, fc);

        char buf[32];
        if (d->exposure_iterations > 0) {
            snprintf(buf, sizeof(buf), "x %d / %d", d->exposure_count,
                     d->exposure_iterations);
        } else {
            snprintf(buf, sizeof(buf), "x %d", d->exposure_count);
        }
        rg_set_text(p->alt.lbl_count, buf);

        if (p->alt.arc_progress) {
            const lv_color_t c = lv_color_hex(fc);
            if (!lv_color_eq(lv_obj_get_style_arc_color(p->alt.arc_progress,
                                                        LV_PART_INDICATOR), c)) {
                lv_obj_set_style_arc_color(p->alt.arc_progress, c,
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
        ui_label_set_text(p->alt.lbl_caption[RG_SLOT_DONE], buf);
        rg_set_color(p->alt.lbl_caption[RG_SLOT_DONE],
                     rg_filter_color(d->current_filter, instance_idx, gb));
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
        rg_set_text(p->alt.lbl_caption[RG_SLOT_EXP_LEN], buf);
        rg_set_color(p->alt.lbl_caption[RG_SLOT_EXP_LEN],
                     rg_filter_color(d->current_filter, instance_idx, gb));
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
        rg_set_text(p->alt.lbl_rms, buf);
        rg_set_color(p->alt.lbl_rms, rg_dim(c, gb));
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
        rg_set_text(p->alt.lbl_hfr, buf);
        rg_set_color(p->alt.lbl_hfr, rg_dim(c, gb));
    }

    /* Star count. */
    {
        char buf[16];
        if (d->stars >= 0) {
            snprintf(buf, sizeof(buf), "%d", d->stars);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        rg_set_text(p->alt.lbl_stars, buf);
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
            rg_upper(cap);
        } else {
            snprintf(cap, sizeof(cap), "TIME LIMIT");
        }
        ui_label_set_text(p->alt.lbl_caption[RG_SLOT_LIMIT_CAP], cap);
    }

    /* The sub ring: the spine owns its progress and its stale dimming. */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* The power strip. */
    rg_power_write(p, d);

    /* The hero digits arrive from the 200 ms tick through p->alt.elapsed_cb;
     * only the idle reset lives here, routed through that same single writer. */
    if (d->exposure_total <= 0.0f) rg_elapsed_cb(p, -1);
}

#endif  /* !CONFIG_NINA_FAMILY_ROUND */
