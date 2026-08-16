#pragma once

/**
 * @file page_conn.h
 * @brief Shared page connection state used by the OctoPrint, JSON, Home
 *        Assistant and AllSky pages.
 *
 * One evaluator so every data page tells the same story about its source:
 *
 *   PAGE_CONN_CONNECTING  Nothing received yet. Show the busy overlay
 *                         ("Connecting to X...").
 *   PAGE_CONN_OK          Last poll succeeded. Show live data.
 *   PAGE_CONN_STALE       Had data, poll is failing but within tolerance.
 *                         Keep the last data on screen, dimmed, with a small
 *                         "Reconnecting..." cue.
 *   PAGE_CONN_DOWN        Give up on the last data. Show the
 *                         "Cannot Reach X" overlay.
 *
 * Inputs come from the page's poller: @p ever_ok is true once any poll has
 * succeeded, @p last_ok is the result of the most recent poll, and
 * @p fail_count is the number of consecutive failed polls.
 */

#include <stdbool.h>

typedef enum {
    PAGE_CONN_CONNECTING,
    PAGE_CONN_OK,
    PAGE_CONN_STALE,
    PAGE_CONN_DOWN
} page_conn_t;

#define PAGE_CONN_STALE_MAX_MISSES 3   /* consecutive failed polls tolerated while still showing last data */

static inline page_conn_t page_conn_eval(bool ever_ok, bool last_ok, int fail_count)
{
    if (!ever_ok) {
        return (fail_count >= PAGE_CONN_STALE_MAX_MISSES) ? PAGE_CONN_DOWN
                                                          : PAGE_CONN_CONNECTING;
    }
    if (last_ok) {
        return PAGE_CONN_OK;
    }
    return (fail_count < PAGE_CONN_STALE_MAX_MISSES) ? PAGE_CONN_STALE
                                                     : PAGE_CONN_DOWN;
}
