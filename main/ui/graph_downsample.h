#pragma once

/**
 * @file graph_downsample.h
 * @brief Pure-math helpers for graph overlay decimation and Y-range sizing.
 *
 * Extracted from nina_graph_overlay.c so the stride/range arithmetic can be
 * unit tested on the host without pulling in LVGL. No LVGL/ESP includes here;
 * callers own everything display-related (lv_chart_* calls stay in the
 * overlay). Behavior must stay byte-for-byte identical to the inline code it
 * replaced.
 */

#include <stddef.h>

/* Max points actually drawn on the chart. The chart is ~720px wide, so more
 * than this yields sub-pixel polyline segments with no visible difference.
 * Decimating the displayed set (storage stays GRAPH_MAX_POINTS) roughly halves
 * the per-point loop and polyline segment count for the heavy locked redraw. */
#define GRAPH_MAX_DISPLAY_POINTS 360

/** Result of decimating `count` source points down to the chart's display width. */
typedef struct {
    int stride;      /**< Step through the source array (>= 1) */
    int disp_count;  /**< Number of points that will actually be pushed to the chart */
} graph_downsample_t;

/**
 * @brief Compute stride/disp_count for decimating a source series.
 *
 * @param count Number of valid source samples. Must be > 0 (caller handles
 *              the count<=0 "no data" case before calling this).
 * @return stride==1, disp_count==count when count <= GRAPH_MAX_DISPLAY_POINTS;
 *         otherwise an integer stride sized so ceil(count/stride) points are
 *         displayed.
 */
graph_downsample_t graph_downsample_compute(int count);

/**
 * @brief Compute the symmetric Y-range (x100, arcsec) for the RMS chart.
 *
 * Mirrors the inline RMS range loop: starts from a 0.5" minimum visible
 * range, tracks the largest |ra|/|dec| magnitude, adds 20% headroom, and
 * floors the result at 100 (i.e. 1.0").
 *
 * @param ra    RA sample array, length >= count.
 * @param dec   DEC sample array, length >= count.
 * @param count Number of valid samples. Must be > 0.
 * @return Range in x100 arcsec units; chart range is [-range, range].
 */
int graph_rms_y_range(const float *ra, const float *dec, int count);

/**
 * @brief Compute the Y-range (x100) for the HFR chart, and the sample sum.
 *
 * Mirrors the inline HFR range loop: starts from a 1.0 minimum visible
 * range, tracks the largest HFR value, adds 20% headroom, and floors the
 * result at 200 (i.e. 2.0). Also accumulates the sum of all samples so the
 * caller can compute the average for the summary label without a second pass.
 *
 * @param hfr     HFR sample array, length >= count.
 * @param count   Number of valid samples. Must be > 0.
 * @param out_sum Optional; if non-NULL, receives the sum of hfr[0..count-1].
 * @return Range in x100 units; chart range is [0, range].
 */
int graph_hfr_y_range(const float *hfr, int count, float *out_sum);
