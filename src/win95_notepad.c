/**
 * @file win95_notepad.c
 * @brief Win95 Notepad - full-screen text editor with KV persistence
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_notepad.h"
#include "win95_recycle.h"
#include "win95_kb.h"

#include "tal_api.h"
#include "tal_kv.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define NP_W          BIOS_SCREEN_WIDTH   /* 480 */
#define NP_H          BIOS_SCREEN_HEIGHT  /* 320 */
#define NP_TITLE_H    18
#define NP_MENU_H     16
#define NP_KB_H       130
#define NP_CONTENT_H  (NP_H - NP_TITLE_H - NP_MENU_H - NP_KB_H)   /* 156 */
#define NP_TXT_MAX    2048
#define KV_KEY_NP     "notepad_txt"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t  *screen;
    lv_obj_t  *textarea;
    lv_obj_t  *kb;
    lv_obj_t  *title_lbl;
    lv_obj_t  *menu_popup;
} NP_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC NP_CTX_T s_np;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __np_close(VOID_T);

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __np_raised(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_x(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 1, 0);
}

/* ---------------------------------------------------------------------------
 * KV operations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __np_save(VOID_T)
{
    if (s_np.textarea == NULL) {
        return;
    }
    CONST CHAR_T *txt = lv_textarea_get_text(s_np.textarea);
    if (txt == NULL) {
        txt = "";
    }
    size_t len = strlen(txt);
    if (len > NP_TXT_MAX) {
        len = NP_TXT_MAX;
    }
    OPERATE_RET rt = tal_kv_set(KV_KEY_NP, (CONST uint8_t *)txt, len);
    if (rt == OPRT_OK) {
        if (s_np.title_lbl) {
            lv_label_set_text(s_np.title_lbl, "Notepad - Untitled");
        }
        PR_DEBUG("Notepad saved %u bytes", (unsigned)len);
    } else {
        PR_ERR("Notepad save failed: %d", rt);
    }
}

STATIC VOID_T __np_new(VOID_T)
{
    if (s_np.textarea) {
        lv_textarea_set_text(s_np.textarea, "");
    }
    if (s_np.title_lbl) {
        lv_label_set_text(s_np.title_lbl, "Notepad - Untitled");
    }
}

STATIC VOID_T __np_load(VOID_T)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    if (tal_kv_get(KV_KEY_NP, &buf, &len) == OPRT_OK && buf && len > 0) {
        size_t cp = (len < NP_TXT_MAX) ? len : NP_TXT_MAX;
        CHAR_T *tmp = (CHAR_T *)tal_malloc(cp + 1);
        if (tmp) {
            memcpy(tmp, buf, cp);
            tmp[cp] = '\0';
            lv_textarea_set_text(s_np.textarea, tmp);
            tal_free(tmp);
        }
        tal_kv_free(buf);
    }
}

/* ---------------------------------------------------------------------------
 * Callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __np_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    __np_close();
}

STATIC VOID_T __np_menu_dismiss(lv_event_t *e)
{
    (VOID_T)e;
    if (s_np.menu_popup) {
        lv_obj_delete(s_np.menu_popup);
        s_np.menu_popup = NULL;
    }
}

STATIC VOID_T __np_menu_new_cb(lv_event_t *e)
{
    (VOID_T)e;
    __np_menu_dismiss(NULL);
    __np_new();
}

STATIC VOID_T __np_menu_save_cb(lv_event_t *e)
{
    (VOID_T)e;
    __np_menu_dismiss(NULL);
    __np_save();
}

STATIC VOID_T __np_menu_exit_cb(lv_event_t *e)
{
    (VOID_T)e;
    __np_close();
}

STATIC VOID_T __np_menu_delete_cb(lv_event_t *e)
{
    (VOID_T)e;
    __np_menu_dismiss(NULL);
    if (s_np.textarea == NULL) return;
    CONST CHAR_T *txt = lv_textarea_get_text(s_np.textarea);
    if (txt && txt[0] != '\0') {
        win95_recycle_add("Untitled.txt", txt);
        lv_textarea_set_text(s_np.textarea, "");
        if (s_np.title_lbl) {
            lv_label_set_text(s_np.title_lbl, "Notepad - Untitled");
        }
    }
}

STATIC VOID_T __np_ta_focus_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_np.kb) {
        win95_kb_set_textarea(s_np.kb, s_np.textarea);
        lv_obj_clear_flag(s_np.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

STATIC VOID_T __np_ta_defocus_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_np.kb) {
        lv_obj_add_flag(s_np.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* File button → show small dropdown popup */
STATIC VOID_T __np_file_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_np.menu_popup) {
        lv_obj_delete(s_np.menu_popup);
        s_np.menu_popup = NULL;
        return;
    }
    /* Popup appears below the menu bar */
    s_np.menu_popup = lv_obj_create(s_np.screen);
    lv_obj_remove_style_all(s_np.menu_popup);
    lv_obj_set_size(s_np.menu_popup, 88, 72);
    lv_obj_set_pos(s_np.menu_popup, 0, NP_TITLE_H + NP_MENU_H);
    lv_obj_set_style_bg_color(s_np.menu_popup, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_np.menu_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_np.menu_popup, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(s_np.menu_popup, 1, 0);
    lv_obj_set_style_radius(s_np.menu_popup, 0, 0);
    lv_obj_clear_flag(s_np.menu_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_np.menu_popup);

    CONST CHAR_T *items[] = {"New",  "Save", "Delete", "Exit"};
    lv_event_cb_t cbs[]   = {__np_menu_new_cb, __np_menu_save_cb,
                              __np_menu_delete_cb, __np_menu_exit_cb};
    for (INT32_T i = 0; i < 4; i++) {
        lv_obj_t *it = lv_obj_create(s_np.menu_popup);
        lv_obj_remove_style_all(it);
        lv_obj_set_size(it, 88, 18);
        lv_obj_set_pos(it, 0, i * 18);
        lv_obj_set_style_bg_opa(it, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(it, lv_color_hex(WIN95_COLOR_TITLEBAR), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_clear_flag(it, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = lv_label_create(it);
        lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(WIN95_COLOR_LIGHT), LV_STATE_PRESSED);
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_label_set_text(l, items[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_add_event_cb(it, cbs[i], LV_EVENT_CLICKED, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */
STATIC VOID_T __np_close(VOID_T)
{
    if (s_np.screen) {
        lv_obj_delete(s_np.screen);
    }
    memset(&s_np, 0, sizeof(NP_CTX_T));
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_notepad_open(VOID_T)
{
    if (s_np.screen) {
        __np_close();
    }
    memset(&s_np, 0, sizeof(NP_CTX_T));

    s_np.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_np.screen);
    lv_obj_set_size(s_np.screen, NP_W, NP_H);
    lv_obj_set_pos(s_np.screen, 0, 0);
    lv_obj_set_style_bg_color(s_np.screen, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_np.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_np.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_np.screen);

    /* Title bar */
    lv_obj_t *tbar = lv_obj_create(s_np.screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, NP_W - 4, NP_TITLE_H);
    lv_obj_set_pos(tbar, 2, 2);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    s_np.title_lbl = lv_label_create(tbar);
    lv_obj_set_style_text_color(s_np.title_lbl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(s_np.title_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_np.title_lbl, "Notepad - Untitled");
    lv_obj_align(s_np.title_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tbar);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    __np_raised(xb);
    lv_obj_add_event_cb(xb, __np_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);

    /* Menu bar */
    lv_obj_t *mbar = lv_obj_create(s_np.screen);
    lv_obj_remove_style_all(mbar);
    lv_obj_set_size(mbar, NP_W, NP_MENU_H);
    lv_obj_set_pos(mbar, 0, NP_TITLE_H);
    lv_obj_set_style_bg_color(mbar, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(mbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mbar, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(mbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(mbar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_clear_flag(mbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *file_btn = lv_obj_create(mbar);
    lv_obj_remove_style_all(file_btn);
    lv_obj_set_size(file_btn, 36, NP_MENU_H - 2);
    lv_obj_set_pos(file_btn, 2, 0);
    lv_obj_set_style_bg_opa(file_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(file_btn, lv_color_hex(WIN95_COLOR_TITLEBAR), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(file_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(file_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(file_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *fl = lv_label_create(file_btn);
    lv_obj_set_style_text_color(fl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(fl, lv_color_hex(WIN95_COLOR_LIGHT), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(fl, &lv_font_unscii_8, 0);
    lv_label_set_text(fl, "File");
    lv_obj_center(fl);
    lv_obj_add_event_cb(file_btn, __np_file_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *edit_btn = lv_obj_create(mbar);
    lv_obj_remove_style_all(edit_btn);
    lv_obj_set_size(edit_btn, 36, NP_MENU_H - 2);
    lv_obj_set_pos(edit_btn, 40, 0);
    lv_obj_set_style_bg_opa(edit_btn, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(edit_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *el = lv_label_create(edit_btn);
    lv_obj_set_style_text_color(el, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_text_font(el, &lv_font_unscii_8, 0);
    lv_label_set_text(el, "Edit");
    lv_obj_center(el);

    /* Text area */
    s_np.textarea = lv_textarea_create(s_np.screen);
    lv_obj_set_size(s_np.textarea, NP_W - 4, NP_CONTENT_H);
    lv_obj_set_pos(s_np.textarea, 2, NP_TITLE_H + NP_MENU_H);
    lv_obj_set_style_bg_color(s_np.textarea, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_color(s_np.textarea, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_np.textarea, &lv_font_unscii_8, 0);
    lv_obj_set_style_radius(s_np.textarea, 0, 0);
    lv_obj_set_style_border_color(s_np.textarea, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(s_np.textarea, 1, 0);
    lv_obj_set_style_pad_all(s_np.textarea, 2, 0);
    lv_obj_set_style_bg_color(s_np.textarea, lv_color_hex(0x000000), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_np.textarea,   LV_OPA_COVER,           LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(s_np.textarea,    2,                       LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_placeholder_text(s_np.textarea, "");
    lv_textarea_set_max_length(s_np.textarea, NP_TXT_MAX);
    lv_obj_add_event_cb(s_np.textarea, __np_ta_focus_cb,   LV_EVENT_FOCUSED,   NULL);
    lv_obj_add_event_cb(s_np.textarea, __np_ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    /* Keyboard */
    s_np.kb = win95_kb_create(s_np.screen);
    lv_obj_set_size(s_np.kb, NP_W, NP_KB_H);
    lv_obj_align(s_np.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    win95_kb_set_textarea(s_np.kb, s_np.textarea);
    lv_obj_set_style_bg_color(s_np.kb, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_text_color(s_np.kb, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_np.kb, LV_OBJ_FLAG_HIDDEN);

    /* Load saved text */
    __np_load();
}
