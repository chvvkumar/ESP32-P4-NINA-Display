#ifndef NINA_DASHBOARD_H
#define NINA_DASHBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "nina_client.h"
#include "app_config.h"
#include "themes.h"

/**
 * @brief Initialize the multi-page Bento NINA Dashboard (720x720px grid layout)
 * @param parent The parent LVGL object (usually the screen)
 * @param instance_count Number of NINA instances to create pages for (1-3)
 */
void create_nina_dashboard(lv_obj_t * parent, int instance_count);

/**
 * @brief Update a single dashboard page with live NINA client data
 * @param instance NINA instance index (0..MAX_NINA_INSTANCES-1); gates on nina_slot_available[instance]
 * @param data Pointer to the NINA client data for this instance
 */
void update_nina_dashboard_page(int instance, const nina_client_t *data);

/**
 * @brief Switch the visible dashboard page (instant)
 * @param page_index Index of the page to show (0-2)
 * @param instance_count Total number of configured instances
 */
void nina_dashboard_show_page(int page_index, int instance_count);

/**
 * @brief Switch the visible dashboard page with an optional transition effect
 * @param page_index Index of the page to show (0-2)
 * @param instance_count Total number of configured instances
 * @param effect Transition effect: 0 = instant, 1 = fade, 2 = slide-left, 3 = slide-right
 */
void nina_dashboard_show_page_animated(int page_index, int instance_count, int effect);

/**
 * @brief Get the currently active page index
 * @return Active page index (0-2)
 */
int nina_dashboard_get_active_page(void);

/** Get the currently active theme. */
const theme_t *nina_dashboard_get_current_theme(void);

/** Alias for nina_dashboard_get_current_theme(). */
const theme_t *nina_dashboard_get_theme(void);

/**
 * @brief Apply a specific theme to all dashboard pages
 * @param theme_index Index of the theme to apply
 */
void nina_dashboard_apply_theme(int theme_index);

/**
 * @brief Update the WiFi signal bars and connection indicator dot for a page.
 *
 * Call this both before an API request (api_active=true, starts pulse) and
 * after (api_active=false, stops pulse).  Safe to call under the display lock.
 *
 * @param instance       NINA instance index (0..MAX_NINA_INSTANCES-1); gates on nina_slot_available[instance]
 * @param rssi           Current WiFi RSSI in dBm; -100 when unknown
 * @param nina_connected true when the NINA HTTP API is reachable
 * @param api_active     true while an HTTP request is in-flight
 */
void nina_dashboard_update_status(int instance, int rssi, bool nina_connected, bool api_active);

/**
 * @brief Callback type for page change events (triggered by swipe gestures)
 * @param new_page The page index that was switched to
 */
typedef void (*nina_page_change_cb_t)(int new_page);

/**
 * @brief Set callback for page change events from swipe gestures
 * @param cb Callback function to invoke on page change
 */
void nina_dashboard_set_page_change_cb(nina_page_change_cb_t cb);

/**
 * @brief Check if the active page is the AllSky page
 * @return true if the AllSky page is currently shown
 */
bool nina_dashboard_is_allsky_page(void);

/**
 * @brief Enable or disable the AllSky page at runtime.
 * When disabled, switches away if currently viewing and removes from navigation.
 * Must be called with LVGL display lock held.
 */
void nina_dashboard_set_allsky_enabled(bool enabled);

/**
 * @brief Check if the active page is the Spotify page
 * @return true if the Spotify page is currently shown
 */
bool nina_dashboard_is_spotify_page(void);

/**
 * @brief Enable or disable the Spotify page at runtime.
 * When disabled, switches away if currently viewing and removes from navigation.
 * Must be called with LVGL display lock held.
 */
void nina_dashboard_set_spotify_enabled(bool enabled);

/**
 * @brief Check if the active page is the Image Display page
 * @return true if the Image Display page is currently shown
 */
bool nina_dashboard_is_image_display_page(void);

/**
 * @brief Enable or disable the Image Display page at runtime.
 * When disabled, switches away if currently viewing and removes from navigation.
 * Must be called with LVGL display lock held.
 */
void nina_dashboard_set_image_display_enabled(bool enabled);

/**
 * @brief Check if the active page is the JSON Display page
 * @return true if the JSON Display page is currently shown
 */
bool nina_dashboard_is_json_page(void);

/**
 * @brief Enable or disable the JSON Display page at runtime.
 * When disabled, switches away if currently viewing and removes from navigation.
 * Must be called with LVGL display lock held.
 */
void nina_dashboard_set_json_enabled(bool enabled);

/**
 * @brief Check if the active page is the Home Assistant page
 * @return true if the Home Assistant page is currently shown
 */
bool nina_dashboard_is_ha_page(void);

/**
 * @brief Enable or disable the Home Assistant page at runtime.
 * When disabled, switches away if currently viewing and removes from navigation.
 * Must be called with LVGL display lock held.
 */
void nina_dashboard_set_ha_enabled(bool enabled);

/**
 * @brief Check if the active page is the clock page
 * @return true if the clock page is currently shown
 */
bool nina_dashboard_is_clock_page(void);

/**
 * @brief Check if the active page is the summary page
 * @return true if the summary page is currently shown
 */
bool nina_dashboard_is_summary_page(void);

/**
 * @brief Check if the active page is the settings page
 * @return true if the settings page is currently shown
 */
bool nina_dashboard_is_settings_page(void);

/**
 * @brief Check if the active page is the system info page
 * @return true if the system info page is currently shown
 */
bool nina_dashboard_is_sysinfo_page(void);

/**
 * @brief Get the total number of navigable pages (NINA + sysinfo)
 * @return Total navigable page count
 */
int nina_dashboard_get_total_page_count(void);

/**
 * @brief Check whether a page index currently maps to an existing, navigable page.
 *
 * Returns false for optional pages that are disabled (AllSky, Spotify, Image
 * Display) and for NINA instance indices beyond the enabled instance count.
 * Used to validate idle/park targets so the screen never goes blank.
 *
 * @param page_idx Absolute page index (e.g. PAGE_IDX_SUMMARY, NINA_PAGE_OFFSET+n)
 * @return true if the page object exists and can be shown
 */
bool nina_dashboard_page_is_available(int page_idx);

/** Recompute availability for one NINA instance and create/destroy its page
 *  one slot at a time. No full dashboard teardown. Call under display lock. */
void nina_dashboard_rebuild_slot(int instance);

/** True iff NINA instance `instance` currently has an allocated, navigable page. */
bool nina_dashboard_slot_available(int instance);

/**
 * @brief Check if a thumbnail image has been requested (target name was clicked)
 * @return true if thumbnail fetch is needed
 */
bool nina_dashboard_thumbnail_requested(void);

/**
 * @brief Clear the thumbnail request flag (call after starting the fetch)
 */
void nina_dashboard_clear_thumbnail_request(void);

/**
 * @brief Set decoded thumbnail image data for display
 * @param rgb565_data Heap-allocated RGB565 pixel data (ownership transferred to dashboard)
 * @param w Image width in pixels
 * @param h Image height in pixels
 * @param data_size Size of rgb565_data in bytes
 */
void nina_dashboard_set_thumbnail(const uint8_t *rgb565_data, uint32_t w, uint32_t h, uint32_t data_size);

/**
 * @brief Hide the thumbnail overlay and free image memory
 */
void nina_dashboard_hide_thumbnail(void);

/**
 * @brief Check if thumbnail overlay is currently visible
 * @return true if visible
 */
bool nina_dashboard_thumbnail_visible(void);

/**
 * @brief Map an ABSOLUTE page index to its NINA instance index.
 *
 * Pure offset (abs - NINA_PAGE_OFFSET) over the reserved NINA band; exact
 * inverse of nina_dashboard_instance_to_page. Availability is NOT folded in:
 * an index inside the band maps even for an unavailable slot. Test availability
 * separately (e.g. nina_slot_available[]).
 *
 * @param abs_page_idx Absolute page index
 * @return Instance index (0..MAX_NINA_INSTANCES-1), or -1 if outside the band
 */
int nina_dashboard_page_to_instance(int abs_page_idx);

/**
 * @brief Map a NINA instance index to its ABSOLUTE page index.
 *
 * Pure offset (NINA_PAGE_OFFSET + instance) over the reserved NINA band; exact
 * inverse of nina_dashboard_page_to_instance. Returns the index regardless of
 * slot availability. Test availability separately (e.g. nina_slot_available[]).
 *
 * @param instance_idx Instance index (0..MAX_NINA_INSTANCES-1)
 * @return Absolute page index, or -1 only for an out-of-range instance index
 */
int nina_dashboard_instance_to_page(int instance_idx);

#ifdef __cplusplus
}
#endif

#endif // NINA_DASHBOARD_H
