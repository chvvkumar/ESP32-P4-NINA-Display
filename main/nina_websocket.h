#pragma once

/**
 * @file nina_websocket.h
 * @brief WebSocket client for NINA event-driven updates.
 */

#include "nina_client.h"

/**
 * @brief Start WebSocket connection to NINA.
 * Derives ws:// URL from the HTTP API base_url.
 */
void nina_websocket_start(const char *base_url, nina_client_t *data);

/**
 * @brief Stop and destroy the WebSocket connection.
 */
void nina_websocket_stop(void);
