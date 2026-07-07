#pragma once
/* Host shim for freertos/FreeRTOS.h — pulls in the portmacro shim and
 * defines the handful of macros firmware headers reference directly. */

#include "freertos/portmacro.h"

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *TimerHandle_t;
