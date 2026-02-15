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
        .filter_text_color = 0x737373,
        .progress_color = 0x404040,
        .rms_color = 0x737373,
        .hfr_color = 0x737373,
        .stars_color = 0x737373,
        .saturated_color = 0x737373
    },
    {
        .name = "Deep Space",
        .bg_main = 0x020205,
        .bento_bg = 0x050510,
        .bento_border = 0x0f172a,
        .label_color = 0x475569,
        .text_color = 0x64748b,
        .header_text_color = 0x0284c7,
        .header_grad_color = 0x082f49,
        .filter_text_color = 0xb45309,
        .progress_color = 0x0369a1,
        .rms_color = 0xbe185d,
        .hfr_color = 0x0f766e,
        .stars_color = 0xa16207,
        .saturated_color = 0x7e22ce
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
        .filter_text_color = 0x991b1b,
        .progress_color = 0x7f1d1d,
        .rms_color = 0xcc0000,
        .hfr_color = 0x991b1b,
        .stars_color = 0x991b1b,
        .saturated_color = 0x7f1d1d
    },
    {
        .name = "Cyberpunk",
        .bg_main = 0x020205,
        .bento_bg = 0x050510,
        .bento_border = 0x1e1b4b,
        .label_color = 0x0f766e,
        .text_color = 0x86198f,
        .header_text_color = 0x0e7490,
        .header_grad_color = 0x1e1b4b,
        .filter_text_color = 0xca8a04,
        .progress_color = 0xbe185d,
        .rms_color = 0xbe123c,
        .hfr_color = 0x059669,
        .stars_color = 0xa16207,
        .saturated_color = 0x7e22ce
    },
    {
        .name = "Midnight Green",
        .bg_main = 0x000000,
        .bento_bg = 0x020602,
        .bento_border = 0x052e16,
        .label_color = 0x166534,
        .text_color = 0x15803d,
        .header_text_color = 0x16a34a,
        .header_grad_color = 0x022c22,
        .filter_text_color = 0x15803d,
        .progress_color = 0x166534,
        .rms_color = 0x991b1b,
        .hfr_color = 0x15803d,
        .stars_color = 0xca8a04,
        .saturated_color = 0x6b21a8
    },
    {
        .name = "Solarized Dark",
        .bg_main = 0x00151a,
        .bento_bg = 0x002b36,
        .bento_border = 0x073642,
        .label_color = 0x586e75,
        .text_color = 0x657b83,
        .header_text_color = 0x268bd2,
        .header_grad_color = 0x002b36,
        .filter_text_color = 0xb58900,
        .progress_color = 0x2aa198,
        .rms_color = 0xdc322f,
        .hfr_color = 0x859900,
        .stars_color = 0xb58900,
        .saturated_color = 0x6c71c4
    },
    {
        .name = "Monochrome",
        .bg_main = 0x050505,
        .bento_bg = 0x0a0a0a,
        .bento_border = 0x262626,
        .label_color = 0x525252,
        .text_color = 0x737373,
        .header_text_color = 0x737373,
        .header_grad_color = 0x171717,
        .filter_text_color = 0x737373,
        .progress_color = 0x404040,
        .rms_color = 0x737373,
        .hfr_color = 0x737373,
        .stars_color = 0x525252,
        .saturated_color = 0x404040
    },
    {
        .name = "Crimson",
        .bg_main = 0x110000,
        .bento_bg = 0x220505,
        .bento_border = 0x450a0a,
        .label_color = 0x7f1d1d,
        .text_color = 0x991b1b,
        .header_text_color = 0xb91c1c,
        .header_grad_color = 0x450a0a,
        .filter_text_color = 0x991b1b,
        .progress_color = 0x7f1d1d,
        .rms_color = 0xb91c1c,
        .hfr_color = 0x064e3b,
        .stars_color = 0x78350f,
        .saturated_color = 0x581c87
    },
    {
        .name = "Slate",
        .bg_main = 0x0f1012,
        .bento_bg = 0x111316,
        .bento_border = 0x1f2937,
        .label_color = 0x4b5563,
        .text_color = 0x6b7280,
        .header_text_color = 0x374151,
        .header_grad_color = 0x111827,
        .filter_text_color = 0x92400e,
        .progress_color = 0x155e75,
        .rms_color = 0x9f1239,
        .hfr_color = 0x065f46,
        .stars_color = 0x92400e,
        .saturated_color = 0x6b21a8
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