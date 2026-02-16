#include "themes.h"
#include <string.h>

static const theme_t themes[] = {
    {
        .name = "Bento Default",
        .bg_main = 0x050505,
        .bento_bg = 0x0a0a0a,
        .bento_border = 0x222222,
        .label_color = 0x6b7280,
        .text_color = 0xa3a3a3,
        .header_text_color = 0x3b82f6,
        .header_grad_color = 0x172554,
        .target_name_color = 0xa3a3a3,
        .filter_text_color = 0xd97706,
        .progress_color = 0x2563eb,
        .rms_color = 0xbe123c,
        .hfr_color = 0x059669,
        .stars_color = 0xc2410c,
        .saturated_color = 0x7e22ce
    },
    {
        .name = "OLED Black",
        .bg_main = 0x000000,
        .bento_bg = 0x000000,
        .bento_border = 0x262626,
        .label_color = 0x525252,
        .text_color = 0x737373,
        .header_text_color = 0x737373,
        .header_grad_color = 0x0a0a0a,
        .target_name_color = 0x737373,
        .filter_text_color = 0x737373,
        .progress_color = 0x404040,
        .rms_color = 0x737373,
        .hfr_color = 0x737373,
        .stars_color = 0x737373,
        .saturated_color = 0x737373
    },
    {
        .name = "Deep Space",
        .bg_main = 0x010203,
        .bento_bg = 0x030508,
        .bento_border = 0x0b121f,
        .label_color = 0x334155,
        .text_color = 0x475569,
        .header_text_color = 0x1e40af,
        .header_grad_color = 0x0f172a,
        .target_name_color = 0x475569,
        .filter_text_color = 0x854d0e,
        .progress_color = 0x1d4ed8,
        .rms_color = 0x9f1239,
        .hfr_color = 0x065f46,
        .stars_color = 0x92400e,
        .saturated_color = 0x5b21b6
    },
    {
        .name = "Red Night",
        .bg_main = 0x000000,
        .bento_bg = 0x000000,
        .bento_border = 0x450a0a,
        .label_color = 0x7f1d1d,
        .text_color = 0xcc0000,
        .header_text_color = 0x991b1b,
        .header_grad_color = 0x000000,
        .target_name_color = 0xcc0000,
        .filter_text_color = 0x991b1b,
        .progress_color = 0x7f1d1d,
        .rms_color = 0xcc0000,
        .hfr_color = 0x991b1b,
        .stars_color = 0x991b1b,
        .saturated_color = 0x7f1d1d
    },
    {
        .name = "Cyberpunk",
        .bg_main = 0x030003,
        .bento_bg = 0x080208,
        .bento_border = 0x1f0b1f,
        .label_color = 0x701a75,
        .text_color = 0x86198f,
        .header_text_color = 0x4c1d95,
        .header_grad_color = 0x2e1065,
        .target_name_color = 0x86198f,
        .filter_text_color = 0xa16207,
        .progress_color = 0xbe185d,
        .rms_color = 0x9f1239,
        .hfr_color = 0x115e59,
        .stars_color = 0x854d0e,
        .saturated_color = 0x6b21a8
    },
    {
        .name = "Midnight Green",
        .bg_main = 0x000000,
        .bento_bg = 0x010501,
        .bento_border = 0x022c22,
        .label_color = 0x14532d,
        .text_color = 0x15803d,
        .header_text_color = 0x166534,
        .header_grad_color = 0x064e3b,
        .target_name_color = 0x15803d,
        .filter_text_color = 0x3f6212,
        .progress_color = 0x15803d,
        .rms_color = 0x7f1d1d,
        .hfr_color = 0x166534,
        .stars_color = 0x854d0e,
        .saturated_color = 0x581c87
    },
    {
        .name = "Solarized Dark",
        .bg_main = 0x001116,
        .bento_bg = 0x001e26,
        .bento_border = 0x002b36,
        .label_color = 0x586e75,
        .text_color = 0x657b83,
        .header_text_color = 0x268bd2,
        .header_grad_color = 0x073642,
        .target_name_color = 0x657b83,
        .filter_text_color = 0xb58900,
        .progress_color = 0x2aa198,
        .rms_color = 0xdc322f,
        .hfr_color = 0x859900,
        .stars_color = 0xb58900,
        .saturated_color = 0x6c71c4
    },
    {
        .name = "Monochrome",
        .bg_main = 0x000000,
        .bento_bg = 0x050505,
        .bento_border = 0x171717,
        .label_color = 0x404040,
        .text_color = 0x525252,
        .header_text_color = 0x525252,
        .header_grad_color = 0x0a0a0a,
        .target_name_color = 0x525252,
        .filter_text_color = 0x525252,
        .progress_color = 0x262626,
        .rms_color = 0x525252,
        .hfr_color = 0x525252,
        .stars_color = 0x525252,
        .saturated_color = 0x525252
    },
    {
        .name = "Crimson",
        .bg_main = 0x050000,
        .bento_bg = 0x0f0202,
        .bento_border = 0x2b0505,
        .label_color = 0x7f1d1d,
        .text_color = 0x991b1b,
        .header_text_color = 0x7f1d1d,
        .header_grad_color = 0x2b0505,
        .target_name_color = 0x991b1b,
        .filter_text_color = 0x7f1d1d,
        .progress_color = 0x450a0a,
        .rms_color = 0x7f1d1d,
        .hfr_color = 0x7f1d1d,
        .stars_color = 0x7f1d1d,
        .saturated_color = 0x7f1d1d
    },
    {
        .name = "Slate",
        .bg_main = 0x020204,
        .bento_bg = 0x0b0c10,
        .bento_border = 0x1f2937,
        .label_color = 0x374151,
        .text_color = 0x4b5563,
        .header_text_color = 0x475569,
        .header_grad_color = 0x0f172a,
        .target_name_color = 0x4b5563,
        .filter_text_color = 0x713f12,
        .progress_color = 0x0e7490,
        .rms_color = 0x881337,
        .hfr_color = 0x064e3b,
        .stars_color = 0x713f12,
        .saturated_color = 0x581c87
    },
    {
        .name = "All Black",
        .bg_main = 0x000000,
        .bento_bg = 0x000000,
        .bento_border = 0x000000,
        .label_color = 0x262626,
        .text_color = 0x404040,
        .header_text_color = 0x333333,
        .header_grad_color = 0x000000,
        .target_name_color = 0x404040,
        .filter_text_color = 0x404040,
        .progress_color = 0x171717,
        .rms_color = 0x404040,
        .hfr_color = 0x404040,
        .stars_color = 0x404040,
        .saturated_color = 0x404040
    },
    {
        .name = "Kumar",
        .bg_main = 0x000000,
        .bento_bg = 0x000000,
        .bento_border = 0x263238,
        .label_color = 0x455A64,
        .text_color = 0x78909C,
        .header_text_color = 0x4FC3F7,
        .header_grad_color = 0x000000,
        .target_name_color = 0xBCAAA4,
        .filter_text_color = 0x78909C,
        .progress_color = 0x78909C,
        .rms_color = 0x78909C,
        .hfr_color = 0x78909C,
        .stars_color = 0x78909C,
        .saturated_color = 0x78909C
    }
};

int themes_get_count(void) {
    return sizeof(themes) / sizeof(theme_t);
}

const theme_t* themes_get(int index) {
    if (index < 0 || index >= themes_get_count()) {
        return &themes[0];
    }
    return &themes[index];
}

int themes_get_index_by_name(const char *name) {
    for (int i = 0; i < themes_get_count(); i++) {
        if (strcmp(themes[i].name, name) == 0) {
            return i;
        }
    }
    return 0;
}