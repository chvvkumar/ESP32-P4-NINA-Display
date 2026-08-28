/**
 * @file nina_summary_round.c
 * @brief Summary page on a round panel: radial board 3.
 *
 * Three stacked cards on a circle would hand the top and bottom rigs the two
 * worst chords, so every quantity moves to a ring instead: one concentric ring
 * per rig, outermost is slot 0, carrying sub blocks for progress, the filter
 * colour, a crown at twelve o'clock for safety and a tick for meridian flip.
 * The cards keep their glass and their tap to open but shed every number the
 * ring already tells, and each is cut to the chord it sits on, so the stack
 * reads as a diamond: narrow, wide, narrow.
 *
 * The page owns the data and the colours; this file creates and places widgets.
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_summary_internal.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_dial.h"
#include "ui_helpers.h"
#include "ui_round.h"

/* -- design tokens -------------------------------------------------------- */

#define SR_RING_PITCH     28    /* between ring centre lines */
#define SR_RING_OFF       12    /* outermost ring, offset from the rim radius */
#define SR_RING_W         14
#define SR_CROWN_DEG      30
#define SR_TICK_W         24

#define SR_CARD_H        120
#define SR_CARD_GAP       12
#define SR_CARD_PAD       22
#define SR_CARD_EDGE      12    /* keep a card's corners off the inner ring */

#define SR_BULL_R         26
#define SR_BULL_DOT       10
#define SR_BULL_INSET     48    /* bullseye centre from the card's near edge */
#define SR_ROW1_DY        20    /* identity row top, from the card top */
#define SR_BULL_DY        76    /* bullseye centre, from the card top */

#define SR_FONT_IDENT    (&lv_font_montserrat_28)
#define SR_FONT_TARGET   (&lv_font_montserrat_28)
#define SR_FONT_VALUE    (&lv_font_montserrat_40)

#define SR_RING_BORDER    0x262a30

/* Inner edge of the innermost ring: every card corner stays inside it. */
static int sr_inner_radius(void) {
    return ui_rim_radius() - SR_RING_OFF - 2 * SR_RING_PITCH - SR_RING_W;
}

/* Half width of a card whose furthest corner is dy_max from the panel centre. */
static int sr_card_half(int dy_max) {
    int rc = sr_inner_radius();
    if (dy_max < 0) dy_max = -dy_max;
    if (dy_max >= rc) return 0;
    return (int)sqrtf((float)(rc * rc - dy_max * dy_max)) - SR_CARD_EDGE;
}

static lv_obj_t *sr_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, const char *text);
static lv_obj_t *sr_bullseye(lv_obj_t *parent, int x, int y, uint32_t dot_color,
                             lv_obj_t **out_dot);

static lv_obj_t *sr_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* Tolerance ring plus a dot placed as its sibling, so an out-of-tolerance
 * value can sit outside the ring without being clipped. x and y are the ring
 * box's top-left in the card. */
static lv_obj_t *sr_bullseye(lv_obj_t *parent, int x, int y, uint32_t dot_color,
                             lv_obj_t **out_dot) {
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ring, 2 * SR_BULL_R, 2 * SR_BULL_R);
    lv_obj_set_pos(ring, x, y);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(SR_RING_BORDER), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);

    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, SR_BULL_DOT, SR_BULL_DOT);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    *out_dot = dot;
    return ring;
}

/* -- build ---------------------------------------------------------------- */

void nina_summary_round_create_card(summary_card_t *sc, lv_obj_t *parent, int slot) {
    if (!sc || !parent || !current_theme) return;
    memset(sc, 0, sizeof(*sc));
    sc->instance_index = slot;

    sc->cached_name_color        = UINT32_MAX;
    sc->cached_filter_text_color = UINT32_MAX;
    sc->cached_filter_bg_color   = UINT32_MAX;
    sc->cached_filter_bg_opa     = UINT8_MAX;
    sc->cached_target_color      = UINT32_MAX;
    sc->cached_bar_ind_color     = UINT32_MAX;
    sc->cached_bar_bg_color      = UINT32_MAX;
    sc->cached_pct_color         = UINT32_MAX;
    sc->cached_seq_name_color    = UINT32_MAX;
    sc->cached_exp_val_color     = UINT32_MAX;
    sc->cached_seq_step_color    = UINT32_MAX;
    sc->cached_rms_color         = UINT32_MAX;
    sc->cached_hfr_color         = UINT32_MAX;
    sc->cached_flip_color        = UINT32_MAX;
    sc->cached_detail_color      = UINT32_MAX;
    sc->cached_safety_color      = UINT32_MAX;

    int gb = app_config_get()->color_brightness;

    /* 1: this rig's ring set. Slot 0 outermost. */
    int r = ui_rim_radius() - SR_RING_OFF - slot * SR_RING_PITCH;
    nina_subbar_create_ring(&sc->ring, parent, r, SR_RING_W, SR_CROWN_DEG);
    sc->ring_crown = ui_dial_arc(parent, r, SR_RING_W,
                                 -SR_CROWN_DEG / 2, SR_CROWN_DEG / 2);
    lv_obj_set_style_arc_color(sc->ring_crown, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    sc->ring_flip_tick = ui_dial_arc(parent, r, SR_TICK_W, 0, 2);
    lv_obj_set_style_arc_color(sc->ring_flip_tick,
        lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)),
        LV_PART_MAIN);
    lv_obj_add_flag(sc->ring_flip_tick, LV_OBJ_FLAG_HIDDEN);

    /* 2: the card, on a fixed slot line and cut to the chord at its furthest
     * corner. Slot identity is fixed across the fleet, so a card does not move
     * when a neighbouring rig goes offline; it is simply not shown. */
    int dy   = (slot - 1) * (SR_CARD_H + SR_CARD_GAP);
    int half = sr_card_half((dy < 0 ? -dy : dy) + SR_CARD_H / 2);
    sc->card = lv_obj_create(parent);
    lv_obj_remove_style_all(sc->card);
    lv_obj_add_style(sc->card, &style_glass_card, 0);
    ui_styles_set_widget_draw_cbs(sc->card);
    lv_obj_set_layout(sc->card, LV_LAYOUT_NONE);
    lv_obj_set_size(sc->card, 2 * half, SR_CARD_H);
    lv_obj_align(sc->card, LV_ALIGN_CENTER, 0, dy);
    lv_obj_remove_flag(sc->card, LV_OBJ_FLAG_SCROLLABLE);
    /* Pad 0, not SR_CARD_PAD: every child below is placed absolutely and its
     * coordinates are measured from the card EDGE, while lv_obj_set_pos() is
     * relative to the content area. A pad here would be counted twice and
     * clip both bullseyes (review C I-3). */
    lv_obj_set_style_pad_all(sc->card, 0, 0);
    summary_bind_card_tap(sc->card, slot);

    /* 3: identity and target on one line. The ring says everything else. */
    lv_obj_t *row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, 2 * half - 2 * SR_CARD_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(row, SR_CARD_PAD, SR_ROW1_DY);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 10, 0);

    sc->lbl_name = sr_label(row, SR_FONT_IDENT,
        app_config_apply_brightness(current_theme->label_color, gb), "N.I.N.A.");
    lv_obj_set_flex_grow(sc->lbl_name, 1);

    lv_obj_t *sep = sr_label(row, SR_FONT_IDENT, 0x3a3f47, "/");
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);

    sc->lbl_target = sr_label(row, SR_FONT_TARGET,
        app_config_apply_brightness(current_theme->target_name_color, gb), "----");
    /* Both halves grow, so a long identity and a long target ellipsise instead
     * of one collapsing to nothing and the other overflowing (review C M-9). */
    lv_obj_set_flex_grow(sc->lbl_target, 1);

    /* 4: guiding RMS, the one figure a ring cannot express, beside its
     * bullseye; HFR is the bullseye alone. */
    sc->rms_bull = sr_bullseye(sc->card, SR_BULL_INSET - SR_BULL_R,
                               SR_BULL_DY - SR_BULL_R,
        app_config_apply_brightness(current_theme->rms_color, gb), &sc->rms_dot);
    sc->lbl_rms_val = sr_label(sc->card, SR_FONT_VALUE,
        app_config_apply_brightness(current_theme->rms_color, gb), "--");
    lv_obj_set_pos(sc->lbl_rms_val, SR_BULL_INSET + SR_BULL_R + 12, SR_BULL_DY - 22);

    sc->hfr_bull = sr_bullseye(sc->card, 2 * half - SR_BULL_INSET - SR_BULL_R,
                               SR_BULL_DY - SR_BULL_R,
        app_config_apply_brightness(current_theme->hfr_color, gb), &sc->hfr_dot);

    /* ui_dial_place_dot() reads the rings' resolved coordinates. */
    lv_obj_update_layout(sc->card);
    ui_dial_place_dot(sc->rms_dot, sc->rms_bull, 0.0f, SR_BEARING_RMS);
    ui_dial_place_dot(sc->hfr_dot, sc->hfr_bull, 0.0f, SR_BEARING_HFR);
}
