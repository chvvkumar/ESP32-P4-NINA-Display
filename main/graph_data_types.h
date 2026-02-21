#pragma once

/**
 * @file graph_data_types.h
 * @brief Data transfer types for graph overlay — shared between API and UI layers.
 *
 * This header has NO dependencies on LVGL or any UI code.
 */

#include <stdint.h>

/** Graph type selector */
typedef enum {
    GRAPH_TYPE_RMS = 0,
    GRAPH_TYPE_HFR = 1,
} graph_type_t;

/** Maximum number of data points the graph can display */
#define GRAPH_MAX_POINTS 500

/** RMS graph data — filled by the API fetcher, consumed by the graph overlay */
typedef struct {
    float ra[GRAPH_MAX_POINTS];
    float dec[GRAPH_MAX_POINTS];
    float total[GRAPH_MAX_POINTS];
    int   count;          /**< Number of valid data points */
    float rms_ra;         /**< Current RMS RA summary */
    float rms_dec;        /**< Current RMS DEC summary */
    float rms_total;      /**< Current RMS Total summary */
    float peak_ra;        /**< Peak RA */
    float peak_dec;       /**< Peak DEC */
    float pixel_scale;    /**< Pixel scale (arcsec/px) */
} graph_rms_data_t;

/** HFR graph data — filled by the API fetcher, consumed by the graph overlay */
typedef struct {
    float hfr[GRAPH_MAX_POINTS];
    int   stars[GRAPH_MAX_POINTS];
    int   count;          /**< Number of valid data points */
} graph_hfr_data_t;
