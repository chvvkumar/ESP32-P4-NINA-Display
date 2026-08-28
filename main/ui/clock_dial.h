#pragma once

/**
 * @file clock_dial.h
 * @brief Where a forecast entry sits on the Classic round face's 12-hour dial.
 *
 * Pure, header-only, no LVGL and no ESP-IDF: host-tested by
 * test/host/test_clock_dial.c. Angles are dial degrees, 0 at twelve o'clock,
 * increasing clockwise, 30 degrees per hour.
 *
 * The step between entries is read out of the hour array, never out of the
 * configured provider: weather_client fills hourly_hours[] with consecutive
 * hours on Open-Meteo and Weather Underground, and with the OpenWeatherMap
 * 3-hourly grid behind a "now" slot on OpenWeatherMap, whose first gap is
 * therefore 0 to 2 hours rather than 3.
 */

#include <stdint.h>

#define CLOCK_DIAL_HOURS      12
#define CLOCK_DIAL_DEG_PER_H  30

/**
 * @brief Signed whole hours from @p base to @p h, in -12..11.
 *
 * OpenWeatherMap's 3-hourly list can start at the grid slot that CONTAINS the
 * current hour, so slot 1 can be level with or a couple of hours behind slot 0
 * (13:20 with list[0] = 12:00 gives hours {13, 12, ...}). An unsigned modulo
 * would read that as 23 hours ahead and swallow the whole forecast, so the
 * delta is signed and a stale slot reads negative.
 */
static inline int clock_dial_delta(uint8_t base, uint8_t h)
{
    int d = (int)h - (int)base;
    if (d < -12) d += 24;
    if (d > 11) d -= 24;
    return d;
}

/**
 * @brief Lay the forecast entries out on the dial.
 *
 * Entry i starts at the clock position of hours[i] % 12 and spans the hours
 * until the next entry that actually moves FORWARD of it (the last such entry
 * reuses the previous gap), clipped so no block runs past 12 hours from
 * hours[0]. An entry that is level with or behind the newest block is a stale
 * grid slot and is skipped, not treated as the end of the list. An entry 12 or
 * more hours ahead ends the list.
 *
 * @param hours      forecast hours, 0..23, oldest first (weather_data_t.hourly_hours)
 * @param n          number of entries in @p hours
 * @param start_deg  out: block start in dial degrees, at least @p n entries
 * @param span_deg   out: block span in dial degrees, at least @p n entries
 * @return number of blocks written, 0..n
 */
static inline int clock_dial_blocks(const uint8_t *hours, int n,
                                    int16_t *start_deg, int16_t *span_deg)
{
    if (!hours || !start_deg || !span_deg || n <= 0) return 0;

    /* A provider whose forecast leg failed leaves the whole array at zero
     * while the current-conditions leg still reports valid. "0 degrees at
     * midnight" is not a forecast: draw nothing. */
    int moved = 0;
    for (int i = 1; i < n; i++) {
        if (hours[i] != hours[0]) { moved = 1; break; }
    }
    if (!moved) return 0;

    int cnt = 0;
    int prev_off = -1;

    for (int i = 0; i < n; i++) {
        int off = clock_dial_delta(hours[0], hours[i]);
        if (i > 0 && off <= prev_off) continue;   /* stale or duplicate slot */
        if (off >= CLOCK_DIAL_HOURS) break;       /* past the half day */

        /* Span to the next entry that is genuinely later than this one. */
        int next = -1;
        for (int j = i + 1; j < n; j++) {
            int d = clock_dial_delta(hours[0], hours[j]);
            if (d > off) { next = d; break; }
        }
        int step = (next >= 0) ? (next - off)
                               : ((prev_off >= 0) ? (off - prev_off) : 1);
        if (step < 1) step = 1;
        if (step > CLOCK_DIAL_HOURS - off) step = CLOCK_DIAL_HOURS - off;

        start_deg[cnt] = (int16_t)(((int)hours[i] % 12) * CLOCK_DIAL_DEG_PER_H);
        span_deg[cnt] = (int16_t)(step * CLOCK_DIAL_DEG_PER_H);
        cnt++;
        prev_off = off;
    }
    return cnt;
}
