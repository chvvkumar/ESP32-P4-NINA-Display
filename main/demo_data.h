#pragma once

#include "nina_client.h"
#include "allsky_client.h"

/**
 * @brief FreeRTOS task that generates realistic demo data for all NINA instances
 *        and AllSky, writing directly into the shared data structures.
 *
 * @param param Pointer to demo_task_params_t (cast from void*)
 */
void demo_data_task(void *param);

/** Parameters passed to demo_data_task */
typedef struct {
    nina_client_t *instances;       /**< Array of MAX_NINA_INSTANCES client structs */
    allsky_data_t *allsky;          /**< AllSky data struct */
    int            instance_count;  /**< Number of enabled instances */
} demo_task_params_t;
