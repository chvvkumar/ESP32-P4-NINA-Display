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

using namespace tgx;

static const char *TAG = "moon_sphere";

/* ----------------------------------------------------------------------------
 * Orientation tunables (compile-time). Defaults are reasoned from the tgx
 * sphere conventions documented in moon_sphere_render() below and yield the
 * STANDARD near-side naked-eye view: north up, selenographic EAST (Mare Crisium,
 * +lon) on the RIGHT, lunar south (Tycho) at the BOTTOM. Flip exactly one of
 * these in one edit if the device shows a residual error:
 *
 *   - If features/terminator are mirrored EAST<->WEST (left-right): set
 *     MOON_TEX_FLIP_U to 1. (Mirrors the texture longitude only.)
 *   - If the disc is upside-down (NORTH<->SOUTH): set MOON_TEX_FLIP_V to 1.
 *   - If the disc rolls the WRONG way (parallactic angle sense reversed): set
 *     MOON_ROLL_SIGN to -1.
 *   - To verify the BASE orientation against a north-up near-side reference
 *     photo, set MOON_DEBUG_GEOCENTRIC to 1: this forces roll=0 AND
 *     libration=0 while keeping lighting correct, so the rendered disc should
 *     be exactly north-up / east-right / south-bottom.
 *
 * MOON_TEX_FLIP_U / _V are applied to the texture buffer at decode time (the
 * tgx drawSphere computes UVs internally, so the only deterministic place to
 * flip u/v is the texture image itself).
 * --------------------------------------------------------------------------*/
#ifndef MOON_TEX_FLIP_U
#define MOON_TEX_FLIP_U     0   /* 1 = mirror texture longitude (east<->west) */
#endif
#ifndef MOON_TEX_FLIP_V
#define MOON_TEX_FLIP_V     0   /* 1 = flip texture latitude (north<->south)  */
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

/* Apply the MOON_TEX_FLIP_U / MOON_TEX_FLIP_V escape-hatch mirrors to the
 * already-filled texture buffer (s_tex_buf, s_tex_w x s_tex_h, RGB565).
 *  - FLIP_U mirrors columns (x -> w-1-x): east<->west longitude swap.
 *  - FLIP_V mirrors rows    (y -> h-1-y): north<->south latitude swap.
 * Both default OFF; the base orientation is already correct without them.
 * Compiled out entirely when the corresponding flag is 0 so there is no
 * per-pixel cost on the default path. */
static void apply_texture_flips(void)
{
#if MOON_TEX_FLIP_U
    for (int y = 0; y < s_tex_h; y++) {
        uint16_t *row = s_tex_buf + (size_t)y * (size_t)s_tex_w;
        for (int x = 0; x < s_tex_w / 2; x++) {
            uint16_t t = row[x];
            row[x] = row[s_tex_w - 1 - x];
            row[s_tex_w - 1 - x] = t;
        }
    }
#endif
#if MOON_TEX_FLIP_V
    for (int y = 0; y < s_tex_h / 2; y++) {
        uint16_t *a = s_tex_buf + (size_t)y * (size_t)s_tex_w;
        uint16_t *b = s_tex_buf + (size_t)(s_tex_h - 1 - y) * (size_t)s_tex_w;
        for (int x = 0; x < s_tex_w; x++) {
            uint16_t t = a[x];
            a[x] = b[x];
            b[x] = t;
        }
    }
#endif
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
    /* Force 1 channel (grayscale). req_comp=1 => stb returns 1 byte per pixel. */
    unsigned char *img = stbi_load_from_memory(moon_equirect_jpg_start,
                                               (int)len, &w, &h, &ch, 1);
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

    /* Convert each grayscale byte to RGB565. Grayscale => R=G=B, so the packed
     * value is identical regardless of channel bit order. Pack manually:
     * R5 = g>>3, G6 = g>>2, B5 = g>>3. */
    const unsigned char *src = img;
    for (int y = 0; y < h; y++) {
        uint16_t *dst = s_tex_buf + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            unsigned int g = *src++;
            dst[x] = (uint16_t)(((g & 0xF8) << 8) | ((g & 0xFC) << 3) | (g >> 3));
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

/* Shaders compiled into the renderer for this evaluation:
 * orthographic projection + z-buffer + flat (Lambert/diffuse) shading +
 * bilinear texture sampling with power-of-two wrapping. Specular is off
 * (no SHADER_GOURAUD needed for a diffuse-only look; FLAT does per-face
 * Lambert which is sufficient and cheaper). */
static const Shader LOADED_SHADERS =
    SHADER_ORTHO | SHADER_ZBUFFER | SHADER_FLAT |
    SHADER_TEXTURE_BILINEAR | SHADER_TEXTURE_WRAP_POW2;

extern "C" uint16_t *moon_sphere_render(int w, int h, const moon_state_t *st,
                                        int nb_sectors, int nb_stacks,
                                        uint8_t bg_style)
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
    renderer.setShaders(SHADER_FLAT | SHADER_TEXTURE_BILINEAR | SHADER_TEXTURE_WRAP_POW2);
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
#else
    const float RAD2DEG = 57.2957795131f;
    float lib_lon_deg = st->lib_lon * RAD2DEG;
    float lib_lat_deg = st->lib_lat * RAD2DEG;
    float roll_deg    = (float)(MOON_ROLL_SIGN) * st->roll * RAD2DEG;
#endif

    fMat4 R;   /* rotation-only (orientation), reused for the light */
    R.setIdentity();
    R.multRotate(lib_lon_deg,  fVec3(0.0f, 1.0f, 0.0f)); /* longitude libration */
    R.multRotate(-lib_lat_deg, fVec3(0.0f, 0.0f, 1.0f)); /* latitude libration  */
    R.multRotate(180.0f,       fVec3(1.0f, 0.0f, 0.0f)); /* base: flip v (north up) */
    R.multRotate(90.0f,        fVec3(0.0f, 1.0f, 0.0f)); /* base: sub-Earth -> +Z   */
    R.multRotate(roll_deg,     fVec3(0.0f, 0.0f, 1.0f)); /* parallactic roll        */

    fMat4 M = R;
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
    fVec4 sun_w4 = R.mult0(sun_body);          /* rotate dir into world space */
    fVec3 sun_w(sun_w4.x, sun_w4.y, sun_w4.z);
    float n = sqrtf(sun_w.x * sun_w.x + sun_w.y * sun_w.y + sun_w.z * sun_w.z);
    if (n > 1e-6f) { sun_w.x /= n; sun_w.y /= n; sun_w.z /= n; }
    renderer.setLightDirection(fVec3(sun_w.x, sun_w.y, sun_w.z));

    /* ----- Draw the textured sphere -------------------------------------- */
    renderer.drawSphere(nb_sectors, nb_stacks, &s_tex);

    /* z-buffer no longer needed; return only the color buffer. */
    heap_caps_free(zbuf);
    return color_buf;
}
