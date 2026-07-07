#pragma once
/* Host shim for esp_timer.h.
 *
 * esp_timer_get_time() returns microseconds since boot on real hardware.
 * On host, it defaults to a monotonic clock, but tests can override it with
 * shim_set_time_us() for deterministic timing-dependent test cases. Once
 * overridden, the fake value sticks until shim_reset_time() is called.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

/* Test-only controls (declared here, implemented in shims.c). */
void shim_set_time_us(int64_t us);
void shim_reset_time(void); /* Return to real monotonic clock. */

#ifdef __cplusplus
}
#endif
