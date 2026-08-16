#pragma once

#include <stdbool.h>

/**
 * @brief NET TRACE debug overlay: WiFi TX/RX activity sparkline, scheduler
 * countdowns and a packet-source ticker over the net_trace ring.
 *
 * Fullscreen modal on lv_layer_top(); created on show(), destroyed on hide().
 * Opened by tapping the System info page background while debug_mode is on.
 * All functions must be called under the LVGL lock.
 */
void nina_net_debug_show(void);
void nina_net_debug_hide(void);
bool nina_net_debug_visible(void);
void nina_net_debug_apply_theme(void);
