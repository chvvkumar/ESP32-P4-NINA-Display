/**
 * @file board_detect.h
 * @brief Pure panel controller resolution: the RDDID signature table and the
 *        match, with nothing else in it.
 *
 * This header and board_detect.c include no ESP-IDF, FreeRTOS, LVGL or NVS
 * header, so test/host/test_board_detect.c compiles them directly. Everything
 * that touches hardware lives in board_profile.c, which reads the three bytes
 * off the DSI link through the BSP and hands them in here as plain data.
 *
 * There is no cache. The probe runs every boot; a read that fails, or a
 * successful read of an id in no captured row, is UNKNOWN. Driving a panel the
 * device could not identify this boot is not a fallback, it is a guess.
 */
#ifndef BOARD_DETECT_H
#define BOARD_DETECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_CTRL_UNKNOWN = 0,
    BOARD_CTRL_JD9365,   /* both round panels */
    BOARD_CTRL_ST7703,   /* square 4B panel   */
} board_controller_t;

/**
 * One RDDID signature. `captured` is the gate: a row whose bytes have not been
 * read off real hardware never matches, so an unprovisioned table resolves
 * UNKNOWN instead of naming a controller. Several rows may name the same
 * controller, which is how a vendor panel revision with a different id is added.
 */
typedef struct {
    uint8_t            rddid[3];
    board_controller_t controller;
    bool               captured;
    const char        *note;
} board_rddid_entry_t;

/** @brief The built-in signature table. @p count receives the row count. */
const board_rddid_entry_t *board_detect_table(size_t *count);

/** @brief Match three RDDID bytes against @p table. UNKNOWN when nothing matches. */
board_controller_t board_detect_controller(const board_rddid_entry_t *table,
                                           size_t count,
                                           const uint8_t rddid[3]);

/** @brief "ST7703", "JD9365" or "unknown". */
const char *board_detect_controller_name(board_controller_t controller);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_DETECT_H */
