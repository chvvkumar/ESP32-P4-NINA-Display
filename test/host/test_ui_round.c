/* Host test for main/ui/ui_round.h: the round layout geometry every phase 2
 * builder derives its pixels from. Header-only and pure, so this test compiles
 * it directly against main/screen_geom.c and needs no LVGL and no ESP-IDF.
 *
 * The host build is the SQUARE family (test/host/shims/sdkconfig.h defines no
 * CONFIG_NINA_FAMILY_ROUND, so SCREEN_ROUND is 0). The rim and chord helpers
 * read the runtime geometry only, so the round rows are reached by calling
 * screen_geom_set() the way board_profile_init() does at boot. ui_page_inset()
 * is the one family-dependent helper: only its square arm exists in this
 * binary, and its round arm is screen_safe_inset(), pinned by test_screen_geom.c.
 *
 * Integer expectations only. sqrtf() truncation is part of the contract: a
 * layout must place the same pixel every time, so the test asserts the
 * truncated value, never a rounded one. */
#include "ui_round.h"

#include <math.h>
#include <stdio.h>

static int fails = 0;

static void check_int(const char *label, int got, int expect)
{
    printf("%-62s got=%-6d expect=%-6d %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void)
{
    printf("== 4c round 720 (safe radius 360, inset 105) ==\n");
    screen_geom_set(720, 105, 360);
    check_int("ui_rim_radius = 0.985 * 360", ui_rim_radius(), 354);
    check_int("ui_chord_half(0) = the rim radius", ui_chord_half(0), 354);
    check_int("ui_chord_half(354) = 0 at the rim", ui_chord_half(354), 0);
    check_int("ui_chord_half(-354) = 0, sign ignored", ui_chord_half(-354), 0);
    check_int("ui_chord_half(400) = 0 outside the rim", ui_chord_half(400), 0);
    check_int("ui_chord_half is symmetric at 240", ui_chord_half(240), ui_chord_half(-240));
    check_int("ui_chord_at_y(360) = full width on the centreline", ui_chord_at_y(360), 684);
    /* 354*354 - 240*240 = 67716; sqrtf gives 260.2, truncated to 260. */
    check_int("ui_chord_at_y(600) = 2 * 260", ui_chord_at_y(600), 2 * (int)sqrtf(354.0f * 354.0f - 240.0f * 240.0f));
    check_int("ui_chord_at_y(600) = 486", ui_chord_at_y(600), 486);
    check_int("ui_chord_at_y(0) = 0 at the panel edge", ui_chord_at_y(0), 0);

    printf("\n== 3.4c round 800 (safe radius 400, inset 118) ==\n");
    screen_geom_set(800, 118, 400);
    check_int("ui_rim_radius = 0.985 * 400", ui_rim_radius(), 394);
    check_int("ui_chord_half(0) = the rim radius", ui_chord_half(0), 394);
    check_int("ui_chord_half(394) = 0 at the rim", ui_chord_half(394), 0);
    check_int("ui_chord_at_y(400) = full width on the centreline", ui_chord_at_y(400), 760);

    printf("\n== 4b square 720 (no safe circle) ==\n");
    screen_geom_set(720, 0, 0);
    check_int("ui_rim_radius = 0 on square", ui_rim_radius(), 0);
    check_int("ui_chord_half(0) = 0 on square", ui_chord_half(0), 0);
    check_int("ui_chord_at_y(360) = 0 on square", ui_chord_at_y(360), 0);

    printf("\n== ui_page_inset / ui_page_root_size (square family binary) ==\n");
    check_int("UI_SQUARE_INSET equals OUTER_PADDING", UI_SQUARE_INSET, 16);
    check_int("SCREEN_ROUND is 0 in the host build", SCREEN_ROUND, 0);
    check_int("ui_page_inset = 16 on square", ui_page_inset(), 16);
    check_int("ui_page_root_size = 688 on the 4B", ui_page_root_size(), 688);
    /* The round arm is screen_safe_inset(): 105 at 720 gives a 510 px root and
     * 118 at 800 gives 564. Those two numbers are pinned by test_screen_geom.c
     * ("safe square side"), which is the same arithmetic, so they are not
     * re-asserted through a family macro this binary does not carry. */

    printf("\n%s (%d failure%s)\n", fails == 0 ? "PASS" : "FAIL", fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
