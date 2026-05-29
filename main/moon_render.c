#include "moon_render.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include "stb_image.h"

static const char *TAG = "moon_render";

/* Embedded PNG (see CMakeLists EMBED_FILES). */
extern const uint8_t moon_png_start[] asm("_binary_moon_texture_png_start");
extern const uint8_t moon_png_end[]   asm("_binary_moon_texture_png_end");

static uint8_t *s_tex = NULL;   /* RGBA8888, TEX*TEX */
static int      s_tex_w = 0, s_tex_h = 0;
/* Guards s_tex against cross-core free (deinit on Core 1) while moon_render
 * samples it on Core 0. Held for the whole render and the whole deinit free. */
static SemaphoreHandle_t s_tex_mtx = NULL;

bool moon_render_init(void)
{
    if (!s_tex_mtx) s_tex_mtx = xSemaphoreCreateMutex();
    if (s_tex) return true;
    int w, h, ch;
    size_t len = (size_t)(moon_png_end - moon_png_start);
    /* Force 4 channels (RGBA) so alpha is available as the disc mask. */
    unsigned char *img = stbi_load_from_memory(moon_png_start, (int)len, &w, &h, &ch, 4);
    if (!img) { ESP_LOGE(TAG, "moon texture decode failed"); return false; }
    size_t bytes = (size_t)w * h * 4;
    s_tex = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_tex) { stbi_image_free(img); ESP_LOGE(TAG, "PSRAM tex alloc failed"); return false; }
    memcpy(s_tex, img, bytes);
    stbi_image_free(img);
    s_tex_w = w; s_tex_h = h;
    ESP_LOGI(TAG, "moon texture %dx%d cached", w, h);
    return true;
}

void moon_render_deinit(void)
{
    if (s_tex_mtx) xSemaphoreTake(s_tex_mtx, portMAX_DELAY);
    if (s_tex) { heap_caps_free(s_tex); s_tex = NULL; s_tex_w = s_tex_h = 0; }
    if (s_tex_mtx) xSemaphoreGive(s_tex_mtx);
}

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Sample texture luminance (0..255) and alpha (0..255) at normalized disc coords
 * nx,ny in [-1,1]; returns false if outside the texture's lunar disc (alpha low). */
static bool sample_tex(double nx, double ny, int *lum, int *alpha)
{
    if (!s_tex) { *lum = 200; *alpha = 255; return true; } /* flat fallback */
    int tx = (int)((nx * 0.5 + 0.5) * (s_tex_w - 1) + 0.5);
    int ty = (int)((ny * 0.5 + 0.5) * (s_tex_h - 1) + 0.5);
    if (tx < 0 || tx >= s_tex_w || ty < 0 || ty >= s_tex_h) { *alpha = 0; *lum = 0; return false; }
    const uint8_t *p = s_tex + ((size_t)ty * s_tex_w + tx) * 4;
    *lum = (p[0]*30 + p[1]*59 + p[2]*11) / 100;
    *alpha = p[3];
    return p[3] > 40;
}

uint16_t *moon_render(int w, int h, const moon_state_t *st, uint8_t bg_style)
{
    /* Hold the texture lock for the whole render so moon_render_deinit()
     * (Core 1) cannot free s_tex while sample_tex() reads it here (Core 0). */
    if (s_tex_mtx) xSemaphoreTake(s_tex_mtx, portMAX_DELAY);

    uint16_t *buf = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM render alloc failed");
        if (s_tex_mtx) xSemaphoreGive(s_tex_mtx);
        return NULL;
    }

    const double R  = (w < h ? w : h) / 2.0 * 0.92;
    const double cx = (w - 1) / 2.0;
    const double cy = (h - 1) / 2.0;
    const double AA = 1.5;
    const double f  = st->illum;
    const int    waxing = st->waxing ? 1 : 0;
    const double ca = cos(st->orient_rad), sa = sin(st->orient_rad);
    const double R2 = R * R;
    const double tint_r = 1.02, tint_g = 0.97, tint_b = 0.86;

    /* Background fill. */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            buf[y*w + x] = 0x0000;
    if (bg_style == 1) { /* deterministic starfield */
        uint32_t seed = 1234567u;
        for (int i = 0; i < (w*h)/900; i++) {
            seed = seed*1103515245u + 12345u;
            int sx = (seed >> 8) % w;
            seed = seed*1103515245u + 12345u;
            int sy = (seed >> 8) % h;
            int br = 120 + ((seed >> 4) & 0x7F);
            buf[sy*w + sx] = rgb565(br, br, br);
        }
    }

    for (int py = 0; py < h; py++) {
        double dy = py - cy;
        for (int px = 0; px < w; px++) {
            double dx = px - cx;
            double r2 = dx*dx + dy*dy;
            double dist = sqrt(r2);
            double edge = R - dist;
            if (edge < 0) continue;                 /* outside disc */
            double edgeA = edge > AA ? 1.0 : edge / AA;

            /* Rotate sample/terminator frame by -orient so the lit limb and
             * surface align with the sky. */
            double rx = ( dx*ca + dy*sa);
            double ry = (-dx*sa + dy*ca);

            double semi = (R2 - ry*ry > 0) ? sqrt(R2 - ry*ry) : 0.0;
            double termX = (1.0 - 2.0*f) * semi;
            double sd = waxing ? (rx - termX) : (-rx - termX);
            double l = sd / AA;
            if (l < 0) l = 0;
            if (l > 1) l = 1;

            int lum, alpha;
            sample_tex(rx / R, ry / R, &lum, &alpha);
            double limb = 1.0 - 0.45 * (r2 / R2);
            double litR = lum * tint_r * limb;
            double litG = lum * tint_g * limb;
            double litB = lum * tint_b * limb;
            double dk = 0.07;
            int r = (int)(l*litR + (1-l)*litR*dk);
            int g = (int)(l*litG + (1-l)*litG*dk);
            int b = (int)(l*litB + (1-l)*litB*dk + (1-l)*4);

            if (edgeA < 1.0) { r = (int)(r*edgeA); g = (int)(g*edgeA); b = (int)(b*edgeA); }
            buf[py*w + px] = rgb565(r, g, b);
        }
    }

    if (bg_style == 2) {
        /* Soft warm halo: additive ring just outside the disc. */
        for (int py = 0; py < h; py++) {
            double dy = py - cy;
            for (int px = 0; px < w; px++) {
                double dx = px - cx;
                double dist = sqrt(dx*dx + dy*dy);
                if (dist > R && dist < R*1.35) {
                    double t = 1.0 - (dist - R) / (R*0.35);
                    int add = (int)(40 * t);
                    uint16_t c = buf[py*w+px];
                    int r = ((c>>11)&0x1F)*255/31 + add;
                    int g = ((c>>5)&0x3F)*255/63 + add*9/10;
                    int b = (c&0x1F)*255/31 + add*7/10;
                    buf[py*w+px] = rgb565(r,g,b);
                }
            }
        }
    }

    if (s_tex_mtx) xSemaphoreGive(s_tex_mtx);
    return buf;
}
