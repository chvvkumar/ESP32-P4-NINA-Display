/* Implementations backing the host shim headers. Link this into every
 * test executable that includes firmware sources depending on esp_timer.h. */

#include "esp_timer.h"

#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static int64_t s_fake_time_us = 0;
static int s_fake_time_active = 0;

void shim_set_time_us(int64_t us)
{
    s_fake_time_us = us;
    s_fake_time_active = 1;
}

void shim_reset_time(void)
{
    s_fake_time_active = 0;
    s_fake_time_us = 0;
}

static int64_t monotonic_time_us(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    LARGE_INTEGER counter;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
#endif
}

int64_t esp_timer_get_time(void)
{
    if (s_fake_time_active) {
        return s_fake_time_us;
    }
    return monotonic_time_us();
}
