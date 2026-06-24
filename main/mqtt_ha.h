#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the MQTT client and publish Home Assistant discovery configs.
 *        Call after WiFi is connected and config is loaded.
 */
void mqtt_ha_start(void);

/**
 * @brief Stop and destroy the MQTT client.
 */
void mqtt_ha_stop(void);

/**
 * @brief Publish current brightness state to MQTT.
 *        Call after brightness changes from web UI or other sources.
 */
void mqtt_ha_publish_state(void);

/**
 * @brief Check if MQTT client is connected to the broker.
 */
bool mqtt_ha_is_connected(void);

/**
 * @brief Apply any pending MQTT commands (brightness, text brightness, theme,
 *        reboot) queued by the MQTT event callback.
 *
 * Must be called from a UI-context task that may take the display lock
 * (e.g. data_update_task). The MQTT event callback only parses and enqueues;
 * the actual apply and config persist happen here so the esp-mqtt event loop
 * is never blocked. Safe to call when MQTT is disabled (no-op).
 */
void mqtt_ha_process_pending(void);

#ifdef __cplusplus
}
#endif
