#pragma once

/**
 * @file settings_wifi_internal.h
 * @brief Layout seam for the Panel Mode WiFi screens.
 *
 * settings_wifi.c builds every WiFi screen exactly as it does on square and
 * publishes the handles below; a round fit pass, compiled only on the round
 * family and called from settings_hub_round_fit(), re-places them. No radio
 * call, no state machine change, no new event callback.
 *
 * The header row is NOT published here: settings_hub_make_header() already
 * publishes hub_header_obj (settings_hub_internal.h) for every settings and
 * WiFi screen that has one, and settings_hub_goto() clears it before each
 * build, so a second parallel handle would only be a copy that can go stale.
 *
 * Display lock held by the caller.
 */

#include "settings_hub.h"
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern lv_obj_t *wifi_list_obj;        /* scan screen's scroller; NULL on the home screen,
                                          whose rows are direct children of the screen */
extern lv_obj_t *wifi_prow_obj;        /* password row, else NULL */
extern lv_obj_t *wifi_srow_obj;        /* hidden-network SSID row, else NULL */
extern lv_obj_t *wifi_rescan_obj;      /* RESCAN button (scan screen), else NULL */
extern lv_obj_t *wifi_connect_obj;     /* CONNECT button, else NULL */
extern lv_obj_t *wifi_kb_obj;          /* on-screen keyboard, else NULL */
extern bool      wifi_hidden_variant;  /* password screen built without a candidate SSID */

/** Round composition pass for the four WiFi screens. */
void settings_wifi_round_fit(lv_obj_t *screen, hub_screen_t which);

#ifdef __cplusplus
}
#endif
