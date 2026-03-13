#pragma once

/**
 * @file nina_spotify.h
 * @brief Spotify immersive player page — full-screen album art with playback controls.
 *
 * Displays album art as background, semi-transparent dim overlay, centered track info,
 * bottom control bar with prev/play-pause/next, progress bar, and idle timer management.
 */

#include "lvgl.h"
#include "spotify_client.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the Spotify page (full-screen, hidden by default).
 * @param parent  The dashboard's main container
 * @return The page object (for dashboard management)
 */
lv_obj_t *spotify_page_create(lv_obj_t *parent);

/**
 * Update the Spotify page with new playback data.
 * Must be called under LVGL display lock.
 */
void nina_spotify_update(const spotify_playback_t *data);

/**
 * Set album art image from decoded RGB565 buffer.
 * Ownership of rgb565_data is transferred -- freed when replaced or page destroyed.
 * Must be called under LVGL display lock.
 */
void nina_spotify_set_album_art(const uint8_t *rgb565_data, uint32_t w, uint32_t h,
                                 uint32_t data_size);

/**
 * Show "nothing playing" idle state.
 * Must be called under LVGL display lock.
 */
void nina_spotify_set_idle(void);

/**
 * Apply current theme colors to the Spotify page.
 */
void spotify_page_apply_theme(void);

/**
 * Check if the controls overlay is currently visible (not idle).
 */
bool nina_spotify_is_overlay_visible(void);

/**
 * Notify Spotify page that it became the active page (start idle timer).
 */
void nina_spotify_on_show(void);

/**
 * Notify Spotify page that it's no longer active (stop idle timer, reset state).
 */
void nina_spotify_on_hide(void);

#ifdef __cplusplus
}
#endif
