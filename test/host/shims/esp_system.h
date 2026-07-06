#pragma once
/* Host shim for esp_system.h — minimal surface for firmware headers that
 * pull it in transitively. Add functions here only as new host tests need
 * them; keep this file small. */

#include "esp_err.h"
#include <stdint.h>

static inline void esp_restart(void)
{
    /* No-op on host: nothing should actually reboot a unit test process. */
}

static inline uint32_t esp_random(void)
{
    return 0;
}
