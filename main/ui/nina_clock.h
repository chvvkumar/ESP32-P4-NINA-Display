#pragma once

/**
 * @file nina_clock.h
 * @brief Clock page — editorial dark clockface with weather data.
 *
 * Always-present page showing time, date, current conditions,
 * and 10-hour forecast bar chart. Uses fixed editorial color
 * palette independent of the dashboard theme.
 */

#include "lvgl.h"

/** Create the clock page widget tree. */
lv_obj_t *clock_page_create(lv_obj_t *parent);

/** Refresh time/date labels and weather data from weather_client. */
void clock_page_update(void);

/** No-op — clock page uses fixed editorial colors, not theme-dependent. */
void clock_page_apply_theme(void);

/** Schedule an immediate UI refresh on the next LVGL tick. Thread-safe. */
void clock_page_request_update(void);
