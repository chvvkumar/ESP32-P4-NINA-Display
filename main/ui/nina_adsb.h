#pragma once

/**
 * @file nina_adsb.h
 * @brief ADS-B page (Sky Dome / Radar Scope / Board) — three tap-cycled modes
 *        over one adsb_client snapshot.
 *
 * The page is created once (hidden) by the dashboard through the ops table
 * below, fed by nina_adsb_update() on the UI update cadence, and re-synced from
 * config by nina_adsb_config_changed() when the web UI writes any flights_*
 * field.
 *
 * LOCKING: every function here runs with the LVGL display lock held by the
 * CALLER (same contract as nina_json.c / nina_allsky.c). This module never
 * takes the display lock itself. It does take the adsb_client mutex briefly
 * (inside adsb_client_snapshot) and the config mutex; both are leaf locks under
 * the display lock, matching the existing page modules.
 *
 * The ops' show/hide hooks own the poller page gate (adsb_page_active), so the
 * integration side does not have to wire it separately.
 */

#include <stdint.h>

#include "lvgl.h"
#include "page_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lifecycle ops for the ADS-B page. Register with
 * page_registry_set_ops(PAGE_REF_ADSB, nina_adsb_page_ops()) and use the same
 * pointer for the dashboard's optional-page create/enable path.
 *
 * create   builds the page as a child of main_cont and returns its root
 * destroy  NULL (the page is created lazily on first enable, never destroyed)
 * get_obj  the root, or NULL before the first create
 * show     arms the poll gate and repaints immediately
 * hide     disarms the poll gate
 * apply_theme  re-colours the existing widget tree in place
 * is_available root exists AND flights_enabled
 */
const page_ops_t *nina_adsb_page_ops(void);

/**
 * Take a fresh adsb_client snapshot plus the NINA mount pointings, recompute
 * every screen coordinate, and repaint. Cheap enough for a 1 s cadence; safe to
 * call when the page is hidden (it returns early if it was never created).
 *
 * CALLER MUST HOLD THE LVGL DISPLAY LOCK.
 */
void nina_adsb_update(void);

/**
 * Re-read flights_mode / flights_up_azimuth / flights_min_el / flights_range_nm
 * from config and repaint. Call from config_trigger_side_effects() whenever any
 * flights_* field changes, so a web edit and an on-device gesture stay in sync.
 *
 * CALLER MUST HOLD THE LVGL DISPLAY LOCK.
 */
void nina_adsb_config_changed(void);

/** Current display mode: 0 Sky Dome, 1 Radar Scope, 2 Board. */
uint8_t nina_adsb_get_mode(void);

#ifdef __cplusplus
}
#endif
