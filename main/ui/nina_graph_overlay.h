#pragma once

/**
 * @file nina_graph_overlay.h
 * @brief Fullscreen RMS/HFR history graph overlays for the NINA dashboard.
 *
 * Tapping the RMS or HFR value widget opens the corresponding graph.
 * The graph displays historical data fetched from the NINA API.
 */

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include "graph_data_types.h"

/** Create the graph overlay (initially hidden). Called once during dashboard init. */
void nina_graph_overlay_create(lv_obj_t *parent);

/**
 * @brief Show the graph overlay and set the request flag for data fetch.
 * @param type   GRAPH_TYPE_RMS or GRAPH_TYPE_HFR
 * @param page_index  The NINA instance page index (for returning via back button)
 */
void nina_graph_show(graph_type_t type, int page_index);

/** Hide the graph overlay. */
void nina_graph_hide(void);

/** Check if the graph overlay is currently visible. */
bool nina_graph_visible(void);

/** Check if graph data has been requested (needs API fetch). */
bool nina_graph_requested(void);

/** Clear the graph request flag (call after starting the fetch). */
void nina_graph_clear_request(void);

/** Get the currently requested graph type. */
graph_type_t nina_graph_get_type(void);

/** Get the requested number of history points. */
int nina_graph_get_requested_points(void);

/** Set RMS graph data and populate the chart. Call under display lock. */
void nina_graph_set_rms_data(const graph_rms_data_t *data);

/** Set HFR graph data and populate the chart. Call under display lock. */
void nina_graph_set_hfr_data(const graph_hfr_data_t *data);

/** Set the request flag for a background refresh (no loading indicator). */
void nina_graph_set_refresh_pending(void);

/** Apply current theme to the graph overlay. */
void nina_graph_overlay_apply_theme(void);
