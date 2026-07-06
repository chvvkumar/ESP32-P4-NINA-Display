#pragma once

/**
 * @file http_fetch_policy.h
 * @brief Pure decision functions for http_fetch.c -- redirect classification,
 * retry-loop continuation, and response-buffer sizing.
 *
 * Header-only, no ESP-IDF/FreeRTOS includes on purpose: these are the parts
 * of the fetcher's control flow worth pinning with a host-side truth table
 * (test/host/test_http_policy.c) without dragging in esp_http_client.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** True for the HTTP redirect status codes esp_http_client_set_redirection()
 * knows how to follow (301/302/303/307/308). */
static inline bool http_status_is_redirect(int status) {
    switch (status) {
        case 301:
        case 302:
        case 303:
        case 307:
        case 308:
            return true;
        default:
            return false;
    }
}

/** True if a retry attempt should follow @p attempt (0-indexed: attempt 0 is
 * the first try). False once @p attempt is the last of @p max_attempts. */
static inline bool http_should_retry(int attempt, int max_attempts) {
    if (max_attempts <= 0) return false;
    return (attempt + 1) < max_attempts;
}

/**
 * Initial response-buffer size (bytes, including 1 for the NUL terminator).
 *
 * @param content_length  Server-reported Content-Length, or <= 0 when
 *                         unknown/chunked (unknown-length streaming case).
 * @param cap              Hard ceiling on total buffer size; 0 means no room
 *                         at all (caller should treat as failure).
 */
static inline size_t http_buf_initial(int64_t content_length, size_t cap) {
    if (cap == 0) return 0;
    if (content_length > 0) {
        size_t want = (size_t)content_length + 1; /* +1 for NUL */
        return (want > cap) ? cap : want;
    }
    /* Unknown length: start small and grow via http_buf_grow(). */
    size_t start = 4096;
    return (start > cap) ? cap : start;
}

/**
 * Next buffer size when the current buffer filled up (doubling growth).
 * Returns 0 when @p cur has already reached @p cap (cannot grow further --
 * caller must stop reading and, for unknown-length responses, treat the
 * body as truncated at the cap).
 */
static inline size_t http_buf_grow(size_t cur, size_t cap) {
    if (cur == 0 || cur >= cap) return 0;
    size_t next = cur * 2;
    if (next < cur) next = cap;   /* overflow guard */
    if (next > cap) next = cap;
    if (next <= cur) return 0;    /* already at cap */
    return next;
}
