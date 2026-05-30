#pragma once
#include "moon_ephemeris.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode the embedded moon texture once into a cached RGBA surface (PSRAM).
 * Safe to call repeatedly; subsequent calls are no-ops. Returns false on failure. */
bool moon_render_init(void);

/* Free the cached texture. */
void moon_render_deinit(void);

/* Render a moon disc into a freshly-allocated PSRAM RGB565 buffer of size w*h.
 * Returns the buffer (caller frees with heap_caps_free) or NULL on failure.
 * bg_style: 0=black, 1=starfield, 2=soft glow. */
uint16_t *moon_render(int w, int h, const moon_state_t *st, uint8_t bg_style);

#ifdef __cplusplus
}
#endif
