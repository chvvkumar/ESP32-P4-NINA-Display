/**
 * @file settings_hub_round.c
 * @brief Round composition of the Panel Mode settings screens, inscribed
 *        board 8 plus the addendum's theme-picker ruling.
 *
 * A fit pass, not a second builder: settings_hub.c builds every screen exactly
 * as it does on square, and this file re-places the result. Nothing here reads
 * config, sets a colour that the builder did not already set, or adds an event
 * callback, so the six tile callbacks, the demo accent, theme_card_cb and
 * hub_back_cb are the shipped ones.
 *
 * Display lock held by the caller.
 */

#include "settings_hub_internal.h"
#include "settings_wifi_internal.h"   /* settings_wifi_round_fit() */
#include "ui_round.h"
#include "screen_geom.h"
#include "lvgl.h"

/* Hub rows, absolute pixels, as offsets from the panel centre (board 8). */
#define HUBR_HEADER_W    420
#define HUBR_HEADER_H     72
#define HUBR_HEADER_DY  (-228)
#define HUBR_TILE_H      136
#define HUBR_A_W         270
#define HUBR_A_GAP        20
#define HUBR_A_DY       (-120)
#define HUBR_B_W         190
#define HUBR_B_GAP        16
#define HUBR_B_DY         32
#define HUBR_C_W         300
#define HUBR_C_DY        184

/* Theme picker (addendum ruling). */
#define HUBR_CARD_W      150
#define HUBR_CARD_H      140
#define HUBR_CARD_GAP_MAX 40
#define HUBR_BACK_W      140
#define HUBR_BACK_H       56

/* The shared header's BACK label ships at lv_font_montserrat_24, under the
 * 27 px round floor, and it appears on every settings and WiFi screen. Child 0
 * of the header is the BACK button, child 0 of that is its label. */
void settings_hub_round_header_font(lv_obj_t *header)
{
    if (!header) return;
    lv_obj_t *back = lv_obj_get_child(header, 0);
    lv_obj_t *lbl  = back ? lv_obj_get_child(back, 0) : NULL;
    if (lbl) lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    /* "< BACK" at 28 px is about 100 px; the shipped 96 px button clipped
     * the K (bench B12). The theme picker overrides this with its own pill. */
    if (back) lv_obj_set_width(back, HUB_BACK_W_ROUND);
}

/* Place one tile at an absolute offset from the screen centre and re-font it
 * for its new width. Child 0 is the name label, child 1 the status line (see
 * settings_hub_make_tile). Both get a bounded width and LV_LABEL_LONG_DOT: the
 * square tile is 330 wide and its name never needed one, but "DEMO MODE ON" at
 * 28 px does not fit a 190 px tile. */
static void fit_tile(lv_obj_t *tile, int w, int dx, int dy)
{
    if (!tile) return;
    lv_obj_set_size(tile, w, HUBR_TILE_H);
    lv_obj_align(tile, LV_ALIGN_CENTER, dx, dy);

    lv_obj_t *name = lv_obj_get_child(tile, 0);
    if (name) {
        /* montserrat_36 is the shipped tile face on both families and fits
         * every A/C-row name ("BRIGHTNESS" 239 px, "THEME" 132, "MORE" 114 in
         * the 246 px box); using it instead of a round-only montserrat_34
         * keeps that font object out of the round link entirely (about 53 KB
         * of glyph bitmap and kerning tables it was the only reference to). */
        lv_obj_set_style_text_font(name,
            (w >= HUBR_A_W) ? &lv_font_montserrat_36 : &lv_font_montserrat_28, 0);
        lv_obj_set_width(name, w - 24);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *status = lv_obj_get_child(tile, 1);
    if (status) {
        /* 24 px is under the round floor; 28 is the nearest enabled size. */
        lv_obj_set_style_text_font(status, &lv_font_montserrat_28, 0);
        lv_obj_set_width(status, w - 24);
    }
}

static void fit_hub_screen(lv_obj_t *screen)
{
    if (!hub_grid_obj) return;

    /* The column flex that stacked header over grid becomes absolute
     * placement: the chord decides where each row can live, not the flow. */
    lv_obj_set_layout(screen, LV_LAYOUT_NONE);

    if (hub_header_obj) {
        lv_obj_set_size(hub_header_obj, HUBR_HEADER_W, HUBR_HEADER_H);
        lv_obj_align(hub_header_obj, LV_ALIGN_CENTER, 0, HUBR_HEADER_DY);
        settings_hub_round_header_font(hub_header_obj);
    }

    /* The grid becomes a transparent full-screen placement layer; the six
     * tiles keep their identity, their order and their callbacks.
     *
     * LV_PCT, not lv_obj_get_width(screen): this pass runs inside
     * settings_hub_goto() immediately after lv_obj_create(s_screen), and LVGL
     * 9.5's size getters read obj->coords with no layout refresh, so a fresh
     * object still reports x2 = x1 - 1 (a negative width). Percent sizes are
     * resolved at layout time and are correct without an
     * lv_obj_update_layout() call. */
    lv_obj_set_layout(hub_grid_obj, LV_LAYOUT_NONE);
    lv_obj_set_size(hub_grid_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(hub_grid_obj, 0, 0);
    lv_obj_align(hub_grid_obj, LV_ALIGN_CENTER, 0, 0);
    /* The layer is child 1, drawn over the header (child 0), and a clickable
     * transparent layer wins the hit test over everything under it: the BACK
     * button was visible and dead (bench B12 on the 3.4C). The tiles keep
     * their own CLICKABLE; only the layer stops claiming taps. */
    lv_obj_remove_flag(hub_grid_obj, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t0 = lv_obj_get_child(hub_grid_obj, 0);   /* THEME */
    lv_obj_t *t1 = lv_obj_get_child(hub_grid_obj, 1);   /* BRIGHTNESS */
    lv_obj_t *t2 = lv_obj_get_child(hub_grid_obj, 2);   /* WIFI */
    lv_obj_t *t3 = lv_obj_get_child(hub_grid_obj, 3);   /* PAGES */
    lv_obj_t *t4 = lv_obj_get_child(hub_grid_obj, 4);   /* DEMO */
    lv_obj_t *t5 = lv_obj_get_child(hub_grid_obj, 5);   /* MORE */

    int ax = (HUBR_A_W + HUBR_A_GAP) / 2;
    fit_tile(t0, HUBR_A_W, -ax, HUBR_A_DY);
    fit_tile(t1, HUBR_A_W,  ax, HUBR_A_DY);

    int bx = HUBR_B_W + HUBR_B_GAP;
    fit_tile(t2, HUBR_B_W, -bx, HUBR_B_DY);
    fit_tile(t3, HUBR_B_W,   0, HUBR_B_DY);
    fit_tile(t4, HUBR_B_W,  bx, HUBR_B_DY);

    fit_tile(t5, HUBR_C_W, 0, HUBR_C_DY);
}

static void fit_theme_screen(lv_obj_t *screen)
{
    if (!hub_grid_obj) return;

    lv_obj_set_layout(screen, LV_LAYOUT_NONE);

    int gap = (ui_page_root_size() - 3 * HUBR_CARD_W) / 2;
    if (gap > HUBR_CARD_GAP_MAX) gap = HUBR_CARD_GAP_MAX;
    if (gap < 0) gap = 0;
    /* The inscribed-square width alone is not the binding constraint here: the
     * grid is a rectangle, so its CORNER decides. At 720 the root is 510 and
     * the formula above gives 30, which puts the corner 350 px out against a
     * 342 px rim. Shrink until the corner is on the chord. */
    while (gap > 0) {
        int half_w = (3 * HUBR_CARD_W + 2 * gap) / 2;
        int half_h = (3 * HUBR_CARD_H + 2 * gap) / 2;
        if (ui_chord_half(half_h) >= half_w) break;
        gap--;
    }
    int grid_w = 3 * HUBR_CARD_W + 2 * gap;
    int grid_h = 3 * HUBR_CARD_H + 2 * gap;

    lv_obj_set_layout(hub_grid_obj, LV_LAYOUT_NONE);
    lv_obj_set_size(hub_grid_obj, grid_w, grid_h);
    lv_obj_set_style_pad_all(hub_grid_obj, 0, 0);
    lv_obj_align(hub_grid_obj, LV_ALIGN_CENTER, 0, 0);

    uint32_t n = lv_obj_get_child_count(hub_grid_obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *card = lv_obj_get_child(hub_grid_obj, i);
        lv_obj_set_size(card, HUBR_CARD_W, HUBR_CARD_H);
        lv_obj_set_pos(card,
                       (int)(i % 3) * (HUBR_CARD_W + gap),
                       (int)(i / 3) * (HUBR_CARD_H + gap));
    }

    /* No room for a 72 px header above a grid this wide: reuse the header's own
     * BACK button as a pill on the bottom cap and drop the rest of the row, so
     * hub_back_cb stays the single back path and no new callback appears. */
    if (hub_header_obj) {
        settings_hub_round_header_font(hub_header_obj);
        lv_obj_t *back = lv_obj_get_child(hub_header_obj, 0);
        if (back) {
            lv_obj_set_parent(back, lv_obj_get_parent(hub_grid_obj));
            lv_obj_set_size(back, HUBR_BACK_W, HUBR_BACK_H);
            lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
            lv_obj_align(back, LV_ALIGN_CENTER, 0, grid_h / 2 + 44);
        }
        lv_obj_add_flag(hub_header_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Raise every label still using an under-floor Montserrat to 28, recursively:
 * the cycle chip labels and the More screen's info-row keys are grandchildren
 * of the screen (chip inside the chip grid, key inside the row), so a
 * direct-children-only walk never reaches them. Compares font POINTERS, not
 * line heights: lv_font_montserrat_24's line height is already 29, so a
 * height test would pass a 24 px face. Public, because the WiFi fit passes in
 * settings_wifi_round.c need the same sweep. */
void settings_hub_round_raise_small_labels(lv_obj_t *cont)
{
    if (!cont) return;
    uint32_t n = lv_obj_get_child_count(cont);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(cont, i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            const lv_font_t *f = lv_obj_get_style_text_font(c, LV_PART_MAIN);
            if (f == &lv_font_montserrat_26 || f == &lv_font_montserrat_24 ||
                f == &lv_font_montserrat_22 || f == &lv_font_montserrat_20 ||
                f == &lv_font_montserrat_18 || f == &lv_font_montserrat_16 ||
                f == &lv_font_montserrat_14 || f == &lv_font_montserrat_12) {
                lv_obj_set_style_text_font(c, &lv_font_montserrat_28, 0);
            }
            continue;
        }
        settings_hub_round_raise_small_labels(c);
    }
}

/* Brightness, Pages, More: the shipped column flex, pulled inside the chord.
 * pad_h is 0 on square, so this whole function is a no-op there (it is not
 * compiled there either). The vertical pad is 3/2 of the horizontal one so the
 * topmost and bottom-most full-width rows clear the chord at their own y, and
 * that costs enough height that these screens have to scroll. */
static void fit_flex_screen(lv_obj_t *screen)
{
    int pad_h = ui_page_inset() - UI_SQUARE_INSET;
    if (pad_h <= 0) return;
    lv_obj_set_style_pad_left(screen, pad_h, 0);
    lv_obj_set_style_pad_right(screen, pad_h, 0);
    lv_obj_set_style_pad_top(screen, pad_h * 3 / 2, 0);
    lv_obj_set_style_pad_bottom(screen, pad_h * 3 / 2, 0);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(screen, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_AUTO);

    settings_hub_round_header_font(hub_header_obj);

    /* A grow child (the Pages screen's CYCLE chip grid) would otherwise get
     * track_main_size - track_fix_main_size from LVGL's flex layout, which on
     * this padded, scrolling column is a sliver (32 px at 720) that never
     * shows a whole chip. Let the screen's own vertical scroll carry the
     * content instead: zero the grow and size the child to its natural
     * height. Brightness and More have no grow children, so this is a no-op
     * there. */
    uint32_t gn = lv_obj_get_child_count(screen);
    for (uint32_t i = 0; i < gn; i++) {
        lv_obj_t *c = lv_obj_get_child(screen, i);
        if (lv_obj_get_style_flex_grow(c, LV_PART_MAIN) > 0) {
            lv_obj_set_flex_grow(c, 0);
            lv_obj_set_height(c, LV_SIZE_CONTENT);
        }
    }

    /* 27 px floor sweep. These screens are flex columns of shipped widgets and
     * three of them still carry lv_font_montserrat_24 text after the padding
     * pass: the brightness hint line, the More screen's "Everything else:"
     * caption, and the cycle chip labels and info-row keys the recursive walk
     * now reaches. */
    settings_hub_round_raise_small_labels(screen);
}

void settings_hub_round_fit(lv_obj_t *screen, hub_screen_t which)
{
    if (!screen) return;
    switch (which) {
    case HUB_SCREEN_HUB:
        fit_hub_screen(screen);
        break;
    case HUB_SCREEN_THEME:
        fit_theme_screen(screen);
        break;
    case HUB_SCREEN_WIFI_HOME:
    case HUB_SCREEN_WIFI_SCAN:
    case HUB_SCREEN_WIFI_PASSWORD:
    case HUB_SCREEN_WIFI_CONNECT:
        settings_wifi_round_fit(screen, which);
        break;
    default:
        fit_flex_screen(screen);
        break;
    }
}
