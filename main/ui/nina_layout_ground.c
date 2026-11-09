/**
 * @file nina_layout_ground.c
 * @brief Layout 8 ("Readout on the ground"), BOTH panel families, one body.
 *
 * The shared sky scene (nina_sky_scene.h) fills the panel; its frame stops at
 * the ground band, and this file writes one calm readout on that band, in the
 * owner's reading order: the target name, then RMS | hero | HFR, then the
 * filter chip with the picture count and the flip line.
 *
 * Geometry is taken at runtime: every reference value below is for a 720 px
 * panel and is scaled by screen_size() / 720 (gr_sc()). On a round panel the
 * row widths come from the chord of the safe circle at the row's far edge
 * (gr_round_w()), never from the square width.
 *
 * The hero has ONE writer, gr_hero_write(), called by the spine's 200 ms tick
 * through p->alt.elapsed_cb (gr_elapsed_cb) and by update(). The tick's seconds
 * are cached per instance so update() can re-word the hero without inventing a
 * second count.
 *
 * Full-refresh panel: every text, colour and flag write below is compared
 * first, so an unchanged value never repaints the screen.
 *
 * All entry points run with the LVGL display lock held by the caller; update()
 * also runs inside the caller's nina_client lock.
 */

#include "nina_layout_alt.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_text_fit.h"
#include "nina_sky_scene.h"
#include "nina_sky_words.h"

LV_FONT_DECLARE(lv_font_hanken_bold_64);
LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens (720 px reference) ----------------------------------- */

#define GR_AMBER        0xf4b860
#define GR_INK          0x0b1019   /* filter name on the chip */

#define GR_FONT_CAP     (&lv_font_montserrat_16)
#define GR_FONT_VAL     (&lv_font_hanken_bold_28)
#define GR_FONT_TOTAL   (&lv_font_hanken_bold_28)
#define GR_FONT_CHIP    (&lv_font_montserrat_16)
#define GR_FONT_COUNT   (&lv_font_montserrat_24)
#define GR_FONT_FLIP    (&lv_font_montserrat_22)

#define GR_CHIP_H       30
#define GR_CHIP_PAD     8
#define GR_ROW_GAP      9
#define GR_ROUND_TRIM   40   /* round rows: chord minus this */

/* Hero faces: Hanken 64 carries digits and words (full ASCII); a word that
 * does not fit the hero box steps down through Montserrat. */
#define GR_HERO_LADDER_N 4
static const lv_font_t *const gr_hero_ladder[GR_HERO_LADDER_N] = {
    &lv_font_hanken_bold_64, &lv_font_montserrat_48,
    &lv_font_montserrat_40,  &lv_font_montserrat_34,
};

enum {
    GR_SLOT_RMS_CAP = 0,   /* p->alt.lbl_caption[0]: "RMS" */
    GR_SLOT_HFR_CAP = 1,   /* p->alt.lbl_caption[1]: "HFR" */
};

/* Per-instance cache the tick and update() share. */
static int      s_gr_secs[MAX_NINA_INSTANCES];
static float    s_gr_total[MAX_NINA_INSTANCES];
static float    s_gr_cur[MAX_NINA_INSTANCES];   /* last polled exposure_current */
static uint32_t s_gr_fc[MAX_NINA_INSTANCES];
static char     s_gr_filter[MAX_NINA_INSTANCES][32];

/* ---- forward declarations ----------------------------------------------- */

static int       gr_inst(const dashboard_page_t *p);
static int       gr_sc(int v);
static int       gr_chord(int y);
static int       gr_round_w(int cy, int h);
static int       gr_target_w(void);
static int       gr_hero_w(void);
static bool      gr_red(void);
static uint32_t  gr_dim(uint32_t color, int gb);
static uint32_t  gr_fixed(uint32_t raw, int gb);
static bool      gr_filter_known(const char *filter);
static uint32_t  gr_filter_color(const char *filter, int inst, int gb);
static void      gr_show(lv_obj_t *obj, bool show);
static void      gr_set_text(lv_obj_t *lbl, const char *text);
static void      gr_set_color(lv_obj_t *obj, uint32_t rgb);
static void      gr_set_bg(lv_obj_t *obj, uint32_t rgb);
static lv_obj_t *gr_label(lv_obj_t *parent, const lv_font_t *font, const char *text,
                          int w, int cx, int cy);
static void      gr_hero_write(dashboard_page_t *p);
static void      gr_elapsed_cb(dashboard_page_t *p, int secs);
static void      gr_theme_page(dashboard_page_t *p, int gb);

/* ---- small helpers ------------------------------------------------------ */

static int gr_inst(const dashboard_page_t *p)
{
    int i = p ? p->alt.inst : 0;
    if (i < 0) {
        i = 0;
    }
    if (i >= MAX_NINA_INSTANCES) {
        i = MAX_NINA_INSTANCES - 1;
    }
    return i;
}

/* Scale a 720 px reference value to this panel, rounded. */
static int gr_sc(int v)
{
    return (v * screen_size() + 360) / 720;
}

/* Full chord of the safe circle at panel y; 0 outside it. */
static int gr_chord(int y)
{
    const int r  = screen_safe_radius();
    const int dy = y - screen_center();
    if (dy >= r || -dy >= r) {
        return 0;
    }
    return (int)(2.0f * sqrtf((float)(r * r - dy * dy)));
}

/* Round row width for a row of height @p h centred at @p cy: the chord at the
 * row's edge farthest from the panel centre, less GR_ROUND_TRIM. */
static int gr_round_w(int cy, int h)
{
    const int edge = (cy >= screen_center()) ? (cy + h / 2) : (cy - h / 2);
    const int w = gr_chord(edge) - gr_sc(GR_ROUND_TRIM);
    return (w > 60) ? w : 60;
}

static int gr_target_w(void)
{
    if (!SCREEN_ROUND) {
        return gr_sc(600);
    }
    return gr_round_w(gr_sc(466), lv_font_get_line_height(UI_FIT_LADDER_NAME_28[0]));
}

static int gr_hero_w(void)
{
    return gr_sc(SCREEN_ROUND ? 240 : 300);
}

static bool gr_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t gr_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

/* A fixed design colour: Red Night luminance map, then brightness. */
static uint32_t gr_fixed(uint32_t raw, int gb)
{
    return gr_dim(gr_red() ? theme_red_luma(raw) : raw, gb);
}

static bool gr_filter_known(const char *filter)
{
    return filter && filter[0] != '\0' && strcmp(filter, "--") != 0;
}

/* Filter tone as the other boards pick it: configured colour (already
 * brightness applied), theme text on Red Night, label tone when unknown. */
static uint32_t gr_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) {
        return gr_dim(0x808080, gb);
    }
    if (gr_red()) {
        return gr_dim(current_theme->text_color, gb);
    }
    if (gr_filter_known(filter)) {
        return app_config_get_filter_color(filter, inst);
    }
    return gr_dim(current_theme->label_color, gb);
}

static void gr_show(lv_obj_t *obj, bool show)
{
    if (!obj) {
        return;
    }
    const bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (show && hidden) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else if (!show && !hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Set-if-changed text for labels that never ellipsise. */
static void gr_set_text(lv_obj_t *lbl, const char *text)
{
    if (!lbl || !text) {
        return;
    }
    if (strcmp(lv_label_get_text(lbl), text) == 0) {
        return;
    }
    lv_label_set_text(lbl, text);
}

static void gr_set_color(lv_obj_t *obj, uint32_t rgb)
{
    if (!obj) {
        return;
    }
    const lv_color_t c = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), c)) {
        return;
    }
    lv_obj_set_style_text_color(obj, c, 0);
}

static void gr_set_bg(lv_obj_t *obj, uint32_t rgb)
{
    if (!obj) {
        return;
    }
    const lv_color_t c = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), c)) {
        return;
    }
    lv_obj_set_style_bg_color(obj, c, 0);
}

/* One-line label, @p w px wide (0 = content width), centred at panel (cx, cy). */
static lv_obj_t *gr_label(lv_obj_t *parent, const lv_font_t *font, const char *text,
                          int w, int cx, int cy)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text ? text : "");
    if (w > 0) {
        lv_obj_set_size(l, w, lv_font_get_line_height(font));
    }
    lv_obj_align(l, LV_ALIGN_CENTER, cx - screen_center(), cy - screen_center());
    return l;
}

/* ---- the hero ----------------------------------------------------------- */

static void gr_hero_write(dashboard_page_t *p)
{
    if (!p || !p->alt.lbl_hero || !current_theme) {
        return;
    }
    const int inst = gr_inst(p);
    const int gb   = app_config_get()->color_brightness;
    const int secs = s_gr_secs[inst];
    const nina_sky_info_t *info = p->alt.sky ? nina_sky_scene_info(p->alt.sky) : NULL;
    const sky_activity_t act = info ? info->activity
                                    : ((secs >= 0) ? SKY_ACT_EXPOSING : SKY_ACT_RESTING);

    char hero[24];
    bool dim = false;
    bool show_total = false;

    switch (act) {
        case SKY_ACT_EXPOSING: {
            /* Full brightness while a picture runs; the same seconds layout 9
             * shows. With no clock at all the place stays blank, never a dim 0. */
            const int v = nina_sky_words_secs(true, secs, s_gr_cur[inst]);
            if (v >= 0) {
                snprintf(hero, sizeof(hero), "%d", v);
                show_total = true;
            } else {
                hero[0] = '\0';
            }
            break;
        }
        case SKY_ACT_WAITING:
            snprintf(hero, sizeof(hero), "0");
            dim = true;
            show_total = s_gr_total[inst] > 0.0f;
            break;
        case SKY_ACT_FLIPPING:
            snprintf(hero, sizeof(hero), "flipping");
            break;
        case SKY_ACT_MOVING:
            snprintf(hero, sizeof(hero), "moving");
            break;
        case SKY_ACT_DONE:
            snprintf(hero, sizeof(hero), "done");
            break;
        case SKY_ACT_BELOW: {
            char t[8];
            if (info && info->rise_known &&
                nina_sky_scene_clock_after(p->alt.sky, info->hours_to_rise, t, sizeof(t))) {
                snprintf(hero, sizeof(hero), "up at %s", t);
            } else {
                snprintf(hero, sizeof(hero), "resting");
            }
            break;
        }
        case SKY_ACT_RESTING:
        default:
            snprintf(hero, sizeof(hero), "resting");
            break;
    }

    ui_label_set_text(p->alt.lbl_hero, hero);
    ui_fit_label(p->alt.lbl_hero, gr_hero_ladder, GR_HERO_LADDER_N, gr_hero_w());
    gr_set_color(p->alt.lbl_hero, gr_dim(dim ? current_theme->label_color
                                             : current_theme->text_color, gb));

    if (show_total && s_gr_total[inst] > 0.0f) {
        float t = s_gr_total[inst];
        if (t > 99999.0f) {
            t = 99999.0f;
        }
        int ti = (int)(t + 0.5f);
        if (ti < 1) {
            ti = 1;
        }
        char tot[24];
        snprintf(tot, sizeof(tot), "/ %d s", ti);
        gr_set_text(p->alt.lbl_exp_total, tot);
        gr_show(p->alt.lbl_exp_total, true);
    } else {
        gr_show(p->alt.lbl_exp_total, false);
    }
}

/* The spine's 200 ms tick (and its idle reset with -1). */
static void gr_elapsed_cb(dashboard_page_t *p, int secs)
{
    if (!p) {
        return;
    }
    const int inst = gr_inst(p);
    s_gr_secs[inst] = secs;
    gr_hero_write(p);

    if (p->alt.sky) {
        float frac = -1.0f;
        if (secs >= 0 && p->cached_total > 0.0f) {
            frac = (float)secs / p->cached_total;
        }
        nina_sky_scene_set_exposure(p->alt.sky, frac, s_gr_fc[inst]);
    }
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_ground_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent) {
        return;
    }

    p->alt.inst = page_index;
    const int inst = gr_inst(p);
    s_gr_secs[inst]      = -1;
    s_gr_total[inst]     = 0.0f;
    s_gr_cur[inst]       = 0.0f;
    s_gr_fc[inst]        = 0;
    s_gr_filter[inst][0] = '\0';

    const int  gb  = app_config_get()->color_brightness;
    const bool rnd = SCREEN_ROUND;
    const int  S   = screen_size();
    const int  c   = screen_center();

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* The scene first, so every word below draws over it. */
    {
        const nina_sky_scene_opts_t opts = { .ground_band_top = gr_sc(rnd ? 430 : 456) };
        p->alt.sky = nina_sky_scene_create(parent, &opts, page_index);
    }

    /* Target name. */
    p->alt.lbl_target = gr_label(parent, UI_FIT_LADDER_NAME_28[0], "----", 0,
                                 c, gr_sc(rnd ? 466 : 494));
    ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME_28, UI_FIT_LADDER_NAME_28_N,
                 gr_target_w());
    nina_dashboard_bind_tap(p->alt.lbl_target, NINA_TAP_CAPTURE);

    /* RMS and HFR, caption over value. */
    {
        const int w     = gr_sc(rnd ? 120 : 160);
        const int y_cap = gr_sc(rnd ? 512 : 548);
        const int y_val = gr_sc(rnd ? 544 : 582);
        const int x_rms = gr_sc(rnd ? 210 : 170);
        const int x_hfr = gr_sc(rnd ? 510 : 550);

        p->alt.lbl_caption[GR_SLOT_RMS_CAP] = gr_label(parent, GR_FONT_CAP, "RMS", w,
                                                       x_rms, y_cap);
        p->alt.lbl_rms = gr_label(parent, GR_FONT_VAL, "", w, x_rms, y_val);
        nina_dashboard_bind_tap(p->alt.lbl_rms, NINA_TAP_RMS);

        p->alt.lbl_caption[GR_SLOT_HFR_CAP] = gr_label(parent, GR_FONT_CAP, "HFR", w,
                                                       x_hfr, y_cap);
        p->alt.lbl_hfr = gr_label(parent, GR_FONT_VAL, "", w, x_hfr, y_val);
        nina_dashboard_bind_tap(p->alt.lbl_hfr, NINA_TAP_HFR);

        gr_show(p->alt.lbl_caption[GR_SLOT_RMS_CAP], false);
        gr_show(p->alt.lbl_rms, false);
        gr_show(p->alt.lbl_caption[GR_SLOT_HFR_CAP], false);
        gr_show(p->alt.lbl_hfr, false);
    }

    /* Hero and the exposure length under it. */
    p->alt.lbl_hero = gr_label(parent, gr_hero_ladder[0], "", gr_hero_w(),
                               c, gr_sc(rnd ? 526 : 568));
    nina_dashboard_bind_tap(p->alt.lbl_hero, NINA_TAP_EXPOSURE);
    p->alt.lbl_exp_total = gr_label(parent, GR_FONT_TOTAL, "", gr_hero_w(),
                                    c, gr_sc(rnd ? 570 : 620));
    gr_show(p->alt.lbl_exp_total, false);

    /* Filter chip plus picture count, one row. */
    {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, gr_sc(GR_ROW_GAP), 0);
        if (rnd) {
            const int y = gr_sc(614);
            lv_obj_set_style_max_width(row, gr_round_w(y, gr_sc(GR_CHIP_H)), 0);
            lv_obj_align(row, LV_ALIGN_CENTER, 0, y - c);
        } else {
            lv_obj_align(row, LV_ALIGN_LEFT_MID, gr_sc(77), gr_sc(670) - c);
        }
        nina_dashboard_bind_tap(row, NINA_TAP_SEQUENCE);
        p->alt.grp_bottom = row;

        lv_obj_t *chip = lv_obj_create(row);
        lv_obj_remove_style_all(chip);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, gr_sc(GR_CHIP_H));
        lv_obj_set_style_min_width(chip, gr_sc(GR_CHIP_H), 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(chip, gr_sc(GR_CHIP_PAD), 0);
        lv_obj_set_style_pad_right(chip, gr_sc(GR_CHIP_PAD), 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        p->alt.ring_inner = chip;

        p->alt.lbl_filter = lv_label_create(chip);
        lv_obj_set_style_text_font(p->alt.lbl_filter, GR_FONT_CHIP, 0);
        lv_label_set_text(p->alt.lbl_filter, "");

        p->alt.lbl_count = lv_label_create(row);
        lv_obj_set_style_text_font(p->alt.lbl_count, GR_FONT_COUNT, 0);
        lv_label_set_text(p->alt.lbl_count, "");

        gr_show(chip, false);
        gr_show(p->alt.lbl_count, false);
    }

    /* Flip line. */
    {
        const int h = lv_font_get_line_height(GR_FONT_FLIP);
        if (rnd) {
            const int y = gr_sc(650);
            p->alt.lbl_flip = gr_label(parent, GR_FONT_FLIP, "", gr_round_w(y, h), c, y);
        } else {
            p->alt.lbl_flip = gr_label(parent, GR_FONT_FLIP, "", gr_sc(300), c, gr_sc(670));
            lv_obj_set_style_text_align(p->alt.lbl_flip, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(p->alt.lbl_flip, LV_ALIGN_RIGHT_MID, -(S - gr_sc(628)),
                         gr_sc(670) - c);
        }
        nina_dashboard_bind_tap(p->alt.lbl_flip, NINA_TAP_FLIP);
        gr_show(p->alt.lbl_flip, false);
    }

    p->alt.elapsed_cb = gr_elapsed_cb;

    gr_theme_page(p, gb);   /* ends with gr_hero_write() */
}

/* ---- theme -------------------------------------------------------------- */

static void gr_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !current_theme) {
        return;
    }
    const int      inst  = gr_inst(p);
    const uint32_t text  = gr_dim(current_theme->text_color, gb);
    const uint32_t label = gr_dim(current_theme->label_color, gb);

    gr_set_color(p->alt.lbl_target, gr_fixed(GR_AMBER, gb));
    gr_set_color(p->alt.lbl_caption[GR_SLOT_RMS_CAP], label);
    gr_set_color(p->alt.lbl_caption[GR_SLOT_HFR_CAP], label);
    gr_set_color(p->alt.lbl_exp_total, label);
    gr_set_color(p->alt.lbl_count, text);
    gr_set_color(p->alt.lbl_flip, label);
    gr_set_color(p->alt.lbl_filter, gr_fixed(GR_INK, gb));

    s_gr_fc[inst] = gr_filter_color(s_gr_filter[inst], inst, gb);
    gr_set_bg(p->alt.ring_inner, s_gr_fc[inst]);

    /* Resting tones; the next update() repaints them from the thresholds. */
    gr_set_color(p->alt.lbl_rms, gr_dim(gr_red() ? current_theme->rms_color
                                                 : current_theme->text_color, gb));
    gr_set_color(p->alt.lbl_hfr, gr_dim(gr_red() ? current_theme->hfr_color
                                                 : current_theme->text_color, gb));

    if (p->alt.sky) {
        nina_sky_scene_apply_theme(p->alt.sky, gb);
    }
    gr_hero_write(p);
}

void nina_layout_ground_apply_theme(dashboard_page_t *p)
{
    if (!p) {
        return;
    }
    gr_theme_page(p, app_config_get()->color_brightness);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_ground_update(dashboard_page_t *p, const nina_client_t *d,
                               int instance_idx, int gb)
{
    if (!p || !d || !current_theme) {
        return;
    }

    p->alt.inst = instance_idx;
    const int  inst = gr_inst(p);
    const bool red  = gr_red();

    if (p->alt.sky) {
        nina_sky_scene_update(p->alt.sky, d, instance_idx, gb);
    }
    const nina_sky_info_t *info = p->alt.sky ? nina_sky_scene_info(p->alt.sky) : NULL;
    const sky_activity_t act = info ? info->activity
                                    : (d->is_exposing ? SKY_ACT_EXPOSING : SKY_ACT_RESTING);

    s_gr_total[inst] = d->exposure_total;
    s_gr_cur[inst]   = d->exposure_current;
    if (act != SKY_ACT_EXPOSING) {
        s_gr_secs[inst] = -1;   /* the tick's last value belongs to the finished picture */
    }

    /* Target name, refitted whenever it changes. */
    if (p->alt.lbl_target) {
        ui_label_set_text(p->alt.lbl_target,
                          (d->target_name[0] != '\0') ? d->target_name : "----");
        ui_fit_label(p->alt.lbl_target, UI_FIT_LADDER_NAME_28, UI_FIT_LADDER_NAME_28_N,
                     gr_target_w());
    }

    /* RMS and HFR, only while working on the target and with a value. */
    {
        const bool working = (act == SKY_ACT_EXPOSING || act == SKY_ACT_WAITING);
        const bool show_rms = working && d->guider.rms_total > 0.0f;
        const bool show_hfr = working && d->hfr > 0.0f;
        char buf[24];

        if (show_rms) {
            float v = d->guider.rms_total;
            if (v > 999.0f) {
                v = 999.0f;
            }
            snprintf(buf, sizeof(buf), "%.2f", (double)v);
            gr_set_text(p->alt.lbl_rms, buf);
            gr_set_color(p->alt.lbl_rms, gr_dim(red ? current_theme->rms_color
                                   : app_config_get_rms_color(d->guider.rms_total, instance_idx), gb));
        }
        gr_show(p->alt.lbl_caption[GR_SLOT_RMS_CAP], show_rms);
        gr_show(p->alt.lbl_rms, show_rms);

        if (show_hfr) {
            float v = d->hfr;
            if (v > 999.0f) {
                v = 999.0f;
            }
            snprintf(buf, sizeof(buf), "%.2f", (double)v);
            gr_set_text(p->alt.lbl_hfr, buf);
            gr_set_color(p->alt.lbl_hfr, gr_dim(red ? current_theme->hfr_color
                                   : app_config_get_hfr_color(d->hfr, instance_idx), gb));
        }
        gr_show(p->alt.lbl_caption[GR_SLOT_HFR_CAP], show_hfr);
        gr_show(p->alt.lbl_hfr, show_hfr);
    }

    /* Filter chip. */
    {
        const bool known = gr_filter_known(d->current_filter);
        if (known) {
            snprintf(s_gr_filter[inst], sizeof(s_gr_filter[inst]), "%s", d->current_filter);
            gr_set_text(p->alt.lbl_filter, s_gr_filter[inst]);
        } else {
            s_gr_filter[inst][0] = '\0';
        }
        s_gr_fc[inst] = gr_filter_color(s_gr_filter[inst], instance_idx, gb);
        gr_set_bg(p->alt.ring_inner, s_gr_fc[inst]);
        gr_show(p->alt.ring_inner, known);
    }

    /* Picture count. */
    {
        char buf[32];
        const bool show = nina_sky_words_count(d, buf, sizeof(buf));
        if (show) {
            gr_set_text(p->alt.lbl_count, buf);
        }
        gr_show(p->alt.lbl_count, show);
    }

    /* Flip line. */
    {
        const bool show = info && info->flip_known && act != SKY_ACT_FLIPPING;
        if (show) {
            char buf[32];
            nina_sky_words_flip(p->alt.sky, info->minutes_to_flip, buf, sizeof(buf));
            gr_set_text(p->alt.lbl_flip, buf);
        }
        gr_show(p->alt.lbl_flip, show);
    }

    gr_hero_write(p);
}
