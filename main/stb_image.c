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

/* Only need JPEG support */
#define STBI_ONLY_JPEG
#define STBI_NO_PNG
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

/* Suppress warnings in third-party code */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma GCC diagnostic pop
