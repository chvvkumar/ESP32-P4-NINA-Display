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
    SemaphoreHandle_t mutex;
} goes_data_t;

void      goes_data_init(goes_data_t *data);
bool      goes_data_lock(goes_data_t *data, int timeout_ms);
void      goes_data_unlock(goes_data_t *data);
esp_err_t goes_client_poll(const char *region, goes_data_t *data);
void      goes_client_cleanup(goes_data_t *data);
