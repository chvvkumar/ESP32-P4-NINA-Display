/**
 * @file screen_geom.h
 * @brief Panel geometry as runtime values, not compile-time constants.
 *
 * The width is resolved once by board_profile_init(), before any UI or encoder
 * code runs, and never changes afterwards. Everything here is a plain integer
 * read, so a page can call screen_size() in a layout pass without cost.
 *
 * No ESP-IDF, FreeRTOS or LVGL dependency: test/host/test_screen_geom.c
 * compiles screen_geom.c directly.
 */
#ifndef SCREEN_GEOM_H
#define SCREEN_GEOM_H

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Compiled shape family: 1 on the round binary, 0 on the square one. */
#if defined(CONFIG_NINA_FAMILY_ROUND)
#define SCREEN_ROUND 1
#else
#define SCREEN_ROUND 0
#endif

/**
 * Integer stand-in for sqrt(2), in thousandths. A layout pass must not run
 * floating point, and the inscribed-square arithmetic needs the ratio.
 */
#define SCREEN_SQRT2_1000 1414

/* Definitions live in screen_geom.c. Read them through the accessors. */
extern int g_screen_size;
extern int g_screen_safe_inset;
extern int g_screen_safe_radius;

/** @brief Panel width in pixels. The panel is square on every board. */
static inline int screen_size(void) { return g_screen_size; }

/** @brief Centre coordinate on either axis. */
static inline int screen_center(void) { return g_screen_size / 2; }

/**
 * @brief Pixels from each edge to the safe square: 0 on square panels, 118 at
 *        800 and 105 at 720 on round ones. A circular safe area is rotation
 *        invariant, so one inset serves all four rotations.
 */
static inline int screen_safe_inset(void) { return g_screen_safe_inset; }

/** @brief Pixels from the centre to the safe circle. 0 on square panels. */
static inline int screen_safe_radius(void) { return g_screen_safe_radius; }

/** @brief Set the geometry. Called once, from board_profile_init(). */
void screen_geom_set(int size, int safe_inset, int safe_radius);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_GEOM_H */
