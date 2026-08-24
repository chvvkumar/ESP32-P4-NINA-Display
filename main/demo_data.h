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

/* Live demo control. demo_data_task stays the task entry (spawned once,
 * parked between runs — psram_task_spawn stacks are never freed, tasks.h). */

/** Start (or restart) the demo generator. First call spawns the persistent
 *  task; later calls just raise the run gate and wake it. @p allsky may be
 *  NULL when a real AllSky poller owns that struct. */
void demo_data_start(nina_client_t *instances, allsky_data_t *allsky, int instance_count);

/** Async stop: lowers the run gate and wakes the task; stop cleanup runs on
 *  the demo task itself. Returns immediately. */
void demo_data_stop(void);

/** True from demo_data_start() until the stop cleanup has completed. */
bool demo_data_is_running(void);
