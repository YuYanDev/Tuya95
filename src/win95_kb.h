/**
 * @file win95_kb.h
 * @brief Win95 Smart-ABC style pixel keyboard — drop-in replacement for
 *        lv_keyboard.  API is intentionally similar so callers only need to
 *        swap the function names.
 */
#pragma once

#include "tuya_cloud_types.h"
#include "lvgl.h"

/**
 * @brief Create a Win95-style keyboard attached to @p parent.
 *        The keyboard is 480 px wide and 110 px tall, positioned at the
 *        bottom of the screen.  It is hidden by default.
 */
lv_obj_t *win95_kb_create(lv_obj_t *parent);

/**
 * @brief Bind the keyboard to a textarea.  Keyboard input is written into
 *        @p ta from that point on.  Pass NULL to unbind.
 */
VOID_T win95_kb_set_textarea(lv_obj_t *kb, lv_obj_t *ta);
