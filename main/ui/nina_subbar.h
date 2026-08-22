#pragma once

/**
 * @file nina_subbar.h
 * @brief Segmented sub-bar block row shared by the alternate NINA layouts.
 *
 * One block per sub in the active filter's target count: completed blocks are
 * solid filter colour, the in-flight block is a 20 % base with a left-anchored
 * 55 %-opacity fill, remaining blocks are flat 0x161616. The widget is ONLY the
 * block row; every label around it belongs to the owning layout.
 *
 * Elapsed seconds: nina_subbar_set_progress() (the 200 ms interpolation tick)
 * interpolates them from the cached exposure length and hands them to the
 * callback registered with nina_subbar_set_elapsed_cb(), so the layout's
 * elapsed label keeps exactly one writer. nina_subbar_update() only refreshes
 * the cached exposure length.
 *
 * Ownership: the caller (a layout module) owns the nina_subbar_t and the parent
 * object. All entry points must be called with the LVGL display lock already
 * held, they never lock.
 */

#include "lvgl.h"
#include "nina_client.h"

/* Hard cap on realized block objects. Above this the row switches to grouped
 * blocks, each standing for ceil(target / NINA_SUBBAR_MAX_BLOCKS) subs. */
#define NINA_SUBBAR_MAX_BLOCKS 60

/* Wider gap after every Nth block so a 40- or 60-sub night stays countable. */
#define NINA_SUBBAR_GROUP_EVERY 10

typedef void (*nina_subbar_elapsed_cb_t)(void *ud, int secs);

typedef struct {
    lv_obj_t *cont;          /* the block row itself */
    lv_obj_t *blocks[NINA_SUBBAR_MAX_BLOCKS];
    lv_obj_t *fill;          /* in-flight overlay, re-parented into the active block */

    int   n_blocks;          /* currently realized block count */
    int   cached_target, cached_done;
    char  cached_filter[32];

    /* Private render state (not part of the consumer-facing contract). */
    int      block_h;        /* 12 on Image-forward */
    int      group_size;     /* subs represented by one block (1 unless grouped) */
    int      active_idx;     /* block currently holding the fill, -1 when none */
    float    cached_total;   /* exposure length in seconds, for the elapsed callback */
    uint32_t cached_block_color;
    int      instance_idx;   /* owning NINA instance, for per-instance filter colours */

    nina_subbar_elapsed_cb_t elapsed_cb;
    void                    *elapsed_ud;
} nina_subbar_t;

/**
 * @brief Build the block row inside @p parent (fills its width).
 * @param sb       Caller-owned state, zeroed by this call
 * @param block_h  Block height in px (12 on Image-forward)
 */
void nina_subbar_create(nina_subbar_t *sb, lv_obj_t *parent, int block_h);

/**
 * @brief Push poll data into the block row.
 *
 * Rebuilds the row only when the filter or the target count changed; a plain
 * progress step just recolours. Safe to call every poll.
 */
void nina_subbar_update(nina_subbar_t *sb, const nina_client_t *d,
                        int instance_idx, int gb);

/** @brief Register the sink for interpolated elapsed seconds (capped at 9999). */
void nina_subbar_set_elapsed_cb(nina_subbar_t *sb, nina_subbar_elapsed_cb_t cb, void *ud);

/**
 * @brief Advance the in-flight fill and report elapsed seconds to the callback.
 * @param frac Interpolated exposure fraction, 0..1, from arc_interp_timer_cb.
 */
void nina_subbar_set_progress(nina_subbar_t *sb, float frac);

/** @brief Re-colour every block in place. */
void nina_subbar_apply_theme(nina_subbar_t *sb);
