#pragma once

/* Pure segment list for the shared sub bar (block row and ring).
 *
 * A segment is one IMAGE of one Smart Exposure of one loop round, in
 * execution order: round 0's items image by image, then round 1's, and so
 * on. Every segment carries weight 1; a 20-iteration item is 20 separate
 * segments, not one wide one. Disabled or zero-iteration items still draw
 * one PLACEHOLDER segment of weight 1 per round. Past NINA_SEG_MAX segments
 * (by total image count) the list collapses to one segment per round.
 *
 * Header-only, pure C, no ESP-IDF, FreeRTOS or LVGL dependency; host-tested by
 * test/host/tests/test_nina_plan_segments.c. */

#include <stdbool.h>

#include "nina_plan_types.h"

#define NINA_SEG_MAX 120

typedef enum { NINA_SEG_DONE, NINA_SEG_ACTIVE, NINA_SEG_REMAINING, NINA_SEG_PLACEHOLDER } nina_seg_state_t;

typedef struct {
    int   item;          /* index into plan->items, or -1 for a per-round segment (overflow mode) */
    int   round;         /* 0-based loop round */
    int   weight;        /* 1 for a per-image segment, or the round's total images in overflow mode */
    float done_frac;     /* 0 or 1 for a per-image segment (fractional only in overflow mode) */
    float fill_frac;     /* ACTIVE only: exp_frac (or the overflow round's fractional fill), else == done_frac */
    bool  round_last;    /* true on the last segment of a round: the wider gap follows it */
    nina_seg_state_t state;
} nina_seg_t;

/* The single-image hide rule, shared with the widget. */
static inline bool nina_plan_hidden(const nina_plan_t *plan)
{
    return !plan || plan->n_items <= 0 || plan->total_images <= 1;
}

static inline float nina_plan_clamp01(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* Images this item contributes to one round: its iteration count, or 1 for a
 * disabled placeholder. */
static inline int nina_plan_item_weight(const nina_plan_item_t *it)
{
    if (!it->enabled || it->iterations <= 0) {
        return 1;
    }
    return it->iterations;
}

/* Fill @p out with the ordered segment list. Returns the count (0 when the
 * plan hides: n_items == 0 or total_images <= 1). @p exp_frac is the live
 * exposure fraction 0..1 of the running image.
 *
 * The list is round-major, item-major within a round, and one segment of
 * weight 1 PER IMAGE within an item (a 3-iteration item is 3 segments, never
 * one merged segment). For image index k (0-based) of an item in the
 * running round: k < completed is DONE (done_frac 1, fill_frac 1); k ==
 * completed of the RUNNING item is ACTIVE (done_frac 0, fill_frac =
 * exp_frac); every other image is REMAINING (0, 0). Items before the running
 * one in the running round are all DONE regardless of their own completed
 * (execution order proves them); items after it are all REMAINING (their
 * completed is stale from the previous round). With running_idx < 0 (the
 * container is still running but nothing is exposing) the running round has
 * no ACTIVE segment and each item reads its images DONE/REMAINING from its
 * own completed count. Rounds before round_done are all DONE; rounds after
 * are all REMAINING. Disabled or zero-iteration items are one PLACEHOLDER
 * segment of weight 1 per round, in every round, and never fill.
 * `round_last` is true on the last segment of each round.
 *
 * Overflow: when the plan's total image count exceeds NINA_SEG_MAX, emit one
 * segment per round instead (item -1, weight = images in that round), DONE
 * for rounds before round_done, ACTIVE for round_done with done_frac =
 * images finished in that round / round images and fill_frac adding
 * exp_frac / round images, REMAINING after. */
static inline int nina_plan_segments(const nina_plan_t *plan, float exp_frac,
                                     nina_seg_t *out, int max)
{
    if (!out || max <= 0 || nina_plan_hidden(plan)) {
        return 0;
    }

    const float frac = nina_plan_clamp01(exp_frac);
    const int n_items = plan->n_items > NINA_PLAN_MAX_ITEMS ? NINA_PLAN_MAX_ITEMS : plan->n_items;
    const int rounds = plan->rounds > 0 ? plan->rounds : 1;
    int round_done = plan->round_done;
    if (round_done < 0) {
        round_done = 0;
    }
    if (round_done > rounds - 1) {
        round_done = rounds - 1;
    }
    const int running_idx = (plan->running_idx >= 0 && plan->running_idx < n_items)
                                ? plan->running_idx : -1;

    /* Images per round, and how many of them the running round has finished. */
    int round_images = 0;
    int round_finished = 0;
    for (int i = 0; i < n_items; i++) {
        const int w = nina_plan_item_weight(&plan->items[i]);
        round_images += w;
        if (running_idx >= 0) {
            if (i < running_idx) {
                round_finished += w;
            } else if (i == running_idx) {
                int c = plan->items[i].completed;
                if (c < 0) {
                    c = 0;
                }
                if (c > w) {
                    c = w;
                }
                round_finished += c;
            }
        } else {
            int c = plan->items[i].completed;
            if (c < 0) {
                c = 0;
            }
            if (c > w) {
                c = w;
            }
            round_finished += plan->items[i].enabled ? c : 0;
        }
    }
    if (round_images <= 0) {
        return 0;
    }

    int n = 0;
    const int total_images = rounds * round_images;

    /* ---- overflow: one neutral segment per round ------------------------ */
    if (total_images > NINA_SEG_MAX) {
        for (int r = 0; r < rounds && n < max; r++) {
            nina_seg_t *s = &out[n++];
            s->item = -1;
            s->round = r;
            s->weight = round_images;
            s->round_last = true;
            if (r < round_done) {
                s->state = NINA_SEG_DONE;
                s->done_frac = 1.0f;
                s->fill_frac = 1.0f;
            } else if (r == round_done) {
                s->state = NINA_SEG_ACTIVE;
                s->done_frac = (float)round_finished / (float)round_images;
                float fill = ((float)round_finished + (running_idx >= 0 ? frac : 0.0f))
                             / (float)round_images;
                s->fill_frac = nina_plan_clamp01(fill);
            } else {
                s->state = NINA_SEG_REMAINING;
                s->done_frac = 0.0f;
                s->fill_frac = 0.0f;
            }
        }
        return n;
    }

    /* ---- one weight-1 segment per image, item-major within a round ------ */
    for (int r = 0; r < rounds && n < max; r++) {
        for (int i = 0; i < n_items && n < max; i++) {
            const nina_plan_item_t *it = &plan->items[i];
            const bool placeholder = (!it->enabled || it->iterations <= 0);
            const int w = placeholder ? 1 : it->iterations;

            int c = it->completed;
            if (c < 0) {
                c = 0;
            }
            if (c > w) {
                c = w;
            }

            for (int k = 0; k < w && n < max; k++) {
                nina_seg_t *s = &out[n++];
                s->item = i;
                s->round = r;
                s->weight = 1;
                s->round_last = (i == n_items - 1) && (k == w - 1);
                s->done_frac = 0.0f;
                s->fill_frac = 0.0f;

                if (placeholder) {
                    s->state = NINA_SEG_PLACEHOLDER;
                    continue;
                }
                if (r < round_done) {
                    s->state = NINA_SEG_DONE;
                    s->done_frac = 1.0f;
                    s->fill_frac = 1.0f;
                    continue;
                }
                if (r > round_done) {
                    s->state = NINA_SEG_REMAINING;
                    continue;
                }

                /* the running round */
                if (running_idx < 0) {
                    /* nothing exposing: each item speaks for itself */
                    if (k < c) {
                        s->state = NINA_SEG_DONE;
                        s->done_frac = 1.0f;
                        s->fill_frac = 1.0f;
                    } else {
                        s->state = NINA_SEG_REMAINING;
                    }
                } else if (i < running_idx) {
                    s->state = NINA_SEG_DONE;
                    s->done_frac = 1.0f;
                    s->fill_frac = 1.0f;
                } else if (i == running_idx) {
                    if (k < c) {
                        s->state = NINA_SEG_DONE;
                        s->done_frac = 1.0f;
                        s->fill_frac = 1.0f;
                    } else if (k == c) {
                        s->state = NINA_SEG_ACTIVE;
                        s->done_frac = 0.0f;
                        s->fill_frac = frac;
                    } else {
                        s->state = NINA_SEG_REMAINING;
                    }
                } else {
                    s->state = NINA_SEG_REMAINING;   /* completed is stale from the previous round */
                }
            }
        }
    }
    return n;
}
