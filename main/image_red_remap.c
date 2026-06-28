#include "image_red_remap.h"
#include "ui/nina_dashboard.h"
#include "ui/themes.h"

bool image_red_remap_active(void)
{
    const theme_t *t = nina_dashboard_get_theme();
    if (t == NULL) {
        return false;
    }
    return theme_is_red_night(t);
}

void image_red_remap_rgb565(uint16_t *buf, size_t px_count)
{
    if (!image_red_remap_active()) {
        return;
    }
    image_red_remap_rgb565_force(buf, px_count);
}

void image_red_remap_rgb565_force(uint16_t *buf, size_t px_count)
{
    if (buf == NULL || px_count == 0) {
        return;
    }
    for (size_t i = 0; i < px_count; i++) {
        uint16_t px = buf[i];
        uint8_t r5 = (uint8_t)((px >> 11) & 0x1f);
        uint8_t g6 = (uint8_t)((px >> 5) & 0x3f);
        uint8_t b5 = (uint8_t)(px & 0x1f);
        uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
        uint8_t luma8 = (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
        uint8_t r5o = (uint8_t)(luma8 >> 3);
        buf[i] = (uint16_t)(r5o << 11);
    }
}
