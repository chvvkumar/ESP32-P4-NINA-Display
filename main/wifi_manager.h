#pragma once

#include <stdint.h>

void wifi_switch_to_network(int index);
int wifi_get_current_network_index(void);

/* Apply the configured WiFi TX power cap to the C6 radio.
 * @p dbm == 0 leaves the chip default (maximum) in place; any other value is
 * a dBm cap. Capping TX power keeps the radio's current bursts from sagging
 * the board rail, which glitches the display panel. Safe to call on every
 * link-up and on every screen-sleep wake. */
void wifi_apply_tx_power(uint8_t dbm);
