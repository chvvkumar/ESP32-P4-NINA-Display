#pragma once

/**
 * @file net_diag.h
 * @brief One-shot outage triage — answers "is it just that NINA computer, or is
 *        this device's whole network dead?"
 *
 * When a poll to a NINA computer first fails, the caller invokes
 * net_diag_log_outage() once for that outage episode. The module ICMP-pings the
 * WiFi gateway and (when MQTT is configured) the MQTT broker, then emits a
 * single ESP_LOGW line under the tag "net_diag" with a wall-clock timestamp and
 * a plain-language verdict, e.g.
 *
 *   2026-08-04 14:32:07 | astromele2 not answering. Gateway: OK, broker: OK
 *       -> problem is astromele2 or path to it
 *
 * Rate limiting is the caller's job (call once per outage episode); the module
 * additionally suppresses any call made within 10 s of the previous one as a
 * guard against future misuse.
 */

/**
 * @brief Probe the local network and log a one-line outage verdict.
 *
 * Synchronous and blocking — worst case roughly 3.0 s, all of it ICMP: two
 * probes of a 1 s echo timeout plus 0.5 s of completion slack each. Name
 * resolution contributes nothing, because the broker host is either a numeric
 * literal or resolved from the app-level DNS cache; a host with neither is
 * reported as "unresolvable" rather than probed, so no blocking DNS retry ever
 * runs here. Safe to call from any FreeRTOS task on either core. Makes no LVGL
 * calls and leaves no allocation behind.
 *
 * Re-entrant callers are dropped, not queued: a call arriving while another run
 * is still in flight returns immediately without logging.
 *
 * @param failed_host Display name of the NINA host that stopped answering.
 *                    Used only in the log line; may be NULL.
 */
void net_diag_log_outage(const char *failed_host);
