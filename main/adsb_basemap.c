/**
 * @file adsb_basemap.c
 * @brief State-boundary basemap for the ADS-B Radar Scope. See adsb_basemap.h.
 */

#include "adsb_basemap.h"

#include "goes_client.h"
#include "image_red_remap.h"
#include "radar_wms.h"
#include "screen_geom.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "adsb_basemap";

/* The shared fetch path (goes_client.c, GOES_IMG_MAX_DIM) rejects any image
 * with a side over 1024 px before decoding, so the request can never exceed
 * it. S = 2 * disc_r + 2 stays under it on every panel (790 at 800). */
#define ADSB_BASEMAP_S_MAX      1024
#define ADSB_BASEMAP_BACKOFF_US (60 * 1000 * 1000LL)

static _Atomic bool s_scope_shown = false;
static _Atomic int  s_disc_r      = 0;
static _Atomic uint32_t s_gen     = 0;

static SemaphoreHandle_t s_mux = NULL;

/* Held source frame (poll task writes, UI task reads under s_mux). */
static image_frame_t s_frame;      /* .buf NULL when nothing is held */
static int           s_frame_s;    /* requested square side used for this frame */
static long          s_key_lat, s_key_lon, s_key_range, s_key_r;
static bool          s_have_key;

static int64_t s_backoff_until_us; /* esp_timer_get_time(); 0 = no backoff armed */

/* UI-task-only display buffer; never touched by the poll task. */
static uint16_t *s_disp = NULL;

/* Created once by adsb_basemap_init() from adsb_client_init(), which runs on
 * one task before the poller is spawned, so no lazy creation and no critical
 * section around a FreeRTOS allocation. Until then every entry point below
 * returns empty-handed. */
void adsb_basemap_init(void)
{
    if (s_mux == NULL) s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) ESP_LOGE(TAG, "mutex create failed; basemap disabled");
}

void adsb_basemap_set_scope(bool scope_shown, int disc_r)
{
    atomic_store(&s_scope_shown, scope_shown);
    atomic_store(&s_disc_r, disc_r);
}

uint32_t adsb_basemap_generation(void)
{
    return atomic_load(&s_gen);
}

/* Builds the state-boundary GetMap URL: same layer/style as Weather Radar
 * map style 1 (nws:state_boundary, boundary_gray), a black-background square
 * of side S centred on the receiver in EPSG:3857. Split out as its own
 * function, taking plain scalars, so it is easy to host-test later. */
static bool basemap_url(char *out, size_t sz, float lat, float lon, float range_nm,
                        int disc_r, int S)
{
    if (out == NULL || sz == 0) return false;
    out[0] = '\0';
    if (disc_r <= 0 || range_nm <= 0.0f || S <= 0) return false;

    float lat_rad = lat * RADAR_WMS_DEG2RAD;
    float mpp = range_nm * 1852.0f / ((float)disc_r * cosf(lat_rad));
    float cx = radar_wms_merc_x(lon);
    float cy = radar_wms_merc_y(lat);
    float half = ((float)S * 0.5f) * mpp;

    int n = snprintf(out, sz,
                     RADAR_WMS_BASE "ows?service=WMS&version=1.3.0&request=GetMap"
                     "&layers=nws:state_boundary&styles=boundary_gray"
                     "&crs=EPSG:3857&bbox=%ld,%ld,%ld,%ld&width=%d&height=%d"
                     "&format=image/gif&bgcolor=0x000000&transparent=false",
                     (long)lroundf(cx - half), (long)lroundf(cy - half),
                     (long)lroundf(cx + half), (long)lroundf(cy + half),
                     S, S);
    if (n < 0 || (size_t)n >= sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool adsb_basemap_poll(float rx_lat, float rx_lon, float range_nm)
{
    bool shown = atomic_load(&s_scope_shown);
    int disc_r = atomic_load(&s_disc_r);
    if (!shown || disc_r <= 0 || range_nm <= 0.0f) return false;
    if (rx_lat == 0.0f && rx_lon == 0.0f) return false;

    long key_lat = lroundf(rx_lat * 1000.0f);
    long key_lon = lroundf(rx_lon * 1000.0f);
    long key_range = lroundf(range_nm);
    long key_r = disc_r;

    if (s_mux == NULL) return false;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        bool unchanged = s_have_key && s_frame.buf != NULL &&
                          key_lat == s_key_lat && key_lon == s_key_lon &&
                          key_range == s_key_range && key_r == s_key_r;
        xSemaphoreGive(s_mux);
        if (unchanged) return false;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_backoff_until_us != 0 && now_us < s_backoff_until_us) return false;

    /* The render samples only inside the disc (radius disc_r about the source
     * centre), and a square of side 2 * disc_r inscribes that circle at every
     * rotation, so no sqrt(2) margin is needed. */
    int S = 2 * disc_r + 2;
    if (S > ADSB_BASEMAP_S_MAX) S = ADSB_BASEMAP_S_MAX;
    if (S < 1) S = 1;

    char url[320];
    if (!basemap_url(url, sizeof(url), rx_lat, rx_lon, range_nm, disc_r, S)) {
        ESP_LOGW(TAG, "basemap URL build failed");
        s_backoff_until_us = now_us + ADSB_BASEMAP_BACKOFF_US;
        return false;
    }
    ESP_LOGD(TAG, "basemap fetch: %s", url);

    image_frame_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    if (image_fetch_custom(url, "", &fresh, NULL) != ESP_OK || fresh.buf == NULL) {
        ESP_LOGW(TAG, "basemap fetch failed, backing off 60s");
        s_backoff_until_us = now_us + ADSB_BASEMAP_BACKOFF_US;
        return false;
    }

    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        if (s_frame.buf != NULL) heap_caps_free(s_frame.buf);
        s_frame = fresh;
        s_frame_s = S;
        s_key_lat = key_lat;
        s_key_lon = key_lon;
        s_key_range = key_range;
        s_key_r = key_r;
        s_have_key = true;
        xSemaphoreGive(s_mux);
    }
    s_backoff_until_us = 0;
    atomic_fetch_add(&s_gen, 1);
    return true;
}

void adsb_basemap_release(void)
{
    if (s_mux == NULL) return;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        if (s_frame.buf != NULL) {
            heap_caps_free(s_frame.buf);
            memset(&s_frame, 0, sizeof(s_frame));
        }
        s_have_key = false;
        xSemaphoreGive(s_mux);
    }
}

void adsb_basemap_release_display(void)
{
    if (s_disp != NULL) {
        heap_caps_free(s_disp);
        s_disp = NULL;
    }
}

const uint16_t *adsb_basemap_render(float up_deg, int *side_out)
{
    if (s_mux == NULL) return NULL;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return NULL;
    bool have_frame = (s_frame.buf != NULL);
    /* The lock stays held for the whole render below: the source buffer is
     * read in place so the poll task cannot free it out from under us. */
    if (!have_frame) {
        xSemaphoreGive(s_mux);
        return NULL;
    }

    int D = screen_size();
    if (s_disp == NULL) {
        s_disp = (uint16_t *)heap_caps_aligned_alloc(128, (size_t)D * D * 2, MALLOC_CAP_SPIRAM);
        if (s_disp == NULL) {
            xSemaphoreGive(s_mux);
            return NULL;
        }
    }
    memset(s_disp, 0, (size_t)D * D * 2);

    int disc_r = atomic_load(&s_disc_r);
    int c = D / 2;
    if (disc_r > 0) {
        int frame_w = s_frame.w;
        int frame_h = s_frame.h;
        float k = (s_frame_s > 0) ? ((float)frame_w / (float)s_frame_s) : 1.0f;
        float src_cx = (float)frame_w * 0.5f;
        float src_cy = (float)frame_h * 0.5f;
        float u = up_deg * RADAR_WMS_DEG2RAD;
        float cs = cosf(u);
        float sn = sinf(u);
        float step_sx = cs * k;
        float step_sy = sn * k;
        float r2 = (float)disc_r * (float)disc_r;

        int y0 = c - disc_r;
        int y1 = c + disc_r;
        if (y0 < 0) y0 = 0;
        if (y1 > D - 1) y1 = D - 1;

        const uint16_t *src = (const uint16_t *)s_frame.buf;

        for (int y = y0; y <= y1; y++) {
            float dy = (float)(y - c);
            float chord2 = r2 - dy * dy;
            if (chord2 < 0.0f) continue;
            int half_chord = (int)lroundf(sqrtf(chord2));
            int x0 = c - half_chord;
            int x1 = c + half_chord;
            if (x0 < 0) x0 = 0;
            if (x1 > D - 1) x1 = D - 1;
            if (x0 > x1) continue;

            float dx0 = (float)(x0 - c);
            float sx = src_cx + (dx0 * cs - dy * sn) * k;
            float sy = src_cy + (dx0 * sn + dy * cs) * k;

            uint16_t *row = &s_disp[y * D];
            for (int x = x0; x <= x1; x++) {
                /* Truncation after +0.5 rounds every non-negative sample;
                 * negatives are out of the source anyway. lroundf() here
                 * would be two libm calls per pixel over half a million
                 * pixels. */
                if (sx >= 0.0f && sy >= 0.0f) {
                    int ix = (int)(sx + 0.5f);
                    int iy = (int)(sy + 0.5f);
                    if (ix < frame_w && iy < frame_h) row[x] = src[iy * frame_w + ix];
                }
                sx += step_sx;
                sy += step_sy;
            }
        }
    }
    xSemaphoreGive(s_mux);

    image_red_remap_rgb565(s_disp, (size_t)D * D);
    if (side_out) *side_out = D;
    return s_disp;
}
