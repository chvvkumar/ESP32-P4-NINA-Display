/**
 * @file board_profile.h
 * @brief The one data row that describes the panel this device is driving.
 *
 * Resolution order at boot, and it matters:
 *   1. The compiled shape family plus the NVS board/panel key pick the row.
 *      That fixes the width BEFORE anything sizes a buffer from it, which is
 *      what screenshot_encoder_init() needs twenty lines before display start.
 *   2. The RDDID probe then VALIDATES the controller against the family. The
 *      probe never chooses the width.
 *
 * A validation failure is not a fallback: the display stays off, the device
 * boots headless with the web server and the console, and if the running slot
 * is still pending verification it is rolled back immediately.
 */
#ifndef BOARD_PROFILE_H
#define BOARD_PROFILE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;           /* "4b", "3.4c", "4c" */
    const char *ha_model;     /* MQTT Home Assistant device model string */
    int         panel_type;   /* BSP_PANEL_* row id */
    int         width;        /* panel width in pixels, height is the same */
    bool        is_round;
    int         safe_inset;   /* pixels from each edge to the safe square */
    int         safe_radius;  /* pixels from centre to the safe circle */
} board_profile_t;

/** @brief Resolve the panel row, tell the BSP, probe and validate. Call once. */
void board_profile_init(void);

/** @brief The resolved row. Never NULL; falls back to the family default. */
const board_profile_t *board_profile(void);

/** @brief False when the panel could not be identified or is the other family. */
bool board_display_present(void);

/** @brief Profile id string, or "unknown" when validation failed. */
const char *board_profile_id(void);

/** @brief "square" or "round", from the compiled family. */
const char *board_profile_shape(void);

/** @brief NVS board/panel value: 1 = 3.4in 800x800, 2 = 4.0in 720x720. */
int board_panel_nvs_get(void);

/** @brief Write the NVS board/panel value. Accepts 1 or 2 only. */
esp_err_t board_panel_nvs_set(int value);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_PROFILE_H */
