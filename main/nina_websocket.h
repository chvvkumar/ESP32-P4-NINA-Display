#pragma once

/**
 * @file nina_websocket.h
 * @brief WebSocket client for NINA event-driven updates.
 *
 * Supports one persistent WebSocket connection per NINA instance,
 * allowing all instances to receive real-time events concurrently.
 */

#include "nina_client.h"
#include "app_config.h"

/**
 * @brief Start (or restart) a WebSocket connection for the given instance index.
 * Derives ws:// URL from the HTTP API base_url. If a connection already exists
 * for this index, it is stopped first.
 */
void nina_websocket_start(int index, const char *base_url, nina_client_t *data);

/**
 * @brief Stop and destroy the WebSocket connection for the given instance index.
 */
void nina_websocket_stop(int index);

/**
 * @brief Stop and destroy all active WebSocket connections.
 */
void nina_websocket_stop_all(void);

/**
 * @brief Check if a WebSocket connection needs reconnection and attempt it.
 * Uses exponential backoff: starts at 5 s, doubles on failure, caps at 60 s.
 * Resets to 5 s on successful connection.
 * Call this periodically from the data task loop.
 */
void nina_websocket_check_reconnect(int index, const char *base_url, nina_client_t *data);

/**
 * @brief Check for deferred camera-disconnect alerts.
 * NINA fires CAMERA-DISCONNECTED before CAMERA-CONNECTED during a connect
 * sequence. This defers the disconnect toast by 3 s so it only shows for
 * true disconnects. Call periodically from the data task loop.
 */
void nina_websocket_check_deferred_alerts(int index);
