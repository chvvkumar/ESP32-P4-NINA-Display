/**
 * @file log_capture.c
 * @brief PSRAM circular log buffer + chained vprintf hook. See log_capture.h.
 */

#include "log_capture.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "log_capture";

/* Stack scratch size for a single formatted log line. Lines longer than this
 * are truncated for capture only (the original vprintf still gets the full
 * format/args and prints the complete line to the console). */
#define LOG_LINE_MAX 256

/* The ring buffer (PSRAM). NULL when allocation failed -> pass-through only. */
static char *s_buf;

/* head: index where the next byte is written.
 * count: number of valid bytes currently in the ring (<= LOG_CAPTURE_SIZE).
 * The oldest valid byte is at (head - count) modulo LOG_CAPTURE_SIZE. */
static size_t s_head;
static size_t s_count;

/* Original vprintf to chain to (console output). Captured at init. */
static vprintf_like_t s_orig_vprintf;

/* Spinlock guarding head/count/buf contents. Held only for short memcpys. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void log_capture_init(void)
{
    if (s_buf) {
        return;  /* already initialised */
    }

    s_buf = heap_caps_malloc(LOG_CAPTURE_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        /* Degrade gracefully: leave the existing logging path untouched. */
        ESP_LOGW(TAG, "PSRAM alloc of %d bytes failed; log capture disabled",
                 LOG_CAPTURE_SIZE);
        return;
    }

    s_head = 0;
    s_count = 0;

    /* Install hook; remember the previous vprintf so we can chain to it. */
    s_orig_vprintf = esp_log_set_vprintf(log_capture_vprintf);

    ESP_LOGI(TAG, "Log capture active (%d KB PSRAM ring)", LOG_CAPTURE_SIZE / 1024);
}

/* Append @p len bytes from @p src into the ring under the spinlock, overwriting
 * the oldest data when full. Handles wraparound at the physical end of buf. */
static void ring_append(const char *src, size_t len)
{
    if (len == 0) {
        return;
    }
    /* A single line can never be longer than the ring; clamp defensively. */
    if (len > LOG_CAPTURE_SIZE) {
        src += (len - LOG_CAPTURE_SIZE);
        len = LOG_CAPTURE_SIZE;
    }

    portENTER_CRITICAL(&s_mux);

    /* First contiguous span: from head to the physical end of the buffer. */
    size_t first = LOG_CAPTURE_SIZE - s_head;
    if (first > len) {
        first = len;
    }
    memcpy(s_buf + s_head, src, first);

    /* Remainder wraps to the start of the buffer. */
    size_t second = len - first;
    if (second) {
        memcpy(s_buf, src + first, second);
    }

    s_head = (s_head + len) % LOG_CAPTURE_SIZE;

    s_count += len;
    if (s_count > LOG_CAPTURE_SIZE) {
        s_count = LOG_CAPTURE_SIZE;  /* oldest bytes were overwritten */
    }

    portEXIT_CRITICAL(&s_mux);
}

int log_capture_vprintf(const char *fmt, va_list args)
{
    /* The capture format needs its own va_list copy; the original args are
     * consumed by the chained vprintf below. */
    char line[LOG_LINE_MAX];
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);

    if (s_buf && n > 0) {
        size_t to_copy = (size_t)n;
        if (to_copy >= sizeof(line)) {
            to_copy = sizeof(line) - 1;  /* vsnprintf truncated; copy what we have */
        }
        ring_append(line, to_copy);
    }

    /* Chain to the original vprintf so console / monitor still see the line. */
    if (s_orig_vprintf) {
        return s_orig_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

size_t log_capture_read(size_t offset, char *dst, size_t max_len)
{
    if (!s_buf || !dst || max_len == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_mux);

    if (offset >= s_count) {
        portEXIT_CRITICAL(&s_mux);
        return 0;
    }

    size_t avail = s_count - offset;
    size_t want = (avail < max_len) ? avail : max_len;

    /* Physical index of the oldest valid byte, advanced by offset. */
    size_t start = (s_head + LOG_CAPTURE_SIZE - s_count + offset) % LOG_CAPTURE_SIZE;

    /* Copy in up to two spans to handle wraparound. */
    size_t first = LOG_CAPTURE_SIZE - start;
    if (first > want) {
        first = want;
    }
    memcpy(dst, s_buf + start, first);

    size_t second = want - first;
    if (second) {
        memcpy(dst + first, s_buf, second);
    }

    portEXIT_CRITICAL(&s_mux);
    return want;
}

void log_capture_clear(void)
{
    if (!s_buf) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_head = 0;
    s_count = 0;
    portEXIT_CRITICAL(&s_mux);
}

size_t log_capture_size(void)
{
    if (!s_buf) {
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    size_t n = s_count;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
