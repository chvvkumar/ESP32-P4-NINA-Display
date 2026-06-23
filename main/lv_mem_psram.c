/**
 * @file lv_mem_psram.c
 * @brief LVGL 9.5 custom memory backend — routes all lv_malloc() allocations to PSRAM.
 *
 * Root-cause fix for internal-SRAM exhaustion reboots. The esp-hosted SDIO WiFi
 * RX path allocates a ~4608-byte buffer from internal DMA-capable SRAM
 * (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT). LVGL widget/object/
 * style metadata previously came from that same internal pool (via
 * CONFIG_LV_USE_CLIB_MALLOC=y system malloc, forced internal by
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096). Thousands of tiny LVGL allocations
 * fragmented the internal pool until the largest contiguous free block dropped
 * below 4608 bytes, the RX alloc returned NULL, and an assert rebooted the device.
 *
 * Selecting CONFIG_LV_USE_CUSTOM_MALLOC=y makes LVGL leave the *_core functions
 * undefined (the clib/builtin core files self-exclude on LV_USE_STDLIB_MALLOC),
 * so this translation unit supplies them. Every lv_malloc()/lv_realloc()/lv_free()
 * now lands in PSRAM (MALLOC_CAP_SPIRAM), freeing the internal/DMA pool for the
 * WiFi RX path.
 *
 * This does NOT touch the LCD/DPI framebuffers or the LVGL draw buffers: in
 * avoid-tearing mode those are the LCD driver's DPI framebuffers obtained via
 * esp_lcd_dpi_panel_get_frame_buffer() (esp_lvgl_port), never lv_malloc.
 */

#include "lvgl.h"

#include "esp_heap_caps.h"

/* Only compile the hooks when LVGL is configured for the custom backend.
 * If the malloc choice is ever switched back to CLIB/BUILTIN, these symbols
 * would otherwise multiply-define against LVGL's own core file. */
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

void lv_mem_init(void)
{
    /* PSRAM heap is initialized by ESP-IDF before app_main(); nothing to do. */
}

void lv_mem_deinit(void)
{
    /* Heap teardown is owned by ESP-IDF; nothing to do. */
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    /* Custom backend uses the ESP-IDF PSRAM heap directly; pools unsupported. */
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    /* Returns NULL on failure; LVGL callers handle NULL gracefully. */
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    /* Per-heap accounting comes from ESP-IDF heap_caps APIs, not LVGL. */
    if (mon_p != NULL) {
        lv_memzero(mon_p, sizeof(lv_mem_monitor_t));
    }
}

lv_result_t lv_mem_test_core(void)
{
    /* ESP-IDF owns heap integrity checking; report OK. */
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
