#include "moon_render.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include "stb_image.h"

static const char *TAG = "moon_render";

/* 4x4 Bayer ordered-dither threshold matrix (0..15). */
static const uint8_t s_bayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

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
static bool sample_tex(float nx, float ny, int *lum, int *alpha)
{
    if (!s_tex) { *lum = 200; *alpha = 255; return true; } /* flat fallback */
    int tx = (int)((nx * 0.5f + 0.5f) * (s_tex_w - 1) + 0.5f);
    int ty = (int)((ny * 0.5f + 0.5f) * (s_tex_h - 1) + 0.5f);
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

    /* All per-pixel math is single-precision: the ESP32-P4 FPU is float-only,
     * so doubles would fall back to slow soft-float (sqrt/floatsidf) and a
     * 600x600 render would take seconds, starving the Core 0 idle task and
     * tripping the task watchdog. float keeps it to tens of milliseconds. */
    const float R  = (w < h ? w : h) * 0.5f * 0.92f;
    const float cx = (w - 1) * 0.5f;
    const float cy = (h - 1) * 0.5f;
    const float AA = 1.5f;
    const float f  = st->illum;
    const int   waxing = st->waxing ? 1 : 0;
    const float ca = cosf(st->orient_rad), sa = sinf(st->orient_rad);
    const float R2 = R * R;
    const float tint_r = 1.02f, tint_g = 0.97f, tint_b = 0.86f;
    const float dk = 0.07f;

    /* Background: black fill (single memset beats a per-pixel loop). */
    memset(buf, 0, (size_t)w * h * 2);
    if (bg_style == 1 || bg_style == 3) { /* deterministic starfield */
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
        float dy = (float)py - cy;
        uint16_t *row = buf + (size_t)py * w;
        for (int px = 0; px < w; px++) {
            float dx = (float)px - cx;
            float r2 = dx*dx + dy*dy;
            if (r2 > R2) continue;                  /* outside disc — skip sqrt */
            float dist = sqrtf(r2);
            float edge = R - dist;
            float edgeA = edge > AA ? 1.0f : edge / AA;

            /* Rotate sample/terminator frame by -orient so the lit limb and
             * surface align with the sky. */
            float rx = ( dx*ca + dy*sa);
            float ry = (-dx*sa + dy*ca);

            float semiArg = R2 - ry*ry;
            float semi = semiArg > 0.0f ? sqrtf(semiArg) : 0.0f;
            float termX = (1.0f - 2.0f*f) * semi;
            float sd = waxing ? (rx - termX) : (-rx - termX);
            float l = sd / AA;
            if (l < 0.0f) l = 0.0f;
            if (l > 1.0f) l = 1.0f;

            int lum, alpha;
            sample_tex(rx / R, ry / R, &lum, &alpha);
            float limb = 1.0f - 0.45f * (r2 / R2);
            float litR = lum * tint_r * limb;
            float litG = lum * tint_g * limb;
            float litB = lum * tint_b * limb;
            int r = (int)(l*litR + (1.0f-l)*litR*dk);
            int g = (int)(l*litG + (1.0f-l)*litG*dk);
            int b = (int)(l*litB + (1.0f-l)*litB*dk + (1.0f-l)*4.0f);

            if (edgeA < 1.0f) { r = (int)(r*edgeA); g = (int)(g*edgeA); b = (int)(b*edgeA); }
            row[px] = rgb565(r, g, b);
        }
    }

    if (bg_style == 2 || bg_style == 3) {
        /* Soft warm halo: additive ring just outside the disc. */
        const float Rout2 = (R * 1.35f) * (R * 1.35f);
        for (int py = 0; py < h; py++) {
            float dy = (float)py - cy;
            uint16_t *row = buf + (size_t)py * w;
            for (int px = 0; px < w; px++) {
                float dx = (float)px - cx;
                float r2 = dx*dx + dy*dy;
                if (r2 <= R2 || r2 >= Rout2) continue;
                float dist = sqrtf(r2);
                float t = 1.0f - (dist - R) / (R*0.35f);
                int add = (int)(40.0f * t);
                uint16_t c = row[px];
                int r = ((c>>11)&0x1F)*255/31 + add;
                int g = ((c>>5)&0x3F)*255/63 + add*9/10;
                int b = (c&0x1F)*255/31 + add*7/10;
                /* Ordered dither fills rgb565 truncation slack (kills rings). */
                int dr = s_bayer4[py & 3][px & 3] >> 1;  /* 0..7, red step 8 */
                int dg = s_bayer4[py & 3][px & 3] >> 2;  /* 0..3, green step 4 */
                int db = dr;                             /* 0..7, blue step 8 */
                row[px] = rgb565(r + dr, g + dg, b + db);
            }
        }
    }

    if (s_tex_mtx) xSemaphoreGive(s_tex_mtx);
    return buf;
}
