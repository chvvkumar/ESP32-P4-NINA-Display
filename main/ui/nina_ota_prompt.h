#pragma once

#include "lvgl.h"

void nina_ota_prompt_create(lv_obj_t *parent);
void nina_ota_prompt_show(const char *new_version, const char *current_version, const char *release_notes);
void nina_ota_prompt_hide(void);
bool nina_ota_prompt_visible(void);
void nina_ota_prompt_show_progress(void);
void nina_ota_prompt_set_progress(int percent);
void nina_ota_prompt_show_error(const char *error_msg);
void nina_ota_prompt_apply_theme(void);
bool nina_ota_prompt_update_accepted(void);
bool nina_ota_prompt_skipped(void);
