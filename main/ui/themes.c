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
    },
    {
        .name = "Midnight Industrial",
        .bg_main = 0x000000,
        .bento_bg = 0x000000,
        .bento_border = 0x263238,
        .label_color = 0x455A64,
        .text_color = 0x90A4AE,
        .header_text_color = 0x0097A7,
        .header_grad_color = 0x000000,
        .target_name_color = 0xB0BEC5,
        .filter_text_color = 0x78909C,
        .progress_color = 0x00838F,
        .rms_color = 0x0097A7,
        .hfr_color = 0x0097A7,
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