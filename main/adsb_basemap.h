#pragma once

/**
 * @file adsb_basemap.h
 * @brief State-boundary basemap drawn under the ADS-B Radar Scope contacts.
 *
 * Source: the same NCEP GeoServer WMS layer and style the Weather Radar
 * page's map style 1 uses (nws:state_boundary, style boundary_gray), fetched
 * as a black-background EPSG:3857 GIF square centred on the receiver
 * position. See radar_wms.h for the request shape this mirrors.
 *
 * Memory: one source frame at S x S RGB565, S = 2 * disc_r + 2 (the render
 * only samples inside the disc, so a square that inscribes it covers every
 * rotation; about 710 px at 720 and 790 px at 800, 1.0 to 1.25 MB) plus one
 * panel-sized display buffer at screen_size() x screen_size() RGB565, both
 * PSRAM. The source frame is released by adsb_basemap_release() on the poll
 * task when the page is parked; the display buffer by
 * adsb_basemap_release_display() on the UI task when the page hides, so
 * nothing is held while the page is hidden and no buffer is ever freed by a
 * task that does not own it.
 *
 * Threading: adsb_basemap_poll() runs on the ADS-B poll task and owns writes
 * to the source frame. adsb_basemap_render() runs on the LVGL/UI task and
 * owns the display buffer, which only the UI task allocates, fills and frees.
 * A mutex guards the source frame.
 *
 * Colours are exactly what the WMS server sends (state lines on black),
 * except for the Red Night theme remap every other image page applies.
 */

#include <stdbool.h>
#include <stdint.h>

/** Creates the module's mutex. Called once from adsb_client_init(), on the
 *  boot path or the runtime enable path, before the poll task exists. Every
 *  other entry point is a no-op (false / NULL) until this has run, which is
 *  what makes a lazy, racy first-use creation unnecessary. */
void adsb_basemap_init(void);

/** Called by the UI whenever the Radar Scope mode becomes shown or stops
 *  being shown. disc_r is the on-screen disc radius in px for the current
 *  range; a basemap is fetched only while scope_shown is true and
 *  disc_r > 0. */
void adsb_basemap_set_scope(bool scope_shown, int disc_r);

/** Called from the ADS-B poll task once per successful poll. Fetches a new
 *  source frame when the receiver position, range or disc radius changed
 *  since the held frame, subject to a 60 s backoff after a failed fetch.
 *  Returns true when a new frame was published this call. */
bool adsb_basemap_poll(float rx_lat, float rx_lon, float range_nm);

/** Poll task: frees the source frame. Called from the poll spine's on_park
 *  hook when the ADS-B page is left. Cheap when nothing is held; does not
 *  touch the generation counter; clears the key so the next poll refetches. */
void adsb_basemap_release(void);

/** UI task: frees the display buffer. Called from the page's hide op, under
 *  the LVGL lock, after the page has dropped the image source that pointed at
 *  it. Cheap when nothing is held. */
void adsb_basemap_release_display(void);

/** Bumps once per newly published source frame; the UI can poll this to know
 *  when to re-render without re-deriving the display buffer every frame. */
uint32_t adsb_basemap_generation(void);

/** Renders the held source frame into a panel-sized disc, rotated so north on
 *  the source lines up with up_deg on screen (the Scope's up-azimuth
 *  rotation), and returns a pointer to it (screen_size() x screen_size()
 *  RGB565, *side_out set to screen_size()). Returns NULL, *side_out
 *  untouched, when no source frame is held or the display buffer could not
 *  be allocated. Must be called from the UI task. */
const uint16_t *adsb_basemap_render(float up_deg, int *side_out);
