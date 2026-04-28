/**
 * @file win95_desktop.c
 * @brief Windows 95 style desktop (480x320 landscape, UNSCII pixel font)
 * @version 3.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_desktop.h"
#include "win95_dialup.h"
#include "win95_ie.h"
#include "win95_notepad.h"
#include "win95_ntp.h"
#include "win95_dos.h"
#include "win95_mypc.h"
#include "win95_recycle.h"
#include "win95_disk.h"
#include "win95_mine.h"
#include "win95_pipes.h"
#include "win95_net.h"
#include "win95_usb.h"
#include "win95_defrag.h"
#include "win95_illegal.h"
#include "win95_taskmgr.h"
#include "win95_spider.h"
#include "win95_winamp.h"
#include "win95_cursor.h"
#include "win95_logos.h"
#include "bios_ui.h"

#include "tal_api.h"
#include "tal_time_service.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define CLOCK_MS        1000
#define BOOT_MS         1500
#define W               BIOS_SCREEN_WIDTH
#define H               BIOS_SCREEN_HEIGHT
#define WIN_W           320
#define WIN_H           200
#define ICO_SZ          34
#define ICO_W           74
#define ICO_H           64
#define ICO_PAD_X       8
#define ICO_PAD_Y       6
#define ICO_BASE_X      8
#define ICO_BASE_Y      8

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *desktop;
    lv_obj_t *taskbar;
    lv_obj_t *start_btn;
    lv_obj_t *start_menu;
    lv_obj_t *clock_lbl;
    lv_obj_t *active_win;
    lv_obj_t *net_ico;
    lv_timer_t *clock_tmr;
    BOOL_T menu_open;
    BOOL_T ntp_requested;
    UINT32_T uptime;
} W95_CTX_T;

typedef enum {
    W95_ICON_MY_COMPUTER = 0,
    W95_ICON_NAVIGATOR,
    W95_ICON_NOTEPAD,
    W95_ICON_DIALUP,
    W95_ICON_RECYCLE,
    W95_ICON_DOS,
    W95_ICON_MINE,
    W95_ICON_NETWORK,
    W95_ICON_DEFRAG,
    W95_ICON_TASKMGR,
    W95_ICON_SPIDER,
    W95_ICON_WINAMP,
} W95_ICON_KIND_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC W95_CTX_T s_w95;

STATIC VOID_T __start_btn_refresh(VOID_T)
{
    if (!s_w95.start_btn) return;
    lv_image_set_src(s_w95.start_btn,
                     s_w95.menu_open ? &g_win95_start_button_open
                                     : &g_win95_start_button);
}

STATIC VOID_T __start_menu_hide(VOID_T)
{
    if (s_w95.start_menu) {
        lv_obj_add_flag(s_w95.start_menu, LV_OBJ_FLAG_HIDDEN);
    }
    s_w95.menu_open = FALSE;
    __start_btn_refresh();
}

STATIC VOID_T __start_menu_show(VOID_T)
{
    if (s_w95.start_menu) {
        lv_obj_clear_flag(s_w95.start_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_w95.start_menu);
    }
    s_w95.menu_open = TRUE;
    __start_btn_refresh();
}

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Win95 raised 3D border
 * @param[in] obj target
 * @return none
 */
STATIC VOID_T __raised(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_x(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 1, 0);
}

/**
 * @brief Win95 sunken border
 * @param[in] obj target
 * @return none
 */
STATIC VOID_T __sunken(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
}

STATIC lv_obj_t *__px(lv_obj_t *parent, INT32_T x, INT32_T y,
                      INT32_T w, INT32_T h, UINT32_T color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

STATIC VOID_T __stroke_rect(lv_obj_t *parent, INT32_T x, INT32_T y,
                            INT32_T w, INT32_T h, UINT32_T color)
{
    __px(parent, x, y, w, 1, color);
    __px(parent, x, y + h - 1, w, 1, color);
    __px(parent, x, y, 1, h, color);
    __px(parent, x + w - 1, y, 1, h, color);
}

STATIC CONST lv_image_dsc_t *__icon_src(W95_ICON_KIND_T kind)
{
    switch (kind) {
    case W95_ICON_MY_COMPUTER:
        return &g_win95_icon_mycomputer;
    case W95_ICON_NAVIGATOR:
        return &g_win95_icon_browser;
    case W95_ICON_NOTEPAD:
        return &g_win95_icon_notepad;
    case W95_ICON_DIALUP:
        return &g_win95_icon_dialup;
    case W95_ICON_RECYCLE:
        return &g_win95_icon_recycle;
    case W95_ICON_DOS:
        return &g_win95_icon_dos;
    case W95_ICON_MINE:
        return &g_win95_icon_mine;
    case W95_ICON_NETWORK:
        return &g_win95_icon_network;
    case W95_ICON_DEFRAG:
        return &g_win95_icon_defrag;
    case W95_ICON_TASKMGR:
        return &g_win95_icon_taskmgr;
    case W95_ICON_SPIDER:
        return &g_win95_icon_spider;
    case W95_ICON_WINAMP:
        return &g_win95_icon_winamp;
    default:
        return NULL;
    }
}

STATIC lv_obj_t *__icon(lv_obj_t *parent, W95_ICON_KIND_T kind,
                        CONST CHAR_T *label, INT32_T col, INT32_T row,
                        lv_event_cb_t cb)
{
    INT32_T x = ICO_BASE_X + col * (ICO_W + ICO_PAD_X);
    INT32_T y = ICO_BASE_Y + row * (ICO_H + ICO_PAD_Y);
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_t *art;
    lv_obj_t *lbl;
    CONST lv_image_dsc_t *src = __icon_src(kind);

    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, ICO_W, ICO_H);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);

    art = lv_image_create(c);
    lv_image_set_src(art, src);
    lv_obj_set_size(art, ICO_SZ, ICO_SZ);
    lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);

    lbl = lv_label_create(c);
    lv_obj_set_style_text_color(lbl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_obj_set_width(lbl, ICO_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, label);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, NULL);
    return c;
}

/**
 * @brief Close active window
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __close_win_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.active_win) {
        lv_obj_delete(s_w95.active_win);
        s_w95.active_win = NULL;
    }
}

/**
 * @brief Create a Win95 window (landscape sized)
 * @param[in] title window title
 * @param[in] body window content text
 * @return window object
 */
STATIC lv_obj_t *__open_win(CONST CHAR_T *title, CONST CHAR_T *body)
{
    if (s_w95.active_win) {
        lv_obj_delete(s_w95.active_win);
        s_w95.active_win = NULL;
    }

    lv_obj_t *w = lv_obj_create(s_w95.desktop);
    lv_obj_remove_style_all(w);
    lv_obj_set_size(w, WIN_W, WIN_H);
    lv_obj_align(w, LV_ALIGN_CENTER, 20, -16);
    lv_obj_set_style_bg_color(w, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(w, 0, 0);
    lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    __raised(w);
    lv_obj_move_foreground(w);

    lv_obj_t *tb = lv_obj_create(w);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, WIN_W - 4, WIN95_WINDOW_TITLE_H);
    lv_obj_set_pos(tb, 2, 2);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, title);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    __raised(xb);
    lv_obj_add_event_cb(xb, __close_win_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);

    lv_obj_t *ca = lv_obj_create(w);
    lv_obj_remove_style_all(ca);
    lv_obj_set_size(ca, WIN_W - 8, WIN_H - WIN95_WINDOW_TITLE_H - 8);
    lv_obj_set_pos(ca, 4, WIN95_WINDOW_TITLE_H + 4);
    lv_obj_set_style_bg_color(ca, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_bg_opa(ca, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ca, LV_OBJ_FLAG_SCROLLABLE);
    __sunken(ca);

    lv_obj_t *cl = lv_label_create(ca);
    lv_obj_set_style_text_color(cl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(cl, &lv_font_unscii_8, 0);
    lv_label_set_text(cl, body);
    lv_obj_set_pos(cl, 4, 4);
    lv_obj_set_width(cl, WIN_W - 20);

    s_w95.active_win = w;
    return w;
}

/**
 * @brief My Computer icon click
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __cb_pc(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_mypc_open();
}

/**
 * @brief Recycle Bin icon click
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __cb_trash(lv_event_t *e)
{
    (VOID_T)e;
    win95_recycle_open();
}

/**
 * @brief Notepad icon click
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __cb_notepad(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_notepad_open();
}

/**
 * @brief MS-DOS Prompt icon click
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __cb_dos(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_dos_open();
}

/**
 * @brief Dial-Up Networking icon click
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __cb_dialup(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_dialup_open();
}

/**
 * @brief Tuya Navigator icon click
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __cb_ie(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_ie_open();
}

/**
 * @brief Toggle start menu
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __cb_mine(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_mine_open(s_w95.desktop);
}

STATIC VOID_T __cb_net(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_net_open(s_w95.desktop);
}

STATIC VOID_T __cb_defrag(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_defrag_open(s_w95.desktop);
}

STATIC VOID_T __cb_taskmgr(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_taskmgr_open();
}

STATIC VOID_T __cb_spider(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_spider_open(s_w95.desktop);
}

STATIC VOID_T __cb_winamp(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.start_menu && s_w95.menu_open) {
        __start_menu_hide();
    }
    win95_winamp_open(s_w95.desktop);
}

STATIC VOID_T __start_btn_press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (!s_w95.start_btn) return;

    if (code == LV_EVENT_PRESSED) {
        lv_image_set_src(s_w95.start_btn, &g_win95_start_button_pressed);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        __start_btn_refresh();
    }
}

/**
 * @brief Toggle start menu
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __start_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.menu_open) {
        __start_menu_hide();
    } else {
        __start_menu_show();
    }
}

/**
 * @brief Shut Down - back to BIOS
 * @param[in] e LVGL event
 * @return none
 */
/**
 * @brief Delayed BIOS return after "safe to turn off" screen is shown
 */
STATIC VOID_T __safe_off_timer_cb(lv_timer_t *t)
{
    lv_obj_t *old_desktop = s_w95.desktop;

    (VOID_T)t;
    lv_timer_delete(t);
    bios_app_get_ctx()->state = APP_STATE_BIOS;
    memset(&s_w95, 0, sizeof(W95_CTX_T));
    bios_ui_init();
    if (old_desktop && old_desktop != lv_screen_active() && lv_obj_is_valid(old_desktop)) {
        lv_obj_delete(old_desktop);
    }
}

STATIC VOID_T __shutdown_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_w95.clock_tmr) {
        lv_timer_delete(s_w95.clock_tmr);
        s_w95.clock_tmr = NULL;
    }
    if (s_w95.start_menu) {
        lv_obj_add_flag(s_w95.start_menu, LV_OBJ_FLAG_HIDDEN);
    }

    /* Full-screen navy blue "safe to turn off" overlay */
    lv_obj_t *soff = lv_obj_create(s_w95.desktop);
    lv_obj_remove_style_all(soff);
    lv_obj_set_size(soff, W, H);
    lv_obj_set_pos(soff, 0, 0);
    lv_obj_set_style_bg_color(soff, lv_color_hex(0x000080), 0);
    lv_obj_set_style_bg_opa(soff, LV_OPA_COVER, 0);
    lv_obj_clear_flag(soff, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(soff);

    lv_obj_t *box = lv_obj_create(soff);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 320, 80);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x000080), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *line1 = lv_label_create(box);
    lv_obj_set_style_text_color(line1, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(line1, &lv_font_unscii_16, 0);
    lv_label_set_text(line1, "It is now safe to turn off");
    lv_obj_align(line1, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *line2 = lv_label_create(box);
    lv_obj_set_style_text_color(line2, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(line2, &lv_font_unscii_16, 0);
    lv_label_set_text(line2, "your computer.");
    lv_obj_align(line2, LV_ALIGN_CENTER, 0, 10);

    /* Auto-dismiss after 4 seconds and return to BIOS */
    lv_timer_create(__safe_off_timer_cb, 4000, NULL);
}

/**
 * @brief About handler
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __about_cb(lv_event_t *e)
{
    (VOID_T)e;
    __start_menu_hide();
    __open_win("About TuyaOS 95",
        "TuyaOS 95\n"
        "Version 4.0 (Build 950)\n"
        "----------------------------\n\n"
        "(c) 2026 Tuya Inc.\n"
        "All rights reserved.\n\n"
        "Licensed to: Tuya Developer\n"
        "Running on T5 AI Board");
}

/**
 * @brief Create a start menu item
 * @param[in] parent container
 * @param[in] text item label
 * @param[in] y vertical position
 * @param[in] cb click callback
 * @return item object
 */
STATIC lv_obj_t *__smenu_item(lv_obj_t *parent, CONST CHAR_T *text,
                               INT32_T y, lv_event_cb_t cb)
{
    lv_obj_t *it = lv_obj_create(parent);
    lv_obj_remove_style_all(it);
    lv_obj_set_size(it, WIN95_START_MENU_W - 22, 18);
    lv_obj_set_pos(it, 18, y);
    lv_obj_set_style_bg_opa(it, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(it, lv_color_hex(WIN95_COLOR_TITLEBAR), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(it, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(it);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(WIN95_COLOR_LIGHT), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);

    if (cb) {
        lv_obj_add_event_cb(it, cb, LV_EVENT_CLICKED, NULL);
    }
    return it;
}

/**
 * @brief Build start menu
 * @return none
 */
STATIC VOID_T __build_start_menu(VOID_T)
{
    INT32_T mh = 180;
    INT32_T my = H - WIN95_TASKBAR_H - mh;

    s_w95.start_menu = lv_obj_create(s_w95.desktop);
    lv_obj_remove_style_all(s_w95.start_menu);
    lv_obj_set_size(s_w95.start_menu, WIN95_START_MENU_W, mh);
    lv_obj_set_pos(s_w95.start_menu, 0, my);
    lv_obj_set_style_bg_color(s_w95.start_menu, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_w95.start_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_w95.start_menu, 0, 0);
    lv_obj_clear_flag(s_w95.start_menu, LV_OBJ_FLAG_SCROLLABLE);
    __raised(s_w95.start_menu);

    /* Blue stripe on left */
    lv_obj_t *str = lv_obj_create(s_w95.start_menu);
    lv_obj_remove_style_all(str);
    lv_obj_set_size(str, 16, mh - 4);
    lv_obj_set_pos(str, 2, 2);
    lv_obj_set_style_bg_color(str, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(str, LV_OPA_COVER, 0);
    lv_obj_clear_flag(str, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *st = lv_label_create(str);
    lv_obj_set_style_text_color(st, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(st, &lv_font_unscii_8, 0);
    lv_label_set_text(st, "T\nu\ny\na\n9\n5");
    lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -4);

    INT32_T iy = 4;
    __smenu_item(s_w95.start_menu, "Navigator",     iy, __cb_ie);       iy += 20;
    __smenu_item(s_w95.start_menu, "Notepad",      iy, __cb_notepad);  iy += 20;
    __smenu_item(s_w95.start_menu, "MS-DOS",       iy, __cb_dos);      iy += 20;
    __smenu_item(s_w95.start_menu, "My Computer",  iy, __cb_pc);       iy += 20;
    __smenu_item(s_w95.start_menu, "Dial-Up Net",  iy, __cb_dialup);   iy += 20;
    __smenu_item(s_w95.start_menu, "About",        iy, __about_cb);    iy += 20;

    lv_obj_t *sep = lv_obj_create(s_w95.start_menu);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, WIN95_START_MENU_W - 24, 1);
    lv_obj_set_pos(sep, 20, iy + 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    iy += 6;

    __smenu_item(s_w95.start_menu, "BIOS Setup",   iy, __shutdown_cb); iy += 20;
    __smenu_item(s_w95.start_menu, "Shut Down...", iy, __shutdown_cb);

    lv_obj_add_flag(s_w95.start_menu, LV_OBJ_FLAG_HIDDEN);
    s_w95.menu_open = FALSE;
}

/**
 * @brief Clock timer callback - also polls WiFi state for tray icon
 * @param[in] timer LVGL timer
 * @return none
 */
STATIC VOID_T __clock_cb(lv_timer_t *timer)
{
    (VOID_T)timer;
    s_w95.uptime++;

    BIOS_APP_CTX_T *app = bios_app_get_ctx();

    CHAR_T buf[8];
    if (win95_ntp_synced()) {
        POSIX_TM_S tm;
        tal_time_get(&tm);
        INT32_T total_min = (INT32_T)tm.tm_hour * 60 + (INT32_T)tm.tm_min
                          + app->tz_offset_minutes;
        if (total_min < 0) total_min += 1440;
        else if (total_min >= 1440) total_min -= 1440;
        snprintf(buf, sizeof(buf), "%2d:%02d", total_min / 60, total_min % 60);
    } else {
        UINT32_T h = (s_w95.uptime / 3600) % 24;
        UINT32_T m = (s_w95.uptime % 3600) / 60;
        snprintf(buf, sizeof(buf), "%2u:%02u", h, m);
    }
    if (s_w95.clock_lbl) {
        lv_label_set_text(s_w95.clock_lbl, buf);
    }
    win95_pipes_tick();  /* advance idle counter; starts screensaver when timeout reached */

    BOOL_T connected = (app->wifi_state == WIFI_ST_CONNECTED) ||
                       (app->pair_state == PAIR_ST_MQTT_CONNECTED);
    if (s_w95.net_ico) {
        if (connected) {
            lv_obj_clear_flag(s_w95.net_ico, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_w95.net_ico, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Trigger NTP sync every 60 s while online and not yet synced. */
    if (connected && !win95_ntp_synced() && (s_w95.uptime % 60 == 1)) {
        win95_ntp_trigger();
    }
}

/**
 * @brief Draw the network tray icon (single 12x12 ARGB sprite that already
 *        depicts a pair of connected PCs), upscaled 2x to 24x24 with
 *        nearest-neighbor for crisp Win95 pixel art.
 * @param[in] parent tray container
 * @param[in] x x position inside the tray (top-left corner of bounding box)
 * @param[in] y y position inside the tray (top-left corner of bounding box)
 * @return image object
 * @note We avoid LV_IMAGE_ALIGN_STRETCH because update_align is only fired
 *       on src / inner_align changes (not on lv_obj_set_size), which can
 *       lock the scale factor before the final size is known. Instead we
 *       set scale=2x explicitly and size the obj to match (nat * 2). Anti-
 *       aliasing is disabled so each source pixel maps to a hard 2x2 block.
 */
STATIC lv_obj_t *__mini_monitor(lv_obj_t *parent, INT32_T x, INT32_T y)
{
    lv_obj_t *mon = lv_image_create(parent);
    lv_obj_remove_style_all(mon);
    lv_image_set_src(mon, &g_win95_icon_traynet);
    lv_image_set_inner_align(mon, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_image_set_antialias(mon, false);
    lv_image_set_pivot(mon, 0, 0);
    lv_image_set_scale(mon, LV_SCALE_NONE * 2);
    INT32_T sw = (INT32_T)g_win95_icon_traynet.header.w * 2;
    INT32_T sh = (INT32_T)g_win95_icon_traynet.header.h * 2;
    lv_obj_set_size(mon, sw, sh);
    lv_obj_set_pos(mon, x, y);
    lv_obj_set_style_bg_opa(mon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mon, 0, 0);
    lv_obj_set_style_outline_width(mon, 0, 0);
    lv_obj_set_style_shadow_width(mon, 0, 0);
    lv_obj_set_style_pad_all(mon, 0, 0);
    lv_obj_clear_flag(mon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mon, LV_OBJ_FLAG_SCROLLABLE);
    return mon;
}

/**
 * @brief Build taskbar (480px wide landscape)
 * @return none
 */
STATIC VOID_T __build_taskbar(VOID_T)
{
    s_w95.taskbar = lv_obj_create(s_w95.desktop);
    lv_obj_remove_style_all(s_w95.taskbar);
    lv_obj_set_size(s_w95.taskbar, W, WIN95_TASKBAR_H);
    lv_obj_align(s_w95.taskbar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_w95.taskbar, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_bg_opa(s_w95.taskbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_w95.taskbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *edge = lv_obj_create(s_w95.taskbar);
    lv_obj_remove_style_all(edge);
    lv_obj_set_size(edge, W, 2);
    lv_obj_set_pos(edge, 0, 0);
    lv_obj_set_style_bg_color(edge, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(edge, LV_OBJ_FLAG_SCROLLABLE);

    /* Start button */
    s_w95.start_btn = lv_image_create(s_w95.taskbar);
    lv_image_set_src(s_w95.start_btn, &g_win95_start_button);
    lv_obj_set_size(s_w95.start_btn, g_win95_start_button.header.w, g_win95_start_button.header.h);
    lv_obj_align(s_w95.start_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(s_w95.start_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_w95.start_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_w95.start_btn, __start_btn_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_w95.start_btn, __start_btn_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_w95.start_btn, __start_btn_press_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_w95.start_btn, __start_cb, LV_EVENT_CLICKED, NULL);

    /* System tray area (sized to fit a 24x24 net icon + clock; tray wraps
     * the full height of the taskbar's content area, leaving 1px for the
     * taskbar bevel on top and bottom) */
    lv_obj_t *tray = lv_obj_create(s_w95.taskbar);
    lv_obj_remove_style_all(tray);
    lv_obj_set_size(tray, 74, 26);
    lv_obj_align(tray, LV_ALIGN_RIGHT_MID, -3, 0);
    lv_obj_set_style_bg_color(tray, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_bg_opa(tray, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tray, LV_OBJ_FLAG_SCROLLABLE);
    __sunken(tray);

    /* Network indicator (24x24 = 2x of the 12x12 sprite, placed in the
     * tray content area inset by the 1px sunken border) */
    s_w95.net_ico = __mini_monitor(tray, 1, 0);
    lv_obj_add_flag(s_w95.net_ico, LV_OBJ_FLAG_HIDDEN);

    /* Clock label */
    s_w95.clock_lbl = lv_label_create(tray);
    lv_obj_set_style_text_color(s_w95.clock_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_w95.clock_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_w95.clock_lbl, " 0:00");
    lv_obj_align(s_w95.clock_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

    s_w95.uptime = 0;
    s_w95.clock_tmr = lv_timer_create(__clock_cb, CLOCK_MS, NULL);
}

/**
 * @brief Boot done - build desktop content
 * @param[in] timer LVGL timer
 * @return none
 */
STATIC VOID_T __boot_done(lv_timer_t *timer)
{
    lv_obj_t *bs = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (bs) {
        lv_obj_delete(bs);
    }
    lv_timer_delete(timer);

    lv_obj_set_style_bg_color(s_w95.desktop, lv_color_hex(WIN95_COLOR_DESKTOP), 0);

    __icon(s_w95.desktop, W95_ICON_MY_COMPUTER, "My Computer",           0, 0, __cb_pc);
    __icon(s_w95.desktop, W95_ICON_NAVIGATOR,   "Tuya Navigator",        0, 1, __cb_ie);
    __icon(s_w95.desktop, W95_ICON_NETWORK,     "Network Neighborhood",  0, 2, __cb_net);
    __icon(s_w95.desktop, W95_ICON_RECYCLE,     "Recycle Bin",           0, 3, __cb_trash);
    __icon(s_w95.desktop, W95_ICON_NOTEPAD,     "Notepad",               1, 0, __cb_notepad);
    __icon(s_w95.desktop, W95_ICON_DIALUP,      "Dial-Up Networking",    1, 1, __cb_dialup);
    __icon(s_w95.desktop, W95_ICON_DOS,         "MS-DOS Prompt",         1, 2, __cb_dos);
    __icon(s_w95.desktop, W95_ICON_TASKMGR,     "Task Manager",          1, 3, __cb_taskmgr);
    __icon(s_w95.desktop, W95_ICON_MINE,        "Minesweeper",           2, 0, __cb_mine);
    __icon(s_w95.desktop, W95_ICON_SPIDER,      "Spider Solitaire",      2, 1, __cb_spider);
    __icon(s_w95.desktop, W95_ICON_DEFRAG,      "Disk Defragmenter",     2, 2, __cb_defrag);
    __icon(s_w95.desktop, W95_ICON_WINAMP,      "Winamp",                2, 3, __cb_winamp);

    __build_taskbar();
    __build_start_menu();
    win95_disk_init();   /* deferred SDIO init: P10/P11 boot pins now free */
    win95_usb_init();    /* USB HID host: keyboard + mouse */
    win95_cursor_set_visible(TRUE);

    /* Load saved screensaver timeout */
    {
        UINT8_T kv_buf[16] = {0};
        UINT32_T kv_len = sizeof(kv_buf) - 1;
        if (tal_kv_get("ss_timeout", kv_buf, &kv_len) == OPRT_OK) {
            UINT32_T minutes = (UINT32_T)atoi((CHAR_T *)kv_buf);
            win95_pipes_set_timeout(minutes);
        }
    }
}

/**
 * @brief Show boot splash (landscape)
 * @return none
 */
STATIC VOID_T __boot_splash(VOID_T)
{
    lv_obj_t *bs = lv_obj_create(s_w95.desktop);
    lv_obj_remove_style_all(bs);
    lv_obj_set_size(bs, W, H);
    lv_obj_set_style_bg_color(bs, lv_color_hex(0x000080), 0);
    lv_obj_set_style_bg_opa(bs, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bs, 0, 0);

    lv_obj_t *logo = lv_image_create(bs);
    lv_image_set_src(logo, &g_win95_boot_logo);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -28);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_bar_create(bs);
    lv_obj_set_size(bar, 240, 8);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 54);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000055), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(WIN95_COLOR_LIGHT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_bar_set_value(bar, 100, LV_ANIM_ON);

    lv_timer_create(__boot_done, BOOT_MS, bs);
}

/**
 * @brief Initialize and show Win95 desktop
 * @return none
 */
VOID_T win95_desktop_init(VOID_T)
{
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    lv_obj_t *old_root = lv_screen_active();

    app->state = APP_STATE_DESKTOP;

    memset(&s_w95, 0, sizeof(W95_CTX_T));

    s_w95.desktop = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_w95.desktop);
    lv_obj_set_size(s_w95.desktop, W, H);
    lv_obj_set_style_bg_color(s_w95.desktop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_w95.desktop, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_w95.desktop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_w95.desktop, 0, 0);
    lv_screen_load(s_w95.desktop);

    if (old_root && old_root != s_w95.desktop && lv_obj_is_valid(old_root)) {
        lv_obj_delete(old_root);
    }

    win95_cursor_set_visible(FALSE);
    __boot_splash();
}

/**
 * @brief Update taskbar network indicator visibility
 * @param[in] connected TRUE to show, FALSE to hide
 * @return none
 */
VOID_T win95_taskbar_set_net(BOOL_T connected)
{
    if (s_w95.net_ico == NULL) {
        return;
    }
    if (connected) {
        lv_obj_clear_flag(s_w95.net_ico, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_w95.net_ico, LV_OBJ_FLAG_HIDDEN);
    }
}
