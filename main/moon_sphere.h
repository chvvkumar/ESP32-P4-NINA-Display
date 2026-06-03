#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "moon_ephemeris.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Decode/prepare the texture into PSRAM once. Returns false on failure. */
bool moon_sphere_init(void);
/* Render a textured, sub-solar-lit sphere for `st` into a freshly malloc'd
 * RGB565 PSRAM buffer of size w*h*2 (caller frees). Returns NULL on failure. */
uint16_t *moon_sphere_render(int w, int h, const moon_state_t *st,
                             int nb_sectors, int nb_stacks, uint8_t bg_style);
#ifdef __cplusplus
}
#endif
