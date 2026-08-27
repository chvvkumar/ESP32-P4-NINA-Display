/**
 * @file brightness_curve.h
 * @brief The backlight percent-to-duty curve, as pure integer arithmetic.
 *
 * Deliberately free of every ESP-IDF, FreeRTOS and LVGL include so
 * test/host/test_screen_geom.c can include it directly and assert that the
 * square panel's numbers are byte for byte what the 4B shipped before the
 * panel table existed. Change nothing here without changing that test.
 */
#ifndef BSP_BRIGHTNESS_CURVE_H
#define BSP_BRIGHTNESS_CURVE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Map a 0..100 user percentage onto the panel's usable range.
 *
 * Two point curve: user 0 maps to @p floor_pct and user 100 maps to 100. With
 * floor 47 this is exactly `47 + (p * (100 - 47)) / 100`, the expression the
 * square BSP hardcoded; with floor 0 it is the identity.
 */
static inline int bsp_brightness_actual_pct(int floor_pct, int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    } else if (brightness_percent < 0) {
        brightness_percent = 0;
    }
    return floor_pct + (brightness_percent * (100 - floor_pct)) / 100;
}

/** @brief The 10 bit LEDC duty for a 0..100 user percentage. */
static inline int bsp_brightness_duty(int floor_pct, int brightness_percent)
{
    return (1023 * bsp_brightness_actual_pct(floor_pct, brightness_percent)) / 100;
}

#ifdef __cplusplus
}
#endif

#endif /* BSP_BRIGHTNESS_CURVE_H */
