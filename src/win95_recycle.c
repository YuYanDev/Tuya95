/**
 * @file win95_recycle.c
 * @brief Win95 Recycle Bin - KV-backed deleted file with Restore / Empty
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_recycle.h"

#include "tal_api.h"
#include "tal_kv.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define RB_W        BIOS_SCREEN_WIDTH
#define RB_H        BIOS_SCREEN_HEIGHT
#define RB_TITLE_H  18
#define RB_BTN_H    32
#define RB_LIST_H   (RB_H - RB_TITLE_H - RB_BTN_H)   /* 270 */

/* KV key for Notepad text (shared with win95_notepad.c) */
#define KV_KEY_NP   "notepad_txt"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *file_lbl;
    lv_obj_t *restore_btn;
    lv_obj_t *empty_btn;
} RB_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC RB_CTX_T s_rb;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __rb_close(VOID_T);
STATIC VOID_T __rb_refresh(VOID_T);

/* ---------------------------------------------------------------------------
 * KV helpers
 * --------------------------------------------------------------------------- */
STATIC BOOL_T __rb_has_file(VOID_T)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    BOOL_T has = FALSE;
    if (tal_kv_get(KV_KEY_RB_NAME, &buf, &len) == OPRT_OK && buf && len > 0 && buf[0] != '\0') {
        has = TRUE;
    }
    if (buf) tal_kv_free(buf);
    return has;
}

STATIC VOID_T __rb_clear(VOID_T)
{
    /* Overwrite with single null byte to effectively clear the entry */
    CONST uint8_t empty = 0;
    tal_kv_set(KV_KEY_RB_NAME, &empty, 1);
    tal_kv_set(KV_KEY_RB_DATA, &empty, 1);
}

/* ---------------------------------------------------------------------------
 * Callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __rb_close_cb(lv_event_t *e) { (VOID_T)e; __rb_close(); }

STATIC VOID_T __rb_restore_cb(lv_event_t *e)
{
    (VOID_T)e;
    /* Read deleted data */
    uint8_t *data_buf = NULL;
    size_t data_len = 0;
    if (tal_kv_get(KV_KEY_RB_DATA, &data_buf, &data_len) == OPRT_OK
        && data_buf && data_len > 0 && data_buf[0] != '\0') {
        size_t cp = (data_len < RB_DATA_MAX) ? data_len : (size_t)RB_DATA_MAX;
        /* Restore to Notepad KV */
        tal_kv_set(KV_KEY_NP, data_buf, cp);
        tal_kv_free(data_buf);
    } else {
        if (data_buf) tal_kv_free(data_buf);
    }
    __rb_clear();
    __rb_refresh();
}

STATIC VOID_T __rb_empty_cb(lv_event_t *e)
{
    (VOID_T)e;
    __rb_clear();
    __rb_refresh();
}

/* ---------------------------------------------------------------------------
 * Refresh file list display
 * --------------------------------------------------------------------------- */
STATIC VOID_T __rb_refresh(VOID_T)
{
    if (s_rb.file_lbl == NULL) return;

    if (!__rb_has_file()) {
        lv_label_set_text(s_rb.file_lbl, "(Recycle Bin is empty)");
        if (s_rb.restore_btn) lv_obj_add_flag(s_rb.restore_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Read filename and data size for display */
    uint8_t *name_buf = NULL;
    size_t name_len = 0;
    uint8_t *data_buf = NULL;
    size_t data_len = 0;

    tal_kv_get(KV_KEY_RB_NAME, &name_buf, &name_len);
    tal_kv_get(KV_KEY_RB_DATA, &data_buf, &data_len);

    CHAR_T disp[128];
    CHAR_T name_str[64] = "Untitled.txt";
    if (name_buf && name_len > 0 && name_buf[0] != '\0') {
        size_t cp = (name_len < sizeof(name_str) - 1) ? name_len : sizeof(name_str) - 1;
        memcpy(name_str, name_buf, cp);
        name_str[cp] = '\0';
    }
    size_t sz = (data_buf && data_buf[0] != '\0') ? data_len : 0;
    snprintf(disp, sizeof(disp), "  %s\n  %u bytes", name_str, (unsigned)sz);

    lv_label_set_text(s_rb.file_lbl, disp);
    if (s_rb.restore_btn) lv_obj_clear_flag(s_rb.restore_btn, LV_OBJ_FLAG_HIDDEN);

    if (name_buf) tal_kv_free(name_buf);
    if (data_buf) tal_kv_free(data_buf);
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */
STATIC VOID_T __rb_close(VOID_T)
{
    if (s_rb.screen) {
        lv_obj_delete(s_rb.screen);
    }
    memset(&s_rb, 0, sizeof(RB_CTX_T));
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
OPERATE_RET win95_recycle_add(CONST CHAR_T *name, CONST CHAR_T *data)
{
    if (name == NULL || data == NULL) return OPRT_INVALID_PARM;
    size_t name_len = strlen(name);
    size_t data_len = strlen(data);
    if (data_len > RB_DATA_MAX) data_len = RB_DATA_MAX;
    OPERATE_RET rt = tal_kv_set(KV_KEY_RB_NAME, (CONST uint8_t *)name, name_len);
    if (rt != OPRT_OK) return rt;
    return tal_kv_set(KV_KEY_RB_DATA, (CONST uint8_t *)data, data_len);
}

VOID_T win95_recycle_open(VOID_T)
{
    if (s_rb.screen) {
        __rb_close();
    }
    memset(&s_rb, 0, sizeof(RB_CTX_T));

    s_rb.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_rb.screen);
    lv_obj_set_size(s_rb.screen, RB_W, RB_H);
    lv_obj_set_pos(s_rb.screen, 0, 0);
    lv_obj_set_style_bg_color(s_rb.screen, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_rb.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_rb.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_rb.screen);

    /* Title bar */
    lv_obj_t *tbar = lv_obj_create(s_rb.screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, RB_W - 4, RB_TITLE_H);
    lv_obj_set_pos(tbar, 2, 2);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(tbar);
    lv_obj_set_style_text_color(ttl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_unscii_8, 0);
    lv_label_set_text(ttl, "Recycle Bin");
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tbar);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_set_style_border_color(xb, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(xb, 1, 0);
    lv_obj_add_event_cb(xb, __rb_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);

    /* File list area */
    lv_obj_t *list_area = lv_obj_create(s_rb.screen);
    lv_obj_remove_style_all(list_area);
    lv_obj_set_size(list_area, RB_W - 8, RB_LIST_H - 4);
    lv_obj_set_pos(list_area, 4, RB_TITLE_H + 2);
    lv_obj_set_style_bg_color(list_area, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_bg_opa(list_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(list_area, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(list_area, 1, 0);
    lv_obj_clear_flag(list_area, LV_OBJ_FLAG_SCROLLABLE);

    s_rb.file_lbl = lv_label_create(list_area);
    lv_obj_set_style_text_color(s_rb.file_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_rb.file_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_pos(s_rb.file_lbl, 4, 8);
    lv_obj_set_width(s_rb.file_lbl, RB_W - 24);

    /* Button bar */
    lv_obj_t *bbar = lv_obj_create(s_rb.screen);
    lv_obj_remove_style_all(bbar);
    lv_obj_set_size(bbar, RB_W, RB_BTN_H);
    lv_obj_set_pos(bbar, 0, RB_H - RB_BTN_H);
    lv_obj_set_style_bg_color(bbar, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_bg_opa(bbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bbar, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(bbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bbar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_clear_flag(bbar, LV_OBJ_FLAG_SCROLLABLE);

    s_rb.restore_btn = lv_btn_create(bbar);
    lv_obj_set_size(s_rb.restore_btn, 80, 22);
    lv_obj_align(s_rb.restore_btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(s_rb.restore_btn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(s_rb.restore_btn, 0, 0);
    lv_obj_set_style_pad_all(s_rb.restore_btn, 0, 0);
    lv_obj_set_style_border_color(s_rb.restore_btn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(s_rb.restore_btn, 1, 0);
    lv_obj_add_event_cb(s_rb.restore_btn, __rb_restore_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_rb.restore_btn);
    lv_obj_set_style_text_color(rl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(rl, &lv_font_unscii_8, 0);
    lv_label_set_text(rl, "Restore");
    lv_obj_center(rl);

    s_rb.empty_btn = lv_btn_create(bbar);
    lv_obj_set_size(s_rb.empty_btn, 120, 22);
    lv_obj_align(s_rb.empty_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_rb.empty_btn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(s_rb.empty_btn, 0, 0);
    lv_obj_set_style_pad_all(s_rb.empty_btn, 0, 0);
    lv_obj_set_style_border_color(s_rb.empty_btn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(s_rb.empty_btn, 1, 0);
    lv_obj_add_event_cb(s_rb.empty_btn, __rb_empty_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(s_rb.empty_btn);
    lv_obj_set_style_text_color(el, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(el, &lv_font_unscii_8, 0);
    lv_label_set_text(el, "Empty Recycle Bin");
    lv_obj_center(el);

    __rb_refresh();
}
