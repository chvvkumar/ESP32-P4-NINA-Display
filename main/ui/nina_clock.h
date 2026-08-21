#pragma once

/**
 * @file nina_clock.h
 * @brief Clock page — editorial dark clockface with weather data.
 *
 * Always-present page showing time, date, current conditions,
 * and 10-hour forecast. Seven layouts selected by config
 * clock_layout: 0 = Classic, 1 = Console 92, 2 = Broadside,
 * 3 = Evensong, 4 = Blueprint, 5 = Transit Line, 6 = Night
 * Network. Uses fixed per-layout color palettes independent
 * of the dashboard theme (except Red Night, which remaps every
 * color to red).
 */

#include "lvgl.h"

/** Create the clock page widget tree. */
lv_obj_t *clock_page_create(lv_obj_t *parent);

/** Refresh time/date labels and weather data from weather_client. */
void clock_page_update(void);

/** Recolor the page for the active theme (Red Night vs fixed editorial palette). */
void clock_page_apply_theme(void);

/** Schedule an immediate UI refresh on the next LVGL tick. Thread-safe. */
void clock_page_request_update(void);

/**
 * Rebuild the page content for the currently configured layout
 * (app_config clock_layout). Keeps the root object alive so the
 * dashboard's page pointer stays valid. Caller must hold the LVGL
 * display lock.
 */
void clock_page_refresh_config(void);

/** Pause the clock timer (call when page is hidden). */
void clock_page_on_hide(void);

/** Resume the clock timer aligned to next minute boundary (call when page is shown). */
void clock_page_on_show(void);
