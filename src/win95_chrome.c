/**
 * @file win95_chrome.c
 * @brief Win95 visual primitives implementation
 * @version 1.0
 * @date 2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_chrome.h"
#include "bios_simulator.h"
#include "tal_api.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define WIN95_CHROME_BLACK       0x000000
#define WIN95_CHROME_WHITE       0xFFFFFF
#define WIN95_CHROME_LIGHT_GRAY  0xDFDFDF
#define WIN95_CHROME_GRAY        0xC0C0C0
#define WIN95_CHROME_DARK_GRAY   0x808080

#define WIN95_CHROME_GRAIN_TILE_W 4
#define WIN95_CHROME_GRAIN_TILE_H 4

/* ---------------------------------------------------------------------------
 * Static helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Create a non-interactive solid rectangle child of \a parent.
 * @param[in] parent parent obj
 * @param[in] x position relative to parent
 * @param[in] y position relative to parent
 * @param[in] w width
 * @param[in] h height
 * @param[in] color RGB color
 * @return new rectangle object
 */
STATIC lv_obj_t *__rect(lv_obj_t *parent, INT32_T x, INT32_T y,
                        INT32_T w, INT32_T h, UINT32_T color)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Apply a Win95 1-pixel raised bevel
 * @param[in] obj target
 * @return none
 */
VOID_T win95_chrome_raised(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_CHROME_WHITE), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(WIN95_CHROME_DARK_GRAY), 0);
    lv_obj_set_style_shadow_width(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_x(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 1, 0);
    lv_obj_set_style_shadow_spread(obj, 0, 0);
}

/**
 * @brief Apply a Win95 1-pixel sunken bevel
 * @param[in] obj target
 * @return none
 */
VOID_T win95_chrome_sunken(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_CHROME_DARK_GRAY), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(WIN95_CHROME_WHITE), 0);
    lv_obj_set_style_shadow_width(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_x(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 1, 0);
    lv_obj_set_style_shadow_spread(obj, 0, 0);
}

/**
 * @brief Apply a Win95 button (2px raised) bevel using 4 child rects
 * @param[in] obj target with fixed size
 * @return none
 * @note Top-left ridge: outer = white, inner = light gray
 *       Bottom-right ridge: outer = black, inner = dark gray
 */
VOID_T win95_chrome_button(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    INT32_T w = lv_obj_get_width(obj);
    INT32_T h = lv_obj_get_height(obj);
    if (w < 4 || h < 4) {
        win95_chrome_raised(obj);
        return;
    }
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 2, 0);

    /* outer ring */
    __rect(obj, 0,     0,     w,     1,     WIN95_CHROME_WHITE);
    __rect(obj, 0,     0,     1,     h,     WIN95_CHROME_WHITE);
    __rect(obj, 0,     h - 1, w,     1,     WIN95_CHROME_BLACK);
    __rect(obj, w - 1, 0,     1,     h,     WIN95_CHROME_BLACK);

    /* inner ring */
    __rect(obj, 1,     1,     w - 2, 1,     WIN95_CHROME_LIGHT_GRAY);
    __rect(obj, 1,     1,     1,     h - 2, WIN95_CHROME_LIGHT_GRAY);
    __rect(obj, 1,     h - 2, w - 2, 1,     WIN95_CHROME_DARK_GRAY);
    __rect(obj, w - 2, 1,     1,     h - 2, WIN95_CHROME_DARK_GRAY);
}

/**
 * @brief Apply a Win95 inset (2px sunken) bevel using 4 child rects
 * @param[in] obj target with fixed size
 * @return none
 */
VOID_T win95_chrome_inset(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    INT32_T w = lv_obj_get_width(obj);
    INT32_T h = lv_obj_get_height(obj);
    if (w < 4 || h < 4) {
        win95_chrome_sunken(obj);
        return;
    }
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 2, 0);

    /* outer ring */
    __rect(obj, 0,     0,     w,     1,     WIN95_CHROME_DARK_GRAY);
    __rect(obj, 0,     0,     1,     h,     WIN95_CHROME_DARK_GRAY);
    __rect(obj, 0,     h - 1, w,     1,     WIN95_CHROME_WHITE);
    __rect(obj, w - 1, 0,     1,     h,     WIN95_CHROME_WHITE);

    /* inner ring */
    __rect(obj, 1,     1,     w - 2, 1,     WIN95_CHROME_BLACK);
    __rect(obj, 1,     1,     1,     h - 2, WIN95_CHROME_BLACK);
    __rect(obj, 1,     h - 2, w - 2, 1,     WIN95_CHROME_LIGHT_GRAY);
    __rect(obj, w - 2, 1,     1,     h - 2, WIN95_CHROME_LIGHT_GRAY);
}

/**
 * @brief Tile a 2-color 4x4 checker dither across \a obj
 * @param[in] obj target with fixed size
 * @param[in] color_a base color
 * @param[in] color_b accent color
 * @return none
 * @note Lays out 1x1 child rectangles for the accent pattern. Should be
 *       called only on small areas (e.g. a 480x320 desktop creates ~9600 dots,
 *       which is unrealistic). Prefer calling on confined sub-regions like
 *       cards / cells / status bars only.
 */
VOID_T win95_chrome_grain(lv_obj_t *obj, UINT32_T color_a, UINT32_T color_b)
{
    if (obj == NULL) {
        return;
    }
    INT32_T w = lv_obj_get_width(obj);
    INT32_T h = lv_obj_get_height(obj);
    if (w <= 0 || h <= 0) {
        return;
    }
    lv_obj_set_style_bg_color(obj, lv_color_hex(color_a), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    /* Bayer-2 ordered dither: every other pixel in a 2x2 tile.
     * Reference pattern (1 = accent):
     *   1 0
     *   0 1
     * Visually similar to the classic Win95 'pinstripe'. */
    for (INT32_T y = 0; y < h; y += WIN95_CHROME_GRAIN_TILE_H) {
        for (INT32_T x = 0; x < w; x += WIN95_CHROME_GRAIN_TILE_W) {
            __rect(obj, x,     y,     1, 1, color_b);
            __rect(obj, x + 2, y + 2, 1, 1, color_b);
        }
    }
}
