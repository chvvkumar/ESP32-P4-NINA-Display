#pragma once

/**
 * @file nina_connection.h
 * @brief Centralized NINA instance connection state manager.
 *
 * Provides a single source of truth for whether each NINA instance is
 * reachable (REST API) and whether its WebSocket is connected.  Applies
 * hysteresis so a single HTTP timeout does not flash "Not Connected": a
 * CONNECTED instance is only demoted once NINA_CONN_GRACE_MULT x
 * connection_timeout_s has elapsed since its last successful poll, which
 * rides out one hung HTTP connect (~9.5 s) on a 2 s poll cycle.
 *
 * Thread safety: state is written from the data task (REST polls) and the
 * WebSocket event handler (ws_connected field only).  Query functions use
 * atomic reads and are safe to call from any context.  The outage_warned
 * bookkeeping field is written and read only on the polling paths inside
 * nina_connection.c, so it needs no additional synchronization.
 */

#include <stdbool.h>
#include <stdint.h>

#ifndef MAX_NINA_INSTANCES
#define MAX_NINA_INSTANCES 3
#endif

/* Patience multiplier applied to connection_timeout_s before a CONNECTED
 * instance is demoted to DISCONNECTED.  connection_timeout_s defaults to 6 s,
 * so the default grace window is 18 s; devices configured for 10 s get 30 s.
 *
 * How that lands per polling tier:
 *   - Foreground poll (update_rate_s, 1-10 s, default 2 s): several polls fit
 *     inside the window, so one hung HTTP connect (~9.5 s) never demotes.
 *   - Background NINA-page heartbeat (HEARTBEAT_INTERVAL_MS = 10 s, tasks.c):
 *     roughly 2 consecutive misses are needed at the 18 s default.
 *   - Idle tier when no NINA page is showing (idle_poll_interval_s, 5-120 s,
 *     default 30 s): the first miss already exceeds the default window, so the
 *     instance demotes immediately on that miss. */
#define NINA_CONN_GRACE_MULT 3

/* ── Connection state machine ──────────────────────────────────────── */

typedef enum {
    NINA_CONN_UNKNOWN,        ///< Never polled yet (boot default)
    NINA_CONN_CONNECTING,     ///< First poll attempt in progress
    NINA_CONN_CONNECTED,      ///< REST API reachable
    NINA_CONN_DISCONNECTED,   ///< REST API unreachable (confirmed)
} nina_conn_state_t;

typedef struct {
    nina_conn_state_t state;
    bool              ws_connected;         ///< WebSocket independently connected
    bool              static_data_ready;    ///< Profile/filters/switch fetched at least once
    int64_t           last_connected_ms;    ///< Timestamp of last successful REST poll (ms)
    int64_t           last_state_change_ms; ///< When state last transitioned (ms)
    int               consecutive_failures; ///< Failed REST polls since last success
    int               consecutive_successes;///< Successful REST polls since last failure
    bool              outage_warned;        ///< "not responding" toast is live for this outage
} nina_conn_info_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/** Initialize the module. Call once from app_main() before tasks start. */
void nina_connection_init(void);

/* ── State reporters (called by polling / WebSocket code) ──────────── */

/**
 * Report a REST poll result for an instance.
 * Called after fetch_camera_info_robust() in every poll variant.
 * Returns the authoritative connection state after applying hysteresis.
 */
nina_conn_state_t nina_connection_report_poll(int instance, bool success);

/**
 * Force an instance to DISCONNECTED immediately, from any state.
 *
 * For explicit shutdowns only (instance disabled or unconfigured), where the
 * device already knows the instance is gone and the patience window would only
 * delay the UI.  Skips the timeout logic and emits no user-facing toast; a
 * genuine poll failure must still go through nina_connection_report_poll().
 */
void nina_connection_force_disconnect(int instance);

/**
 * Report WebSocket connect/disconnect for an instance.
 * Called from the WebSocket event handler.
 */
void nina_connection_report_ws(int instance, bool connected);

/** Mark instance as CONNECTING (call before first poll attempt). */
void nina_connection_set_connecting(int instance);

/** Mark whether static data (profile, filters, switch) has been fetched. */
void nina_connection_set_static_data_ready(int instance, bool ready);

/* ── Query functions (thread-safe, callable from any context) ──────── */

/** Get the full connection state enum for an instance. */
nina_conn_state_t nina_connection_get_state(int instance);

/** Convenience: returns true only if state == NINA_CONN_CONNECTED. */
bool nina_connection_is_connected(int instance);

/** Returns true if the WebSocket for this instance is connected. */
bool nina_connection_is_ws_connected(int instance);

/** Returns true if static data has been fetched at least once. */
bool nina_connection_has_static_data(int instance);

/** Timestamp (ms) of last successful REST poll, or 0 if never connected. */
int64_t nina_connection_last_seen_ms(int instance);

/** Get read-only pointer to the full connection info struct. */
const nina_conn_info_t *nina_connection_get_info(int instance);

/** Count of currently connected instances (state == CONNECTED). */
int nina_connection_connected_count(void);
