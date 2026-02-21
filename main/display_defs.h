#pragma once

/**
 * @file display_defs.h
 * @brief Display-related constants shared across modules.
 *
 * Provides constants that are about the display hardware or LVGL usage,
 * not about application configuration.
 */

/** Standardized LVGL display lock timeout (ms).
 *  All callers should use this instead of 0 (infinite) to prevent deadlocks. */
#define LVGL_LOCK_TIMEOUT_MS 1000

/** Display resolution (720x720 square panel) */
#define SCREEN_SIZE 720
