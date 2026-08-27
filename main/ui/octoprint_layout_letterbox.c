/**
 * @file octoprint_layout_letterbox.c
 * @brief OctoPrint layout 6 — "Letterbox" (sandwich mockup 2B).
 *
 * Geometry only. Every widget comes from the shared library in
 * nina_octoprint_internal.h and every colour comes from an octo_color() token or
 * a shared style, so all nine themes work without a change here. This file never
 * reads octoprint_data_t.
 *
 * The page is a sandwich: two OPAQUE bands with the picture between them. No
 * text ever sits over the picture, which is the whole point of the design — the
 * image can be any brightness, any contrast, and the readings stay legible
 * because they are never on it. The image is CONTAINed rather than cropped, so
 * a tall G-code preview is shown whole with side bars rather than trimmed.
 *
 * Structure (720 x 720 full-bleed page, absolute placement, LV_LAYOUT_NONE):
 *     0   top band     72  STATUS | TIME ELAPSED | LAYER  ....  NOZZLE  BED
 *    78   error strip  28  absolute, hidden unless faulted (overlaps the image)
 *   112   conn chip    28  absolute, hidden while the link is healthy
 *    72   image       538  letterboxed hero, radius 0, border 0
 *   610   bottom band 110  61.8 %  ....  ETA 15:56 (content width)  / track
 *
 * Top band horizontal budget (worst-case samples, kerning ignored, so a few px
 * conservative), content box 720 - 2 x 16 pad = 688:
 *   left   173 status ("OPERATIONAL" bold_22 + 2)
 *          + 33 (16 margin + 1 rule + 16 margin) + 117 elapsed ("TIME ELAPSED"
 *          m14 + 12) + 33 + 134 layer ("9999" 56 + 6 gap + "/ 9999" 70 + 2) = 490
 *   right  68 NOZZLE + 24 gap + 73 BED = 165
 *   490 + 165 = 655 <= 688, 33 px to spare. The LAYER cell carries its own
 *   leading rule, so hiding it (no DisplayLayerProgress) takes the rule too.
 *
 * Bottom band horizontal budget, content box 720 - 2 x 24 pad - 2 border = 670:
 *   left   153 pct ("100.0" bold_56 + 2) + 4 + 27 "%" = 184
 *   right  35 "ETA" (m16 + letter space) + 10 + time, CONTENT-DRIVEN: the time
 *          label is its natural width and the cell packs from the row's right
 *          edge, so "ETA" rides 10 px off the first digit whether the time
 *          reads "10:27" (~160 px, cell ~205) or the worst-case 12-hour
 *          "12:56 PM" (~250 px, cell 295)
 *   184 + 295 = 479 <= 670 at worst.
 *
 * Each band is built TWICE (see band()): a bare filled rectangle parented to the
 * page, and a transparent twin of identical size, position and padding parented
 * to the single overlay layer (octo_w_overlay_layer() -> w->overlay_layer),
 * which carries the band's contents. The picture host stays on the page too, and
 * the two fault chips join the layer. So the "show readings" toggle — one HIDDEN
 * flag on the layer — empties both bands and clears the picture while the bands
 * themselves stay put, which is the shape the design asked for. Nothing moves
 * either way: every position on this page is absolute.
 *
 * Type scale (both bands, ~1.25x the first cut; band heights are UNCHANGED and
 * the extra height is taken out of the padding — see LB_TOP_PAD_VER and
 * LB_BOT_PAD_TOP for the two budgets):
 *   captions STATUS / TIME ELAPSED / LAYER / NOZZLE / BED  montserrat_14, l-s 1
 *   "ETA"                                           montserrat_16, letter-space 1
 *   state "Operational", temps "23.8°"              bold_22
 *   elapsed "01:35:00", layer "86 / 86"             montserrat_22
 *   percent "61.8" and finish "15:56"               bold_56
 *   "%"                                             bold_28
 *
 * The two bands are painted in OCTO_COL_CARDBG, not OCTO_COL_BG: the card fill
 * is the theme's "lifted" ground and it separates the bands from the picture
 * (and from a dark image's own black) without a shadow or a heavy border. A
 * single hairline in OCTO_COL_BORDER on the inner edge of each band draws the
 * sandwich line.
 *
 * The error strip and the connection chip are the only elements that break the
 * "no text on the picture" rule, and only while something is wrong. They are
 * placed absolutely just under the top band, so showing or hiding them reflows
 * nothing.
 *
 * This layout sets octoprint_layout_ops_t::full_bleed, so nina_octoprint.c hands
 * it the whole 720 x 720 screen with the dashboard's 16 px outer padding negated
 * — the bands run to the physical panel edge and carry their own padding.
 */

#include "display_defs.h"   /* screen_size(): LB_PAGE is the live panel width */
#include "nina_octoprint_internal.h"

/* ── Geometry ─────────────────────────────────────────────────────────── */

#define LB_PAGE        (screen_size())   /* full-bleed page is the whole panel */
#define LB_TOP_H        72
#define LB_BOT_H       110
#define LB_IMG_H       (LB_PAGE - LB_TOP_H - LB_BOT_H)   /* 538 */

#define LB_TOP_PAD_HOR  16
#define LB_BOT_PAD_HOR  24

/* Top band vertical budget, height UNCHANGED at 72 px. LVGL insets the content
 * box by the border width only on the sides named in border_side (here BOTTOM),
 * so the band really has 71 px of content; the budget below assumes 70 (one
 * spare px, conservative), and the
 * tallest cell in it is caption + 2 gap + value:
 *   state    16 (montserrat_14) + 2 + 23 (bold_22)       = 41
 *   elapsed  16 (montserrat_14) + 2 + 24 (montserrat_22) = 42
 *   layer    16 (montserrat_14) + 2 + 24 (montserrat_22) = 42
 *   temps    16 (montserrat_14) + 2 + 23 (bold_22)       = 41
 * 12 + 42 + 12 = 66 <= 70, so the nominal 12 px padding survives the 1.25x
 * scale with 4 px to spare. */
#define LB_TOP_PAD_VER  12

/* Bottom band vertical budget, packed from the BOTTOM edge (flex main END) so
 * the track keeps its clearance whatever the 56 px face measures. The band
 * height is UNCHANGED at 110 px; the 1.25x type scale is paid for out of the
 * padding. LVGL insets the content box by the border width only on the sides
 * named in border_side (here TOP), so the budget below is conservative by 1 px;
 * counting both edges, the band's 110 px break down as:
 *   1 border + 12 pad_top + 83 content + 13 pad_bottom + 1 border = 110
 * and the content box carries exactly
 *   59 row (lv_font_montserrat_bold_56 line_height) + 8 gap + 16 track = 83.
 * The row's height is the tallest child: the percentage and the finish time
 * are both bold_56 (59), the "ETA" caption 18 + 8 lift = 26 and the "%"
 * 28 + 6 lift = 34, all under 59. That leaves ZERO slack, so a taller
 * face cannot be dropped in here without taking it back out of LB_BOT_PAD_TOP
 * (floor 8) or LB_BOT_GAP (floor 6). The track's bottom edge sits 14 px clear
 * of the panel edge. */
#define LB_BOT_PAD_TOP  12
#define LB_BOT_PAD_BOT  13
#define LB_BOT_GAP       8
#define LB_TRACK_H      16

#define LB_DIV_W         1
#define LB_DIV_H        40
#define LB_DIV_MARGIN   16      /* each side */
#define LB_TEMP_GAP     24
#define LB_ETA_GAP      10      /* between "ETA" and the 56 px time */

/* Baseline lift for the small type beside a 56 px face. The row packs on its
 * bottom EDGE, but the eye wants a shared BASELINE, so each small label is
 * raised by the difference in font base_line (descender depth), read off the
 * generated font tables:
 *   bold_56 base_line 11, bold_28 base_line 5       -> 11 - 5 = 6  (the "%")
 *   bold_56 base_line 11, montserrat_16 base_line 3 -> 11 - 3 = 8  ("ETA") */
#define LB_LIFT_UNIT     6
#define LB_LIFT_ETA      8

/* Absolute slots for the two hidden-until-needed chips, stacked under the top
 * band. Fixed heights so the second slot's y is arithmetic, not layout. */
#define LB_CHIP_H       28
#define LB_ERR_Y        (LB_TOP_H + 6)
#define LB_CONN_Y       (LB_ERR_Y + LB_CHIP_H + 6)

/* Fixed-width samples: worst case for every value that changes, so nothing on
 * the page shifts sideways as the numbers move. */
#define LB_S_STATE      "OPERATIONAL"
#define LB_S_ELAPSED    "00:00:00"
#define LB_S_TEMP       "999.9\xC2\xB0"
#define LB_S_PCT        "100.0"
#define LB_S_LAYER_TOT  "/ 9999"

/* ── Small shapers ────────────────────────────────────────────────────── */

/**
 * Freeze @p lbl at the pixel width of @p sample in whatever font it already
 * carries, clipped rather than wrapped, aligned per the half of the screen it
 * sits on. Reading the font back off the label is what keeps the sample and the
 * face from drifting apart when either changes.
 */
static void fix_width(lv_obj_t *lbl, const char *sample, lv_text_align_t align)
{
    if (!lbl) {
        return;
    }
    const lv_font_t *font = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
    lv_obj_set_width(lbl, octo_text_width(font, sample) + 2);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl, align, 0);
}

/**
 * Caption at the sandwich type scale: uppercase, letter-space 1, justified to
 * its own half of the screen. octo_w_caption() is montserrat_14, which is what
 * the top-band captions want, so the font is re-set anyway rather than assumed;
 * the bottom one ("ETA") is stepped UP to 16.
 */
static lv_obj_t *caption(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, lv_text_align_t align)
{
    lv_obj_t *cap = octo_w_caption(parent, text);
    lv_obj_set_style_text_font(cap, font, 0);
    lv_obj_set_style_text_letter_space(cap, 1, 0);
    lv_obj_set_style_text_align(cap, align, 0);
    return cap;
}

/**
 * One band, built TWICE at the same size and position: once @p painted on the
 * page (the card fill plus its hairline, no children ever) and once transparent
 * on the overlay layer (the content twin every reading is parented to). An
 * object cannot have two parents, so this is how "the bars stay, just empty"
 * falls out of hiding one object: the toggle hides the layer, the twin goes with
 * it, and the painted rectangle underneath is untouched.
 *
 * The border width is kept at 1 on BOTH copies and only its colour goes
 * transparent on the twin, because LVGL insets the content box by the border
 * width on the sides named in border_side (border_opa is never consulted):
 * identical border width AND border_side is what keeps the twin's content
 * box, and therefore every vertical budget documented above (LB_TOP_PAD_VER,
 * LB_BOT_PAD_TOP), exactly as it was when the band carried its own children.
 */
static lv_obj_t *band(lv_obj_t *parent, int height, bool at_top, bool painted)
{
    lv_obj_t *b = octo_w_row(parent, true, 0);
    lv_obj_set_size(b, LV_PCT(100), height);
    lv_obj_align(b, at_top ? LV_ALIGN_TOP_LEFT : LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_side(b, at_top ? LV_BORDER_SIDE_BOTTOM
                                           : LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(b, 0, 0);

    if (painted) {
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(octo_color(OCTO_COL_CARDBG)), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
    } else {
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(b, LV_OPA_TRANSP, 0);
    }
    return b;
}

/**
 * COMPACT temperature element, right-justified in the top band. Caption and
 * value are frozen at ONE shared width — the wider of the two — so the pair
 * reads as a single right-aligned column and "23.8°" -> "215.0°" cannot nudge
 * anything beside it. No icon, no dot, no bar: the mockup dropped all three.
 */
static void compact_temp(lv_obj_t *parent, const char *name, bool hot,
                         octo_temp_el_t *t)
{
    octo_w_temp(parent, name, OCTO_TEMP_COMPACT, hot, t);
    if (!t->lbl_name || !t->lbl_value) {
        return;
    }
    const lv_font_t *cap_font = lv_obj_get_style_text_font(t->lbl_name, LV_PART_MAIN);
    const lv_font_t *val_font = lv_obj_get_style_text_font(t->lbl_value, LV_PART_MAIN);

    /* octo_text_width() measures at letter_space 0, and the caption is tracked
     * out by 1 px per character below, so its measurement is topped up by the
     * longest caption this layout passes ("NOZZLE", 6 characters) plus slack. */
    int32_t cap_w = octo_text_width(cap_font, name) + 8;
    int32_t val_w = octo_text_width(val_font, LB_S_TEMP);
    int32_t width = ((cap_w > val_w) ? cap_w : val_w) + 2;

    lv_obj_set_style_text_letter_space(t->lbl_name, 1, 0);
    lv_obj_set_width(t->lbl_name, width);
    lv_obj_set_style_text_align(t->lbl_name, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_set_width(t->lbl_value, width);
    lv_label_set_long_mode(t->lbl_value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(t->lbl_value, LV_TEXT_ALIGN_RIGHT, 0);
}

/* ── Image ────────────────────────────────────────────────────────────── */

static void build_image(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Built FIRST so both bands and both chips draw over it. It fills the gap
     * between the bands exactly; radius and border are dropped because the
     * bands already frame it top and bottom. */
    lv_obj_t *host = octo_w_image_hero(page, w);
    lv_obj_set_size(host, LV_PCT(100), LB_IMG_H);
    lv_obj_set_pos(host, 0, LB_TOP_H);
    lv_obj_set_style_radius(host, 0, 0);
    lv_obj_set_style_border_width(host, 0, 0);

    /* CONTAIN, not COVER: the whole frame stays visible, letterboxed inside the
     * box. stage_image() already aspect-fits the staged copy to this exact box,
     * so the zoom resolves to 1.0 in the steady state and no software transform
     * runs; CONTAIN only earns its keep for the one cycle after the box moves,
     * where it shrinks an oversized frame instead of cropping it. */
    if (w->img_hero) {
        lv_image_set_inner_align(w->img_hero, LV_IMAGE_ALIGN_CONTAIN);
    }
}

/* ── Top band ─────────────────────────────────────────────────────────── */

/** Vertical hairline in OCTO_COL_BORDER with LB_DIV_MARGIN either side. */
static lv_obj_t *hairline(lv_obj_t *parent)
{
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, LB_DIV_W, LB_DIV_H);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(octo_color(OCTO_COL_BORDER)), 0);
    lv_obj_set_style_margin_left(rule, LB_DIV_MARGIN, 0);
    lv_obj_set_style_margin_right(rule, LB_DIV_MARGIN, 0);
    return rule;
}

static void build_state_cell(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *cell = octo_w_row(parent, false, 2);
    lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    caption(cell, "STATUS", &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *line = octo_w_state_line(cell, w);
    lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* The mockup has no status dot — the state text is already the accent
     * element. Hidden rather than skipped so the update path keeps its handle
     * and recolours something harmless. */
    if (w->state_dot) {
        lv_obj_add_flag(w->state_dot, LV_OBJ_FLAG_HIDDEN);
    }
    if (w->lbl_state) {
        lv_obj_set_style_text_font(w->lbl_state, &lv_font_montserrat_bold_22, 0);
        lv_obj_set_style_text_letter_space(w->lbl_state, 0, 0);
        fix_width(w->lbl_state, LB_S_STATE, LV_TEXT_ALIGN_LEFT);
        /* The one label in this band that is NOT clipped: state_text() passes
         * the printer's own words through ("Printing from SD", "Offline after
         * error"), which run past the LB_S_STATE box, and an ellipsis reads as
         * a truncation while a clip reads as a broken word. */
        lv_label_set_long_mode(w->lbl_state, LV_LABEL_LONG_MODE_DOTS);
    }
}

static void build_elapsed_cell(lv_obj_t *parent, octoprint_widgets_t *w)
{
    lv_obj_t *cell = octo_w_row(parent, false, 2);
    lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    caption(cell, "TIME ELAPSED", &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);

    /* Regular weight, not bold: elapsed is context, the state above it and the
     * percentage below are the readings that carry the page. */
    w->lbl_elapsed = octo_w_label(cell, "--", &lv_font_montserrat_22,
                                  &octo_style_value);
    fix_width(w->lbl_elapsed, LB_S_ELAPSED, LV_TEXT_ALIGN_LEFT);
}

static void build_layer_cell(lv_obj_t *parent, octoprint_widgets_t *w)
{
    /* LAYER tile, dressed exactly like TIME ELAPSED. The cell is a ROW holding
     * its own leading hairline plus the caption/value column, so the update
     * path's single HIDDEN flag (no DisplayLayerProgress) removes the rule
     * along with the numbers and the left group closes up cleanly. */
    w->layer_cell = octo_w_row(parent, true, 0);
    lv_obj_set_size(w->layer_cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->layer_cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    hairline(w->layer_cell);

    lv_obj_t *cell = octo_w_row(w->layer_cell, false, 2);
    lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    caption(cell, "LAYER", &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);

    /* The two live numbers the update path writes as "245" and "/ 267". The
     * current layer keeps its natural width: the caption above is
     * left-justified, so the digits start under its first letter and any slack
     * falls at the cell's right end, where nothing lives (this is the last cell
     * of the left group, so a digit-count change moves no neighbour). */
    lv_obj_t *nums = octo_w_row(cell, true, 6);
    lv_obj_set_size(nums, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    w->lbl_layer_cur = octo_w_label(nums, "--", &lv_font_montserrat_22,
                                    &octo_style_value);
    w->lbl_layer_total = octo_w_label(nums, "/ --", &lv_font_montserrat_22,
                                      &octo_style_value);
    fix_width(w->lbl_layer_total, LB_S_LAYER_TOT, LV_TEXT_ALIGN_LEFT);
}

static void build_top_band(lv_obj_t *layer, octoprint_widgets_t *w)
{
    /* The transparent content twin; the painted rectangle is on the page. */
    lv_obj_t *bar = band(layer, LB_TOP_H, true, false);
    lv_obj_set_style_pad_hor(bar, LB_TOP_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(bar, LB_TOP_PAD_VER, 0);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Left half: state, elapsed and layer, all left-justified, split by
     * hairlines. */
    lv_obj_t *left = octo_w_row(bar, true, 0);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    build_state_cell(left, w);
    hairline(left);
    build_elapsed_cell(left, w);
    build_layer_cell(left, w);

    /* Right half: both temperatures, right-justified. */
    lv_obj_t *right = octo_w_row(bar, true, LB_TEMP_GAP);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    compact_temp(right, "NOZZLE", true, &w->nozzle);
    compact_temp(right, "BED", false, &w->bed);
}

/* ── Fault slots ──────────────────────────────────────────────────────── */

static void build_fault_slots(lv_obj_t *layer, octoprint_widgets_t *w)
{
    /* Both are placed absolutely and left-anchored just under the top band, so
     * appearing and disappearing moves nothing. Created LAST so they draw over
     * the picture. The strip is born hidden by its factory; the chip is hidden
     * by the update path while the link is healthy. They are readings like any
     * other, so they hang off the overlay layer and go with it. */
    octo_w_status_strip(layer, w);
    if (w->error_strip) {
        lv_obj_set_height(w->error_strip, LB_CHIP_H);
        lv_obj_set_pos(w->error_strip, 0, LB_ERR_Y);
        lv_obj_set_style_bg_opa(w->error_strip, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(w->error_strip,
                                  lv_color_hex(octo_color(OCTO_COL_BG)), 0);
        lv_obj_set_style_border_width(w->error_strip, 0, 0);
        lv_obj_set_style_radius(w->error_strip, 6, 0);
        lv_obj_set_style_pad_left(w->error_strip, LB_TOP_PAD_HOR, 0);
    }

    octo_w_conn_chip(layer, w);
    if (w->conn_chip) {
        lv_obj_set_height(w->conn_chip, LB_CHIP_H);
        lv_obj_set_pos(w->conn_chip, 0, LB_CONN_Y);
        lv_obj_set_style_bg_opa(w->conn_chip, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(w->conn_chip,
                                  lv_color_hex(octo_color(OCTO_COL_BG)), 0);
        lv_obj_set_style_border_width(w->conn_chip, 0, 0);
        lv_obj_set_style_radius(w->conn_chip, 6, 0);
        lv_obj_set_style_pad_left(w->conn_chip, LB_TOP_PAD_HOR, 0);
    }
}

/* ── Bottom band ──────────────────────────────────────────────────────── */

static void build_finish_cell(lv_obj_t *parent, octoprint_widgets_t *w)
{
    /* DisplayLayerProgress-sourced, so the caption and the value hide together.
     * "ETA" sits to the LEFT of the time on its baseline, exactly as the "%"
     * sits beside the percentage, and the time wears the percentage's own 56 px
     * face at its NATURAL width, not a worst-case box. The cell is the
     * right-hand end of the row and packs from its own right edge, so a change
     * of digits grows the cell leftward and "ETA" stays welded LB_ETA_GAP px off
     * the first digit. */
    w->finish_cell = octo_w_row(parent, true, LB_ETA_GAP);
    lv_obj_set_size(w->finish_cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(w->finish_cell, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *cap = caption(w->finish_cell, "ETA", &lv_font_montserrat_16,
                            LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_margin_bottom(cap, LB_LIFT_ETA, 0);

    w->lbl_finish = octo_w_label(w->finish_cell, "--",
                                 &lv_font_montserrat_bold_56, &octo_style_accent);
    lv_obj_set_style_text_letter_space(w->lbl_finish, 0, 0);
    lv_obj_set_width(w->lbl_finish, LV_SIZE_CONTENT);
}

static void build_bottom_band(lv_obj_t *layer, octoprint_widgets_t *w)
{
    /* The transparent content twin; the painted rectangle is on the page. */
    lv_obj_t *bar = band(layer, LB_BOT_H, false, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(bar, LB_BOT_PAD_HOR, 0);
    lv_obj_set_style_pad_top(bar, LB_BOT_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(bar, LB_BOT_PAD_BOT, 0);
    lv_obj_set_style_pad_row(bar, LB_BOT_GAP, 0);
    /* Packed from the bottom: the track keeps its LB_BOT_PAD_BOT clearance and
     * the readings sit above it. At the 56 px scale the column fills the content
     * box exactly (see LB_BOT_PAD_TOP), so END packing no longer has slack to
     * absorb — it now only guarantees that any future overflow eats into the top
     * of the band rather than pushing the track off the panel edge. */
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *row = octo_w_row(bar, true, 0);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    lv_obj_t *left = octo_w_row(row, true, 0);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    /* RIGHT aligned inside its fixed box, which is the one place this layout
     * departs from "left half, left justified": the unit has to stay welded to
     * the digits, and "61.8" drifting away from its "%" reads as a bug. The box
     * itself never moves, so nothing else on the row shifts. */
    w->lbl_pct = octo_w_label(left, "--", &lv_font_montserrat_bold_56,
                              &octo_style_value);
    fix_width(w->lbl_pct, LB_S_PCT, LV_TEXT_ALIGN_RIGHT);

    w->lbl_pct_unit = octo_w_label(left, "%", &lv_font_montserrat_bold_28,
                                   &octo_style_accent);
    lv_obj_set_style_margin_left(w->lbl_pct_unit, 4, 0);
    /* Lifted off the row's bottom edge so it sits on the 56 px baseline rather
     * than below it (the two faces have different descenders). */
    lv_obj_set_style_margin_bottom(w->lbl_pct_unit, LB_LIFT_UNIT, 0);

    build_finish_cell(row, w);

    /* Track: the band's full content width (720 - 2 x 24 pad - 2 x 1 border =
     * 670), flat, no border of its own. */
    octo_w_progress_bar(bar, w);
    if (!w->bar_progress) {
        return;
    }
    lv_obj_set_width(w->bar_progress, LV_PCT(100));
    lv_obj_set_height(w->bar_progress, LB_TRACK_H);
    lv_obj_set_style_radius(w->bar_progress, LB_TRACK_H / 2, 0);
    lv_obj_set_style_radius(w->bar_progress, LB_TRACK_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(w->bar_progress, 0, 0);
    lv_obj_set_style_bg_color(w->bar_progress,
                              lv_color_hex(octo_color(OCTO_COL_TEXT)), 0);
    lv_obj_set_style_bg_opa(w->bar_progress, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(w->bar_progress,
                              lv_color_hex(octo_color(OCTO_COL_ACCENT)),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(w->bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

static void letterbox_build(lv_obj_t *page, octoprint_widgets_t *w)
{
    /* Absolute placement: three stacked bands at fixed heights, plus two chips
     * that float over the picture without reflowing it. No flex flow on the
     * page can express that. */
    lv_obj_set_layout(page, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(page, 0, 0);

    /* Draw order is child order. On the page: the picture, then the two bare
     * band rectangles. Then one transparent full-page overlay layer, and on it
     * every reading — the two content twins and the fault chips on top. Hiding
     * the layer empties both bands and takes the chips off the picture; the
     * bands and the picture themselves stay, since they live on the page. */
    build_image(page, w);
    band(page, LB_TOP_H, true, true);
    band(page, LB_BOT_H, false, true);

    lv_obj_t *layer = octo_w_overlay_layer(page, w);
    build_top_band(layer, w);
    build_bottom_band(layer, w);
    build_fault_slots(layer, w);
}

const octoprint_layout_ops_t octoprint_layout_letterbox = {
    .name       = "Letterbox",
    .full_bleed = true,   /* three bands edge to edge: 720x720, no outer inset */
    .build      = letterbox_build,
};
