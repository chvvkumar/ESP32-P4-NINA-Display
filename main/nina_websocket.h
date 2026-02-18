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
