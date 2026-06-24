#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Boost AXI ICM read-arbitration priority for the MIPI-DSI scanout path.
 *
 * The 720x720 DPI panel scans out its framebuffer from PSRAM via DW-GDMA. That
 * DMA shares the PSRAM bus with the Cache/CPU masters (LVGL widget allocs, PPA
 * blits, general CPU reads). Under heavy LVGL/PPA traffic the DSI DMA can be
 * starved of PSRAM read bandwidth, the scanout FIFO underruns, and the panel
 * briefly shows the fill color (a blue-screen flash).
 *
 * This raises both DW-GDMA master ports to the maximum read priority on the AXI
 * ICM and lowers the Cache and CPU master read priorities so they yield to the
 * scanout. Write priorities are left low. Only register writes are performed;
 * the call is idempotent and lock-free.
 *
 * Call once after the display/DPI panel is started (after
 * bsp_display_start_with_config), so the DPI panel and its DW-GDMA channel
 * already exist.
 */
void board_boost_dsi_axi_qos(void);

#ifdef __cplusplus
}
#endif
