#ifndef THEMES_H
#define THEMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t bg_main;
    uint32_t bento_bg;
    uint32_t bento_border;
    uint32_t label_color;
    uint32_t text_color;
    uint32_t header_text_color;
    uint32_t header_grad_color;
    uint32_t filter_text_color;
    uint32_t progress_color;
    uint32_t rms_color;
    uint32_t hfr_color;
    uint32_t stars_color;
    uint32_t saturated_color;
} theme_t;

/**
 * @brief Get the total number of available themes
 */
int themes_get_count(void);

/**
 * @brief Get a theme by index
 * @param index Theme index (0 to count-1)
 * @return Pointer to the theme structure, or default theme if index invalid
 */
const theme_t* themes_get(int index);

/**
 * @brief Get the index of a theme by name
 * @param name Name of the theme
 * @return Index of the theme, or 0 if not found
 */
int themes_get_index_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif // THEMES_H
