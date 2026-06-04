/**
 * @file moon_sphere.cpp
 * @brief Phase 0 evaluation gate: render the Moon page as a textured, sub-solar-lit
 *        3D sphere using the tgx software renderer (https://github.com/vindar/tgx).
 *
 * This translation unit is compiled unconditionally; the tgx sphere renderer is
 * the default (and only) Moon page renderer (see main/CMakeLists.txt, which adds
 * the tgx dependency and embeds the lunar texture).
 *
 * ESP32-P4 notes:
 *  - The P4 FPU is single precision only. tgx defaults TGX_SINGLE_PRECISION_COMPUTATIONS
 *    to 1, and all math in this file uses float / sqrtf. No double in hot paths.
 *  - Big buffers (color + z-buffer) live in PSRAM, 128-byte aligned for PPA.
 */

#include "sdkconfig.h"

/* Force single precision before pulling in tgx (matches tgx default; explicit
 * here in case a global -D ever flips it). */
#ifndef TGX_SINGLE_PRECISION_COMPUTATIONS
#define TGX_SINGLE_PRECISION_COMPUTATIONS 1
#endif

#include "moon_sphere.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

#include <tgx.h>

/* Project's existing stb_image (implementation lives in main/stb_image.c, already
 * compiled as C and linked because moon_render.c uses it). Include the header
 * with C linkage so this C++ TU does not name-mangle stbi_load_from_memory /
 * stbi_image_free. Do NOT define STB_IMAGE_IMPLEMENTATION here. */
extern "C" {
#include "stb_image.h"
}

/* Runtime orientation config (moon_flip_u/v, moon_roll/yaw/pitch_offset). The
 * header self-guards with its own extern "C", so include it at file scope. */
#include "app_config.h"

using namespace tgx;

static const char *TAG = "moon_sphere";

/* ----------------------------------------------------------------------------
 * Orientation tunables (compile-time). Defaults are reasoned from the tgx
 * sphere conventions documented in moon_sphere_render() below and yield the
 * STANDARD near-side naked-eye view: north up, selenographic EAST (Mare Crisium,
 * +lon) on the RIGHT, lunar south (Tycho) at the BOTTOM.
 *
 * The texture U/V flips and the roll/yaw/pitch trims are now RUNTIME config
 * (app_config_t: moon_flip_u, moon_flip_v, moon_roll_offset, moon_yaw_offset,
 * moon_pitch_offset), tunable live from the web UI; all default to 0, so the
 * default behavior matches this base orientation:
 *
 *   - If features/terminator are mirrored EAST<->WEST (left-right): set
 *     moon_flip_u (mirrors the texture longitude only).
 *   - If the disc is upside-down (NORTH<->SOUTH): set moon_flip_v.
 *   - To trim parallactic roll / drag yaw / drag pitch: use moon_roll_offset /
 *     moon_yaw_offset / moon_pitch_offset (degrees).
 *
 * The MOON_TEX_FLIP_U / _V / MOON_ROLL_SIGN #defines below are now unused
 * fallbacks kept for reference; the live path reads config instead. To verify
 * the BASE orientation against a north-up near-side reference photo, set
 * MOON_DEBUG_GEOCENTRIC to 1: this forces roll=0 AND libration=0 (and ignores
 * yaw/pitch/offsets) while keeping lighting correct, so the rendered disc should
 * be exactly north-up / east-right / south-bottom.
 *
 * The runtime flips are applied to the texture buffer at decode time (the tgx
 * drawSphere computes UVs internally, so the only deterministic place to flip
 * u/v is the texture image itself); a flip change re-decodes the texture live.
 * --------------------------------------------------------------------------*/
#ifndef MOON_TEX_FLIP_U
#define MOON_TEX_FLIP_U     0   /* unused fallback: runtime flip now from config */
#endif
#ifndef MOON_TEX_FLIP_V
#define MOON_TEX_FLIP_V     0   /* unused fallback: runtime flip now from config */
#endif
#ifndef MOON_ROLL_SIGN
#define MOON_ROLL_SIGN      (+1) /* flip to -1 if the disc rolls the wrong way */
#endif
#ifndef MOON_DEBUG_GEOCENTRIC
#define MOON_DEBUG_GEOCENTRIC 0 /* 1 = force roll=0 AND libration=0 to show the
                                   north-up base for direct comparison to a
                                   north-up reference photo */
#endif

/* RGB565 packer with clamping, named pack565() so it does not collide with the
 * tgx::RGB565 type. Mirrors moon_render.c's rgb565() bit layout. Used only to
 * draw the starfield + glow background directly into the color buffer. */
static inline uint16_t pack565(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* 4x4 ordered-dither (Bayer) matrix, ported from moon_render.c, used to fill
 * RGB565 truncation slack in the warm glow halo so it does not band. */
static const uint8_t s_bayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

/* Embedded real lunar texture: main/moon_equirect.jpg (1024x512 grayscale JPEG,
 * public domain USGS Clementine 750nm). Embedded via CMakeLists.txt. */
extern const uint8_t moon_equirect_jpg_start[] asm("_binary_moon_equirect_jpg_start");
extern const uint8_t moon_equirect_jpg_end[]   asm("_binary_moon_equirect_jpg_end");

/* ----------------------------------------------------------------------------
 * Lunar texture.
 *
 * Primary path: decode the embedded real Clementine equirectangular map
 * (moon_equirect.jpg, 1024x512 grayscale) into an RGB565 PSRAM buffer.
 * Fallback path: if decode fails, generate the procedural placeholder below so
 * the page never blanks.
 *
 * The placeholder dimensions are power-of-two (512x256) so SHADER_TEXTURE_WRAP_POW2
 * can wrap the longitude seam cleanly. The real Clementine map is also power-of-two
 * (1024x512). The texture buffer is allocated to whatever dimensions are actually
 * used (s_tex_w/s_tex_h), and s_tex wraps it.
 * --------------------------------------------------------------------------*/
static const int   PLACEHOLDER_TEX_W = 512;
static const int   PLACEHOLDER_TEX_H = 256;

static uint16_t        *s_tex_buf = nullptr;   /* RGB565, s_tex_w*s_tex_h, PSRAM */
static int              s_tex_w   = 0;         /* actual texture width           */
static int              s_tex_h   = 0;         /* actual texture height          */
static Image<RGB565>    s_tex;                  /* wraps s_tex_buf               */
static bool             s_inited = false;
static SemaphoreHandle_t s_init_mtx = nullptr;

/* Runtime flip state actually baked into s_tex_buf. Used to detect a config
 * change in render_core and trigger a texture re-decode so flip toggles from
 * the web UI take effect live. -1 = "no texture decoded yet". */
static int              s_applied_flip_u = -1;
static int              s_applied_flip_v = -1;

/* Simple value-noise-ish banding + a few darker "maria" blobs, grayscale.
 * All float math; runs once at init so cost is irrelevant. */
static void generate_placeholder_texture()
{
    /* "Maria" blob centers in texture space (u,v in [0,1]) and radii. */
    struct { float u, v, r, depth; } maria[] = {
        { 0.32f, 0.42f, 0.13f, 0.45f },
        { 0.44f, 0.36f, 0.09f, 0.40f },
        { 0.55f, 0.30f, 0.07f, 0.35f },
        { 0.40f, 0.55f, 0.10f, 0.30f },
        { 0.62f, 0.50f, 0.06f, 0.25f },
        { 0.26f, 0.30f, 0.05f, 0.20f },
    };
    const int n_maria = (int)(sizeof(maria) / sizeof(maria[0]));

    for (int y = 0; y < PLACEHOLDER_TEX_H; y++) {
        float v = (float)y / (float)(PLACEHOLDER_TEX_H - 1);
        for (int x = 0; x < PLACEHOLDER_TEX_W; x++) {
            float u = (float)x / (float)(PLACEHOLDER_TEX_W - 1);

            /* Base highland brightness with subtle latitude/longitude banding. */
            float base = 0.72f
                       + 0.06f * sinf(u * 18.0f)
                       + 0.05f * sinf(v * 22.0f + 1.3f)
                       + 0.03f * sinf((u + v) * 40.0f);

            /* Darken inside maria blobs (wrap u for the longitude seam). */
            float dark = 0.0f;
            for (int m = 0; m < n_maria; m++) {
                float du = u - maria[m].u;
                /* shortest wrapped distance in u */
                if (du >  0.5f) du -= 1.0f;
                if (du < -0.5f) du += 1.0f;
                float dv = v - maria[m].v;
                float d2 = du * du + dv * dv;
                float r2 = maria[m].r * maria[m].r;
                if (d2 < r2) {
                    float t = 1.0f - sqrtf(d2) / maria[m].r;   /* 1 at center */
                    dark += maria[m].depth * t * t;
                }
            }

            float g = base - dark;
            if (g < 0.05f) g = 0.05f;
            if (g > 1.00f) g = 1.00f;

            /* RGB565 from float [0,1] grayscale. */
            s_tex(x, y) = RGB565(g, g, g);
        }
    }
}

/* Allocate the PSRAM RGB565 texture buffer for w*h pixels and wrap it with s_tex.
 * Returns true on success and sets s_tex_w/s_tex_h. */
static bool alloc_texture(int w, int h)
{
    /* Free any previously decoded buffer so re-decode (flip toggle) does not
     * leak PSRAM. On the first call s_tex_buf is NULL and this is a no-op. */
    if (s_tex_buf) {
        heap_caps_free(s_tex_buf);
        s_tex_buf = nullptr;
    }
    size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
    s_tex_buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_tex_buf) {
        ESP_LOGE(TAG, "PSRAM lunar texture alloc failed (%dx%d, %u bytes)",
                 w, h, (unsigned)bytes);
        return false;
    }
    s_tex_w = w;
    s_tex_h = h;
    /* Wrap the PSRAM buffer; stride == width for a tight buffer. */
    s_tex.set(s_tex_buf, w, h, w);
    return true;
}

/* RUNTIME texture mirrors applied in place to the already-filled texture buffer
 * (s_tex_buf, s_tex_w x s_tex_h, RGB565). The flip state comes from the live
 * config (cfg->moon_flip_u / cfg->moon_flip_v), NOT the compile-time #defines:
 *  - flip_u mirrors columns (x -> w-1-x): east<->west longitude swap.
 *  - flip_v mirrors rows    (y -> h-1-y): north<->south latitude swap.
 * Both default OFF (config defaults 0), so the base orientation is unchanged on
 * defaults. apply_texture_flips() records the applied state in s_applied_flip_u/v
 * so render_core can detect a later config change and re-flip the buffer in place
 * (no JPEG re-decode). The mirror helpers are self-inverse, so toggling an axis
 * from the web UI is just one more mirror of that axis. */
/* Mirror the decoded texture columns in place (east<->west, the flip_u axis).
 * Self-inverse: calling it again restores the original. */
static void mirror_texture_u(void)
{
    for (int y = 0; y < s_tex_h; y++) {
        uint16_t *row = s_tex_buf + (size_t)y * (size_t)s_tex_w;
        for (int x = 0; x < s_tex_w / 2; x++) {
            uint16_t t = row[x];
            row[x] = row[s_tex_w - 1 - x];
            row[s_tex_w - 1 - x] = t;
        }
    }
}

/* Mirror the decoded texture rows in place (north<->south, the flip_v axis).
 * Self-inverse. */
static void mirror_texture_v(void)
{
    for (int y = 0; y < s_tex_h / 2; y++) {
        uint16_t *a = s_tex_buf + (size_t)y * (size_t)s_tex_w;
        uint16_t *b = s_tex_buf + (size_t)(s_tex_h - 1 - y) * (size_t)s_tex_w;
        for (int x = 0; x < s_tex_w; x++) {
            uint16_t t = a[x];
            a[x] = b[x];
            b[x] = t;
        }
    }
}

static void apply_texture_flips(void)
{
    const app_config_t *cfg = app_config_get();
    int flip_u = (cfg && cfg->moon_flip_u) ? 1 : 0;
    int flip_v = (cfg && cfg->moon_flip_v) ? 1 : 0;

    if (flip_u) mirror_texture_u();
    if (flip_v) mirror_texture_v();

    /* Remember what is now baked into the buffer for live re-flip detection. */
    s_applied_flip_u = flip_u;
    s_applied_flip_v = flip_v;
}

/* Build the procedural placeholder texture (fallback when the real JPEG fails
 * to decode). Allocates its own buffer at the placeholder dimensions. */
static bool init_placeholder_texture(void)
{
    if (!alloc_texture(PLACEHOLDER_TEX_W, PLACEHOLDER_TEX_H)) return false;
    generate_placeholder_texture();
    apply_texture_flips();
    ESP_LOGW(TAG, "using procedural placeholder lunar texture %dx%d "
                  "(real texture decode unavailable)",
             PLACEHOLDER_TEX_W, PLACEHOLDER_TEX_H);
    return true;
}

/* Decode the embedded real Clementine equirectangular JPEG into the RGB565 PSRAM
 * texture. Returns true on success. On any failure leaves s_tex untouched and
 * s_tex_buf NULL so the caller can fall back to the placeholder.
 *
 * Source convention: equirectangular, north-up, longitude -180..+180 left-to-
 * right with 0 centered. tgx drawSphere samples the texture as equirectangular,
 * top row = north pole. The body-frame mapping in moon_sphere_render() assumes
 * the prime meridian (lon=0) at a specific texture column; if features appear
 * mirrored in longitude or rotated 180 deg, adjust a u-offset/flip here at gate
 * time (e.g. mirror x via (w-1-x), or rotate columns by w/2). Loaded as-is for
 * now: top row = north, columns left-to-right as stored. */
static bool init_real_texture(void)
{
    size_t len = (size_t)(moon_equirect_jpg_end - moon_equirect_jpg_start);
    if (len == 0) {
        ESP_LOGW(TAG, "embedded moon_equirect.jpg is empty");
        return false;
    }

    int w = 0, h = 0, ch = 0;
    /* Force 3 channels (RGB). req_comp=3 => stb returns 3 bytes per pixel (R,G,B). */
    unsigned char *img = stbi_load_from_memory(moon_equirect_jpg_start,
                                               (int)len, &w, &h, &ch, 3);
    if (img == nullptr) {
        ESP_LOGW(TAG, "stbi decode of moon_equirect.jpg failed: %s",
                 stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return false;
    }

    if (w <= 0 || h <= 0) {
        ESP_LOGW(TAG, "moon_equirect.jpg bad dimensions %dx%d", w, h);
        stbi_image_free(img);
        return false;
    }

    if (!alloc_texture(w, h)) {
        stbi_image_free(img);
        return false;
    }

    /* Convert each RGB triplet to RGB565: R5 = r>>3, G6 = g>>2, B5 = b>>3.
     * stb returned 3 bytes/pixel in R,G,B order (req_comp=3). */
    const unsigned char *src = img;
    for (int y = 0; y < h; y++) {
        uint16_t *dst = s_tex_buf + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            unsigned int r = *src++;
            unsigned int g = *src++;
            unsigned int b = *src++;
            dst[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }

    stbi_image_free(img);
    apply_texture_flips();
    ESP_LOGI(TAG, "decoded real lunar texture %dx%d (src channels=%d) from "
                  "moon_equirect.jpg", w, h, ch);
    return true;
}

extern "C" bool moon_sphere_init(void)
{
    if (!s_init_mtx) s_init_mtx = xSemaphoreCreateMutex();
    if (s_init_mtx) xSemaphoreTake(s_init_mtx, portMAX_DELAY);

    if (!s_inited) {
        /* Primary path: real embedded texture. Fallback: procedural placeholder. */
        if (init_real_texture() || init_placeholder_texture()) {
            s_inited = true;
        }
    }

    if (s_init_mtx) xSemaphoreGive(s_init_mtx);
    return s_inited;
}

/* Apply a live flip-config change to the EXISTING decoded texture buffer in
 * place, with no JPEG re-decode. A flip is a row/column mirror, which is its own
 * inverse, so toggling an axis = mirroring that axis once. This avoids the large
 * (~1.5MB) PSRAM decode spike that init_real_texture() incurs; that spike landed
 * mid-render (on top of the live 720x720 color+z scratch) and could fail with
 * outofmem under concurrent load (e.g. an OTA check), falling back to the ugly
 * placeholder. Guarded by the init mutex. No-op when the flip state already
 * matches (the common steady-state path, zero cost). */
static void moon_sphere_reflip_if_changed(void)
{
    const app_config_t *cfg = app_config_get();
    int want_u = (cfg && cfg->moon_flip_u) ? 1 : 0;
    int want_v = (cfg && cfg->moon_flip_v) ? 1 : 0;
    if (want_u == s_applied_flip_u && want_v == s_applied_flip_v) return;

    if (s_init_mtx) xSemaphoreTake(s_init_mtx, portMAX_DELAY);
    /* Re-check under the lock in case another task already re-flipped. */
    if (want_u != s_applied_flip_u || want_v != s_applied_flip_v) {
        if (s_tex_buf == nullptr) {
            /* No texture decoded yet (shouldn't happen post-init): do a full
             * decode, which applies the current flips itself. */
            if (!init_real_texture()) init_placeholder_texture();
        } else {
            /* Apply only the delta in place. Column/row mirrors commute and are
             * self-inverse, so mirroring the changed axis once reaches the target
             * state from any prior state. */
            if (want_u != s_applied_flip_u) {
                mirror_texture_u();
                s_applied_flip_u = want_u;
            }
            if (want_v != s_applied_flip_v) {
                mirror_texture_v();
                s_applied_flip_v = want_v;
            }
        }
    }
    if (s_init_mtx) xSemaphoreGive(s_init_mtx);
}

/* Shaders compiled into the renderer for this evaluation:
 * orthographic projection + z-buffer + Gouraud (per-vertex Lambert/diffuse)
 * shading + bilinear texture sampling with power-of-two wrapping.
 *
 * GOURAUD (not FLAT) is used so the sphere is lit per-vertex with the normals
 * interpolated across each triangle. FLAT shading gives one normal per facet,
 * so every quad of the 96x48 tessellation was uniformly lit and the facet
 * boundaries showed up as a faint lat/long grid across the disc (visible once
 * the resting render went to native 720). drawSphere() supplies the surface
 * vertex POSITIONS as the per-vertex NORMALS (the sphere is unit-radius and
 * centered at the origin, so position == analytic outward normal); see
 * Renderer3D.inl drawTriangle/drawQuad calls that pass &P1,&P3,&P2 as both
 * positions and normals. The rasterizer has an explicit GOURAUD+TEXTURE path
 * (Shaders.h ~L333) that multiplies the interpolated per-vertex shade into the
 * sampled texel, so smooth lighting and the texture coexist. SHADER_GOURAUD is
 * bit 5 (ShaderParams.h L55) and lives in TGX_SHADER_MASK_SHADING with FLAT, so
 * it is a drop-in replacement. It MUST be present in LOADED_SHADERS (the
 * template list) AND selected via setShaders() (Renderer3D.h L84-87: a draw
 * that needs a shader absent from LOADED_SHADERS fails). */
static const Shader LOADED_SHADERS =
    SHADER_ORTHO | SHADER_ZBUFFER | SHADER_GOURAUD |
    SHADER_TEXTURE_BILINEAR | SHADER_TEXTURE_WRAP_POW2;

/* Core sphere renderer. Rasterizes into `color_buf` (RGB565) using `zbuf` as the
 * depth buffer; both must be w*h uint16 and 128-byte aligned (PPA / cache line).
 * The caller owns both buffers and their lifetime — this function neither
 * allocates nor frees them. moon_sphere_render_ex() wraps this with a per-call
 * alloc/free (the resting full-res path), while the drag loop passes persistent
 * scratch buffers so no per-frame heap churn occurs. */
static uint16_t *moon_sphere_render_core(int w, int h, const moon_state_t *st,
                                         int nb_sectors, int nb_stacks,
                                         uint8_t bg_style,
                                         float yaw_deg, float pitch_deg,
                                         moon_light_mode_t light_mode,
                                         uint16_t *color_buf, uint16_t *zbuf)
{
    const size_t npix      = (size_t)w * (size_t)h;
    const size_t color_sz  = npix * sizeof(uint16_t);

    /* Live orientation config, read once. Controls the runtime texture flips and
     * the roll/yaw/pitch offsets applied below. Defaults are all 0 so default
     * behavior matches the pre-config render exactly. */
    const app_config_t *cfg = app_config_get();

    /* If the flip config changed since the texture was last decoded, re-decode
     * now (frees + re-allocs the buffer with the current flips applied) so web-UI
     * flip toggles take effect live. No-op when the flip state is unchanged. */
    moon_sphere_reflip_if_changed();

    /* ----- Background ----------------------------------------------------
     * Drawn DIRECTLY into color_buf BEFORE the sphere is rasterized. The
     * Renderer3D only writes the pixels the disc covers, so the background
     * shows through everywhere outside the lunar disc. Do NOT call
     * im.fillScreen() after this, it would erase the background.
     *   bit 0 (bg_style 1 or 3): deterministic starfield
     *   bit 1 (bg_style 2 or 3): soft warm glow halo just outside the disc
     *   bg_style 0: plain black
     * Ported from moon_render.c's flat-path background. Single precision. */
    Image<RGB565> im(color_buf, w, h, w);

    /* 1. Black base. */
    memset(color_buf, 0, color_sz);   /* RGB565 0x0000 */

    /* 2. Deterministic starfield (fixed LCG seed for frame-stable stars). */
    if (bg_style & 1) {
        uint32_t seed = 1234567u;
        int nstars = (w * h) / 900;
        for (int i = 0; i < nstars; i++) {
            seed = seed * 1103515245u + 12345u;
            int sx = (int)((seed >> 8) % (uint32_t)w);
            seed = seed * 1103515245u + 12345u;
            int sy = (int)((seed >> 8) % (uint32_t)h);
            int br = 120 + (int)((seed >> 4) & 0x7F);
            color_buf[(size_t)sy * w + sx] = pack565(br, br, br);
        }
    }

    /* 3. Soft warm glow halo: additive ring in [R_disc, R_disc*1.35].
     * Disc geometry: the sphere is unit radius and setOrtho uses half-extent
     * ORTHO_R (defined below as 1.08f), so the disc pixel radius is
     * (1.0/ORTHO_R) * 0.5 * min(w,h). */
    if (bg_style & 2) {
        const float ORTHO_R = 1.08f;
        const float R_disc  = (1.0f / ORTHO_R) * 0.5f * (float)((w < h) ? w : h);
        const float cx = (float)(w - 1) * 0.5f;
        const float cy = (float)(h - 1) * 0.5f;
        const float R_disc2 = R_disc * R_disc;
        const float Rout    = R_disc * 1.35f;
        const float Rout2   = Rout * Rout;
        const float inv_band = 1.0f / (R_disc * 0.35f);
        for (int py = 0; py < h; py++) {
            float dy = (float)py - cy;
            uint16_t *row = color_buf + (size_t)py * w;
            for (int px = 0; px < w; px++) {
                float dx = (float)px - cx;
                float r2 = dx * dx + dy * dy;
                if (r2 <= R_disc2 || r2 >= Rout2) continue;
                float dist = sqrtf(r2);
                float t = 1.0f - (dist - R_disc) * inv_band;   /* 1 at disc edge -> 0 */
                int add = (int)(40.0f * t);
                uint16_t c = row[px];
                int r = ((c >> 11) & 0x1F) * 255 / 31 + add;
                int g = ((c >> 5)  & 0x3F) * 255 / 63 + add * 9 / 10;   /* warm */
                int b = ( c        & 0x1F) * 255 / 31 + add * 7 / 10;   /* warm */
                /* Ordered dither fills RGB565 truncation slack (kills rings). */
                int dr = s_bayer4[py & 3][px & 3] >> 1;   /* 0..7 */
                int dg = s_bayer4[py & 3][px & 3] >> 2;   /* 0..3 */
                int db = dr;
                row[px] = pack565(r + dr, g + dg, b + db);
            }
        }
    }

    /* ----- Set up the renderer ------------------------------------------- */
    Renderer3D<RGB565, LOADED_SHADERS, uint16_t> renderer;
    renderer.setViewportSize(w, h);
    renderer.setOffset(0, 0);
    renderer.setImage(&im);
    renderer.setZbuffer(zbuf);
    renderer.clearZbuffer();
    renderer.setCulling(1);

    /* Orthographic box sized to a unit sphere (radius 1) with a little margin
     * so the disc fills the viewport without clipping. The sphere is unit
     * radius and centered at the origin; near/far bracket it on the z axis. */
    const float ORTHO_R = 1.08f;   /* half-extent: 1.0 sphere + ~8% margin */
    renderer.setOrtho(-ORTHO_R, ORTHO_R, -ORTHO_R, ORTHO_R, 0.1f, 10.0f);

    /* Diffuse-only (Lambert) material: ambient low so the unlit limb goes dark
     * (terminator), diffuse high, specular off. Warm tint (matches the old flat
     * renderer's tint_r/g/b ~ 1.0/0.96/0.86) so the lit moon reads warm-gray
     * rather than neutral; the texture grayscale still shows through. */
    renderer.setShaders(SHADER_GOURAUD | SHADER_TEXTURE_BILINEAR | SHADER_TEXTURE_WRAP_POW2);
    renderer.setMaterialColor(RGBf(1.0f, 0.96f, 0.86f));
    renderer.setMaterialAmbiantStrength(0.06f);
    renderer.setMaterialDiffuseStrength(1.0f);
    renderer.setMaterialSpecularStrength(0.0f);

    renderer.setLightAmbiant(RGBf(1.0f, 1.0f, 1.0f));
    renderer.setLightDiffuse(RGBf(1.0f, 1.0f, 1.0f));
    renderer.setLightSpecular(RGBf(0.0f, 0.0f, 0.0f));

    /* ----- Model matrix: orient the disc -------------------------------
     * tgx sphere + camera conventions (verified against components/tgx/src):
     *
     *  drawSphere() (Renderer3D.inl ~L4115-4251): a surface vertex at stack
     *  angle phi (measured from the +Y pole) and sector angle theta is placed at
     *      P = ( sinPhi*cosTheta,  cosPhi,  sinPhi*sinTheta )
     *  so the POLE axis is +Y (top vertex {0,1,0}, bottom {0,-1,0}) and longitude
     *  theta winds +X -> +Z -> -X -> -Z. Texture coords are u = theta/2pi and
     *      v = 0.5*cosPhi + 0.5   (Renderer3D.inl L4132/4178)
     *  The rasterizer (Shaders.h L322/330) indexes texel row = v*height directly,
     *  so v=0 is texture ROW 0 (top) and v=1 is the bottom row. Since our
     *  equirectangular map is north-up (top row = lunar NORTH), tgx maps the
     *  texture's NORTH row to v=0 = geometry phi=pi = the -Y (south) pole. i.e.
     *  the texture is mapped UPSIDE-DOWN in v: geometry +Y is selenographic SOUTH.
     *
     *  Working the texture columns (u=0 -> theta=0; lon 0 is texture-centre
     *  u=0.5 -> theta=pi) gives the selenographic->geometry axis map at the
     *  sub-Earth point:
     *      sub-Earth (lon 0, lat 0)        -> geometry -X
     *      selenographic NORTH (+lat)      -> geometry -Y
     *      selenographic EAST  (+lon)      -> geometry -Z
     *  In tgx ortho the camera sits at the origin looking down -Z with +Y up and
     *  +X to the right (Renderer3D.h L290-291; setOrtho is a proper, NON-mirrored
     *  RH projection, Mat4.h L191). Model rotations (Mat4 setRotate, L261) are
     *  standard glRotate; multRotate PRE-multiplies (Mat4.h L319-323), so the
     *  FIRST multRotate call is applied to the vertex first (innermost).
     *
     *  ROOT CAUSE OF THE LEFT-RIGHT MIRROR: the previous matrix used only
     *  Ry(90 - lib_lon). At lib=0 that is Ry(90), which sends selenographic
     *  EAST (-Z) to screen -X (LEFT) and NORTH (-Y) to screen -Y (DOWN): the
     *  near side came out mirrored east<->west (and upside-down). The deterministic
     *  cure is to compose the base orientation as Ry(90)*Rx(180) (a proper det=+1
     *  rotation, so culling/winding is unchanged and the disc is NOT inside-out),
     *  which maps EAST->+X (right), NORTH->+Y (up), SOUTH->-Y (bottom), and the
     *  sub-Earth point ->+Z (toward the eye). No texture u-flip and no culling
     *  flip are required.
     *
     *  Libration shifts the sub-Earth point to selenographic (lib_lon, lib_lat).
     *  We pre-rotate the body so that point lands back at -X before the base
     *  rotation: Ry(lib_lon) swings the lib_lon meridian onto the central -X
     *  meridian (Ry(a) maps theta -> theta-a), then Rz(-lib_lat) tilts the
     *  lib_lat parallel onto the equator. Finally Rz(roll) applies the parallactic
     *  roll about the view (+Z) axis.
     *
     *  Net vertex transform (innermost -> outermost):
     *      Ry(lib_lon) -> Rz(-lib_lat) -> Rx(180) -> Ry(90) -> Rz(roll)
     *  Lighting is computed in this same body frame and rotated by R, and the
     *  lit fraction is invariant under R (rotation preserves N.L), so this purely
     *  re-orients the already-correct lit disc on screen without touching the
     *  phase. MOON_DEBUG_GEOCENTRIC zeroes libration + roll to expose the base.
     */
#if MOON_DEBUG_GEOCENTRIC
    float lib_lon_deg = 0.0f;
    float lib_lat_deg = 0.0f;
    float roll_deg    = 0.0f;
    /* Geocentric debug exposes the clean base view: also ignore the user
     * drag-to-rotate so yaw/pitch cannot perturb the reference orientation. */
    yaw_deg   = 0.0f;
    pitch_deg = 0.0f;
#else
    const float RAD2DEG = 57.2957795131f;
    float lib_lon_deg = st->lib_lon * RAD2DEG;
    float lib_lat_deg = st->lib_lat * RAD2DEG;
    /* moon_north_up: when set, ignore the parallactic/topocentric tilt (st->roll)
     * and keep the disc upright/north-up; libration, phase, and the live roll trim
     * offset still apply. When clear, use the true sky tilt. */
    float roll_deg    = ((cfg && cfg->moon_north_up) ? 0.0f
                         : (float)(MOON_ROLL_SIGN) * st->roll * RAD2DEG)
                        + (cfg ? cfg->moon_roll_offset : 0.0f); /* north-up forces 0 tilt; offset still applies */
    /* Live yaw/pitch offsets from config, added to the drag-supplied yaw/pitch.
     * Inside the #else (normal) branch so the MOON_DEBUG_GEOCENTRIC override
     * (which zeroes yaw/pitch) still wins when debug is enabled. */
    if (cfg) {
        yaw_deg   += cfg->moon_yaw_offset;
        pitch_deg += cfg->moon_pitch_offset;
    }
#endif

    /* R_sky: the SKY orientation rotation (libration + base + parallactic roll).
     * This is rotation-only and is used UNCHANGED to place the sub-solar light
     * into world space, so the sun stays fixed in the sky regardless of the
     * user's drag-to-rotate yaw/pitch. */
    fMat4 R_sky;   /* rotation-only (sky orientation), reused for the light */
    R_sky.setIdentity();
    R_sky.multRotate(lib_lon_deg,  fVec3(0.0f, 1.0f, 0.0f)); /* longitude libration */
    R_sky.multRotate(-lib_lat_deg, fVec3(0.0f, 0.0f, 1.0f)); /* latitude libration  */
    R_sky.multRotate(180.0f,       fVec3(1.0f, 0.0f, 0.0f)); /* base: flip v (north up) */
    R_sky.multRotate(90.0f,        fVec3(0.0f, 1.0f, 0.0f)); /* base: sub-Earth -> +Z   */
    R_sky.multRotate(roll_deg,     fVec3(0.0f, 0.0f, 1.0f)); /* parallactic roll        */

    /* M_rot: the MODEL rotation = R_sky with the user drag-to-rotate applied
     * OUTERMOST. After the full sky orientation the world axes coincide with the
     * screen axes (camera looks -Z, +Y up, +X right). multRotate PRE-multiplies
     * (the first call is applied to the vertex first / innermost), so appending
     * pitch then yaw AFTER the sky-orientation calls makes them the LAST
     * (outermost) transforms on the vertex: pitch about screen-horizontal (+X),
     * yaw about screen-vertical (+Y). The light is built from R_sky only, so the
     * features rotate under a fixed sub-solar point in TRUE_PHASE. */
    fMat4 M_rot = R_sky;
    M_rot.multRotate(pitch_deg, fVec3(1.0f, 0.0f, 0.0f)); /* drag: pitch (screen-horiz) */
    M_rot.multRotate(yaw_deg,   fVec3(0.0f, 1.0f, 0.0f)); /* drag: yaw   (screen-vert)  */

    fMat4 M = M_rot;
    /* Push the unit sphere down -Z so it lands between zNear (0.1) and zFar (10),
     * centered at z = -2 (camera at origin looking toward -Z). */
    M.multTranslate(fVec3(0.0f, 0.0f, -2.0f));
    renderer.setModelMatrix(M);

    /* ----- Lighting: directional light from the sub-solar point ----------
     * st->sun_lon / st->sun_lat are the sub-solar selenographic coordinates.
     * Build the unit vector to the sun-lit surface point in the body frame, then
     * rotate it by R into world space. tgx lights world-space normals (the light
     * is transformed by the view matrix only, not the model matrix, so it must
     * be supplied in world space consistent with the rotated sphere).
     *
     * IMPORTANT: this vector and its sign are the lighting that was just
     * corrected and verified (right phase, no dark-moon inversion); the lit
     * fraction is invariant under the model rotation R (R preserves N.L), so the
     * orientation fix above does NOT change the phase. Leave this math unchanged.
     * setLightDirection() expects the direction pointing TOWARD the light source
     * (diffuse = max(0, dot(N, L))); sun_w already points toward the Sun, so pass
     * it directly, no negation. */
    float clat = cosf(st->sun_lat);
    fVec3 sun_body(clat * cosf(st->sun_lon),
                   sinf(st->sun_lat),
                   clat * sinf(st->sun_lon));
    /* Light depends ONLY on the SKY orientation R_sky (NOT the user yaw/pitch),
     * so in TRUE_PHASE the sun stays fixed in the sky while the features spin
     * under it as the user drags. */
    fVec4 sun_w4 = R_sky.mult0(sun_body);      /* rotate dir into world space */
    fVec3 sun_w(sun_w4.x, sun_w4.y, sun_w4.z);
    float n = sqrtf(sun_w.x * sun_w.x + sun_w.y * sun_w.y + sun_w.z * sun_w.z);
    if (n > 1e-6f) { sun_w.x /= n; sun_w.y /= n; sun_w.z /= n; }

    if (light_mode == MOON_LIGHT_EXPLORE) {
        /* Explore view: light the whole disc so the user can inspect the far
         * side while spinning. Raise ambient near full and drop diffuse to a
         * gentle view-aligned headlight (direction along the view axis, -Z) for
         * mild shading with no dark/night side. The sky light vector computed
         * above is intentionally NOT used here; only the material/light params
         * are overridden. */
        renderer.setMaterialAmbiantStrength(0.95f);
        renderer.setMaterialDiffuseStrength(0.15f);
        renderer.setLightDirection(fVec3(0.0f, 0.0f, -1.0f));
    } else {
        /* True sub-solar phase: keep the directional sub-solar light so the real
         * phase terminator shows (material/light defaults set above). */
        renderer.setLightDirection(fVec3(sun_w.x, sun_w.y, sun_w.z));
    }

    /* ----- Draw the textured sphere -------------------------------------- */
    renderer.drawSphere(nb_sectors, nb_stacks, &s_tex);

    /* Both buffers are caller-owned; do NOT free here. Return the color buffer
     * so callers can treat the result like the previous alloc-and-return API. */
    return color_buf;
}

/* Render into CALLER-PROVIDED color + z buffers (no per-frame alloc/free). Both
 * must be w*h uint16 and 128-byte aligned (PPA / cache line). Returns color_buf
 * on success, nullptr on bad args. Used by the drag loop with persistent scratch
 * buffers so realtime frames cause zero heap churn. */
extern "C" uint16_t *moon_sphere_render_into(int w, int h, const moon_state_t *st,
                                             int nb_sectors, int nb_stacks,
                                             uint8_t bg_style,
                                             float yaw_deg, float pitch_deg,
                                             moon_light_mode_t light_mode,
                                             uint16_t *color_buf, uint16_t *zbuf)
{
    if (w <= 0 || h <= 0 || st == nullptr || color_buf == nullptr || zbuf == nullptr)
        return nullptr;
    if (!moon_sphere_init()) return nullptr;
    return moon_sphere_render_core(w, h, st, nb_sectors, nb_stacks, bg_style,
                                   yaw_deg, pitch_deg, light_mode, color_buf, zbuf);
}

extern "C" uint16_t *moon_sphere_render_ex(int w, int h, const moon_state_t *st,
                                           int nb_sectors, int nb_stacks,
                                           uint8_t bg_style,
                                           float yaw_deg, float pitch_deg,
                                           moon_light_mode_t light_mode)
{
    if (w <= 0 || h <= 0 || st == nullptr) return nullptr;

    /* Lazy init of the texture, mirroring how moon_render_init() is invoked
     * inline before first use in the flat path. */
    if (!moon_sphere_init()) return nullptr;

    const size_t npix      = (size_t)w * (size_t)h;
    const size_t color_sz  = npix * sizeof(uint16_t);
    const size_t z_sz      = npix * sizeof(uint16_t);

    /* Color + z-buffer in PSRAM, 128-byte aligned (PPA / cache line). */
    uint16_t *color_buf =
        (uint16_t *)heap_caps_aligned_alloc(128, color_sz, MALLOC_CAP_SPIRAM);
    uint16_t *zbuf =
        (uint16_t *)heap_caps_aligned_alloc(128, z_sz, MALLOC_CAP_SPIRAM);

    if (!color_buf || !zbuf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (color=%p z=%p, %dx%d)",
                 (void *)color_buf, (void *)zbuf, w, h);
        if (color_buf) heap_caps_free(color_buf);
        if (zbuf)      heap_caps_free(zbuf);
        return nullptr;   /* caller keeps previous frame */
    }

    moon_sphere_render_core(w, h, st, nb_sectors, nb_stacks,
                            bg_style, yaw_deg, pitch_deg,
                            light_mode, color_buf, zbuf);

    /* z-buffer no longer needed; return only the color buffer (caller frees).
     * render_core always succeeds once color_buf/zbuf are valid (no error path). */
    heap_caps_free(zbuf);
    return color_buf;
}

/* Thin wrapper: the original sub-solar-lit, no-user-rotation render. Identical
 * output to the pre-refactor moon_sphere_render() (yaw=pitch=0, TRUE_PHASE). */
extern "C" uint16_t *moon_sphere_render(int w, int h, const moon_state_t *st,
                                        int nb_sectors, int nb_stacks,
                                        uint8_t bg_style)
{
    return moon_sphere_render_ex(w, h, st, nb_sectors, nb_stacks, bg_style,
                                 0.0f, 0.0f, MOON_LIGHT_TRUE_PHASE);
}
