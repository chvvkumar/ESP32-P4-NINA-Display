/**
 * @file nina_connection.c
 * @brief Centralized NINA instance connection state manager.
 *
 * Owns the authoritative connection state for all NINA instances.
 * Uses a time-based timeout (configurable via web UI) to determine when
 * an instance is considered offline, preventing single-failure disconnects.
 */

#include "nina_connection.h"
#include "app_config.h"
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
            /* Stay connected */
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
            /* Time-based timeout: only transition to DISCONNECTED if the
             * configured timeout has elapsed since the last successful poll. */
            int64_t timeout_ms = (int64_t)app_config_get()->connection_timeout_s * 1000;
            if (c->last_connected_ms > 0 && (ts - c->last_connected_ms) >= timeout_ms) {
                c->state = NINA_CONN_DISCONNECTED;
                c->static_data_ready = false;
            }
            break;
        }
        case NINA_CONN_DISCONNECTED:
            /* Stay disconnected, reset static data on each failure */
            c->static_data_ready = false;
            break;
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
