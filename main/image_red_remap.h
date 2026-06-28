#ifndef IMAGE_RED_REMAP_H
#define IMAGE_RED_REMAP_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
// True when the active theme is Red Night (star-party). Reads live theme.
bool image_red_remap_active(void);
// In-place: map each RGB565 pixel to a red shade by luminance (R=luma, G=0, B=0). No-op unless image_red_remap_active().
void image_red_remap_rgb565(uint16_t *buf, size_t px_count);
// Same remap, always applied regardless of active theme (caller already gated).
void image_red_remap_rgb565_force(uint16_t *buf, size_t px_count);
#endif
