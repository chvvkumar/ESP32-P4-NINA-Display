/**
 * @file graph_downsample.c
 * @brief Pure-math helpers for graph overlay decimation and Y-range sizing.
 *
 * No LVGL/ESP includes. Only math.h/stddef.h. Single-precision throughout
 * (float/fabsf) — the P4 FPU is single-precision only.
 */

#include "graph_downsample.h"
#include <math.h>

graph_downsample_t graph_downsample_compute(int count) {
    graph_downsample_t result;
    result.stride = 1;
    result.disp_count = count;
    if (count > GRAPH_MAX_DISPLAY_POINTS) {
        result.stride = (count + GRAPH_MAX_DISPLAY_POINTS - 1) / GRAPH_MAX_DISPLAY_POINTS;
        result.disp_count = (count + result.stride - 1) / result.stride;
    }
    return result;
}

int graph_rms_y_range(const float *ra, const float *dec, int count) {
    float max_val = 0.5f; /* minimum visible range */
    for (int i = 0; i < count; i++) {
        float abs_ra = fabsf(ra[i]);
        float abs_dec = fabsf(dec[i]);
        if (abs_ra > max_val) max_val = abs_ra;
        if (abs_dec > max_val) max_val = abs_dec;
    }
    /* Add 20% headroom and round up */
    int range = (int)(max_val * 120.0f + 50.0f); /* x100 then +headroom */
    if (range < 100) range = 100;
    return range;
}

int graph_hfr_y_range(const float *hfr, int count, float *out_sum) {
    float max_val = 1.0f;
    float sum = 0;
    for (int i = 0; i < count; i++) {
        if (hfr[i] > max_val) max_val = hfr[i];
        sum += hfr[i];
    }
    int range = (int)(max_val * 120.0f + 0.5f); /* x100 + 20% headroom */
    if (range < 200) range = 200;
    if (out_sum) *out_sum = sum;
    return range;
}
