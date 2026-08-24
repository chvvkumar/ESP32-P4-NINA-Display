#pragma once

#include <stdint.h>

void wifi_switch_to_network(int index);
int wifi_get_current_network_index(void);

/* Build the wifi_config_t for saved slot @p index and issue esp_wifi_connect().
 * Non-static in main.c since the setup web handler; declared here so the
 * wifi_join backend can re-target a slot without a local extern. */
void wifi_connect_to_slot(int index);

/* Stop the multi-network reconnect timer while the on-device join flow owns
 * the radio, and re-arm it (one shot, 1 s) when normal recovery should
 * resume. Implemented in main.c next to the timer they control. */
void wifi_suspend_auto_reconnect(void);
void wifi_resume_auto_reconnect(void);

/* Point the reconnect walk at saved slot @p index (resets the per-network
 * attempt counters). Called after a successful on-device join so the walk
 * rejoins the new network, not the pre-join one. */
void wifi_manager_adopt_slot(int index);

/* Apply the configured WiFi TX power cap to the C6 radio.
 * @p dbm == 0 leaves the chip default (maximum) in place; any other value is
 * a dBm cap. Capping TX power keeps the radio's current bursts from sagging
 * the board rail, which glitches the display panel. Safe to call on every
 * link-up and on every screen-sleep wake. */
void wifi_apply_tx_power(uint8_t dbm);
