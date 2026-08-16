/**
 * @file stb_image.c
 * @brief Compile unit for stb_image — software JPEG decoder fallback.
 *
 * The ESP32-P4 hardware JPEG decoder rejects images with >= 4 color
 * components (CMYK/YCCK).  stb_image handles these gracefully by
 * converting CMYK to RGB during decode.
 *
 * Memory is allocated from PSRAM via heap_caps_malloc so large album
 * art images don't exhaust internal RAM.
 */

#include "esp_heap_caps.h"
#include <stdlib.h>

/* Route stb_image allocations to PSRAM */
#define STBI_MALLOC(sz)        heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, newsz) heap_caps_realloc(p, newsz, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p)           free(p)

/* Need JPEG (album/satellite art) + PNG (moon texture); disable the rest */
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM

/* No stdio on ESP32 */
#define STBI_NO_STDIO

/* No SIMD on Xtensa/RISC-V */
#define STBI_NO_SIMD

/* CRITICAL: stb's optional thread-local state must stay disabled on this
 * platform. With thread-locals enabled, stb keeps its flip-vertically-on-load
 * flag in _Thread_local variables; on this firmware the TLS blocks of the
 * PSRAM-stack poller tasks (octoprint, goes, ...) are NOT zero-initialized, so
 * those flags read heap garbage and stb randomly vertically mirrors decoded
 * images per task, per build. Proven on-device 2026-08-14: the octoprint and
 * goes tasks both logged set=-16775497 local=-1888151811 for variables that
 * must be 0. This was the root cause of months of "recurring vertical mirror"
 * sightings (GOES/Solar/Custom/OctoPrint) that no flip toggle could stably
 * fix, and of orientation differing between build environments (TLS layout
 * shifts moved the garbage). With this define the flag collapses to one
 * zero-initialized .bss global and decode is always top-down. Do not remove. */
#define STBI_NO_THREAD_LOCALS

/* Suppress warnings in third-party code */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma GCC diagnostic pop
