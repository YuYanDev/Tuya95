/**
 * @file win95_cursor.c
 * @brief Touch-tracking cursor with 1px pixel-art black outline.
 *        The outline is computed algorithmically at init from the A8 source
 *        bitmaps; no separate hand-coded outline array is needed.
 *
 *        Layout inside the transparent container (hotspot = container TL):
 *          [outline]  @ (0,0)  — black A8 (w+2)×(h+2), drawn first (behind)
 *          [fill]     @ (1,1)  — white A8  w×h,         drawn second (front)
 */
#include "win95_cursor.h"
#include "lv_vendor.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Normal arrow bitmap — 8×13 A8 (alpha-only, stride=8)
 * --------------------------------------------------------------------------- */
STATIC CONST uint8_t s_cur_map[] = {
    /* row  0 */ 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* row  1 */ 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* row  2 */ 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* row  3 */ 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    /* row  4 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
    /* row  5 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
    /* row  6 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    /* row  7 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* row  8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
    /* row  9 */ 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0x00,
    /* row 10 */ 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00,
    /* row 11 */ 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    /* row 12 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

STATIC CONST lv_image_dsc_t s_cur_dsc = {
    .header = {
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = LV_COLOR_FORMAT_A8,
        .w      = 8,
        .h      = 13,
        .stride = 8,
    },
    .data_size = sizeof(s_cur_map),
    .data      = s_cur_map,
};

/* ---------------------------------------------------------------------------
 * Hourglass / busy bitmap — 16×16 A8
 * --------------------------------------------------------------------------- */
STATIC CONST uint8_t s_busy_map[] = {
    /* row  0 */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* row  1 */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* row  2 */ 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,
    /* row  3 */ 0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,
    /* row  4 */ 0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,
    /* row  5 */ 0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    /* row  6 */ 0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,
    /* row  7 */ 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,
    /* row  8 */ 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,
    /* row  9 */ 0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,
    /* row 10 */ 0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    /* row 11 */ 0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,
    /* row 12 */ 0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,
    /* row 13 */ 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,
    /* row 14 */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    /* row 15 */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};

STATIC CONST lv_image_dsc_t s_busy_dsc = {
    .header = {
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = LV_COLOR_FORMAT_A8,
        .w      = 16,
        .h      = 16,
        .stride = 16,
    },
    .data_size = sizeof(s_busy_map),
    .data      = s_busy_map,
};

/* ---------------------------------------------------------------------------
 * Computed outline buffers (A8, (w+2)×(h+2), computed once at init)
 * Arrow outline: 10×15 = 150 bytes
 * Busy outline:  18×18 = 324 bytes
 * --------------------------------------------------------------------------- */
STATIC uint8_t s_arr_out_buf[10 * 15];
STATIC uint8_t s_busy_out_buf[18 * 18];

STATIC lv_image_dsc_t s_arr_out_dsc;
STATIC lv_image_dsc_t s_busy_out_dsc;

/* ---------------------------------------------------------------------------
 * Compute a 1-pixel black outline mask from a source A8 bitmap.
 *   src_data : original A8 data (sw × sh, stride=sw)
 *   out      : output buffer ((sw+2)×(sh+2)), pre-zeroed
 *   ow,oh    : output dimensions (sw+2, sh+2)
 *
 * Algorithm (3-pass, no extra allocation):
 *   1. Copy src into out at (+1,+1) offset, marking white cells as 0xFE.
 *   2. For each non-0xFE cell adjacent (8-connected) to a 0xFE cell → 0xFF.
 *   3. Clear all 0xFE markers (interior covered by fill image on top).
 * --------------------------------------------------------------------------- */
STATIC VOID_T __compute_outline(CONST uint8_t *src_data, INT32_T sw, INT32_T sh,
                                 uint8_t *out, INT32_T ow, INT32_T oh)
{
    memset(out, 0, (size_t)(ow * oh));

    /* Pass 1: mark white pixels at (+1,+1) offset */
    for (INT32_T r = 0; r < sh; r++) {
        for (INT32_T c = 0; c < sw; c++) {
            if (src_data[r * sw + c]) {
                out[(r + 1) * ow + (c + 1)] = 0xFE;
            }
        }
    }

    /* Pass 2: expand 1px border — any transparent cell adj to 0xFE → 0xFF */
    for (INT32_T r = 0; r < oh; r++) {
        for (INT32_T c = 0; c < ow; c++) {
            if (out[r * ow + c] != 0) continue;   /* skip already-set */
            INT32_T found = 0;
            for (INT32_T dr = -1; dr <= 1 && !found; dr++) {
                for (INT32_T dc = -1; dc <= 1 && !found; dc++) {
                    INT32_T nr = r + dr, nc = c + dc;
                    if ((UINT32_T)nr < (UINT32_T)oh &&
                        (UINT32_T)nc < (UINT32_T)ow &&
                        out[nr * ow + nc] == 0xFE) {
                        found = 1;
                    }
                }
            }
            if (found) out[r * ow + c] = 0xFF;
        }
    }

    /* Pass 3: erase interior markers — fill image covers them */
    for (INT32_T i = 0; i < ow * oh; i++) {
        if (out[i] == 0xFE) out[i] = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *s_cont   = NULL;   /* container bound to indev (hotspot = TL) */
STATIC lv_obj_t *s_outline = NULL;  /* outline image — drawn first (behind) */
STATIC lv_obj_t *s_fill   = NULL;   /* white fill image — drawn second (front) */

/* ---------------------------------------------------------------------------
 * Apply a new bitmap source to both layers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __cursor_apply(CONST lv_image_dsc_t *fill_dsc,
                              CONST lv_image_dsc_t *out_dsc)
{
    /* outline: recoloured solid black */
    lv_image_set_src(s_outline, out_dsc);
    lv_obj_set_style_img_recolor(s_outline, lv_color_hex(0x000000), 0);
    lv_obj_set_style_img_recolor_opa(s_outline, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_outline, LV_OPA_COVER, 0);

    /* fill: recoloured white, sits 1 pixel inside the outline */
    lv_image_set_src(s_fill, fill_dsc);
    lv_obj_set_style_img_recolor(s_fill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_img_recolor_opa(s_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_fill, LV_OPA_COVER, 0);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_cursor_init(VOID_T)
{
    if (s_cont != NULL) return;

    /* ---- Compute outline bitmaps ---- */
    __compute_outline(s_cur_map,  8, 13, s_arr_out_buf,  10, 15);
    __compute_outline(s_busy_map, 16, 16, s_busy_out_buf, 18, 18);

    s_arr_out_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_arr_out_dsc.header.cf     = LV_COLOR_FORMAT_A8;
    s_arr_out_dsc.header.w      = 10;
    s_arr_out_dsc.header.h      = 15;
    s_arr_out_dsc.header.stride = 10;
    s_arr_out_dsc.data_size     = sizeof(s_arr_out_buf);
    s_arr_out_dsc.data          = s_arr_out_buf;

    s_busy_out_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_busy_out_dsc.header.cf     = LV_COLOR_FORMAT_A8;
    s_busy_out_dsc.header.w      = 18;
    s_busy_out_dsc.header.h      = 18;
    s_busy_out_dsc.header.stride = 18;
    s_busy_out_dsc.data_size     = sizeof(s_busy_out_buf);
    s_busy_out_dsc.data          = s_busy_out_buf;

    /* ---- Transparent container, 18×18, overflow visible ---- */
    s_cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cont);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_cont, 0, 0);
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_size(s_cont, 18, 18);
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Outline image — created first → rendered behind fill */
    s_outline = lv_image_create(s_cont);
    lv_obj_set_pos(s_outline, 0, 0);
    lv_obj_clear_flag(s_outline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_outline, LV_OBJ_FLAG_SCROLLABLE);

    /* Fill image — created second → rendered in front */
    s_fill = lv_image_create(s_cont);
    lv_obj_set_pos(s_fill, 1, 1);
    lv_obj_clear_flag(s_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_fill, LV_OBJ_FLAG_SCROLLABLE);

    __cursor_apply(&s_cur_dsc, &s_arr_out_dsc);

    /* Bind to ALL pointer indevs */
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_set_cursor(indev, s_cont);
        }
        indev = lv_indev_get_next(indev);
    }
}

VOID_T win95_cursor_set_visible(BOOL_T visible)
{
    if (s_cont == NULL) return;
    if (visible) {
        lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

VOID_T win95_cursor_set_busy(BOOL_T busy)
{
    if (s_cont == NULL) return;
    if (busy) {
        __cursor_apply(&s_busy_dsc, &s_busy_out_dsc);
    } else {
        __cursor_apply(&s_cur_dsc, &s_arr_out_dsc);
    }
}
