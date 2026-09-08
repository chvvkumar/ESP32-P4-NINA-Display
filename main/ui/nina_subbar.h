#pragma once

/**
 * @file nina_subbar.h
 * @brief Segmented sub bar (block row and ring) shared by the NINA layouts.
 *
 * A segment is one Smart Exposure of one loop round of the container that is
 * running now, in execution order: round 0's items, then round 1's, and so on
 * (L R G B, L R G B, ...). A segment is as wide as that item's iteration
 * count, so a 20-iteration item is one long segment that fills gradually.
 * Finished segments are solid in their own filter colour, the running one is a
 * dim base with a fill riding on top, the rest are a dim (about 12 %) preview
 * of that same item's filter colour, and a disabled item is a flat 0x161616
 * placeholder one image wide that never fills. Past
 * NINA_SUBBAR_MAX_BLOCKS segments the list collapses to one neutral segment
 * per round. The whole widget hides when the container plans a single image.
 * The widget is ONLY the segment row or ring; every label around it belongs to
 * the owning layout.
 *
 * The segment list itself is decided by main/nina_plan_segments.h from the
 * plan the sequence parser puts in nina_client_t. A source that fills only the
 * legacy per-filter counts (demo mode) falls back to one segment per planned
 * image of the running filter, which is what this widget always drew.
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
#include "nina_plan_segments.h"

/* Hard cap on realized segment objects. Above this the plan collapses to one
 * segment per loop round (see nina_plan_segments.h). */
#define NINA_SUBBAR_MAX_BLOCKS NINA_SEG_MAX

/* Wider gap after every Nth segment when a round is a single segment (one
 * filter, or the collapsed per-round list), so a 40- or 60-sub night stays
 * countable. With several items per round the wider gap marks the round
 * boundary instead. */
#define NINA_SUBBAR_GROUP_EVERY 10

/** Longest the sub-boundary hold waits for the done count (ms): twice the
 *  15 s sequence tier. Past this the restart is a retry of the same sub. */
#define NINA_SUBBAR_HOLD_MS 30000

typedef void (*nina_subbar_elapsed_cb_t)(void *ud, int secs);

/* The widget's bulk (the segment object pointers and the cached plan, about
 * 1.3 KB) lives in PSRAM, one block per widget address, so the six embedded
 * widgets cost internal RAM only for this pointer (see nina_subbar.c). */
struct nina_subbar_store;

typedef struct {
    lv_obj_t *cont;          /* the segment row or ring itself */
    struct nina_subbar_store *st;   /* PSRAM: blocks[] and the cached plan; NULL = not created */
    lv_obj_t *fill;          /* in-flight overlay, re-parented into the running segment */

    int   n_blocks;          /* currently realized segment count, 0 = hidden */
    int   cached_target, cached_done;
    char  cached_filter[32]; /* the running filter, read by the owning layouts */

    /* Private render state (not part of the consumer-facing contract). */
    int      block_h;        /* 12 on Image-forward */
    int      active_idx;     /* segment currently holding the fill, -1 when none */
    float    act_base;       /* whole images of the running segment already done, 0..1 */
    int      act_weight;     /* images the running segment stands for */
    int      act_a0, act_a1; /* ring mode: the running segment's angles */
    float    cached_total;   /* exposure length in seconds, for the elapsed callback */
    uint32_t cached_block_color;
    uint32_t fp;             /* fingerprint of the drawn geometry (see nina_subbar.c) */
    int      gb;             /* colour brightness the last paint used */
    int      instance_idx;   /* owning NINA instance, for per-instance filter colours */
    bool     unknown;        /* no plan and no planned count: one segment acting as a
                              * plain progress bar, never hidden, never completed */

    /* Ring mode (round layouts): the segments are lv_arc arcs on one circle
     * instead of a flex row of lv_obj. Zero in the flex-row form, which is
     * what the square family builds. Angles are degrees clockwise from twelve
     * o'clock; ring_gap is left free at twelve for the caller's safety crown. */
    bool  ring;
    int   ring_radius;       /* segment centre-line radius in px */
    int   ring_width;        /* stroke in px */
    float ring_span;         /* 360 - ring_gap */
    float ring_gap;          /* reserved gap at twelve o'clock, degrees */
    float ring_frac;         /* last in-flight fraction, so a repaint keeps it */
    bool  stale;             /* ring mode only: arcs at 40 % while data is stale */
    bool  shown;             /* the layout's view-switch state, set through
                              * nina_subbar_set_shown(); true at create */

    /* Sub-boundary ratchet (see nina_subbar_set_progress). */
    float last_frac;         /* frac seen on the previous set_progress call */
    float hold_within;       /* fill held here while the done count catches up; <= 0 = no hold */
    uint32_t hold_tick;      /* lv_tick_get() when the hold armed; expires after NINA_SUBBAR_HOLD_MS */

    nina_subbar_elapsed_cb_t elapsed_cb;
    void                    *elapsed_ud;
} nina_subbar_t;

/**
 * @brief Build the segment row inside @p parent (fills its width).
 * @param sb       Caller-owned state, zeroed by this call
 * @param block_h  Segment height in px (12 on Image-forward)
 */
void nina_subbar_create(nina_subbar_t *sb, lv_obj_t *parent, int block_h);

/**
 * @brief Build the ring form of the segment row: N lv_arc segments on one circle.
 *
 * Same segment list, colour rule, done / in-flight / remaining decision and
 * in-flight fill as the flex row; only the geometry differs. The container is
 * 2*radius+width square and centred on @p parent, so @p parent must be the
 * full-panel page root.
 *
 * @param sb       Caller-owned state, zeroed by this call
 * @param radius   Segment centre-line radius in px (ui_rim_radius() - k)
 * @param width    Stroke width in px
 * @param gap_deg  Angular gap left free at twelve o'clock for a safety crown
 */
void nina_subbar_create_ring(nina_subbar_t *sb, lv_obj_t *parent,
                             int radius, int width, int gap_deg);

/**
 * @brief Move a ring to a new centre-line radius (ring mode only).
 *
 * Resizes and re-centres the container and forces the segments to rebuild on
 * the next nina_subbar_update(). No-op when the radius is unchanged.
 */
void nina_subbar_ring_set_radius(nina_subbar_t *sb, int radius);

/** @brief Push -1 to the elapsed callback (the layout's idle reset). The
 *         sub-boundary hold stays armed: this fires in the inter-exposure gap. */
void nina_subbar_reset_elapsed(nina_subbar_t *sb);

/** @brief Genuine idle park: drop the sub-boundary hold and take the fill back
 *         to the whole images the running segment has already finished. */
void nina_subbar_park(nina_subbar_t *sb);

/**
 * @brief The layout's own show/hide for the sub bar (its view switch).
 *
 * The sub bar is drawn only when the layout wants it shown AND the plan has
 * segments to show, so a view switch and a plan change never fight over the
 * container's HIDDEN flag. Layouts that toggle the sub bar per view must go
 * through this instead of flagging sb->cont themselves.
 */
void nina_subbar_set_shown(nina_subbar_t *sb, bool shown);

/**
 * @brief Push poll data into the segment row.
 *
 * Rebuilds the objects only when the segment geometry changed (count, weights,
 * item identity, round boundaries); a plain progress step just recolours, and
 * every colour write is compared before it lands. Safe to call every poll.
 */
void nina_subbar_update(nina_subbar_t *sb, const nina_client_t *d,
                        int instance_idx, int gb);

/** @brief Register the sink for interpolated elapsed seconds (capped at 9999). */
void nina_subbar_set_elapsed_cb(nina_subbar_t *sb, nina_subbar_elapsed_cb_t cb, void *ud);

/**
 * @brief Advance the in-flight fill and report elapsed seconds to the callback.
 *
 * The fill covers (whole images done + @p frac) / images of the RUNNING
 * segment, so a 20-iteration item advances a twentieth per exposure. It is
 * ratcheted across a sub boundary: the completed count comes from the 15 s
 * sequence poll while @p frac comes from the 2 s camera poll, so a new
 * exposure restarts while the finished one still owns the fill. The fill holds
 * at its last extent until the count catches up and moves it on; the elapsed
 * callback always sees the raw fraction, so the digits restart.
 *
 * @param frac Interpolated exposure fraction, 0..1, from arc_interp_timer_cb.
 */
void nina_subbar_set_progress(nina_subbar_t *sb, float frac);

/**
 * @brief Ring mode: dim every segment to 40 % while the source data is stale.
 *
 * The round layouts have no room for the square "Last update" text label (it
 * sits outside the disc), so the stale cue is the ring itself dimming, the same
 * cue the round Dashboard gives on its exposure ring. No-op on the flex-row
 * form, which is what the square family builds, and no-op when the flag is
 * already at the requested value.
 */
void nina_subbar_set_stale(nina_subbar_t *sb, bool stale);

/** @brief Re-colour every segment in place. */
void nina_subbar_apply_theme(nina_subbar_t *sb);
