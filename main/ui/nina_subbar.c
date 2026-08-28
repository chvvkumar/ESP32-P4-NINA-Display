/**
 * @file nina_subbar.c
 * @brief Segmented sub-bar block row (see nina_subbar.h).
 *
 * Blocks flex 1 1 0 with a 2 px floor, 3 px radius, 3 px gap widening to 9 px
 * after every 10th block; completed = filter colour, in-flight = 20 % base plus
 * a left-anchored 55 % fill, remaining = flat 0x161616.
 *
 * Ring mode (nina_subbar_create_ring, round layouts) builds the same blocks as
 * lv_arc segments on one circle, spaced uniformly: the wider gap after every
 * 10th block is a flex-row property. The done / in-flight / remaining decision,
 * the block grouping and the in-flight fill are shared by both modes.
 *
 * Every entry point runs with the LVGL display lock held by the caller.
 */

#include "nina_subbar.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "themes.h"
#include "ui_helpers.h"

#define SB_GAP_TIGHT      3
#define SB_GAP_WIDE       9
#define SB_BLOCK_RADIUS   3
#define SB_FILL_OPA       140   /* 55 % of 255 */
#define SB_DIM_OPA        LV_OPA_20
#define SB_REMAIN_COLOR   0x161616

/* ── helpers ─────────────────────────────────────────────────────────────── */

static uint32_t sb_block_color(const char *filter, int instance_idx, int gb);
static void sb_rebuild_blocks(nina_subbar_t *sb, int target);
static void sb_paint_blocks(nina_subbar_t *sb, int done, uint32_t color);
static void sb_place_fill(nina_subbar_t *sb, int idx, uint32_t color);
static void sb_block_geom(const nina_subbar_t *sb, int i, int *a0, int *a1);
static lv_obj_t *sb_arc(nina_subbar_t *sb, int a0, int a1);
static void sb_style_block(const nina_subbar_t *sb, lv_obj_t *b, uint32_t color, lv_opa_t opa);
static void sb_ring_fill_angles(nina_subbar_t *sb, float frac);

/* Block colour: the active filter's configured colour, clamped to the theme's
 * progress colour on Red Night so no non-red hue reaches the panel. */
static uint32_t sb_block_color(const char *filter, int instance_idx, int gb) {
    if (!current_theme) return app_config_apply_brightness(0x808080, gb);
    if (theme_is_red_night(current_theme)) {
        return app_config_apply_brightness(current_theme->progress_color, gb);
    }
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, instance_idx);
    }
    return app_config_apply_brightness(current_theme->progress_color, gb);
}

/* ---- ring geometry ------------------------------------------------------ */

/* Angles of block i, degrees clockwise from twelve o'clock. The gap between
 * blocks is 5 degrees, or 18 % of the pitch when the pitch is small, which is
 * the same proportion the flex row uses between its 3 px and 9 px gaps. The
 * ring spaces every block alike: the wider gap the flex row puts after every
 * NINA_SUBBAR_GROUP_EVERY block is a flex-row property. */
static void sb_block_geom(const nina_subbar_t *sb, int i, int *a0, int *a1) {
    int n = (sb->n_blocks > 0) ? sb->n_blocks : 1;
    float pitch = sb->ring_span / (float)n;
    float gap   = (pitch * 0.18f < 5.0f) ? (pitch * 0.18f) : 5.0f;
    float start = sb->ring_gap * 0.5f + (float)i * pitch;
    *a0 = (int)(start + 0.5f);
    *a1 = (int)(start + pitch - gap + 0.5f);
    if (*a1 <= *a0) *a1 = *a0 + 1;
}

/* One fixed arc segment on the ring, drawn with LV_PART_MAIN only. */
static lv_obj_t *sb_arc(nina_subbar_t *sb, int a0, int a1) {
    lv_obj_t *a = lv_arc_create(sb->cont);
    int side = 2 * sb->ring_radius + sb->ring_width;
    lv_obj_set_size(a, side, side);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_rotation(a, (270 + a0) % 360);
    lv_arc_set_bg_angles(a, 0, a1 - a0);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(a, sb->ring_width, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return a;
}

/* The one place a block's colour reaches pixels, in either mode. */
static void sb_style_block(const nina_subbar_t *sb, lv_obj_t *b,
                           uint32_t color, lv_opa_t opa) {
    if (sb->ring) {
        lv_obj_set_style_arc_color(b, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(b, opa, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(b, opa, 0);
    }
}

/* Open the in-flight arc to @p frac of the active block's sweep. */
static void sb_ring_fill_angles(nina_subbar_t *sb, float frac) {
    if (!sb->fill || sb->active_idx < 0) return;
    int a0, a1;
    sb_block_geom(sb, sb->active_idx, &a0, &a1);
    int span = (int)((float)(a1 - a0) * frac + 0.5f);
    if (span < 1) {
        lv_obj_add_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_arc_set_rotation(sb->fill, (270 + a0) % 360);
    lv_arc_set_bg_angles(sb->fill, 0, span);
    lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
}

/* ── block row ───────────────────────────────────────────────────────────── */

/* Realize exactly the block objects the current target needs. Above
 * NINA_SUBBAR_MAX_BLOCKS each block stands for a group of subs. A target of
 * 0 or less collapses to one full-width block acting as a progress bar. */
static void sb_rebuild_blocks(nina_subbar_t *sb, int target) {
    if (!sb->cont) return;

    /* lv_obj_clean deletes the blocks and, with them, the fill that lives
     * inside the active block. Drop the pointer before it dangles. */
    sb->fill = NULL;
    lv_obj_clean(sb->cont);
    for (int i = 0; i < NINA_SUBBAR_MAX_BLOCKS; i++) sb->blocks[i] = NULL;

    int group = 1;
    int n;
    if (target <= 0) {
        n = 1;
    } else if (target <= NINA_SUBBAR_MAX_BLOCKS) {
        n = target;
    } else {
        group = (target + NINA_SUBBAR_MAX_BLOCKS - 1) / NINA_SUBBAR_MAX_BLOCKS;
        n = (target + group - 1) / group;
        if (n > NINA_SUBBAR_MAX_BLOCKS) n = NINA_SUBBAR_MAX_BLOCKS;
    }

    sb->n_blocks   = n;
    sb->group_size = group;
    sb->active_idx = -1;
    sb->ring_frac  = 0.0f;

    for (int i = 0; i < n; i++) {
        lv_obj_t *b;
        if (sb->ring) {
            int a0, a1;
            sb_block_geom(sb, i, &a0, &a1);
            b = sb_arc(sb, a0, a1);
        } else {
            b = lv_obj_create(sb->cont);
            lv_obj_remove_style_all(b);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_width(b, 2);              /* 2 px floor; flex grow does the rest */
            lv_obj_set_flex_grow(b, 1);
            lv_obj_set_height(b, sb->block_h);
            lv_obj_set_style_radius(b, SB_BLOCK_RADIUS, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_clip_corner(b, true, 0);
            /* 3 px between blocks, widening to 9 px after every 10th. */
            if (i < n - 1) {
                bool wide = (((i + 1) % NINA_SUBBAR_GROUP_EVERY) == 0);
                lv_obj_set_style_margin_right(b, wide ? SB_GAP_WIDE : SB_GAP_TIGHT, 0);
            }
        }
        sb->blocks[i] = b;
    }
}

static void sb_paint_blocks(nina_subbar_t *sb, int done, uint32_t color) {
    int group = (sb->group_size > 0) ? sb->group_size : 1;
    int done_blocks = done / group;

    /* Unknown target: the lone block is a plain progress bar, never a completed
     * one. Without this it paints solid COVER on the first finished sub and
     * sb_place_fill(-1) hides the fill for the rest of the sequence. */
    if (sb->n_blocks == 1 && sb->cached_target <= 0) done_blocks = 0;

    for (int i = 0; i < sb->n_blocks; i++) {
        if (!sb->blocks[i]) continue;
        if (i < done_blocks) {
            sb_style_block(sb, sb->blocks[i], color, LV_OPA_COVER);
        } else if (i == done_blocks) {
            /* In-flight: dim base, the fill rides on top. */
            sb_style_block(sb, sb->blocks[i], color, SB_DIM_OPA);
        } else {
            sb_style_block(sb, sb->blocks[i], SB_REMAIN_COLOR, LV_OPA_COVER);
        }
    }

    sb_place_fill(sb, (done_blocks < sb->n_blocks) ? done_blocks : -1, color);
}

/* Move the single fill object into block @p idx (-1 hides it). */
static void sb_place_fill(nina_subbar_t *sb, int idx, uint32_t color) {
    if (idx < 0 || idx >= sb->n_blocks || !sb->blocks[idx]) {
        if (sb->fill) lv_obj_add_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
        sb->active_idx = -1;
        return;
    }
    if (sb->ring) {
        if (!sb->fill) {
            sb->fill = sb_arc(sb, 0, 1);
            lv_obj_set_style_arc_opa(sb->fill, SB_FILL_OPA, LV_PART_MAIN);
            sb->active_idx = idx;
            sb->ring_frac  = 0.0f;
        } else if (sb->active_idx != idx) {
            sb->active_idx = idx;
            sb->ring_frac  = 0.0f;
        }
        lv_obj_set_style_arc_color(sb->fill, lv_color_hex(color), LV_PART_MAIN);
        sb_ring_fill_angles(sb, sb->ring_frac);
        return;
    }
    if (!sb->fill) {
        sb->fill = lv_obj_create(sb->blocks[idx]);
        lv_obj_remove_style_all(sb->fill);
        lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(sb->fill, SB_BLOCK_RADIUS, 0);
        lv_obj_set_style_bg_opa(sb->fill, SB_FILL_OPA, 0);
        lv_obj_set_size(sb->fill, 0, LV_PCT(100));
        lv_obj_align(sb->fill, LV_ALIGN_LEFT_MID, 0, 0);
        sb->active_idx = idx;
    } else if (sb->active_idx != idx) {
        lv_obj_set_parent(sb->fill, sb->blocks[idx]);
        lv_obj_align(sb->fill, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_width(sb->fill, 0);
        sb->active_idx = idx;
    }
    lv_obj_set_style_bg_color(sb->fill, lv_color_hex(color), 0);
    lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
}

/* ── public API ──────────────────────────────────────────────────────────── */

void nina_subbar_create(nina_subbar_t *sb, lv_obj_t *parent, int block_h) {
    if (!sb || !parent) return;
    memset(sb, 0, sizeof(*sb));
    sb->block_h   = (block_h > 0) ? block_h : 12;
    sb->group_size = 1;
    sb->active_idx = -1;
    sb->cached_target = -1;
    sb->cached_done   = -1;

    sb->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(sb->cont);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(sb->cont, LV_PCT(100));
    lv_obj_set_height(sb->cont, sb->block_h);
    lv_obj_set_style_pad_all(sb->cont, 0, 0);
    lv_obj_set_style_pad_gap(sb->cont, 0, 0);
    lv_obj_set_flex_flow(sb->cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb->cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    sb_rebuild_blocks(sb, 0);
    nina_subbar_apply_theme(sb);
}

void nina_subbar_create_ring(nina_subbar_t *sb, lv_obj_t *parent,
                             int radius, int width, int gap_deg) {
    if (!sb || !parent || radius <= 0 || width <= 0) return;
    memset(sb, 0, sizeof(*sb));
    sb->block_h    = width;
    sb->group_size = 1;
    sb->active_idx = -1;
    sb->cached_target = -1;
    sb->cached_done   = -1;
    sb->ring        = true;
    sb->ring_radius = radius;
    sb->ring_width  = width;
    sb->ring_gap    = (float)gap_deg;
    sb->ring_span   = 360.0f - (float)gap_deg;

    int side = 2 * radius + width;
    sb->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(sb->cont);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sb->cont, side, side);
    lv_obj_center(sb->cont);
    lv_obj_set_style_pad_all(sb->cont, 0, 0);

    sb_rebuild_blocks(sb, 0);
    nina_subbar_apply_theme(sb);
}

void nina_subbar_reset_elapsed(nina_subbar_t *sb) {
    if (!sb || !sb->elapsed_cb) return;
    sb->elapsed_cb(sb->elapsed_ud, -1);
}

void nina_subbar_update(nina_subbar_t *sb, const nina_client_t *d,
                        int instance_idx, int gb) {
    if (!sb || !sb->cont || !d || !current_theme) return;

    const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
    int target = d->exposure_iterations;
    int done   = d->exposure_count;
    if (done < 0) done = 0;
    if (target > 0 && done > target) done = target;

    sb->instance_idx = instance_idx;
    uint32_t color = sb_block_color(filter, instance_idx, gb);

    /* Rebuild the block row only when the filter or the target count changed,
     * never on a plain progress step. */
    bool filter_changed = (strncmp(sb->cached_filter, filter, sizeof(sb->cached_filter) - 1) != 0);
    if (filter_changed || target != sb->cached_target) {
        sb_rebuild_blocks(sb, target);
        sb->cached_target = target;
        sb->cached_done   = -1;             /* force the repaint below */
        snprintf(sb->cached_filter, sizeof(sb->cached_filter), "%s", filter);
    }

    if (done != sb->cached_done || color != sb->cached_block_color) {
        sb_paint_blocks(sb, done, color);
        sb->cached_done = done;
        sb->cached_block_color = color;
    }

    /* Elapsed seconds belong to nina_subbar_set_progress() (the 200 ms tick);
     * only the cached length is refreshed here. */
    sb->cached_total = d->exposure_total;
}

void nina_subbar_set_elapsed_cb(nina_subbar_t *sb, nina_subbar_elapsed_cb_t cb, void *ud) {
    if (!sb) return;
    sb->elapsed_cb = cb;
    sb->elapsed_ud = ud;
}

void nina_subbar_set_progress(nina_subbar_t *sb, float frac) {
    if (!sb || !sb->cont) return;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    if (sb->fill && sb->active_idx >= 0) {
        /* In grouped mode the fill spans the group: the subs already done
         * inside this block plus the in-flight fraction. */
        float within = frac;
        int group = (sb->group_size > 0) ? sb->group_size : 1;
        if (group > 1 && sb->cached_done >= 0) {
            within = ((float)(sb->cached_done % group) + frac) / (float)group;
        }
        if (within < 0.0f) within = 0.0f;
        if (within > 1.0f) within = 1.0f;
        if (sb->ring) {
            sb->ring_frac = within;
            sb_ring_fill_angles(sb, within);
        } else {
            lv_obj_set_width(sb->fill, LV_PCT((int)(within * 100.0f + 0.5f)));
        }
    }

    if (sb->elapsed_cb && sb->cached_total > 0.0f) {
        int secs = (int)(frac * sb->cached_total + 0.5f);
        if (secs > 9999) secs = 9999;
        sb->elapsed_cb(sb->elapsed_ud, secs);
    }
}

void nina_subbar_apply_theme(nina_subbar_t *sb) {
    if (!sb || !sb->cont || !current_theme) return;
    int gb = app_config_get()->color_brightness;

    /* Re-derive the filter colour from the cached values so a theme or
     * brightness change repaints without waiting for the next poll. */
    uint32_t color = sb_block_color(sb->cached_filter, sb->instance_idx, gb);
    sb->cached_block_color = color;
    sb_paint_blocks(sb, (sb->cached_done > 0) ? sb->cached_done : 0, color);
}
