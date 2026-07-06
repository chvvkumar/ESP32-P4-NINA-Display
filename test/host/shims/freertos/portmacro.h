#pragma once
/* Host shim for freertos/portmacro.h — single-threaded, no real locking. */

#include <stdint.h>

typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE ((BaseType_t)0)
#define pdTRUE  ((BaseType_t)1)
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS 1

typedef struct {
    int dummy;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0 }

#define portENTER_CRITICAL(mux)   do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux)    do { (void)(mux); } while (0)
#define taskENTER_CRITICAL(mux)   do { (void)(mux); } while (0)
#define taskEXIT_CRITICAL(mux)    do { (void)(mux); } while (0)
