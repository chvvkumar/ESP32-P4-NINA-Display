/* Host test for main/board_detect.c: the RDDID signature table and the match.
 * There is no cache and no precedence logic to test; the probe runs every boot
 * and an unknown id is UNKNOWN, full stop. Assert-style like the other host
 * tests in this directory. */
#include "board_detect.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void check_ctrl(const char *label, board_controller_t got, board_controller_t expect)
{
    printf("%-58s got=%-8s expect=%-8s %s\n", label,
           board_detect_controller_name(got), board_detect_controller_name(expect),
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_bool(const char *label, bool got, bool expect)
{
    printf("%-58s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void)
{
    size_t count = 0;
    const board_rddid_entry_t *table = board_detect_table(&count);

    printf("== table ==\n");
    check_bool("table is not NULL", table != NULL, true);
    check_bool("table has at least two rows", count >= 2, true);
    for (size_t i = 0; i < count; i++) {
        char label[80];
        snprintf(label, sizeof(label), "row %u captured", (unsigned)i);
        check_bool(label, table[i].captured, true);
        snprintf(label, sizeof(label), "row %u has a note", (unsigned)i);
        check_bool(label, table[i].note != NULL, true);
    }

    printf("\n== captured signatures ==\n");
    const uint8_t st7703[3] = {0x38, 0x21, 0x1F};
    const uint8_t jd9365[3] = {0x93, 0x65, 0x04};
    check_ctrl("38 21 1F is ST7703 (ninadash4, 2026-07-25)",
               board_detect_controller(table, count, st7703), BOARD_CTRL_ST7703);
    check_ctrl("93 65 04 is JD9365 (round 800x800, 2026-07-25)",
               board_detect_controller(table, count, jd9365), BOARD_CTRL_JD9365);

    printf("\n== non-matches are UNKNOWN, never a guess ==\n");
    const uint8_t zeros[3]  = {0x00, 0x00, 0x00};
    const uint8_t ones[3]   = {0xFF, 0xFF, 0xFF};
    const uint8_t nearby[3] = {0x38, 0x21, 0x1E};   /* one byte off ST7703 */
    const uint8_t shift[3]  = {0x21, 0x1F, 0x38};   /* ST7703 bytes rotated */
    check_ctrl("all zeros", board_detect_controller(table, count, zeros), BOARD_CTRL_UNKNOWN);
    check_ctrl("all ones",  board_detect_controller(table, count, ones),  BOARD_CTRL_UNKNOWN);
    check_ctrl("one byte off ST7703", board_detect_controller(table, count, nearby), BOARD_CTRL_UNKNOWN);
    check_ctrl("ST7703 bytes rotated", board_detect_controller(table, count, shift), BOARD_CTRL_UNKNOWN);

    printf("\n== degenerate arguments ==\n");
    check_ctrl("NULL table",  board_detect_controller(NULL, count, st7703), BOARD_CTRL_UNKNOWN);
    check_ctrl("NULL rddid",  board_detect_controller(table, count, NULL),  BOARD_CTRL_UNKNOWN);
    check_ctrl("zero count",  board_detect_controller(table, 0, st7703),    BOARD_CTRL_UNKNOWN);

    printf("\n== names ==\n");
    check_bool("ST7703 name", strcmp(board_detect_controller_name(BOARD_CTRL_ST7703), "ST7703") == 0, true);
    check_bool("JD9365 name", strcmp(board_detect_controller_name(BOARD_CTRL_JD9365), "JD9365") == 0, true);
    check_bool("unknown name", strcmp(board_detect_controller_name(BOARD_CTRL_UNKNOWN), "unknown") == 0, true);

    printf("\n%s (%d failure%s)\n", fails == 0 ? "PASS" : "FAIL", fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
