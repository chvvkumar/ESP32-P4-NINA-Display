#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool              connected;
    int64_t           last_poll_ms;
    uint8_t          *image_buf;
    uint16_t          image_w;
    uint16_t          image_h;
    bool              vflip;        /* true: buffer is vertically flipped (sw JPEG) */
    char              label[48];    /* human-readable name of the source this buffer holds (region/band); rendered verbatim by the page so it can never desync from image_buf */
    SemaphoreHandle_t mutex;
} goes_data_t;

void      goes_data_init(goes_data_t *data);
bool      goes_data_lock(goes_data_t *data, int timeout_ms);
void      goes_data_unlock(goes_data_t *data);
esp_err_t goes_client_poll(const char *region, goes_data_t *data);
esp_err_t goes_client_poll_url(const char *url, goes_data_t *data, bool vflip, const char *label);
void      goes_client_cleanup(goes_data_t *data);

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
