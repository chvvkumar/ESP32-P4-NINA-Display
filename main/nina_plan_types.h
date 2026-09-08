#pragma once

/* The running container's plan: the Smart Exposures of the parent container of
 * the RUNNING Smart Exposure, in Items order, with the loop round counters.
 *
 * These two types live here rather than in nina_client.h so the pure segment
 * header (nina_plan_segments.h) and its host test can use them without pulling
 * in FreeRTOS. nina_client.h includes this file and appends a nina_plan_t to
 * nina_client_t; nina_sequence.c fills it. */

#include <stdbool.h>

#define NINA_PLAN_MAX_ITEMS 16

typedef struct {
    char filter[32];     /* Smart Exposure "Filter", "" when absent */
    int  iterations;     /* per round; placeholders carry 1 */
    int  completed;      /* CompletedIterations of the current round, clamped 0..iterations */
    bool enabled;        /* false = Status DISABLED or Iterations <= 0: a dim placeholder */
    bool running;        /* this item is the RUNNING Smart Exposure */
} nina_plan_item_t;

typedef struct {
    int  n_items;        /* 0 = no plan (ring hidden). Smart Exposures of the parent
                          * container in Items order, capped at NINA_PLAN_MAX_ITEMS */
    int  rounds;         /* the container's "Loop For Iterations" Iterations, 1 when none */
    int  round_done;     /* that condition's CompletedIterations, clamped 0..rounds-1 */
    int  running_idx;    /* index of the running item, -1 when nothing is exposing */
    int  total_images;   /* rounds x sum(iterations) over ALL items (placeholders count 1 each);
                          * the ring hides when this is <= 1 */
    char container[64];  /* parent container Name with "_Container" stripped, for change detection */
    nina_plan_item_t items[NINA_PLAN_MAX_ITEMS];
} nina_plan_t;
