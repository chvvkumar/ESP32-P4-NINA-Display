/* Pure controller resolution. See board_detect.h. Nothing in this file may
 * include an ESP-IDF, FreeRTOS or LVGL header: the host test compiles it. */

#include "board_detect.h"

#include <string.h>

static const board_rddid_entry_t s_rddid_table[] = {
    {
        /* Captured 2026-07-25 on ninadash4, the square 720x720 board, under
         * ESP-IDF v5.5.2. Boot log line: "RDDID 38 21 1F". */
        .rddid      = {0x38, 0x21, 0x1F},
        .controller = BOARD_CTRL_ST7703,
        .captured   = true,
        .note       = "square 4B, 720x720 ST7703",
    },
    {
        /* Captured 2026-07-25 on a round 800x800 board (chip rev v1.0) under
         * ESP-IDF v5.5.2. Boot log line: "RDDID 93 65 04". The 3.4 inch versus
         * 4.0 inch axis is not visible here and comes from NVS board/panel. */
        .rddid      = {0x93, 0x65, 0x04},
        .controller = BOARD_CTRL_JD9365,
        .captured   = true,
        .note       = "round, 800x800 JD9365, 3.4in and 4.0in share the id",
    },
};

const board_rddid_entry_t *board_detect_table(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_rddid_table) / sizeof(s_rddid_table[0]);
    }
    return s_rddid_table;
}

board_controller_t board_detect_controller(const board_rddid_entry_t *table,
                                           size_t count,
                                           const uint8_t rddid[3])
{
    if (table == NULL || rddid == NULL) {
        return BOARD_CTRL_UNKNOWN;
    }
    for (size_t i = 0; i < count; i++) {
        if (!table[i].captured) {
            continue;
        }
        if (memcmp(table[i].rddid, rddid, 3) == 0) {
            return table[i].controller;
        }
    }
    return BOARD_CTRL_UNKNOWN;
}

const char *board_detect_controller_name(board_controller_t controller)
{
    switch (controller) {
    case BOARD_CTRL_JD9365: return "JD9365";
    case BOARD_CTRL_ST7703: return "ST7703";
    default:                return "unknown";
    }
}
