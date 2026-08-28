/* Host test for main/ui/clock_dial.h: mapping the ten weather_client forecast
 * entries onto the Classic round face's 12-hour dial.
 *
 * weather_client fills hourly_hours[10] differently per provider:
 *   Open-Meteo and Weather Underground: ten consecutive hours from now.
 *   OpenWeatherMap 2.5: slot 0 is the current hour, slots 1..9 are the API's
 *   3-hourly grid, so the FIRST gap is short (0 to 2 hours) and the rest are 3.
 * The mapping must read the step out of the data, never out of the config. */
#include "clock_dial.h"

#include <stdio.h>

static int fails = 0;

static void check_int(const char *label, int got, int expect)
{
    printf("%-58s got=%-5d expect=%-5d %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void)
{
    int16_t start[10], span[10];
    uint8_t src[10];

    /* 1-hour steps from 09:00. Twelve hours of dial, ten entries, so every
     * entry is drawn and each spans one hour mark. No slot is ever skipped,
     * so the source index tracks the block index one for one. */
    const uint8_t h1[10] = { 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
    check_int("1h steps: block count", clock_dial_blocks(h1, 10, start, span, src), 10);
    check_int("1h steps: first start = 9 o'clock", start[0], 270);
    check_int("1h steps: first span", span[0], 30);
    check_int("1h steps: entry 3 is noon, back at twelve", start[3], 0);
    check_int("1h steps: last start = 6 o'clock", start[9], 180);
    check_int("1h steps: last span", span[9], 30);
    check_int("1h steps: last src is entry 9 (nothing skipped)", src[9], 9);

    /* 3-hour steps with the short OpenWeatherMap first gap: 10, then the
     * 12/15/18/21 grid. Offsets 0, 2, 5, 8, 11 are inside the half day; the
     * offset-11 block is clipped to the one hour that is left. */
    const uint8_t h3[10] = { 10, 12, 15, 18, 21, 0, 3, 6, 9, 12 };
    check_int("3h steps: block count", clock_dial_blocks(h3, 10, start, span, src), 5);
    check_int("3h steps: first start = 10 o'clock", start[0], 300);
    check_int("3h steps: first span is the short gap", span[0], 60);
    check_int("3h steps: second start = 12 o'clock", start[1], 0);
    check_int("3h steps: second span", span[1], 90);
    check_int("3h steps: last start = 9 o'clock", start[4], 270);
    check_int("3h steps: last span clipped to the half day", span[4], 30);

    /* OpenWeatherMap's 3-hourly list can START at the slot that CONTAINS the
     * current hour, so slot 1 is level with or behind slot 0. A slot that does
     * not advance is a duplicate to skip, not the end of the list: the whole
     * forecast must still be drawn. Entry 1 (12:00) is the skipped stale slot,
     * so block 1's source is entry 2 (15:00), not entry 1: a caller indexing a
     * parallel temperature array by block number instead of src_idx would read
     * the wrong entry from here on. */
    const uint8_t hb[10] = { 13, 12, 15, 18, 21, 0, 3, 6, 9, 12 };
    check_int("grid behind now: block count", clock_dial_blocks(hb, 10, start, span, src), 5);
    check_int("grid behind now: first start = 1 o'clock", start[0], 30);
    check_int("grid behind now: first span skips the stale slot", span[0], 60);
    check_int("grid behind now: block 1 src skips the stale entry 1", src[1], 2);
    check_int("grid behind now: last start = twelve", start[4], 0);
    check_int("grid behind now: last span clipped", span[4], 30);

    const uint8_t hd[10] = { 13, 13, 15, 18, 21, 0, 3, 6, 9, 12 };
    check_int("grid level with now: block count", clock_dial_blocks(hd, 10, start, span, src), 5);
    check_int("grid level with now: first start", start[0], 30);
    check_int("grid level with now: first span", span[0], 60);
    check_int("grid level with now: block 1 src skips the duplicate entry 1", src[1], 2);

    /* Hours that wrap midnight keep marching forward. */
    const uint8_t hw[10] = { 22, 23, 0, 1, 2, 3, 4, 5, 6, 7 };
    check_int("wrap: block count", clock_dial_blocks(hw, 10, start, span, src), 10);
    check_int("wrap: 22 o'clock sits at ten", start[0], 300);
    check_int("wrap: midnight sits at twelve", start[2], 0);

    /* weather_client.c:236-241 returns true without touching hourly_* when the
     * OpenWeatherMap forecast leg fails, so the array is all zero while
     * wd.valid is set. "0 degrees at midnight" is not a forecast. */
    const uint8_t hz[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    check_int("all zero: nothing drawn", clock_dial_blocks(hz, 10, start, span, src), 0);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
