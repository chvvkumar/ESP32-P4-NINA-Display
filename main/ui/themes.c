#include "themes.h"
#include <string.h>

static const theme_t themes[] = {
    {
        .name = "Default",
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
        .is_red_night = true,
    },
    {
        .name = "Cyber Dusk",
        .bg_main = 0x050508,
        .bento_bg = 0x0b0b12,
        .bento_border = 0x1e2e3d,
        .label_color = 0x607888,
        .text_color = 0x94aebf,
        .header_text_color = 0x3d5c73,
        .header_grad_color = 0x050508,
        .target_name_color = 0x94aebf,
        .filter_text_color = 0x6b8da5,
        .progress_color = 0x3d5c73,
        .rms_color = 0x507090,
        .hfr_color = 0x3d5c73,
    },
    {
        .name = "Stellar Ember",
        .bg_main = 0x080604,
        .bento_bg = 0x120e08,
        .bento_border = 0x2a1f10,
        .label_color = 0x7a5a30,
        .text_color = 0xd4a054,
        .header_text_color = 0xb8702a,
        .header_grad_color = 0x080604,
        .target_name_color = 0xd4a054,
        .filter_text_color = 0x9a7040,
        .progress_color = 0xb8702a,
        .rms_color = 0xd06030,
        .hfr_color = 0x8a6535,
    },
    {
        .name = "Arctic Steel",
        .bg_main = 0x040608,
        .bento_bg = 0x0a1018,
        .bento_border = 0x1a2838,
        .label_color = 0x4a6478,
        .text_color = 0x8ab0c8,
        .header_text_color = 0x3a7090,
        .header_grad_color = 0x040608,
        .target_name_color = 0x8ab0c8,
        .filter_text_color = 0x5a8098,
        .progress_color = 0x3a7090,
        .rms_color = 0xaa4455,
        .hfr_color = 0x3a7090,
    },
    {
        .name = "Oxidized Copper",
        .bg_main = 0x040806,
        .bento_bg = 0x081210,
        .bento_border = 0x18352a,
        .label_color = 0x3a7a60,
        .text_color = 0x70c8a0,
        .header_text_color = 0x4a9070,
        .header_grad_color = 0x040806,
        .target_name_color = 0x70c8a0,
        .filter_text_color = 0x508868,
        .progress_color = 0x4a9070,
        .rms_color = 0xc87848,
        .hfr_color = 0x4a9070,
    },
    {
        .name = "Solar Flare",
        .bg_main = 0x060600,
        .bento_bg = 0x0c0c04,
        .bento_border = 0x2a2808,
        .label_color = 0x6a6520,
        .text_color = 0xc8b830,
        .header_text_color = 0xa89820,
        .header_grad_color = 0x060600,
        .target_name_color = 0xc8b830,
        .filter_text_color = 0x888018,
        .progress_color = 0xa89820,
        .rms_color = 0xc86030,
        .hfr_color = 0x8a8018,
    },
    {
        .name = "Phantom Green",
        .bg_main = 0x020402,
        .bento_bg = 0x040a04,
        .bento_border = 0x0a2010,
        .label_color = 0x1a5028,
        .text_color = 0x30b050,
        .header_text_color = 0x208838,
        .header_grad_color = 0x020402,
        .target_name_color = 0x30b050,
        .filter_text_color = 0x208838,
        .progress_color = 0x208838,
        .rms_color = 0xcc3333,
        .hfr_color = 0x208838,
    },
    {
        .name = "Bloodmoon",
        .bg_main = 0x080200,
        .bento_bg = 0x120804,
        .bento_border = 0x301808,
        .label_color = 0x6a3018,
        .text_color = 0xd05828,
        .header_text_color = 0xa84020,
        .header_grad_color = 0x080200,
        .target_name_color = 0xd05828,
        .filter_text_color = 0x904020,
        .progress_color = 0xa84020,
        .rms_color = 0xd05828,
        .hfr_color = 0xa84020,
    },
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
