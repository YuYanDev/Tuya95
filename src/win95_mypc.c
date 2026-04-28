/**
 * @file win95_mypc.c
 * @brief Win95 My Computer — 3-tab window: General, Disk, Time Zone
 * @version 2.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_mypc.h"
#include "win95_pairing.h"
#include "win95_disk.h"
#include "win95_pipes.h"
#include "win95_logos.h"

#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define MYPC_W        BIOS_SCREEN_WIDTH     /* 480 */
#define MYPC_H        BIOS_SCREEN_HEIGHT    /* 320 */
#define MYPC_TITLE_H  18
#define MYPC_TABBAR_H 20
#define MYPC_PANEL_Y  (MYPC_TITLE_H + 4 + MYPC_TABBAR_H)
#define MYPC_PANEL_H  (MYPC_H - MYPC_PANEL_Y - 4)
#define MYPC_PANEL_W  (MYPC_W - 8)

#define TAB_GENERAL  0
#define TAB_DISK     1
#define TAB_TZ       2
#define TAB_SCREEN   3
#define TAB_COUNT    4

/* ---------------------------------------------------------------------------
 * Timezone table
 * --------------------------------------------------------------------------- */
typedef struct { CONST CHAR_T *label; INT32_T offset_min; } TZ_ENTRY_T;

STATIC CONST TZ_ENTRY_T s_tz_table[] = {
    {"UTC-12 / Etc/GMT+12",          -720},
    {"UTC-11 / Pacific/Midway",      -660},
    {"UTC-10 / Pacific/Honolulu",    -600},
    {"UTC-9  / America/Anchorage",   -540},
    {"UTC-8  / America/LA",          -480},
    {"UTC-7  / America/Denver",      -420},
    {"UTC-6  / America/Chicago",     -360},
    {"UTC-5  / America/New_York",    -300},
    {"UTC-4  / America/Halifax",     -240},
    {"UTC-3:30 / America/St_Johns",  -210},
    {"UTC-3  / America/Sao_Paulo",   -180},
    {"UTC-2  / Etc/GMT+2",           -120},
    {"UTC-1  / Atlantic/Azores",      -60},
    {"UTC+0  / UTC",                    0},
    {"UTC+1  / Europe/London",         60},
    {"UTC+2  / Europe/Paris",         120},
    {"UTC+3  / Europe/Moscow",        180},
    {"UTC+3:30 / Asia/Tehran",        210},
    {"UTC+4  / Asia/Dubai",           240},
    {"UTC+4:30 / Asia/Kabul",         270},
    {"UTC+5  / Asia/Karachi",         300},
    {"UTC+5:30 / Asia/Kolkata",       330},
    {"UTC+5:45 / Asia/Kathmandu",     345},
    {"UTC+6  / Asia/Dhaka",           360},
    {"UTC+6:30 / Asia/Yangon",        390},
    {"UTC+7  / Asia/Bangkok",         420},
    {"UTC+8  / Asia/Shanghai",        480},
    {"UTC+9  / Asia/Tokyo",           540},
    {"UTC+9:30 / Australia/Darwin",   570},
    {"UTC+10 / Australia/Sydney",     600},
    {"UTC+11 / Pacific/Noumea",       660},
    {"UTC+12 / Pacific/Auckland",     720},
    {"UTC+14 / Pacific/Kiritimati",   840},
};
#define TZ_COUNT ((UINT32_T)(sizeof(s_tz_table) / sizeof(s_tz_table[0])))

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *tab_btns[TAB_COUNT];
    lv_obj_t *panels[TAB_COUNT];
    INT32_T   active_tab;

    /* Disk tab */
    CHAR_T    disk_path[256];
    CHAR_T    disk_sel[WIN95_DISK_NAME_MAX];
    lv_obj_t *disk_path_lbl;
    lv_obj_t *disk_size_lbl;
    lv_obj_t *disk_file_list;
    lv_obj_t *disk_newdlg;    /* NULL or active new-file modal */

    /* Time Zone tab */
    lv_obj_t *tz_list;
    INT32_T   tz_sel_idx;
    lv_obj_t *tz_status_lbl;

    /* Screen / Screensaver tab */
    lv_obj_t *ss_list;
    INT32_T   ss_sel_idx;
    lv_obj_t *ss_preview_host;
    lv_obj_t *ss_status_lbl;
} MYPC_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope state
 * --------------------------------------------------------------------------- */
STATIC MYPC_CTX_T s_mypc;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mypc_close(VOID_T);
STATIC VOID_T __disk_refresh(VOID_T);
STATIC VOID_T __ss_preview_refresh(VOID_T);

/* ---------------------------------------------------------------------------
 * Win95-style button helper
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *__w95_btn(lv_obj_t *parent, INT32_T x, INT32_T y,
                             INT32_T w, INT32_T h,
                             CONST CHAR_T *lbl_txt, lv_event_cb_t cb,
                             VOID_T *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 1, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(WIN95_COLOR_LIGHT),  0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(WIN95_COLOR_SHADOW),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, lbl_txt);
    lv_obj_center(l);
    return btn;
}

/* ---------------------------------------------------------------------------
 * Tab switching
 * --------------------------------------------------------------------------- */
STATIC VOID_T __switch_tab(INT32_T idx)
{
    if (s_mypc.active_tab == TAB_SCREEN && idx != TAB_SCREEN) {
        win95_pipes_preview_stop();
    }

    s_mypc.active_tab = idx;
    for (INT32_T i = 0; i < TAB_COUNT; i++) {
        if (s_mypc.panels[i]) {
            if (i == idx) lv_obj_clear_flag(s_mypc.panels[i], LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(s_mypc.panels[i],   LV_OBJ_FLAG_HIDDEN);
        }
        if (s_mypc.tab_btns[i]) {
            UINT32_T bg = (i == idx) ? WIN95_COLOR_TITLEBAR : WIN95_COLOR_WINDOW;
            UINT32_T fg = (i == idx) ? WIN95_COLOR_LIGHT     : 0x000000;
            lv_obj_set_style_bg_color(s_mypc.tab_btns[i], lv_color_hex(bg), 0);
            lv_obj_t *lbl = lv_obj_get_child(s_mypc.tab_btns[i], 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
        }
    }
    if (idx == TAB_DISK) __disk_refresh();
    if (idx == TAB_SCREEN) __ss_preview_refresh();
}

STATIC VOID_T __tab0_cb(lv_event_t *e) { (VOID_T)e; __switch_tab(TAB_GENERAL); }
STATIC VOID_T __tab1_cb(lv_event_t *e) { (VOID_T)e; __switch_tab(TAB_DISK); }
STATIC VOID_T __tab2_cb(lv_event_t *e) { (VOID_T)e; __switch_tab(TAB_TZ); }
STATIC VOID_T __tab3_cb(lv_event_t *e) { (VOID_T)e; __switch_tab(TAB_SCREEN); }

/* ---------------------------------------------------------------------------
 * Disk tab helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __disk_entry_cb(lv_event_t *e)
{
    VOID_T *ud = lv_event_get_user_data(e);
    if (!ud) return;
    CHAR_T *info = (CHAR_T *)ud;  /* format: "0|name" or "1|name" (0=file,1=dir) */
    if (info[0] == '1') {
        /* Directory: navigate into it */
        UINT32_T plen = (UINT32_T)strlen(s_mypc.disk_path);
        CONST CHAR_T *dname = info + 2;
        if (plen + strlen(dname) + 2 < sizeof(s_mypc.disk_path)) {
            if (s_mypc.disk_path[plen - 1] != '/') {
                s_mypc.disk_path[plen] = '/';
                s_mypc.disk_path[plen + 1] = '\0';
            }
            strncat(s_mypc.disk_path, dname,
                    sizeof(s_mypc.disk_path) - strlen(s_mypc.disk_path) - 1);
        }
        s_mypc.disk_sel[0] = '\0';
        __disk_refresh();
    } else {
        /* File: select it */
        strncpy(s_mypc.disk_sel, info + 2, WIN95_DISK_NAME_MAX - 1);
        s_mypc.disk_sel[WIN95_DISK_NAME_MAX - 1] = '\0';
    }
}

STATIC VOID_T __disk_up_cb(lv_event_t *e)
{
    (VOID_T)e;
    /* Strip last path component */
    UINT32_T len = (UINT32_T)strlen(s_mypc.disk_path);
    if (len <= strlen(WIN95_DISK_MOUNT) + 1) {
        /* Already at root */
        strncpy(s_mypc.disk_path, WIN95_DISK_MOUNT, sizeof(s_mypc.disk_path));
        s_mypc.disk_path[sizeof(s_mypc.disk_path) - 1] = '\0';
    } else {
        /* Remove trailing slash first if present */
        if (s_mypc.disk_path[len - 1] == '/') { s_mypc.disk_path[--len] = '\0'; }
        CHAR_T *last = strrchr(s_mypc.disk_path, '/');
        if (last && last > s_mypc.disk_path) {
            *last = '\0';
        }
    }
    s_mypc.disk_sel[0] = '\0';
    __disk_refresh();
}

STATIC VOID_T __disk_newdlg_cancel_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_mypc.disk_newdlg) {
        lv_obj_delete(s_mypc.disk_newdlg);
        s_mypc.disk_newdlg = NULL;
    }
}

STATIC VOID_T __disk_newdlg_ok_cb(lv_event_t *e)
{
    lv_obj_t *dlg = (lv_obj_t *)lv_event_get_user_data(e);
    if (!dlg) return;
    lv_obj_t *ta = lv_obj_get_child(dlg, 1);  /* second child: textarea */
    if (!ta) { lv_obj_delete(dlg); s_mypc.disk_newdlg = NULL; return; }
    CONST CHAR_T *fname = lv_textarea_get_text(ta);
    if (fname && fname[0]) {
        CHAR_T full[320];
        snprintf(full, sizeof(full), "%s/%s", s_mypc.disk_path, fname);
        win95_disk_create_file(full);
    }
    lv_obj_delete(dlg);
    s_mypc.disk_newdlg = NULL;
    __disk_refresh();
}

STATIC VOID_T __disk_new_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!win95_disk_is_mounted()) return;
    if (s_mypc.disk_newdlg) return;  /* already open */

    lv_obj_t *dlg = lv_obj_create(s_mypc.panels[TAB_DISK]);
    lv_obj_set_size(dlg, 280, 80);
    lv_obj_align(dlg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dlg);
    s_mypc.disk_newdlg = dlg;

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_color(title, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(title, &lv_font_unscii_8, 0);
    lv_label_set_text(title, "New file name:");
    lv_obj_set_pos(title, 4, 4);

    lv_obj_t *ta = lv_textarea_create(dlg);
    lv_obj_set_size(ta, 260, 22);
    lv_obj_set_pos(ta, 4, 18);
    lv_obj_set_style_text_font(ta, &lv_font_unscii_8, 0);
    lv_textarea_set_one_line(ta, TRUE);
    lv_textarea_set_max_length(ta, 60);

    __w95_btn(dlg, 140, 52, 60, 20, "OK",     __disk_newdlg_ok_cb,     dlg);
    __w95_btn(dlg,  72, 52, 60, 20, "Cancel", __disk_newdlg_cancel_cb, NULL);
}

STATIC VOID_T __disk_del_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!win95_disk_is_mounted() || s_mypc.disk_sel[0] == '\0') return;
    CHAR_T full[320];
    snprintf(full, sizeof(full), "%s/%s", s_mypc.disk_path, s_mypc.disk_sel);
    win95_disk_delete(full);
    s_mypc.disk_sel[0] = '\0';
    __disk_refresh();
}

STATIC VOID_T __disk_refresh(VOID_T)
{
    if (!s_mypc.disk_path_lbl || !s_mypc.disk_file_list) return;

    /* Update path label */
    lv_label_set_text(s_mypc.disk_path_lbl, s_mypc.disk_path);

    /* Update size label */
    if (s_mypc.disk_size_lbl) {
        if (win95_disk_is_mounted()) {
            UINT32_T tot = 0, fr = 0;
            win95_disk_get_info(&tot, &fr);
            CHAR_T sbuf[48];
            if (tot > 0) {
                snprintf(sbuf, sizeof(sbuf), "Total:%uMB Free:%uMB", (unsigned)tot, (unsigned)fr);
            } else {
                snprintf(sbuf, sizeof(sbuf), "Mounted");
            }
            lv_label_set_text(s_mypc.disk_size_lbl, sbuf);
        } else {
            lv_label_set_text(s_mypc.disk_size_lbl, "No card");
        }
    }

    /* Clear file list */
    lv_obj_clean(s_mypc.disk_file_list);

    if (!win95_disk_is_mounted()) {
        lv_obj_t *l = lv_label_create(s_mypc.disk_file_list);
        lv_obj_set_style_text_color(l, lv_color_hex(WIN95_COLOR_SHADOW), 0);
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_label_set_text(l, "SD card not mounted");
        return;
    }

    CHAR_T names[WIN95_DISK_LIST_MAX][WIN95_DISK_NAME_MAX];
    UINT8_T is_dir[WIN95_DISK_LIST_MAX];
    UINT32_T cnt = win95_disk_list_dir(s_mypc.disk_path, names, is_dir, WIN95_DISK_LIST_MAX);

    if (cnt == 0) {
        lv_obj_t *l = lv_label_create(s_mypc.disk_file_list);
        lv_obj_set_style_text_color(l, lv_color_hex(WIN95_COLOR_SHADOW), 0);
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_label_set_text(l, "(empty)");
        return;
    }

    for (UINT32_T i = 0; i < cnt; i++) {
        CHAR_T row_text[WIN95_DISK_NAME_MAX + 8];
        snprintf(row_text, sizeof(row_text), "%s %s",
                 is_dir[i] ? "[D]" : "[F]", names[i]);

        lv_obj_t *row = lv_obj_create(s_mypc.disk_file_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, MYPC_PANEL_W - 4, 14);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_label_set_text(lbl, row_text);
        lv_obj_set_pos(lbl, 2, 2);

        /* Build user_data string: "0|name" or "1|name" */
        UINT32_T info_len = 2 + (UINT32_T)strlen(names[i]) + 1;
        CHAR_T *info = (CHAR_T *)lv_malloc(info_len);
        if (info) {
            info[0] = is_dir[i] ? '1' : '0';
            info[1] = '|';
            strncpy(info + 2, names[i], info_len - 2);
            info[info_len - 1] = '\0';
            lv_obj_add_event_cb(row, __disk_entry_cb, LV_EVENT_CLICKED, info);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Time Zone tab helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __tz_apply_cb(lv_event_t *e)
{
    (VOID_T)e;
    UINT32_T idx = (UINT32_T)s_mypc.tz_sel_idx;
    if (idx >= TZ_COUNT) return;
    INT32_T minutes = s_tz_table[idx].offset_min;
    bios_app_get_ctx()->tz_offset_minutes = minutes;
    win95_pairing_save_tz(minutes);
    if (s_mypc.tz_status_lbl) lv_label_set_text(s_mypc.tz_status_lbl, "Applied.");
}

/* ---------------------------------------------------------------------------
 * Build panel: General
 * --------------------------------------------------------------------------- */
STATIC VOID_T __build_general(lv_obj_t *panel)
{
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    CHAR_T reg_line[96];
    CHAR_T net_line[64];

    snprintf(reg_line, sizeof(reg_line), "%.18s",
             app->uuid[0] ? app->uuid : "00000-OEM-0000000-00000");
    snprintf(net_line, sizeof(net_line), "%s",
             app->wifi_ssid[0] ? app->wifi_ssid : "Tuya Developer");
    lv_obj_t *ico = lv_image_create(panel);
    lv_image_set_src(ico, &g_win95_sysprops_monitor);
    lv_obj_set_pos(ico, 28, 34);
    lv_obj_clear_flag(ico, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ico, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "System:");
    lv_obj_set_pos(l, 206, 28);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "Microsoft TuyaOS 95");
    lv_obj_set_pos(l, 232, 48);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "4.00.950 B");
    lv_obj_set_pos(l, 232, 68);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "Chromium 124 Shell");
    lv_obj_set_pos(l, 232, 88);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "Registered to:");
    lv_obj_set_pos(l, 206, 122);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, net_line);
    lv_obj_set_pos(l, 232, 142);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, reg_line);
    lv_obj_set_pos(l, 232, 162);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "Computer:");
    lv_obj_set_pos(l, 206, 204);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "BK7236");
    lv_obj_set_pos(l, 232, 224);

    l = lv_label_create(panel);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, "8.0MB RAM");
    lv_obj_set_pos(l, 232, 244);
}

/* ---------------------------------------------------------------------------
 * Build panel: Disk
 * --------------------------------------------------------------------------- */
STATIC VOID_T __build_disk(lv_obj_t *panel)
{
    /* Row 1: path label */
    s_mypc.disk_path_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(s_mypc.disk_path_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_mypc.disk_path_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_pos(s_mypc.disk_path_lbl, 4, 4);
    lv_obj_set_width(s_mypc.disk_path_lbl, MYPC_PANEL_W - 100);
    lv_label_set_text(s_mypc.disk_path_lbl, s_mypc.disk_path);

    /* Buttons: Up, New, Del */
    INT32_T bx = MYPC_PANEL_W - 94;
    __w95_btn(panel, bx,     2, 28, 16, "Up",  __disk_up_cb,  NULL);
    __w95_btn(panel, bx + 30, 2, 30, 16, "New", __disk_new_cb, NULL);
    __w95_btn(panel, bx + 62, 2, 28, 16, "Del", __disk_del_cb, NULL);

    /* Row 2: size info */
    s_mypc.disk_size_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(s_mypc.disk_size_lbl, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_text_font(s_mypc.disk_size_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_pos(s_mypc.disk_size_lbl, 4, 22);
    lv_obj_set_width(s_mypc.disk_size_lbl, MYPC_PANEL_W - 8);
    lv_label_set_text(s_mypc.disk_size_lbl, win95_disk_is_mounted() ? "Mounted" : "No card");

    /* Separator */
    lv_obj_t *sep = lv_obj_create(panel);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, MYPC_PANEL_W - 4, 1);
    lv_obj_set_pos(sep, 2, 36);
    lv_obj_set_style_bg_color(sep, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    /* Scrollable file list container */
    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_set_size(list, MYPC_PANEL_W - 2, MYPC_PANEL_H - 40);
    lv_obj_set_pos(list, 2, 40);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_mypc.disk_file_list = list;
}

/* ---------------------------------------------------------------------------
 * Win95 scrollable list helpers
 * --------------------------------------------------------------------------- */
/* Refresh row highlight colours in a list based on selected index */
STATIC VOID_T __list_refresh_sel(lv_obj_t *list, INT32_T sel_idx)
{
    if (!list) return;
    UINT32_T n = (UINT32_T)lv_obj_get_child_count(list);
    for (UINT32_T i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(list, (INT32_T)i);
        if (!row) continue;
        BOOL_T selected = ((INT32_T)i == sel_idx);
        lv_obj_set_style_bg_color(row, lv_color_hex(
            selected ? WIN95_COLOR_TITLEBAR : 0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(
                selected ? WIN95_COLOR_LIGHT : 0x000000), 0);
        }
    }
}

/* Create a Win95-style scrollable list container */
STATIC lv_obj_t *__list_create(lv_obj_t *parent,
                                INT32_T x, INT32_T y,
                                INT32_T w, INT32_T h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(c, 0, 0);
    return c;
}

/* TZ list row clicked */
STATIC VOID_T __tz_item_cb(lv_event_t *e)
{
    INT32_T idx = (INT32_T)(intptr_t)lv_event_get_user_data(e);
    s_mypc.tz_sel_idx = idx;
    __list_refresh_sel(s_mypc.tz_list, idx);
}

/* SS list row clicked */
STATIC VOID_T __ss_item_cb(lv_event_t *e)
{
    INT32_T idx = (INT32_T)(intptr_t)lv_event_get_user_data(e);
    s_mypc.ss_sel_idx = idx;
    __list_refresh_sel(s_mypc.ss_list, idx);
}

/* ---------------------------------------------------------------------------
 * Build panel: Time Zone
 * --------------------------------------------------------------------------- */
STATIC VOID_T __build_tz(lv_obj_t *panel)
{
    /* Find initial selection */
    INT32_T cur_min = bios_app_get_ctx()->tz_offset_minutes;
    INT32_T sel = 13;  /* default UTC+0 */
    for (UINT32_T i = 0; i < TZ_COUNT; i++) {
        if (s_tz_table[i].offset_min == cur_min) { sel = (INT32_T)i; break; }
    }
    s_mypc.tz_sel_idx = sel;

    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(lbl, "Time Zone:");
    lv_obj_set_pos(lbl, 4, 4);

    INT32_T list_h = MYPC_PANEL_H - 40;
    s_mypc.tz_list = __list_create(panel, 4, 16, MYPC_PANEL_W - 8, list_h);

    for (UINT32_T i = 0; i < TZ_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_mypc.tz_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, MYPC_PANEL_W - 24, 14);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *rl = lv_label_create(row);
        lv_obj_set_style_text_font(rl, &lv_font_unscii_8, 0);
        lv_label_set_text(rl, s_tz_table[i].label);
        lv_obj_set_pos(rl, 2, 2);
        lv_obj_add_event_cb(row, __tz_item_cb, LV_EVENT_CLICKED,
                             (VOID_T *)(intptr_t)(INT32_T)i);
    }
    __list_refresh_sel(s_mypc.tz_list, sel);
    /* Scroll to selected item */
    lv_obj_t *sel_row = lv_obj_get_child(s_mypc.tz_list, sel);
    if (sel_row) lv_obj_scroll_to_view(sel_row, LV_ANIM_OFF);

    __w95_btn(panel, 4, MYPC_PANEL_H - 22, 60, 20, "Apply", __tz_apply_cb, NULL);

    s_mypc.tz_status_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(s_mypc.tz_status_lbl,
                                lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_text_font(s_mypc.tz_status_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_mypc.tz_status_lbl, "");
    lv_obj_set_pos(s_mypc.tz_status_lbl, 72, MYPC_PANEL_H - 18);
}

/* ---------------------------------------------------------------------------
 * Build panel: Screen / Screensaver
 * --------------------------------------------------------------------------- */
STATIC UINT32_T __ss_timeout_values[] = {0, 1, 2, 5, 10, 15, 30};
#define SS_TIMEOUT_COUNT ((INT32_T)(sizeof(__ss_timeout_values)/sizeof(__ss_timeout_values[0])))

STATIC VOID_T __ss_apply_cb(lv_event_t *e)
{
    (VOID_T)e;
    INT32_T idx = s_mypc.ss_sel_idx;
    if (idx < 0 || idx >= SS_TIMEOUT_COUNT) idx = 0;
    UINT32_T minutes = (UINT32_T)__ss_timeout_values[idx];
    win95_pipes_set_timeout(minutes);

    CHAR_T val[8];
    snprintf(val, sizeof(val), "%lu", (UINT32_T)minutes);
    tal_kv_set("ss_timeout", (UINT8_T *)val, (UINT32_T)strlen(val));
    lv_label_set_text(s_mypc.ss_status_lbl, "Applied.");
}

STATIC INT32_T __ss_find_timeout_idx(UINT32_T minutes)
{
    for (INT32_T i = 0; i < SS_TIMEOUT_COUNT; i++) {
        if (__ss_timeout_values[i] == minutes) return i;
    }
    return 0;
}

STATIC VOID_T __ss_preview_refresh(VOID_T)
{
    /* Embedded preview disabled for now: Preview button should always enter
     * the real full-screen saver, matching Win95 behavior. */
    if (s_mypc.ss_preview_host) {
        win95_pipes_preview_stop();
    }
}

STATIC VOID_T __ss_preview_cb(lv_event_t *e)
{
    (VOID_T)e;
    win95_pipes_preview_stop();
    win95_pipes_start();
    lv_label_set_text(s_mypc.ss_status_lbl, "Full-screen preview. Tap to exit.");
}

STATIC CONST CHAR_T *s_ss_labels[] = {
    "Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"
};

STATIC VOID_T __build_screen(lv_obj_t *panel)
{
    UINT8_T kv_buf[16] = {0};
    UINT32_T kv_len = sizeof(kv_buf) - 1;
    UINT32_T saved_minutes = 0;

    if (tal_kv_get("ss_timeout", kv_buf, &kv_len) == OPRT_OK) {
        saved_minutes = (UINT32_T)atoi((CHAR_T *)kv_buf);
    }

    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(lbl, "Screensaver:  3D Pipes");
    lv_obj_set_pos(lbl, 4, 4);

    lv_obj_t *wlbl = lv_label_create(panel);
    lv_obj_set_style_text_color(wlbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(wlbl, &lv_font_unscii_8, 0);
    lv_label_set_text(wlbl, "Wait:");
    lv_obj_set_pos(wlbl, 4, 18);

    s_mypc.ss_sel_idx = __ss_find_timeout_idx(saved_minutes);
    INT32_T list_h = SS_TIMEOUT_COUNT * 16 + 4;
    s_mypc.ss_list = __list_create(panel, 40, 16, 120, list_h);

    for (INT32_T i = 0; i < SS_TIMEOUT_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_mypc.ss_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 100, 14);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *rl = lv_label_create(row);
        lv_obj_set_style_text_font(rl, &lv_font_unscii_8, 0);
        lv_label_set_text(rl, s_ss_labels[i]);
        lv_obj_set_pos(rl, 2, 2);
        lv_obj_add_event_cb(row, __ss_item_cb, LV_EVENT_CLICKED,
                             (VOID_T *)(intptr_t)i);
    }
    __list_refresh_sel(s_mypc.ss_list, s_mypc.ss_sel_idx);

    lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(lbl, "Full-screen preview:");
    lv_obj_set_pos(lbl, 198, 18);

    lv_obj_t *monitor = lv_obj_create(panel);
    lv_obj_remove_style_all(monitor);
    lv_obj_set_size(monitor, 176, 132);
    lv_obj_set_pos(monitor, 206, 34);
    lv_obj_set_style_bg_color(monitor, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(monitor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(monitor, 2, 0);
    lv_obj_set_style_border_color(monitor, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_side(monitor, LV_BORDER_SIDE_FULL, 0);
    lv_obj_clear_flag(monitor, LV_OBJ_FLAG_SCROLLABLE);

    s_mypc.ss_preview_host = lv_obj_create(monitor);
    lv_obj_remove_style_all(s_mypc.ss_preview_host);
    lv_obj_set_size(s_mypc.ss_preview_host, 148, 98);
    lv_obj_set_pos(s_mypc.ss_preview_host, 12, 12);
    lv_obj_set_style_bg_color(s_mypc.ss_preview_host, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_mypc.ss_preview_host, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mypc.ss_preview_host, 1, 0);
    lv_obj_set_style_border_color(s_mypc.ss_preview_host, lv_color_hex(0x202020), 0);
    lv_obj_clear_flag(s_mypc.ss_preview_host, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(panel);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xD8D8D8), 0);
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_label_set_text(hint, "Press Preview");
    lv_obj_align_to(hint, s_mypc.ss_preview_host, LV_ALIGN_CENTER, 0, -6);

    hint = lv_label_create(panel);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xD8D8D8), 0);
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_label_set_text(hint, "to test full screen");
    lv_obj_align_to(hint, s_mypc.ss_preview_host, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t *stand = lv_obj_create(panel);
    lv_obj_remove_style_all(stand);
    lv_obj_set_size(stand, 36, 10);
    lv_obj_set_pos(stand, 276, 166);
    lv_obj_set_style_bg_color(stand, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(stand, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stand, 1, 0);
    lv_obj_set_style_border_color(stand, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_clear_flag(stand, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *base = lv_obj_create(panel);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, 86, 8);
    lv_obj_set_pos(base, 251, 176);
    lv_obj_set_style_bg_color(base, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(base, 1, 0);
    lv_obj_set_style_border_color(base, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *energy_frame = lv_obj_create(panel);
    lv_obj_remove_style_all(energy_frame);
    lv_obj_set_size(energy_frame, 82, 50);
    lv_obj_set_pos(energy_frame, 253, 186);
    lv_obj_set_style_bg_color(energy_frame, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(energy_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(energy_frame, 0, 0);
    lv_obj_clear_flag(energy_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(energy_frame, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *energy = lv_image_create(energy_frame);
    lv_image_set_src(energy, &g_win95_energy_blue);
    lv_obj_set_pos(energy, 1, 1);
    lv_obj_clear_flag(energy, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(energy, LV_OBJ_FLAG_SCROLLABLE);

    INT32_T btn_y = 16 + list_h + 6;
    __w95_btn(panel,  4, btn_y, 68, 20, "Preview", __ss_preview_cb, NULL);
    __w95_btn(panel, 78, btn_y, 52, 20, "Apply",   __ss_apply_cb,   NULL);

    s_mypc.ss_status_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(s_mypc.ss_status_lbl,
                                lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_text_font(s_mypc.ss_status_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_mypc.ss_status_lbl, "");
    lv_obj_set_pos(s_mypc.ss_status_lbl, 4, btn_y + 24);
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mypc_close(VOID_T)
{
    win95_pipes_preview_stop();
    if (s_mypc.screen) lv_obj_delete(s_mypc.screen);
    memset(&s_mypc, 0, sizeof(MYPC_CTX_T));
}

STATIC VOID_T __mypc_close_cb(lv_event_t *e) { (VOID_T)e; __mypc_close(); }

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_mypc_open(VOID_T)
{
    if (s_mypc.screen) __mypc_close();
    memset(&s_mypc, 0, sizeof(MYPC_CTX_T));

    /* Init disk path */
    strncpy(s_mypc.disk_path, WIN95_DISK_MOUNT, sizeof(s_mypc.disk_path) - 1);

    s_mypc.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_mypc.screen);
    lv_obj_set_size(s_mypc.screen, MYPC_W, MYPC_H);
    lv_obj_set_pos(s_mypc.screen, 0, 0);
    lv_obj_set_style_bg_color(s_mypc.screen, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_mypc.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_mypc.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_mypc.screen);

    /* ----- Title bar ------------------------------------ */
    lv_obj_t *tbar = lv_obj_create(s_mypc.screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, MYPC_W - 4, MYPC_TITLE_H);
    lv_obj_set_pos(tbar, 2, 2);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(tbar);
    lv_obj_set_style_text_color(ttl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_unscii_8, 0);
    lv_label_set_text(ttl, "My Computer");
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tbar);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_set_style_border_color(xb, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(xb, 1, 0);
    lv_obj_add_event_cb(xb, __mypc_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);

    /* ----- Tab bar --------------------------------------- */
    CONST CHAR_T *tab_names[TAB_COUNT] = {"General", "Disk", "Time Zone", "Screen"};
    lv_event_cb_t tab_cbs[TAB_COUNT]   = {__tab0_cb, __tab1_cb, __tab2_cb, __tab3_cb};
    INT32_T tab_w = (MYPC_W - 8) / TAB_COUNT;

    lv_obj_t *tbar2 = lv_obj_create(s_mypc.screen);
    lv_obj_remove_style_all(tbar2);
    lv_obj_set_size(tbar2, MYPC_W - 4, MYPC_TABBAR_H);
    lv_obj_set_pos(tbar2, 2, MYPC_TITLE_H + 2);
    lv_obj_set_style_bg_color(tbar2, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(tbar2, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar2, LV_OBJ_FLAG_SCROLLABLE);

    for (INT32_T i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *tb = lv_btn_create(tbar2);
        lv_obj_set_size(tb, tab_w - 2, MYPC_TABBAR_H - 2);
        lv_obj_set_pos(tb, 2 + i * tab_w, 1);
        lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
        lv_obj_set_style_radius(tb, 0, 0);
        lv_obj_set_style_pad_all(tb, 0, 0);
        lv_obj_set_style_border_color(tb, lv_color_hex(WIN95_COLOR_SHADOW), 0);
        lv_obj_set_style_border_width(tb, 1, 0);
        lv_obj_add_event_cb(tb, tab_cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *tl = lv_label_create(tb);
        lv_obj_set_style_text_color(tl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
        lv_label_set_text(tl, tab_names[i]);
        lv_obj_center(tl);
        s_mypc.tab_btns[i] = tb;
    }

    /* ----- Content panels -------------------------------- */
    for (INT32_T i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *p = lv_obj_create(s_mypc.screen);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, MYPC_PANEL_W, MYPC_PANEL_H);
        lv_obj_set_pos(p, 4, MYPC_PANEL_Y);
        lv_obj_set_style_bg_color(p, lv_color_hex(WIN95_COLOR_WINDOW), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        s_mypc.panels[i] = p;
    }

    __build_general(s_mypc.panels[TAB_GENERAL]);
    __build_disk(s_mypc.panels[TAB_DISK]);
    __build_tz(s_mypc.panels[TAB_TZ]);
    __build_screen(s_mypc.panels[TAB_SCREEN]);

    /* Show first tab */
    __switch_tab(TAB_GENERAL);
}
