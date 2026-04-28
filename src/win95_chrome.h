/**
 * @file win95_chrome.h
 * @brief Win95 visual primitives shared by desktop, dialogs and games
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_CHROME_H__
#define __WIN95_CHROME_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "lvgl.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Apply a Win95 1-pixel raised bevel:
 *        white highlight on top + left, shadow on bottom + right.
 * @param[in] obj target object
 * @return none
 */
VOID_T win95_chrome_raised(lv_obj_t *obj);

/**
 * @brief Apply a Win95 1-pixel sunken bevel:
 *        shadow on top + left, white highlight on bottom + right.
 * @param[in] obj target object
 * @return none
 */
VOID_T win95_chrome_sunken(lv_obj_t *obj);

/**
 * @brief Apply a Win95 2-pixel button bevel (raised, 4 distinct ridge colors).
 *        Implemented with four 1px child rectangles for pixel-perfect output.
 * @param[in] obj target object (must have a fixed size set first)
 * @return none
 * @note No rounded corners, no anti-aliasing. Caller should still configure
 *       background color and clear LV_OBJ_FLAG_SCROLLABLE separately.
 */
VOID_T win95_chrome_button(lv_obj_t *obj);

/**
 * @brief Apply a Win95 2-pixel inset bevel (sunken, 4 distinct ridge colors).
 * @param[in] obj target object (must have a fixed size set first)
 * @return none
 */
VOID_T win95_chrome_inset(lv_obj_t *obj);

/**
 * @brief Paint a chunky 1-pixel checker dither across the object.
 *        Used to imitate the grainy desktop / felt of classic Win95 themes.
 *        Builds a small (8x8) tile of pixel rectangles and tiles via repeat.
 * @param[in] obj target object (must have a fixed size set first)
 * @param[in] color_a base color
 * @param[in] color_b accent color
 * @return none
 * @note The dithered tile is drawn as a single static lv_image_dsc_t every
 *       call; it does not allocate memory. The pattern is anchored to the
 *       object's top-left so it lines up across siblings.
 */
VOID_T win95_chrome_grain(lv_obj_t *obj, UINT32_T color_a, UINT32_T color_b);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_CHROME_H__ */
