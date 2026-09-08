#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "http_fetch.h"           /* http_fetch_conn_t (optional keep-alive slot) */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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
/* @p auth_header is an optional raw "Name: value" request header (the Custom
 * URL page's user-supplied header, e.g. "Authorization: Bearer xyz"); NULL or
 * "" sends none, which is the previous behaviour.
 *
 * @p conn is an optional keep-alive slot (see http_fetch.h): NULL is the
 * one-shot behaviour every caller had before. A caller that fetches several
 * images from ONE host in a row (the Cloud Cover loop) passes its own slot so
 * only the first request pays TCP+TLS setup. The slot is owned by exactly one
 * task and MUST be destroyed when that task stops fetching -- a parked slot
 * holds an open socket against the board's ~9 connection ceiling. */
esp_err_t image_fetch_custom(const char *url, const char *auth_header, image_frame_t *out,
                             http_fetch_conn_t *conn);

/* Same fetch+decode as image_fetch_custom(), but the COMPRESSED source bytes
 * survive the call so the caller can re-decode them later without re-fetching.
 *
 * OWNERSHIP: on ESP_OK, *out_src is a PSRAM allocation the caller must
 * heap_caps_free() (independently of out->buf); *out_src_len is its length. On
 * any error return the bytes are freed internally and *out_src is NULL, so the
 * caller frees only what it was handed. Both out params are required.
 *
 * Only the radar ring uses this: it retains ~37 KB per frame so a crop /
 * dark-mode / Red Night change re-derives the ring locally instead of
 * re-downloading it. The other pages call image_fetch_custom() and the
 * compressed buffer is freed at decode time exactly as before. */
esp_err_t image_fetch_custom_retain(const char *url, image_frame_t *out,
                                    uint8_t **out_src, size_t *out_src_len,
                                    http_fetch_conn_t *conn);

/* NESDIS sector code -> human-readable region name. Returns the code itself
 * when no match is found. Single source of truth for region labels. */
const char *goes_region_name(const char *code);

/* Writes the fetch URL for a solar band into out (always NUL-terminated).
 * Bands 0..17 are fixed URLs; the GOES SUVI bands (18..23) are rendered on
 * demand and their URL carries the current UTC time, so it is built per call
 * and needs a live NTP clock. 256 bytes is enough for every band. */
void        solar_band_url(uint8_t idx, char *out, size_t sz);
const char *solar_band_label(uint8_t idx);
bool        solar_band_vflip(uint8_t idx);

/* Per-band center-crop percentage (100 = no crop). */
uint8_t     solar_band_crop_pct(uint8_t idx);

/* Fills an upright-full-image fractional rect [x0,y0,x1,y1] (0..1, origin
 * top-left, y down) for a band whose caption needs masking; returns false
 * when no mask is needed. Any of the out pointers may be NULL. */
bool        solar_band_text_mask(uint8_t idx, float *x0, float *y0, float *x1, float *y1);
