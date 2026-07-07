#pragma once

/**
 * @file poll_task.h
 * @brief Shared poll-loop skeleton for independent background pollers.
 *
 * Expresses the shape common to allsky_poll_task (main/tasks.c) and
 * weather_poll_task (main/weather_client.c): wait for WiFi once, then loop
 * suspending while OTA is in progress or (optionally) while some page-active
 * flag is false, call a poll function, and sleep for a live-config interval
 * -- with exponential backoff on failure if the caller opts in.
 *
 * The pure backoff step lives in poll_backoff.h (host-testable). This header
 * + poll_task.c own the FreeRTOS wait/notify plumbing around it.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    const char *name;              /**< Log tag for this poll loop. */
    EventGroupHandle_t wifi_group; /**< Waited on once before the first poll. */
    EventBits_t wifi_bits;         /**< Bits to wait for (e.g. WIFI_CONNECTED_BIT). */

    /** Optional gate; NULL = always active. Matches the `_Atomic bool
     * xxx_page_active` flags declared in tasks.h (e.g. allsky_page_active,
     * clock_page_active). */
    _Atomic bool *page_active;

    /** Perform one fetch. Return false on failure (triggers backoff, if
     * configured). @p arg is the opaque pointer passed to poll_loop_run(). */
    bool (*poll_once)(void *arg);

    /** Live-config poll interval in ms, re-read every cycle so a config
     * change takes effect on the very next sleep. */
    uint32_t (*interval_ms)(void *arg);

    uint32_t backoff_initial_ms; /**< 0 = no failure backoff (retry at interval_ms()). */
    uint32_t backoff_max_ms;     /**< Ceiling the doubling saturates at; 0 = unbounded. */
} poll_loop_spec_t;

/**
 * Run the poll loop described by @p spec. Never returns -- call this as (or
 * from) a FreeRTOS task entry point.
 */
void poll_loop_run(const poll_loop_spec_t *spec, void *arg);
