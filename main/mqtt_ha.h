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

#ifdef __cplusplus
}
#endif
