#ifndef NINA_DASHBOARD_H
#define NINA_DASHBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "nina_client.h"
#include "app_config.h"

/**
 * @brief Initialize the multi-page Bento NINA Dashboard (720x720px grid layout)
 * @param parent The parent LVGL object (usually the screen)
 * @param instance_count Number of NINA instances to create pages for (1-3)
 */
void create_nina_dashboard(lv_obj_t * parent, int instance_count);

/**
 * @brief Update a single dashboard page with live NINA client data
 * @param page_index Index of the page to update (0-2)
 * @param data Pointer to the NINA client data for this page
 */
void update_nina_dashboard_page(int page_index, const nina_client_t *data);

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
 * @param effect Transition effect: 0 = instant, 1 = fade
 */
void nina_dashboard_show_page_animated(int page_index, int instance_count, int effect);

/**
 * @brief Get the currently active page index
 * @return Active page index (0-2)
 */
int nina_dashboard_get_active_page(void);

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
 * @param page_index     Dashboard page to update (0-based)
 * @param rssi           Current WiFi RSSI in dBm; -100 when unknown
 * @param nina_connected true when the NINA HTTP API is reachable
 * @param api_active     true while an HTTP request is in-flight
 */
void nina_dashboard_update_status(int page_index, int rssi, bool nina_connected, bool api_active);

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

#ifdef __cplusplus
}
#endif

#endif // NINA_DASHBOARD_H
