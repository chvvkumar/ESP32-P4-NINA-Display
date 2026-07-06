#pragma once
/* Host shim for freertos/semphr.h — no real synchronization; tests run
 * single-threaded, so a non-NULL opaque handle plus always-succeeding
 * take/give is sufficient. */

#include "freertos/FreeRTOS.h"
#include <stdlib.h>

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    /* Any non-NULL, distinct-enough pointer works as a handle on host. */
    return malloc(1);
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return malloc(1);
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)sem;
    (void)ticks;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    free(sem);
}
