#include "axi_qos.h"

/* axi_icm_ll.h is a PRIVATE HAL header, pinned to ESP-IDF v5.5.2. The QoS
 * arbiter-priority register fields and the inline helper signatures used here
 * are not part of the stable public API and may change on an IDF upgrade. If a
 * future IDF drops or renames these helpers, this file is the single place to
 * update. */
#include "hal/axi_icm_ll.h"
#include "esp_log.h"

static const char *TAG = "axi_qos";

/* The arbiter-priority register fields are 4 bits wide (bitpos [3:0] per
 * master, default 0); higher value = higher priority. Verified against
 * soc/esp32p4/.../icm_sys_struct.h (reg_*_arqos:4 / reg_*_awqos:4). */
#define AXI_QOS_PRIO_MAX   0xF  /* top read priority for the DSI scanout DMA */
#define AXI_QOS_PRIO_LOW   1    /* de-prioritized Cache/CPU reads             */
#define AXI_QOS_PRIO_WRITE 0    /* keep writes low; the fix is read-side only */
#define AXI_QOS_PRIO_DMA2D 4    /* PPA/DMA2D: above Cache/CPU(1), below DSI   */

void board_boost_dsi_axi_qos(void)
{
    /* DW-GDMA has two master ports (0 -> gdma_mst1, 1 -> gdma_mst2); the DSI
     * scanout reads come through these. Give both top read priority.
     * Signature: axi_icm_ll_set_dw_gdma_qos_arbiter_prio(master_port,
     *            write_prio, read_prio). */
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, AXI_QOS_PRIO_WRITE, AXI_QOS_PRIO_MAX);
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, AXI_QOS_PRIO_WRITE, AXI_QOS_PRIO_MAX);

    /* Lower Cache and CPU read priority so LVGL/PPA/CPU PSRAM reads yield to
     * the scanout DMA. Signature: (write_prio, read_prio). */
    axi_icm_ll_set_cache_qos_arbiter_prio(AXI_QOS_PRIO_WRITE, AXI_QOS_PRIO_LOW);
    axi_icm_ll_set_cpu_qos_arbiter_prio(AXI_QOS_PRIO_WRITE, AXI_QOS_PRIO_LOW);

    /* PPA (screen rotation, JPEG scaling) runs through the DMA2D master, whose
     * arqos/awqos default to 0 — i.e. below the Cache/CPU floor set above. Raise
     * both directions (PPA reads the source frame and writes the destination
     * frame, both in PSRAM) so hardware rotation is not starved, but keep it
     * well under the DSI scanout so the panel still wins the arbiter.
     * Signature: (write_prio, read_prio). */
    axi_icm_ll_set_dma2d_qos_arbiter_prio(AXI_QOS_PRIO_DMA2D, AXI_QOS_PRIO_DMA2D);

    /* Follow-up lever if priority alone is insufficient: the burstiness and
     * peak/transaction-rate regulators (axi_icm_ll_set_qos_burstiness,
     * axi_icm_ll_set_qos_peak_transaction_rate) can throttle the Cache/CPU
     * masters harder. Intentionally not touched here. */

    ESP_LOGI(TAG, "AXI ICM QoS boosted: DW-GDMA(DSI) read prio=%d, Cache/CPU read prio=%d, DMA2D(PPA) prio=%d (blue-flash/PSRAM-starvation fix)",
             AXI_QOS_PRIO_MAX, AXI_QOS_PRIO_LOW, AXI_QOS_PRIO_DMA2D);
}
