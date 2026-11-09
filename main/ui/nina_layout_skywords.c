/**
 * @file nina_layout_skywords.c
 * @brief Layout 9 ("In the sky"), BOTH panel families, one body.
 *
 * The shared sky scene (nina_sky_scene.h) fills the whole safe area, and this
 * file writes the words next to the objects they belong to: the target block
 * (name, picture count, filter and seconds) beside the target, the flip line
 * beside the flip mark, and the rig word ("resting", "moving", "flipping")
 * beside the telescope.
 *
 * Placement (sw_place(), run from update() and from the scene's moved
 * callback while the view flies or the mount moves) tries two or three fixed
 * spots per label and scores each one: off the glass, over the telescope, over
 * a drawn mark, over a label already placed. The spot the label used last time
 * gets a bonus, so a label stays put unless that spot really went bad, and the
 * words never jump from one poll to the next. Every spot is checked against
 * the scene frame (nina_sky_scene_box_fits()), which on a round panel is the
 * safe circle, so a label that is shown always sits fully on the glass when
 * any of its spots allows it.
 *
 * The seconds have ONE writer: p->alt.elapsed_cb (sw_elapsed_cb), the spine's
 * 200 ms tick, which also feeds the scene's exposure ring.
 *
 * Full-refresh panel: every text, colour, position and flag write below is
 * compared first, so an unchanged value never repaints the screen.
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

LV_FONT_DECLARE(lv_font_hanken_bold_28);

/* ---- design tokens (720 px reference) ----------------------------------- */

#define SW_AMBER        0xf4b860
#define SW_CYAN         0x6fd3e0
#define SW_COUNT_OPA    217        /* 85 % */

#define SW_FONT_NAME    (&lv_font_montserrat_26)
#define SW_FONT_COUNT   (&lv_font_montserrat_24)
#define SW_FONT_FILTER  (&lv_font_montserrat_26)
#define SW_FONT_SECS    (&lv_font_hanken_bold_28)
#define SW_FONT_FLIP    (&lv_font_montserrat_24)
#define SW_FONT_RIG     (&lv_font_montserrat_24)

#define SW_NAME_MAX_W   300        /* longest target name box before dots */
#define SW_FALLBACK_Y   120        /* target block top when there is no anchor */

/* Spot offsets from the anchor. */
#define SW_TGT_SIDE     40
#define SW_TGT_BELOW    40
#define SW_FLIP_ABOVE   24
#define SW_FLIP_SIDE    24
#define SW_RIG_SIDE     12
#define SW_RIG_ABOVE    8
#define SW_RIG_GROW     8

/* Scores. */
#define SW_PEN_OFF      1000
#define SW_PEN_RIG      300
#define SW_PEN_MARK     150
#define SW_PEN_LABEL    100
#define SW_BONUS_PREV   60

#define SW_N_SPOTS      3

enum { SW_L_TARGET = 0, SW_L_FLIP = 1, SW_L_RIG = 2, SW_L_N = 3 };

/* Target block spots. */
enum { SW_T_RIGHT = 0, SW_T_LEFT = 1, SW_T_BELOW = 2 };

/* Per-instance state. */
static uint8_t  s_sw_choice[MAX_NINA_INSTANCES][SW_L_N];
static int      s_sw_secs[MAX_NINA_INSTANCES];
static float    s_sw_total[MAX_NINA_INSTANCES];
static float    s_sw_cur[MAX_NINA_INSTANCES];   /* last polled exposure_current */
static bool     s_sw_run[MAX_NINA_INSTANCES];   /* a picture is running now */
static uint32_t s_sw_fc[MAX_NINA_INSTANCES];
static char     s_sw_name[MAX_NINA_INSTANCES][64];
static bool     s_sw_flip_on[MAX_NINA_INSTANCES];
static bool     s_sw_rig_on[MAX_NINA_INSTANCES];
static bool     s_sw_flipping[MAX_NINA_INSTANCES];

/* ---- forward declarations ----------------------------------------------- */

static int       sw_inst(const dashboard_page_t *p);
static int       sw_sc(int v);
static bool      sw_red(void);
static uint32_t  sw_dim(uint32_t color, int gb);
static uint32_t  sw_fixed(uint32_t raw, int gb);
static bool      sw_filter_known(const char *filter);
static uint32_t  sw_filter_color(const char *filter, int inst, int gb);
static void      sw_show(lv_obj_t *obj, bool show);
static void      sw_set_text(lv_obj_t *lbl, const char *text);
static void      sw_set_color(lv_obj_t *obj, uint32_t rgb);
static lv_obj_t *sw_label(lv_obj_t *parent, const lv_font_t *font, const char *text);
static lv_obj_t *sw_group(lv_obj_t *parent, lv_flex_flow_t flow);
static void      sw_hero_write(dashboard_page_t *p);
static void      sw_set_name(dashboard_page_t *p, const char *name);
static void      sw_box(lv_area_t *a, int x, int y, int w, int h);
static bool      sw_overlap(const lv_area_t *a, const lv_area_t *b);
static int       sw_pick(const dashboard_page_t *p, const lv_point_t *spots, int w, int h,
                         uint8_t prev, const lv_area_t *rig, const lv_area_t *placed,
                         int n_placed, int *score_out);
static void      sw_move(lv_obj_t *obj, int x, int y);
static void      sw_block_align(dashboard_page_t *p, int choice);
static void      sw_place(dashboard_page_t *p);
static void      sw_moved_cb(void *user);
static void      sw_elapsed_cb(dashboard_page_t *p, int secs);
static void      sw_theme_page(dashboard_page_t *p, int gb);

/* ---- small helpers ------------------------------------------------------ */

static int sw_inst(const dashboard_page_t *p)
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

static int sw_sc(int v)
{
    return (v * screen_size() + 360) / 720;
}

static bool sw_red(void)
{
    return current_theme && theme_is_red_night(current_theme);
}

static uint32_t sw_dim(uint32_t color, int gb)
{
    return app_config_apply_brightness(color, gb);
}

static uint32_t sw_fixed(uint32_t raw, int gb)
{
    return sw_dim(sw_red() ? theme_red_luma(raw) : raw, gb);
}

static bool sw_filter_known(const char *filter)
{
    return filter && filter[0] != '\0' && strcmp(filter, "--") != 0;
}

static uint32_t sw_filter_color(const char *filter, int inst, int gb)
{
    if (!current_theme) {
        return sw_dim(0x808080, gb);
    }
    if (sw_red()) {
        return sw_dim(current_theme->text_color, gb);
    }
    if (sw_filter_known(filter)) {
        return app_config_get_filter_color(filter, inst);
    }
    return sw_dim(current_theme->label_color, gb);
}

static void sw_show(lv_obj_t *obj, bool show)
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

static void sw_set_text(lv_obj_t *lbl, const char *text)
{
    if (!lbl || !text) {
        return;
    }
    if (strcmp(lv_label_get_text(lbl), text) == 0) {
        return;
    }
    lv_label_set_text(lbl, text);
}

static void sw_set_color(lv_obj_t *obj, uint32_t rgb)
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

/* Content-sized one-line label. */
static lv_obj_t *sw_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* Transparent, content-sized flex group. */
static lv_obj_t *sw_group(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, flow);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return g;
}

/* Target name, boxed to its own text width up to SW_NAME_MAX_W, then dots.
 * Only rewritten when the name changes. */
static void sw_set_name(dashboard_page_t *p, const char *name)
{
    lv_obj_t *l = p->alt.lbl_target;
    if (!l || !name) {
        return;
    }
    const int inst = sw_inst(p);
    if (strncmp(s_sw_name[inst], name, sizeof(s_sw_name[inst]) - 1) == 0) {
        return;
    }
    strncpy(s_sw_name[inst], name, sizeof(s_sw_name[inst]) - 1);
    s_sw_name[inst][sizeof(s_sw_name[inst]) - 1] = '\0';

    lv_point_t sz;
    lv_text_get_size(&sz, name, SW_FONT_NAME, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int max_w = sw_sc(SW_NAME_MAX_W);
    int w = (int)sz.x + 1;
    if (w > max_w) {
        w = max_w;
    }
    if (w < 1) {
        w = 1;
    }
    lv_obj_set_size(l, w, lv_font_get_line_height(SW_FONT_NAME));
    ui_label_set_text(l, name);
}

static void sw_box(lv_area_t *a, int x, int y, int w, int h)
{
    a->x1 = x;
    a->y1 = y;
    a->x2 = x + w - 1;
    a->y2 = y + h - 1;
}

static bool sw_overlap(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* Lowest score over SW_N_SPOTS spots (ties to the lower index). @p rig NULL =
 * do not score against the telescope. */
static int sw_pick(const dashboard_page_t *p, const lv_point_t *spots, int w, int h,
                   uint8_t prev, const lv_area_t *rig, const lv_area_t *placed,
                   int n_placed, int *score_out)
{
    int best = 0;
    int best_score = 0;
    for (int i = 0; i < SW_N_SPOTS; i++) {
        lv_area_t box;
        sw_box(&box, (int)spots[i].x, (int)spots[i].y, w, h);
        int score = 0;
        if (!nina_sky_scene_box_fits(p->alt.sky, &box)) {
            score += SW_PEN_OFF;
        }
        if (rig && sw_overlap(&box, rig)) {
            score += SW_PEN_RIG;
        }
        if (nina_sky_scene_box_hits_marks(p->alt.sky, &box)) {
            score += SW_PEN_MARK;
        }
        for (int k = 0; k < n_placed; k++) {
            if (sw_overlap(&box, &placed[k])) {
                score += SW_PEN_LABEL;
            }
        }
        if (i == (int)prev) {
            score -= SW_BONUS_PREV;
        }
        if (i == 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    *score_out = best_score;
    return best;
}

static void sw_move(lv_obj_t *obj, int x, int y)
{
    if (!obj) {
        return;
    }
    if (lv_obj_get_x(obj) == x && lv_obj_get_y(obj) == y) {
        return;
    }
    lv_obj_set_pos(obj, x, y);
}

/* Text alignment follows the side: right of the anchor reads left aligned,
 * left of it right aligned, below it centred. */
static void sw_block_align(dashboard_page_t *p, int choice)
{
    lv_flex_align_t cross;
    switch (choice) {
        case SW_T_LEFT:
            cross = LV_FLEX_ALIGN_END;
            break;
        case SW_T_BELOW:
            cross = LV_FLEX_ALIGN_CENTER;
            break;
        case SW_T_RIGHT:
        default:
            cross = LV_FLEX_ALIGN_START;
            break;
    }
    lv_obj_set_flex_align(p->alt.grp_mid, LV_FLEX_ALIGN_START, cross, cross);
}

/* ---- placement ---------------------------------------------------------- */

static void sw_place(dashboard_page_t *p)
{
    if (!p || !p->alt.grp_mid) {
        return;
    }
    const int inst = sw_inst(p);
    nina_sky_scene_t *sky = p->alt.sky;

    lv_obj_update_layout(p->alt.grp_mid);

    lv_area_t placed[SW_L_N] = { { 0, 0, 0, 0 } };
    int n_placed = 0;

    /* The telescope box, grown, for the two labels that must not cover it. */
    lv_area_t rig_box = { 0, 0, 0, 0 };
    bool have_rig = false;
    if (sky) {
        nina_sky_anchor_t ra;
        have_rig = nina_sky_scene_anchor(sky, NINA_SKY_ANCHOR_RIG, &ra);
        nina_sky_scene_rig_box(sky, &rig_box);
    }
    lv_area_t rig_grown = { 0, 0, 0, 0 };
    if (have_rig) {
        rig_grown.x1 = rig_box.x1 - SW_RIG_GROW;
        rig_grown.y1 = rig_box.y1 - SW_RIG_GROW;
        rig_grown.x2 = rig_box.x2 + SW_RIG_GROW;
        rig_grown.y2 = rig_box.y2 + SW_RIG_GROW;
    }

    /* 1: the target block, always shown. */
    {
        lv_obj_t *blk = p->alt.grp_mid;
        const int w = (int)lv_obj_get_width(blk);
        const int h = (int)lv_obj_get_height(blk);
        nina_sky_anchor_t a;
        int x;
        int y;
        if (sky && nina_sky_scene_anchor(sky, NINA_SKY_ANCHOR_TARGET, &a)) {
            const int side  = sw_sc(SW_TGT_SIDE);
            const int below = sw_sc(SW_TGT_BELOW);
            const lv_point_t spots[SW_N_SPOTS] = {
                { (int32_t)(a.x + side),         (int32_t)(a.y - h / 2) },
                { (int32_t)(a.x - side - w),     (int32_t)(a.y - h / 2) },
                { (int32_t)(a.x - w / 2),        (int32_t)(a.y + below) },
            };
            int score;
            const int pick = sw_pick(p, spots, w, h, s_sw_choice[inst][SW_L_TARGET],
                                     have_rig ? &rig_grown : NULL, placed, n_placed,
                                     &score);
            if (pick != (int)s_sw_choice[inst][SW_L_TARGET]) {
                s_sw_choice[inst][SW_L_TARGET] = (uint8_t)pick;
                sw_block_align(p, pick);
            }
            x = (int)spots[pick].x;
            y = (int)spots[pick].y;
        } else {
            x = screen_center() - w / 2;
            y = sw_sc(SW_FALLBACK_Y);
        }
        sw_move(blk, x, y);
        sw_show(blk, true);
        sw_box(&placed[n_placed++], x, y, w, h);
    }

    /* 2: the flip line, hidden when every spot covers something. */
    {
        lv_obj_t *lbl = p->alt.lbl_flip;
        nina_sky_anchor_t a;
        bool shown = false;
        if (lbl && sky && s_sw_flip_on[inst] &&
            nina_sky_scene_anchor(sky, NINA_SKY_ANCHOR_FLIP, &a)) {
            const int w = (int)lv_obj_get_width(lbl);
            const int h = (int)lv_obj_get_height(lbl);
            const int above = sw_sc(SW_FLIP_ABOVE);
            const int side  = sw_sc(SW_FLIP_SIDE);
            const lv_point_t spots[SW_N_SPOTS] = {
                { (int32_t)(a.x - w / 2),        (int32_t)(a.y - above - h) },
                { (int32_t)(a.x + side),         (int32_t)(a.y - h / 2) },
                { (int32_t)(a.x - side - w),     (int32_t)(a.y - h / 2) },
            };
            int score;
            const int pick = sw_pick(p, spots, w, h, s_sw_choice[inst][SW_L_FLIP],
                                     have_rig ? &rig_grown : NULL, placed, n_placed,
                                     &score);
            if (score < SW_PEN_RIG) {
                s_sw_choice[inst][SW_L_FLIP] = (uint8_t)pick;
                sw_move(lbl, (int)spots[pick].x, (int)spots[pick].y);
                sw_box(&placed[n_placed++], (int)spots[pick].x, (int)spots[pick].y, w, h);
                shown = true;
            }
        }
        sw_show(lbl, shown);
    }

    /* 3: the rig word, beside the telescope box. */
    {
        lv_obj_t *lbl = p->alt.lbl_caption[0];
        bool shown = false;
        if (lbl && sky && have_rig && s_sw_rig_on[inst]) {
            const int w  = (int)lv_obj_get_width(lbl);
            const int h  = (int)lv_obj_get_height(lbl);
            const int cx = (int)((rig_box.x1 + rig_box.x2) / 2);
            const int cy = (int)((rig_box.y1 + rig_box.y2) / 2);
            const int side  = sw_sc(SW_RIG_SIDE);
            const int above = sw_sc(SW_RIG_ABOVE);
            const lv_point_t spots[SW_N_SPOTS] = {
                { (int32_t)(rig_box.x2 + side),     (int32_t)(cy - h / 2) },
                { (int32_t)(rig_box.x1 - side - w), (int32_t)(cy - h / 2) },
                { (int32_t)(cx - w / 2),            (int32_t)(rig_box.y1 - above - h) },
            };
            int score;
            const int pick = sw_pick(p, spots, w, h, s_sw_choice[inst][SW_L_RIG],
                                     NULL, placed, n_placed, &score);
            if (score < SW_PEN_OFF || s_sw_flipping[inst]) {
                s_sw_choice[inst][SW_L_RIG] = (uint8_t)pick;
                sw_move(lbl, (int)spots[pick].x, (int)spots[pick].y);
                shown = true;
            }
        }
        sw_show(lbl, shown);
    }
}

static void sw_moved_cb(void *user)
{
    sw_place((dashboard_page_t *)user);
}

/* ---- the seconds -------------------------------------------------------- */

/* Sole writer of the seconds text; called by the tick and by update(). */
static void sw_hero_write(dashboard_page_t *p)
{
    const int inst = sw_inst(p);

    if (p->alt.lbl_hero) {
        char buf[32];
        buf[0] = '\0';
        const int v = nina_sky_words_secs(s_sw_run[inst], s_sw_secs[inst], s_sw_cur[inst]);
        if (v >= 0 && s_sw_total[inst] > 0.0f) {
            float t = s_sw_total[inst];
            if (t > 99999.0f) {
                t = 99999.0f;
            }
            int ti = (int)(t + 0.5f);
            if (ti < 1) {
                ti = 1;
            }
            snprintf(buf, sizeof(buf), "%d / %d s", v, ti);
        }
        const char *cur = lv_label_get_text(p->alt.lbl_hero);
        if (strcmp(cur, buf) != 0) {
            const bool resize = strlen(cur) != strlen(buf);
            lv_label_set_text(p->alt.lbl_hero, buf);
            if (resize) {
                sw_place(p);
            }
        }
    }
}

static void sw_elapsed_cb(dashboard_page_t *p, int secs)
{
    if (!p) {
        return;
    }
    const int inst = sw_inst(p);
    s_sw_secs[inst] = secs;
    sw_hero_write(p);

    if (p->alt.sky) {
        float frac = -1.0f;
        if (secs >= 0 && p->cached_total > 0.0f) {
            frac = (float)secs / p->cached_total;
        }
        nina_sky_scene_set_exposure(p->alt.sky, frac, s_sw_fc[inst]);
    }
}

/* ---- create ------------------------------------------------------------- */

void nina_layout_skywords_create(dashboard_page_t *p, lv_obj_t *parent, int page_index)
{
    if (!p || !parent) {
        return;
    }

    p->alt.inst = page_index;
    const int inst = sw_inst(p);
    for (int i = 0; i < SW_L_N; i++) {
        s_sw_choice[inst][i] = 0;
    }
    s_sw_secs[inst]     = -1;
    s_sw_total[inst]    = 0.0f;
    s_sw_cur[inst]      = 0.0f;
    s_sw_run[inst]      = false;
    s_sw_fc[inst]       = 0;
    s_sw_name[inst][0]  = '\0';
    s_sw_flip_on[inst]  = false;
    s_sw_rig_on[inst]   = false;
    s_sw_flipping[inst] = false;

    const int gb = app_config_get()->color_brightness;

    lv_obj_set_layout(parent, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_gap(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* The scene first, so every word below draws over it. */
    {
        const nina_sky_scene_opts_t opts = { .ground_band_top = 0 };
        p->alt.sky = nina_sky_scene_create(parent, &opts, page_index);
    }

    /* Target block: name, count, then filter and seconds on one row. */
    p->alt.grp_mid = sw_group(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(p->alt.grp_mid, sw_sc(2), 0);
    nina_dashboard_bind_tap(p->alt.grp_mid, NINA_TAP_SEQUENCE);

    p->alt.lbl_target = sw_label(p->alt.grp_mid, SW_FONT_NAME, "");
    lv_label_set_long_mode(p->alt.lbl_target, LV_LABEL_LONG_DOT);
    sw_set_name(p, "----");

    p->alt.lbl_count = sw_label(p->alt.grp_mid, SW_FONT_COUNT, "");
    lv_obj_set_style_text_opa(p->alt.lbl_count, SW_COUNT_OPA, 0);
    sw_show(p->alt.lbl_count, false);

    p->alt.grp_top = sw_group(p->alt.grp_mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p->alt.grp_top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(p->alt.grp_top, sw_sc(10), 0);
    p->alt.lbl_filter = sw_label(p->alt.grp_top, SW_FONT_FILTER, "");
    p->alt.lbl_hero = sw_label(p->alt.grp_top, SW_FONT_SECS, "");
    sw_show(p->alt.grp_top, false);

    /* Flip line and rig word, placed by sw_place(). */
    p->alt.lbl_flip = sw_label(parent, SW_FONT_FLIP, "");
    nina_dashboard_bind_tap(p->alt.lbl_flip, NINA_TAP_FLIP);
    sw_show(p->alt.lbl_flip, false);

    p->alt.lbl_caption[0] = sw_label(parent, SW_FONT_RIG, "");
    sw_show(p->alt.lbl_caption[0], false);

    if (p->alt.sky) {
        nina_sky_scene_set_moved_cb(p->alt.sky, sw_moved_cb, p);
    }
    p->alt.elapsed_cb = sw_elapsed_cb;

    sw_theme_page(p, gb);
    sw_place(p);
}

/* ---- theme -------------------------------------------------------------- */

static void sw_theme_page(dashboard_page_t *p, int gb)
{
    if (!p || !current_theme) {
        return;
    }
    const int inst = sw_inst(p);

    sw_set_color(p->alt.lbl_target, sw_fixed(SW_AMBER, gb));
    sw_set_color(p->alt.lbl_count, sw_fixed(SW_AMBER, gb));
    sw_set_color(p->alt.lbl_hero, sw_dim(current_theme->text_color, gb));
    sw_set_color(p->alt.lbl_flip, sw_fixed(SW_CYAN, gb));
    sw_set_color(p->alt.lbl_caption[0], sw_fixed(SW_CYAN, gb));

    const char *filter = p->alt.lbl_filter ? lv_label_get_text(p->alt.lbl_filter) : NULL;
    s_sw_fc[inst] = sw_filter_color(filter, inst, gb);
    sw_set_color(p->alt.lbl_filter, s_sw_fc[inst]);

    if (p->alt.sky) {
        nina_sky_scene_apply_theme(p->alt.sky, gb);
    }
}

void nina_layout_skywords_apply_theme(dashboard_page_t *p)
{
    if (!p) {
        return;
    }
    sw_theme_page(p, app_config_get()->color_brightness);
}

/* ---- update ------------------------------------------------------------- */

void nina_layout_skywords_update(dashboard_page_t *p, const nina_client_t *d,
                                 int instance_idx, int gb)
{
    if (!p || !d || !current_theme) {
        return;
    }

    p->alt.inst = instance_idx;
    const int inst = sw_inst(p);

    if (p->alt.sky) {
        nina_sky_scene_update(p->alt.sky, d, instance_idx, gb);
    }
    const nina_sky_info_t *info = p->alt.sky ? nina_sky_scene_info(p->alt.sky) : NULL;
    const sky_activity_t act = info ? info->activity
                                    : (d->is_exposing ? SKY_ACT_EXPOSING : SKY_ACT_RESTING);

    s_sw_total[inst] = d->exposure_total;
    s_sw_cur[inst]   = d->exposure_current;
    s_sw_run[inst]   = (act == SKY_ACT_EXPOSING);
    if (!s_sw_run[inst]) {
        s_sw_secs[inst] = -1;   /* the tick's last value belongs to the finished picture */
    }
    sw_hero_write(p);

    sw_set_name(p, (d->target_name[0] != '\0') ? d->target_name : "----");

    /* Count row, or "up at HH:MM" while the target is down. */
    {
        char buf[32];
        buf[0] = '\0';
        bool show = false;
        if (info && info->target_known && !info->target_up) {
            char t[8];
            if (info->rise_known &&
                nina_sky_scene_clock_after(p->alt.sky, info->hours_to_rise, t, sizeof(t))) {
                snprintf(buf, sizeof(buf), "up at %s", t);
                show = true;
            }
        } else {
            show = nina_sky_words_count(d, buf, sizeof(buf));
        }
        if (show) {
            sw_set_text(p->alt.lbl_count, buf);
        }
        sw_show(p->alt.lbl_count, show);
    }

    /* Filter and seconds row: only while a picture runs on the target. */
    {
        const bool known = sw_filter_known(d->current_filter);
        sw_set_text(p->alt.lbl_filter, known ? d->current_filter : "");
        s_sw_fc[inst] = sw_filter_color(known ? d->current_filter : NULL, instance_idx, gb);
        sw_set_color(p->alt.lbl_filter, s_sw_fc[inst]);
        sw_show(p->alt.lbl_filter, known);

        const bool row = act == SKY_ACT_EXPOSING && info && info->on_target;
        sw_show(p->alt.grp_top, row);
    }

    /* Flip line. */
    s_sw_flip_on[inst] = info && info->flip_known && act != SKY_ACT_FLIPPING;
    if (s_sw_flip_on[inst]) {
        char buf[32];
        nina_sky_words_flip(p->alt.sky, info->minutes_to_flip, buf, sizeof(buf));
        sw_set_text(p->alt.lbl_flip, buf);
    }

    /* Rig word. */
    {
        const char *word = NULL;
        switch (act) {
            case SKY_ACT_RESTING:
                word = "resting";
                break;
            case SKY_ACT_MOVING:
                word = "moving";
                break;
            case SKY_ACT_FLIPPING:
                word = "flipping";
                break;
            default:
                break;
        }
        s_sw_rig_on[inst]   = word != NULL;
        s_sw_flipping[inst] = act == SKY_ACT_FLIPPING;
        if (word) {
            sw_set_text(p->alt.lbl_caption[0], word);
        }
    }

    sw_place(p);
}
