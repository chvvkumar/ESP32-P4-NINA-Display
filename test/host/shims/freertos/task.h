#pragma once
/* Host shim for freertos/task.h — single-threaded stand-ins. */

#include "freertos/FreeRTOS.h"

static inline void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

static inline TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    /* Single "task" (the test process itself); a stable non-NULL identity
     * is enough for code that uses it as a registry/lookup key. */
    static int fake_task_marker;
    return (TaskHandle_t)&fake_task_marker;
}

static inline TickType_t xTaskGetTickCount(void)
{
    return 0;
}
