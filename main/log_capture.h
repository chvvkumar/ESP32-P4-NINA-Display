#pragma once

/**
 * @file log_capture.h
 * @brief Boot log capture into a bounded PSRAM circular buffer.
 *
 * Hooks the ESP-IDF logging vprintf so everything printed to the console since
 * boot is also copied into a fixed-size circular byte buffer in PSRAM. The hook
 * chains to the original vprintf, so the serial console and `idf.py monitor`
 * are unaffected. When the buffer fills, the oldest bytes are overwritten.
 *
 * Memory is a single fixed allocation made once at init; it never grows,
 * regardless of uptime or log volume. If the PSRAM allocation fails, capture
 * degrades to pass-through (console-only) logging.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

/* Fixed circular buffer size: 512 KB, PSRAM-allocated. */
#define LOG_CAPTURE_SIZE (512 * 1024)

/**
 * @brief Allocate the PSRAM ring and install the vprintf hook.
 *
 * Safe to call once early in app_main(). On allocation failure, logs one
 * warning and leaves the original logging path intact (capture disabled).
 */
void log_capture_init(void);

/**
 * @brief vprintf hook installed via esp_log_set_vprintf().
 *
 * Formats the line into a small stack buffer, copies it into the ring under a
 * spinlock, then chains to the original vprintf and returns its result.
 */
int log_capture_vprintf(const char *fmt, va_list args);

/**
 * @brief Copy up to @p max_len bytes of captured log, oldest-first, into @p dst.
 *
 * Reads starting at logical byte offset @p offset (0 = oldest valid byte). The
 * ring is locked only for the duration of the copy, not across an entire HTTP
 * send, so callers stream by calling repeatedly with an advancing offset.
 *
 * @param offset   Logical offset from the oldest valid byte.
 * @param dst      Destination buffer.
 * @param max_len  Capacity of @p dst.
 * @return Number of bytes copied (0 when @p offset is at or past the end).
 */
size_t log_capture_read(size_t offset, char *dst, size_t max_len);

/**
 * @brief Reset the ring to empty.
 */
void log_capture_clear(void);

/**
 * @brief Current number of valid (readable) bytes in the ring.
 */
size_t log_capture_size(void);
