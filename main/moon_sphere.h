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

typedef enum { MOON_LIGHT_TRUE_PHASE = 0, MOON_LIGHT_EXPLORE = 1 } moon_light_mode_t;

/* Render with an additional user rotation (drag-to-rotate) on top of the sky
 * orientation in `st`. yaw_deg rotates the disc about the screen-vertical axis,
 * pitch_deg about the screen-horizontal axis. light_mode selects true sub-solar
 * phase (sun fixed in the sky, features rotate under it) or a fully-lit "explore"
 * view. With yaw=0,pitch=0,light_mode=TRUE_PHASE this is identical to
 * moon_sphere_render(). Returns an RGB565 PSRAM buffer (caller frees) or NULL. */
uint16_t *moon_sphere_render_ex(int w, int h, const moon_state_t *st,
                                int nb_sectors, int nb_stacks, uint8_t bg_style,
                                float yaw_deg, float pitch_deg,
                                moon_light_mode_t light_mode);
#ifdef __cplusplus
}
#endif
