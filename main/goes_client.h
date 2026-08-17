#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* One decoded RGB565 frame. Produced by image_fetch_*() into a caller-supplied,
 * zeroed struct; the caller (an image_page_t, see ui/nina_image_page.h) owns
 * buf and guards the struct with its own mutex. No shared state lives here. */
typedef struct {
    uint8_t  *buf;            /* RGB565 rows top-down, PSRAM, w*h*2 bytes; NULL = no frame */
    uint16_t  w, h;
    char      label[48];      /* caption text for this frame (region / band / "Custom"); "" for Moon */
    char      error_msg[48];  /* failure reason ("" on success) */
    int64_t   stamp_ms;       /* esp_timer ms when buf was produced; 0 = never */
} image_frame_t;

/* Fetch + decode one image (JPEG, PNG or GIF) into *out (out must be zeroed by
 * the caller). On
 * ESP_OK out->buf is a fresh PSRAM allocation the caller owns; on failure
 * out->buf stays NULL and out->error_msg names the reason. */
esp_err_t image_fetch_goes(const char *region, image_frame_t *out);
esp_err_t image_fetch_solar(uint8_t band, image_frame_t *out);
esp_err_t image_fetch_custom(const char *url, image_frame_t *out);

/* NESDIS sector code -> human-readable region name. Returns the code itself
 * when no match is found. Single source of truth for region labels. */
const char *goes_region_name(const char *code);

const char *solar_band_url(uint8_t idx);
const char *solar_band_label(uint8_t idx);
bool        solar_band_vflip(uint8_t idx);

/* Per-band center-crop percentage (100 = no crop). */
uint8_t     solar_band_crop_pct(uint8_t idx);

/* Fills an upright-full-image fractional rect [x0,y0,x1,y1] (0..1, origin
 * top-left, y down) for a band whose caption needs masking; returns false
 * when no mask is needed. Any of the out pointers may be NULL. */
bool        solar_band_text_mask(uint8_t idx, float *x0, float *y0, float *x1, float *y1);
