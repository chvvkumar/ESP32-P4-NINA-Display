/**
 * @file nina_layout_image_round.c
 * @brief NINA layout 1 (Image-forward) on a round panel: radial board 2.
 *
 * A circle has no bottom edge, so the 12 px block ledge becomes the panel's
 * outermost ring, one block per sub, flush with the glass the way the OctoPrint
 * round layout puts its progress rim there, closing the full circle with no
 * notch; safety is a Material shield glyph in the value row instead.
 *
 * Everything the square layout kept on one baseline now reads as one bottom
 * stack, top to bottom, the same shape octoprint_layout_glass_round.c uses:
 *
 *   rim top     target name (arclabel, guideline G1)
 *   row         TOTAL RMS | shield | sub counter | filter name
 *   rule        hairline separating the row from the hero
 *   hero        elapsed seconds, centred, 64 px Hanken
 *   rim bottom  sequence step (arclabel), between the hero and the ring
 *
 * The stack is measured UPWARD from the ring: the step cell sits just inside
 * the ring's inner edge, the hero clears the step, the rule clears the hero and
 * the row clears the rule, so both panel sizes get the same composition with
 * the extra diameter spent on chord width and on the picture in the middle.
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
#include "ui_round.h"

LV_FONT_DECLARE(lv_font_hanken_bold_64);
LV_FONT_DECLARE(lv_font_hanken_bold_28);
LV_FONT_DECLARE(lv_font_material_safety);

/* ---- design tokens ------------------------------------------------------ */

/* The ledge ring is FLUSH with the panel edge: its centreline is half a stroke
 * in, so the ring's outer edge lands on the glass and no ground shows between
 * it and the picture. Same rule as octoprint_layout_glass_round.c's rim; it
 * replaces the old ui_rim_radius() - 12 inset, which left an 11 px black band
 * outside the ring. */
#define IFR_W_LEDGE       14

static inline int ifr_ring_r(void)  { return screen_center() - IFR_W_LEDGE / 2; }

/* Outer edge of a rim glyph cell, a hairline inside the ring's inner edge.
 * ui_arclabel_* take the OUTER edge and grow the cell inward, so one radius
 * serves the target on the top rim and the step on the bottom rim. */
#define IFR_TEXT_GAP       6
static inline int ifr_text_r(void)
{
    return screen_center() - IFR_W_LEDGE - IFR_TEXT_GAP;
}

/* Bottom stack, measured upward from the step cell on the rim. The step cell
 * height comes from lv_font_get_line_height(IFR_FONT_STEP); the rest are
 * gaps, so a font swap moves the whole stack instead of overlapping the rim
 * text. */
#define IFR_STEP_GAP      10    /* hero bottom to the step cell top */
#define IFR_RULE_GAP      10    /* rule to the row above and the hero below */
#define IFR_RULE_W         2
#define IFR_RULE_COLOR    0x262a30

/* The value row's ends must stay inside the ring. ui_chord_half() measures on
 * ui_rim_radius() (Rs = 0.985 R), which is about 10 px wider than the ring's
 * inner edge now that the ring sits on the glass, so the pad covers that and
 * leaves margin: a worst case of RMS 99.99" (about 80 px now that it is in
 * the 28 px row font) + the 40 px shield + counter 999 / 999 (122 px) + a
 * filter name (about 170 px) is about 412 px against the 512 px the chord
 * gives at 720. */
#define IFR_ROW_PAD       26

#define IFR_TARGET_FG     0xf2f2f4

#define IFR_ICON_SAFE     "\xee\xa3\xa8"  /* U+E8E8 verified_user */
#define IFR_ICON_UNSAFE   "\xef\x80\x92"  /* U+F012 gpp_bad       */
#define IFR_ICON_UNKNOWN  "\xef\x80\x94"  /* U+F014 gpp_maybe     */

#define IFR_FONT_TARGET   (&lv_font_montserrat_28)
#define IFR_FONT_STEP     (&lv_font_montserrat_24)
#define IFR_FONT_ELAPSED  (&lv_font_hanken_bold_64)
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

/* RMS value label, in the row's own font so it matches the other readings on
 * that row. LVGL has no baseline, so the row bottom-aligns its children (flex
 * cross END). */
static lv_obj_t *ifr_value_label(lv_obj_t *parent) {
    return ifr_label(parent, IFR_FONT_COUNT, "--");
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

    /* 2: the ledge, now the outermost ring and flush with the glass, closing
     * the full circle with no notch. Safety moves to a shield glyph in the
     * value row (see lbl_safety below). */
    nina_subbar_create_ring(&p->subbar, parent, ifr_ring_r(),
                            IFR_W_LEDGE, 0);
    nina_subbar_set_elapsed_cb(&p->subbar, ifr_elapsed_cb, p);

    /* 3: identity on the rim (guideline G1). The target keeps twelve o'clock;
     * the sequence step moves to the BOTTOM rim, under the elapsed hero and
     * inside the ring, so the whole reading stack runs top to bottom in one
     * place instead of straddling the panel. */
    p->alt.lbl_target = ui_arclabel_top(parent, IFR_FONT_TARGET, ifr_text_r());
    p->alt.lbl_seq_step = ui_arclabel_bottom(parent, IFR_FONT_STEP, ifr_text_r());

    /* 4: the bottom stack, measured upward from the step cell on the rim.
     * lv_font_get_line_height() is asked for the hero's face rather than
     * hard-coding it, so a font swap moves the whole stack instead of
     * overlapping the rim text. */
    const int el_h      = lv_font_get_line_height(IFR_FONT_ELAPSED);
    const int step_h    = lv_font_get_line_height(IFR_FONT_STEP);
    const int el_bottom = ifr_text_r() - step_h - IFR_STEP_GAP;
    const int el_top    = el_bottom - el_h;
    const int rule_dy   = el_top - IFR_RULE_GAP;
    const int row_bottom = rule_dy - IFR_RULE_GAP;
    const int row_w     = 2 * ui_chord_half(row_bottom) - 2 * IFR_ROW_PAD;
    const int row_h     = LV_MAX(lv_font_get_line_height(IFR_FONT_COUNT),
                                 lv_font_get_line_height(&lv_font_material_safety));

    /* Row: TOTAL RMS, the shield, the sub counter and the filter name, all in
     * the row's own font. Children are bottom aligned: LVGL has no baseline,
     * and the shield's face differs from the text faces. */
    p->alt.row_vals = lv_obj_create(parent);
    lv_obj_remove_style_all(p->alt.row_vals);
    lv_obj_remove_flag(p->alt.row_vals, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->alt.row_vals, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(p->alt.row_vals, row_w, row_h);
    lv_obj_align(p->alt.row_vals, LV_ALIGN_TOP_MID, 0,
                 screen_center() + row_bottom - row_h);
    lv_obj_set_style_pad_all(p->alt.row_vals, 0, 0);
    lv_obj_set_style_pad_gap(p->alt.row_vals, 0, 0);
    lv_obj_set_flex_flow(p->alt.row_vals, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p->alt.row_vals, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    p->alt.lbl_rms = ifr_value_label(p->alt.row_vals);
    nina_dashboard_bind_tap(p->alt.lbl_rms, NINA_TAP_RMS);

    p->alt.lbl_safety = ifr_label(p->alt.row_vals, &lv_font_material_safety, IFR_ICON_UNKNOWN);

    p->alt.lbl_count = ifr_label(p->alt.row_vals, IFR_FONT_COUNT, "--");
    lv_obj_set_style_text_align(p->alt.lbl_count, LV_TEXT_ALIGN_CENTER, 0);
    nina_dashboard_bind_tap(p->alt.lbl_count, NINA_TAP_SEQUENCE);

    p->alt.lbl_filter = ifr_label(p->alt.row_vals, IFR_FONT_FILTER, "");
    lv_obj_set_style_text_align(p->alt.lbl_filter, LV_TEXT_ALIGN_RIGHT, 0);
    nina_dashboard_bind_tap(p->alt.lbl_filter, NINA_TAP_FILTER);

    /* Rule: the hairline that separates the row from the hero. A flat fixed
     * tone, like the ring's own unfilled blocks, so no theme handle is needed
     * and apply_theme has nothing to repaint. */
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, row_w, IFR_RULE_W);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0,
                 screen_center() + rule_dy - IFR_RULE_W / 2);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(IFR_RULE_COLOR), 0);

    /* Hero: the elapsed seconds, centred, with the step arclabel under it. */
    p->alt.lbl_elapsed = ifr_label(parent, IFR_FONT_ELAPSED, "--s");
    lv_obj_set_width(p->alt.lbl_elapsed, row_w);
    lv_obj_set_style_text_align(p->alt.lbl_elapsed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(p->alt.lbl_elapsed, LV_ALIGN_TOP_MID, 0,
                 screen_center() + el_top);
    nina_dashboard_bind_tap(p->alt.lbl_elapsed, NINA_TAP_EXPOSURE);

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

/* ---- view mode ---------------------------------------------------------- */

/* Layout 1 is not selectable on a round panel any more (layout_for_family
 * resolves a stored 1 to the Dashboard there), so this board has one
 * composition and nothing to switch. The definition stays because the spine
 * dispatches to it on every family. */
void nina_layout_image_set_view(dashboard_page_t *p, nina_view_mode_t mode) {
    LV_UNUSED(p);
    LV_UNUSED(mode);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_image_update(dashboard_page_t *p, const nina_client_t *d,
                              int instance_idx, int gb) {
    if (!p || !d || !p->alt.cap_img || !current_theme) return;

    p->alt.inst = instance_idx;
    bool red = ifr_red();

    /* Shield glyph in the value row. */
    {
        const char *icon;
        uint32_t icon_color;
        if (!d->safety_connected) {
            icon = IFR_ICON_UNKNOWN;
            icon_color = red ? current_theme->label_color : 0x999999;
        } else if (d->safety_is_safe) {
            icon = IFR_ICON_SAFE;
            icon_color = red ? 0x7f1d1d : 0x4CAF50;
        } else {
            icon = IFR_ICON_UNSAFE;
            icon_color = red ? 0xff0000 : 0xF44336;
        }
        ifr_set_text(p->alt.lbl_safety, icon);
        if (p->alt.lbl_safety) {
            lv_obj_set_style_text_color(p->alt.lbl_safety,
                lv_color_hex(ifr_dim(icon_color, gb)), 0);
        }
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
