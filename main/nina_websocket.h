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
 * @brief Legacy stub — camera disconnect grace period removed.
 * Kept for API compatibility. Now a no-op.
 */
void nina_websocket_check_deferred_alerts(int index);

/**
 * @brief Update the "ever connected" equipment mask for an instance.
 * Call from the HTTP poll task after parsing /equipment/info to seed
 * the mask from NINA's actual Connected state. This ensures disconnect
 * toasts are only shown for equipment that has a driver selected.
 * Bits: 0=Camera, 1=Mount, 2=Guider, 3=Focuser, 4=Filterwheel,
 *       5=Rotator, 6=Safety, 7=Dome, 8=Flat, 9=Switch, 10=Weather
 */
void nina_websocket_update_equipment_mask(int index, uint16_t connected_mask);

/**
 * @brief Check if a WebSocket client exists (started) for this instance.
 */
bool nina_websocket_is_running(int index);
