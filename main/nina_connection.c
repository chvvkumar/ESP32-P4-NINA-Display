/**
 * @file nina_connection.c
 * @brief Centralized NINA instance connection state manager.
 *
 * Owns the authoritative connection state for all NINA instances.
 * Uses a time-based timeout (configurable via web UI) scaled by
 * NINA_CONN_GRACE_MULT to determine when an instance is considered offline,
 * preventing single-failure disconnects.
 */

#include "nina_connection.h"
#include "app_config.h"
#include "net_diag.h"
#include "ui/nina_toast.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nina_conn";

static nina_conn_info_t s_instances[MAX_NINA_INSTANCES];

/* ── Helpers ───────────────────────────────────────────────────────── */

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static bool valid_index(int i) {
    return i >= 0 && i < MAX_NINA_INSTANCES;
}

/* Outage toasts use the same gate as the WebSocket connect toasts in
 * nina_websocket.c: notify-mask bit 0 plus the per-instance mute switch. */
static bool outage_toasts_enabled(int instance) {
    const app_config_t *cfg = app_config_get();
    return (cfg->toast_notify_mask & (1 << 0)) && !cfg->toast_instance_muted[instance];
}

/* Display name for an instance ("astromele2.lan"), with a generic fallback so
 * a blank/unparseable URL never produces an empty toast. */
static const char *instance_host(int instance, char *buf, size_t buf_size) {
    app_config_get_instance_host(instance, buf, buf_size);
    return buf[0] ? buf : "NINA host";
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void nina_connection_init(void) {
    memset(s_instances, 0, sizeof(s_instances));
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        s_instances[i].state = NINA_CONN_UNKNOWN;
    }
    ESP_LOGI(TAG, "Connection state manager initialized (timeout=%ds)",
             app_config_get()->connection_timeout_s);
}

/* ── State reporters ───────────────────────────────────────────────── */

void nina_connection_set_connecting(int instance) {
    if (!valid_index(instance)) return;
    nina_conn_info_t *c = &s_instances[instance];
    if (c->state == NINA_CONN_UNKNOWN) {
        c->state = NINA_CONN_CONNECTING;
        c->last_state_change_ms = now_ms();
        ESP_LOGI(TAG, "Instance %d: UNKNOWN -> CONNECTING", instance);
    }
}

nina_conn_state_t nina_connection_report_poll(int instance, bool success) {
    if (!valid_index(instance)) return NINA_CONN_UNKNOWN;
    nina_conn_info_t *c = &s_instances[instance];
    int64_t ts = now_ms();
    nina_conn_state_t prev = c->state;

    /* Outage-notification bookkeeping for this call (see the block below). */
    bool    episode_start = false;   /* first failed poll of a new outage */
    bool    recovered     = false;   /* success while an outage warning is live */
    int64_t remaining_ms  = 0;       /* grace left when the outage started */

    if (success) {
        c->consecutive_successes++;
        c->consecutive_failures = 0;
        c->last_connected_ms = ts;

        switch (c->state) {
        case NINA_CONN_UNKNOWN:
        case NINA_CONN_CONNECTING:
            c->state = NINA_CONN_CONNECTED;
            break;
        case NINA_CONN_DISCONNECTED:
            /* Reconnect immediately on first success */
            c->state = NINA_CONN_CONNECTED;
            break;
        case NINA_CONN_CONNECTED:
            /* Stay connected — the instance answered before the grace window
             * ran out, so retract the warning we showed when it went quiet. */
            if (c->outage_warned) {
                recovered = true;
                c->outage_warned = false;
            }
            break;
        }
    } else {
        c->consecutive_failures++;
        c->consecutive_successes = 0;

        switch (c->state) {
        case NINA_CONN_UNKNOWN:
        case NINA_CONN_CONNECTING:
            /* Fail fast on initial connection attempt */
            c->state = NINA_CONN_DISCONNECTED;
            break;
        case NINA_CONN_CONNECTED: {
            /* Demote only when both patience conditions hold: GRACE_MULT x the
             * configured timeout has elapsed since the last successful poll AND
             * at least NINA_CONN_MIN_FAILURES consecutive polls have failed.
             * The elapsed window keeps one hung HTTP connect (~9.5 s) from
             * taking a rig offline; the failure count keeps a poll tier slower
             * than that window (idle tier) from demoting on a single miss. */
            int64_t timeout_ms = (int64_t)app_config_get()->connection_timeout_s * 1000;
            int64_t grace_ms = timeout_ms * NINA_CONN_GRACE_MULT;
            int64_t quiet_ms = (c->last_connected_ms > 0) ? (ts - c->last_connected_ms) : 0;
            if (c->last_connected_ms > 0 && quiet_ms >= grace_ms &&
                c->consecutive_failures >= NINA_CONN_MIN_FAILURES) {
                c->state = NINA_CONN_DISCONNECTED;
                c->static_data_ready = false;
            } else if (c->consecutive_failures == 1) {
                /* First miss of this outage: warn the user with a countdown. */
                episode_start = true;
                remaining_ms = grace_ms - quiet_ms;
            }
            break;
        }
        case NINA_CONN_DISCONNECTED:
            /* Stay disconnected, reset static data on each failure */
            c->static_data_ready = false;
            break;
        }
    }

    /* Outage notifications. The warning countdown, its retraction, and the
     * final "offline" toast all fire from here so every navigation-visible
     * connection change has exactly one user-facing message. */
    bool demoted = (prev == NINA_CONN_CONNECTED && c->state == NINA_CONN_DISCONNECTED);
    if (episode_start || recovered || demoted) {
        char host_buf[48];
        const char *host = instance_host(instance, host_buf, sizeof(host_buf));
        bool toasts_on = outage_toasts_enabled(instance);

        if (episode_start) {
            /* Only worth a countdown if the user can read it before it expires. */
            if (toasts_on && remaining_ms > 3000) {
                nina_toast_show_timed(TOAST_WARNING, (uint32_t)remaining_ms,
                                      "[%d] %s not responding - offline in %ds",
                                      instance + 1, host, (int)(remaining_ms / 1000));
                c->outage_warned = true;
            }
            /* Triage the local network once per outage. Blocking (~3.0 s worst
             * case) and internally rate-limited; accepted here so the log line
             * lands at the moment the outage started. */
            net_diag_log_outage(host);
        }
        if (recovered && toasts_on) {
            nina_toast_show_timed(TOAST_SUCCESS, 0, "[%d] %s reconnected",
                                  instance + 1, host);
        }
        if (demoted) {
            if (toasts_on) {
                nina_toast_show_timed(TOAST_WARNING, 0, "[%d] %s offline",
                                      instance + 1, host);
            }
            c->outage_warned = false;
            /* Also triage here, not only on episode_start: that fires on the
             * first failure, while demotion can now land several failures (and
             * minutes) later, so the log line at the moment the rig actually
             * goes offline is the useful one. net_diag's own 10 s guard
             * suppresses the duplicate when the episode already logged one. */
            net_diag_log_outage(host);
        }
    }

    if (c->state != prev) {
        c->last_state_change_ms = ts;
        ESP_LOGI(TAG, "Instance %d: %s -> %s (failures=%d, successes=%d)",
                 instance,
                 prev == NINA_CONN_UNKNOWN ? "UNKNOWN" :
                 prev == NINA_CONN_CONNECTING ? "CONNECTING" :
                 prev == NINA_CONN_CONNECTED ? "CONNECTED" : "DISCONNECTED",
                 c->state == NINA_CONN_CONNECTED ? "CONNECTED" : "DISCONNECTED",
                 c->consecutive_failures, c->consecutive_successes);
    }

    return c->state;
}

void nina_connection_force_disconnect(int instance) {
    if (!valid_index(instance)) return;
    nina_conn_info_t *c = &s_instances[instance];
    /* Idempotent: the disabled-instance sweep calls this every cycle, so do
     * nothing (and log nothing) unless this is an actual transition. */
    if (c->state == NINA_CONN_DISCONNECTED) {
        return;
    }
    c->state = NINA_CONN_DISCONNECTED;
    c->static_data_ready = false;
    c->consecutive_failures = 0;
    c->consecutive_successes = 0;
    c->outage_warned = false;
    c->last_state_change_ms = now_ms();
    ESP_LOGI(TAG, "Instance %d: forced offline (disabled)", instance);
}

void nina_connection_report_ws(int instance, bool connected) {
    if (!valid_index(instance)) return;
    s_instances[instance].ws_connected = connected;
    ESP_LOGD(TAG, "Instance %d: WS %s", instance,
             connected ? "connected" : "disconnected");
}

void nina_connection_set_static_data_ready(int instance, bool ready) {
    if (!valid_index(instance)) return;
    s_instances[instance].static_data_ready = ready;
}

/* ── Query functions ───────────────────────────────────────────────── */

nina_conn_state_t nina_connection_get_state(int instance) {
    if (!valid_index(instance)) return NINA_CONN_UNKNOWN;
    return s_instances[instance].state;
}

bool nina_connection_is_connected(int instance) {
    if (!valid_index(instance)) return false;
    return s_instances[instance].state == NINA_CONN_CONNECTED;
}

bool nina_connection_is_ws_connected(int instance) {
    if (!valid_index(instance)) return false;
    return s_instances[instance].ws_connected;
}

bool nina_connection_has_static_data(int instance) {
    if (!valid_index(instance)) return false;
    return s_instances[instance].static_data_ready;
}

int64_t nina_connection_last_seen_ms(int instance) {
    if (!valid_index(instance)) return 0;
    return s_instances[instance].last_connected_ms;
}

const nina_conn_info_t *nina_connection_get_info(int instance) {
    if (!valid_index(instance)) return NULL;
    return &s_instances[instance];
}

int nina_connection_connected_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (s_instances[i].state == NINA_CONN_CONNECTED) count++;
    }
    return count;
}
