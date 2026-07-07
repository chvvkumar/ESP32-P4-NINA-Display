#pragma once

/**
 * @file poll_backoff.h
 * @brief Pure exponential-backoff step function for poll_task.c.
 *
 * Header-only, no ESP-IDF/FreeRTOS includes -- host-testable in isolation
 * (test/host/test_poll_backoff.c).
 *
 * Reset-on-success is intentionally NOT encoded here: the caller (poll_task.c)
 * simply assigns its backoff-state variable back to 0 after a successful
 * poll_once(). That is a one-line caller responsibility, not policy worth a
 * pure function of its own.
 */

#include <stdint.h>

/**
 * Compute the next backoff delay (ms) after a failed poll.
 *
 * @param cur      Current backoff delay in ms; 0 means "no failure yet"
 *                  (first failure since the last success or task start).
 * @param initial  Delay to use on the first failure. 0 disables backoff
 *                  entirely -- always returns 0, telling the caller to fall
 *                  back to its normal poll interval instead.
 * @param max      Ceiling the doubling saturates at. 0 means unbounded.
 */
static inline uint32_t poll_backoff_next(uint32_t cur, uint32_t initial, uint32_t max) {
    if (initial == 0) return 0; /* backoff disabled */

    if (cur == 0) {
        return (max != 0 && initial > max) ? max : initial;
    }

    uint32_t next = cur * 2;
    if (next < cur) next = (max != 0) ? max : cur; /* overflow guard */
    if (max != 0 && next > max) next = max;
    return next;
}
