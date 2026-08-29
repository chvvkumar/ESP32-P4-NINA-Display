/**
 * @file nina_layout_image_round.c
 * @brief NINA layout 1 (Image-forward) on a round panel: radial board 2.
 *
 * The approved square composition survives intact; only its two anchors move.
 * A circle has no bottom edge, so the 12 px block ledge becomes the panel's
 * outermost ring, one block per sub, and its notch at twelve o'clock is the
 * safety crown, which is where the shield used to sit beside the step. The
 * identity text goes on the rim as two arclabels (guideline G1), and the value
 * row keeps its shipped order, fonts and baseline, now on the chord at the
 * baseline 180 px below centre.
 *
 * The capture, its CONTAIN fit and the whole retained-buffer handoff are the
 * shipped ones in nina_layout_image.c, which stays compiled on both families
 * and whose square builder is compiled out of this binary.
 *
 * Runs with the LVGL display lock held by the caller.
 */

#include "nina_layout_alt.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_arclabel.h"
#include "ui_dial.h"
#include "ui_round.h"

LV_FONT_DECLARE(lv_font_hanken_bold_64);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens ------------------------------------------------------ */

#define IFR_R_LEDGE_OFF   12    /* ledge ring, offset from the rim radius */
#define IFR_W_LEDGE       14
#define IFR_CROWN_DEG     40

#define IFR_R_TARGET_OFF  40    /* target arclabel baseline radius */
#define IFR_R_STEP_OFF    86    /* step arclabel, one line further in */

/* Value row. IFR_ROW_DY is the TEXT BASELINE offset from the panel centre, not
 * the row's top: the digits sit on that chord. That chord alone is not enough
 * to keep the row clear of the ledge ring, whose 14 px stroke centred at
 * Rs - 12 reaches inward to Rs - 19: IFR_ROW_PAD trims the row further, to
 * the ring's inner edge with 4 px to spare (review C3 important I-1; the
 * original 8 px pad left the outer glyphs overlapping the ring by 13-14 px).
 * ui_chord_half(180) is 290 at 720 and 334 at 800, so the chord is 580 / 668
 * and the row, after the pad, is 528 / 616. The labels are content sized
 * rather than sample sized (review C important I-5): a worst case of RMS
 * 99.99" (182 px) + counter 999 / 999 (122 px) + elapsed 9999s (174 px) is
 * 478 px, so 528 has margin; a filter name wider than about 170 px at
 * Montserrat 24 would crowd the centre column (the shipped square row has the
 * same exposure). The cost is that the closing quote and the "s" walk by a
 * digit width when the digit count changes. */
#define IFR_ROW_DY       180
#define IFR_ROW_PAD       26    /* keep the row's ends inside the ledge ring's
                                  * inner edge (Rs - 19) with 4 px to spare */

#define IFR_CROWN_IDLE    0x2a2a2a
#define IFR_TARGET_FG     0xf2f2f4

#define IFR_FONT_TARGET   (&lv_font_montserrat_40)
#define IFR_FONT_STEP     (&lv_font_montserrat_28)
#define IFR_FONT_ELAPSED  (&lv_font_hanken_bold_64)   /* also the RMS value's font */
#define IFR_FONT_COUNT    (&lv_font_hanken_bold_28)
#define IFR_FONT_FILTER   (&lv_font_montserrat_24)

/* Last string written to each rim arclabel. lv_arclabel has no text getter and
 * re-lays every glyph on a write, so a shadow copy is what keeps the per-poll
 * update from relaying text that did not change (review C important I-6).
 * target_name and container_step are 64 bytes in nina_client.h. */
static char s_rim_target[MAX_NINA_INSTANCES][64];
static char s_rim_step[MAX_NINA_INSTANCES][64];

/* ---- helpers ------------------------------------------------------------ */

static bool      ifr_red(void);
static uint32_t  ifr_dim(uint32_t color, int gb);
static uint32_t  ifr_filter_color(const char *filter, int inst, int gb);
static lv_obj_t *ifr_label(lv_obj_t *parent, const lv_font_t *font, const char *text);
static lv_obj_t *ifr_value_label(lv_obj_t *parent);
static void      ifr_set_text(lv_obj_t *lbl, const char *text);
static void      ifr_set_arc_text(lv_obj_t *al, char *shadow, size_t n, const char *text);
static void      ifr_set_arc_color(lv_obj_t *arc, uint32_t color);
static void      ifr_elapsed_cb(void *ud, int secs);
static void      ifr_theme_page(dashboard_page_t *p, int gb);

static bool ifr_red(void) {
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t ifr_dim(uint32_t color, int gb) {
    return app_config_apply_brightness(color, gb);
}

/* Filter name tone: the configured filter colour, theme text on Red Night,
 * label tone when no filter is known. Already brightness applied. */
static uint32_t ifr_filter_color(const char *filter, int inst, int gb) {
    if (!current_theme) return ifr_dim(0x808080, gb);
    if (ifr_red()) return ifr_dim(current_theme->text_color, gb);
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, inst);
    }
    return ifr_dim(current_theme->label_color, gb);
}

static lv_obj_t *ifr_label(lv_obj_t *parent, const lv_font_t *font, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* Value-row label, always in the 64 px elapsed font. LVGL has no baseline, so
 * the row bottom-aligns its children (flex cross END); both callers use the
 * same font, so there is no other baseline to nudge onto (review C3 minor
 * M-3: the previous per-font translate_y was always zero here). */
static lv_obj_t *ifr_value_label(lv_obj_t *parent) {
    return ifr_label(parent, IFR_FONT_ELAPSED, "--");
}

static void ifr_set_text(lv_obj_t *lbl, const char *text) {
    if (lbl && text && strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

/* An arclabel re-lays every glyph on a text change, so write it only when the
 * text actually differs; @p shadow holds what was written last. */
static void ifr_set_arc_text(lv_obj_t *al, char *shadow, size_t n, const char *text) {
    if (!al || !shadow || !text) return;
    if (strcmp(shadow, text) == 0) return;
    snprintf(shadow, n, "%s", text);
    lv_arclabel_set_text(al, text);
}

/* A local style write always refreshes the object and every invalidation on
 * this display is a full-frame redraw, so the per-poll crown paint compares
 * first (review C minor M-4). */
static void ifr_set_arc_color(lv_obj_t *arc, uint32_t color) {
    if (!arc) return;
    lv_color_t c = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_arc_color(arc, LV_PART_MAIN), c)) {
        lv_obj_set_style_arc_color(arc, c, LV_PART_MAIN);
    }
}

/* Sole writer of the elapsed label: the 200 ms tick through the sub ring, and
 * the idle reset, which passes -1. */
static void ifr_elapsed_cb(void *ud, int secs) {
    dashboard_page_t *p = (dashboard_page_t *)ud;
    if (!p || !p->alt.lbl_elapsed) return;
    char buf[16];
    if (secs < 0) {
        snprintf(buf, sizeof(buf), "--s");
    } else {
        if (secs > 9999) secs = 9999;
        snprintf(buf, sizeof(buf), "%ds", secs);
    }
    ifr_set_text(p->alt.lbl_elapsed, buf);
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_image_create(dashboard_page_t *p, lv_obj_t *parent, int page_index) {
    if (!p || !parent) return;

    p->alt.inst = page_index;
    if (page_index >= 0 && page_index < MAX_NINA_INSTANCES) {
        s_rim_target[page_index][0] = '\0';
        s_rim_step[page_index][0]   = '\0';
    }

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 1: the capture, unchanged. CONTAIN is the shipped decision; on a circle
     * it costs two lens shaped caps instead of two bars, and the ring lives in
     * the top one. Kept alive with a NULL source so the tap target for the
     * full-screen preview is always present. */
    p->alt.cap_img = lv_image_create(parent);
    lv_obj_set_size(p->alt.cap_img, screen_size(), screen_size());
    lv_obj_center(p->alt.cap_img);
    lv_image_set_inner_align(p->alt.cap_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(p->alt.cap_img, NULL);
    nina_dashboard_bind_tap(p->alt.cap_img, NINA_TAP_CAPTURE);

    /* 2: the ledge, now the outermost ring, plus its crown at twelve o'clock.
     * lbl_safety names the crown: the Material shield is not drawn here. */
    nina_subbar_create_ring(&p->subbar, parent, ui_rim_radius() - IFR_R_LEDGE_OFF,
                            IFR_W_LEDGE, IFR_CROWN_DEG);
    nina_subbar_set_elapsed_cb(&p->subbar, ifr_elapsed_cb, p);
    p->alt.lbl_safety = ui_dial_arc(parent, ui_rim_radius() - IFR_R_LEDGE_OFF,
                                    IFR_W_LEDGE, -IFR_CROWN_DEG / 2, IFR_CROWN_DEG / 2);
    lv_obj_set_style_arc_color(p->alt.lbl_safety, lv_color_hex(IFR_CROWN_IDLE),
                               LV_PART_MAIN);

    /* 3: identity on the rim (guideline G1). Target on the outer arc, sequence
     * step on the one inside it, both centred on twelve o'clock. */
    p->alt.lbl_target = ui_arclabel_top(parent, IFR_FONT_TARGET,
                                        ui_rim_radius() - IFR_R_TARGET_OFF);
    p->alt.lbl_seq_step = ui_arclabel_top(parent, IFR_FONT_STEP,
                                          ui_rim_radius() - IFR_R_STEP_OFF);

    /* 4: the value row on the chord below the picture. The row box is placed so
     * the 64 px font's baseline lands on IFR_ROW_DY: the labels are bottom
     * aligned in a row one line high, so the baseline sits base_line px above
     * the row's bottom edge. */
    int row_h = lv_font_get_line_height(IFR_FONT_ELAPSED);
    int row_w = 2 * ui_chord_half(IFR_ROW_DY) - 2 * IFR_ROW_PAD;
    p->alt.row_vals = lv_obj_create(parent);
    lv_obj_remove_style_all(p->alt.row_vals);
    lv_obj_remove_flag(p->alt.row_vals, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.row_vals, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(p->alt.row_vals, row_w, row_h);
    lv_obj_align(p->alt.row_vals, LV_ALIGN_TOP_MID, 0,
                 screen_center() + IFR_ROW_DY + IFR_FONT_ELAPSED->base_line - row_h);
    lv_obj_set_style_pad_all(p->alt.row_vals, 0, 0);
    lv_obj_set_style_pad_gap(p->alt.row_vals, 0, 0);
    lv_obj_set_flex_flow(p->alt.row_vals, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p->alt.row_vals, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    /* Left: TOTAL RMS on the elapsed baseline. */
    p->alt.lbl_rms = ifr_value_label(p->alt.row_vals);
    nina_dashboard_bind_tap(p->alt.lbl_rms, NINA_TAP_RMS);

    /* Centre: counter over filter name. The page dots are hidden on this
     * layout, so the centre column is free. */
    lv_obj_t *grp_center = lv_obj_create(p->alt.row_vals);
    lv_obj_remove_style_all(grp_center);
    lv_obj_remove_flag(grp_center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(grp_center, row_h);
    lv_obj_set_width(grp_center, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grp_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grp_center, 2, 0);
    nina_dashboard_bind_tap(grp_center, NINA_TAP_SEQUENCE);

    p->alt.lbl_count = ifr_label(grp_center, IFR_FONT_COUNT, "--");
    lv_obj_set_style_text_align(p->alt.lbl_count, LV_TEXT_ALIGN_CENTER, 0);
    p->alt.lbl_filter = ifr_label(grp_center, IFR_FONT_FILTER, "");
    lv_obj_set_style_text_align(p->alt.lbl_filter, LV_TEXT_ALIGN_CENTER, 0);
    nina_dashboard_bind_tap(p->alt.lbl_filter, NINA_TAP_FILTER);

    /* Right: elapsed seconds. */
    p->alt.lbl_elapsed = ifr_value_label(p->alt.row_vals);
    lv_label_set_text(p->alt.lbl_elapsed, "--s");

    /* A rebuild that kept the retained frame re-attaches it. */
    nina_layout_image_reattach_capture(page_index);

    ifr_theme_page(p, app_config_get()->color_brightness);
}

/* ---- theme -------------------------------------------------------------- */

static void ifr_theme_page(dashboard_page_t *p, int gb) {
    if (!p || !p->alt.cap_img || !current_theme) return;

    bool red = ifr_red();
    uint32_t target_fg = red ? current_theme->text_color : IFR_TARGET_FG;
    uint32_t text      = ifr_dim(current_theme->text_color, gb);

    /* Both are ui_arclabel_top() results, which are NULL when the widget
     * allocation failed; the rest of this file guards every handle. */
    if (p->alt.lbl_target) {
        lv_obj_set_style_text_color(p->alt.lbl_target,
            lv_color_hex(ifr_dim(target_fg, gb)), 0);
    }
    if (p->alt.lbl_seq_step) {
        lv_obj_set_style_text_color(p->alt.lbl_seq_step,
            lv_color_hex(ifr_dim(current_theme->header_text_color, gb)), 0);
    }
    lv_obj_set_style_text_color(p->alt.lbl_count, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(p->alt.lbl_elapsed, lv_color_hex(text), 0);
    lv_obj_set_style_text_color(p->alt.lbl_filter,
        lv_color_hex(ifr_filter_color(p->subbar.cached_filter, p->alt.inst, gb)), 0);
    /* The RMS tone depends on the live value; the next update() repaints it. */
    lv_obj_set_style_text_color(p->alt.lbl_rms,
        lv_color_hex(ifr_dim(red ? current_theme->rms_color
                                 : current_theme->label_color, gb)), 0);
}

void nina_layout_image_apply_theme(dashboard_page_t *p) {
    if (!p) return;
    nina_layout_image_note_theme_switch(p->alt.inst);
    ifr_theme_page(p, app_config_get()->color_brightness);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_image_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb) {
    if (!p || !d || !p->alt.cap_img || !current_theme) return;

    p->alt.inst = instance_idx;
    bool red = ifr_red();

    /* Safety is the rim crown, not a glyph beside the step. */
    {
        uint32_t crown;
        if (!d->safety_connected)     crown = red ? current_theme->label_color : 0x999999;
        else if (d->safety_is_safe)   crown = red ? 0x7f1d1d : 0x4CAF50;
        else                          crown = red ? 0xff0000 : 0xF44336;
        ifr_set_arc_color(p->alt.lbl_safety, ifr_dim(crown, gb));
    }

    if (instance_idx >= 0 && instance_idx < MAX_NINA_INSTANCES) {
        ifr_set_arc_text(p->alt.lbl_seq_step, s_rim_step[instance_idx],
                         sizeof(s_rim_step[instance_idx]),
                         (d->container_step[0] != '\0') ? d->container_step : "--");
        ifr_set_arc_text(p->alt.lbl_target, s_rim_target[instance_idx],
                         sizeof(s_rim_target[instance_idx]),
                         (d->target_name[0] != '\0') ? d->target_name : "--");
    }

    /* Ledge ring: the spine owns set_progress() and apply_theme(). */
    nina_subbar_update(&p->subbar, d, instance_idx, gb);

    /* TOTAL RMS, threshold tone. */
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
        ifr_set_text(p->alt.lbl_rms, buf);
        lv_obj_set_style_text_color(p->alt.lbl_rms, lv_color_hex(ifr_dim(rms_c, gb)), 0);
    }

    /* Counter "done / target". */
    {
        char buf[32];
        int target = d->exposure_iterations;
        int done   = d->exposure_count;
        if (done < 0) done = 0;
        if (target > 0 && done > target) done = target;
        if (target > 0) {
            snprintf(buf, sizeof(buf), "%d / %d", done, target);
        } else {
            snprintf(buf, sizeof(buf), "%d", done);
        }
        ifr_set_text(p->alt.lbl_count, buf);
    }

    /* Filter name in its configured colour. */
    {
        const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
        ifr_set_text(p->alt.lbl_filter, filter);
        lv_obj_set_style_text_color(p->alt.lbl_filter,
            lv_color_hex(ifr_filter_color(filter, instance_idx, gb)), 0);
    }

    /* Elapsed: the tick writes the digits; only the idle reset lives here,
     * routed through the same writer. */
    if (d->exposure_total <= 0.0f) ifr_elapsed_cb(p, -1);
}
