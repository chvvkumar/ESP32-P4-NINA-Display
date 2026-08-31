#include "screen_geom.h"

/* Defaults are the square 4B panel, so a caller that somehow runs before
 * screen_geom_set() sees the historical constants rather than zero. */
int g_screen_size        = 720;
int g_screen_safe_inset  = 0;
int g_screen_safe_radius = 0;

void screen_geom_set(int size, int safe_inset, int safe_radius)
{
    if (size < 240 || size > 1024) {
        return;   /* nonsense width: keep the default rather than size buffers from it */
    }
    if (safe_inset < 0 || safe_inset * 2 >= size) {
        safe_inset = 0;
    }
    if (safe_radius < 0 || safe_radius > size / 2) {
        safe_radius = 0;
    }
    g_screen_size        = size;
    g_screen_safe_inset  = safe_inset;
    g_screen_safe_radius = safe_radius;
}
