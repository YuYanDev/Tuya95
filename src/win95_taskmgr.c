/**
 * @file win95_taskmgr.c
 * @brief Win95-style Windows Task Manager.
 *
 * Two tabs:
 *   Applications — list of running simulated tasks with status.
 *   Performance  — CPU bar + memory bar, updated every second.
 */
#include "win95_taskmgr.h"
#include "win95_desktop.h"
#include "bios_simulator.h"
#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Layout
 * --------------------------------------------------------------------------- */
#define TM_W         440
#define TM_H         300
#define TM_TITLE_H   18
#define TM_TABBAR_H  20
#define TM_PANEL_Y   (TM_TITLE_H + 4 + TM_TABBAR_H)
#define TM_PANEL_H   (TM_H - TM_PANEL_Y - 28)   /* leave room for btn row */
#define TM_PANEL_W   (TM_W - 8)

#define TAB_APPS  0
#define TAB_PERF  1
#define N_TABS    2

/* ---------------------------------------------------------------------------
 * Fake task list
 * --------------------------------------------------------------------------- */
typedef struct { CONST CHAR_T *name; CONST CHAR_T *status; } FAKE_TASK_T;

STATIC CONST FAKE_TASK_T s_tasks[] = {
    {"Win95 Desktop",          "Running"},
    {"Tuya Navigator",         "Running"},
    {"Notepad",                "Running"},
    {"MS-DOS Prompt",          "Running"},
    {"Minesweeper",            "Running"},
    {"Disk Defragmenter",      "Running"},
    {"Network Neighborhood",   "Running"},
    {"3D Pipes Screensaver",   "Running"},
    {"Explorer",               "Running"},
    {"LVGL Render Engine",     "Running"},
};
#define N_TASKS ((INT32_T)(sizeof(s_tasks)/sizeof(s_tasks[0])))

/* ---------------------------------------------------------------------------
 * Context
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t  *win;
    lv_obj_t  *tab_btns[N_TABS];
    lv_obj_t  *panels[N_TABS];
    INT32_T    active_tab;

    /* Applications tab */
    lv_obj_t  *task_list;
    INT32_T    sel_task;        /* -1 = none */
    lv_obj_t  *end_task_btn;

    /* Performance tab */
    lv_obj_t  *cpu_bar;
    lv_obj_t  *cpu_lbl;
    lv_obj_t  *mem_bar;
    lv_obj_t  *mem_lbl;
    lv_obj_t  *handle_lbl;
    lv_timer_t *perf_timer;

    UINT32_T   fake_cpu;
    UINT32_T   fake_seed;
} TM_CTX_T;

STATIC TM_CTX_T s_tm;

/* ---------------------------------------------------------------------------
 * Simple LCG for fake CPU fluctuation
 * --------------------------------------------------------------------------- */
STATIC UINT32_T __tm_rand(VOID_T)
{
    s_tm.fake_seed = s_tm.fake_seed * 1664525u + 1013904223u;
    return s_tm.fake_seed >> 16;
}

/* ---------------------------------------------------------------------------
 * Win95 button helper
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *__tm_btn(lv_obj_t *parent, INT32_T x, INT32_T y,
                            INT32_T w, INT32_T h,
                            CONST CHAR_T *txt, lv_event_cb_t cb, VOID_T *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

/* ---------------------------------------------------------------------------
 * Switch tab
 * --------------------------------------------------------------------------- */
STATIC VOID_T __tm_switch(INT32_T idx)
{
    s_tm.active_tab = idx;
    for (INT32_T i = 0; i < N_TABS; i++) {
        if (s_tm.panels[i]) {
            if (i == idx) lv_obj_clear_flag(s_tm.panels[i], LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(s_tm.panels[i],   LV_OBJ_FLAG_HIDDEN);
        }
        if (s_tm.tab_btns[i]) {
            UINT32_T bg = (i == idx) ? WIN95_COLOR_TITLEBAR : WIN95_COLOR_WINDOW;
            UINT32_T fg = (i == idx) ? WIN95_COLOR_LIGHT     : 0x000000;
            lv_obj_set_style_bg_color(s_tm.tab_btns[i], lv_color_hex(bg), 0);
            lv_obj_t *l = lv_obj_get_child(s_tm.tab_btns[i], 0);
            if (l) lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        }
    }
}

STATIC VOID_T __tab0_cb(lv_event_t *e) { (VOID_T)e; __tm_switch(TAB_APPS); }
STATIC VOID_T __tab1_cb(lv_event_t *e) { (VOID_T)e; __tm_switch(TAB_PERF); }

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */
STATIC VOID_T __tm_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_tm.perf_timer) { lv_timer_delete(s_tm.perf_timer); s_tm.perf_timer = NULL; }
    if (s_tm.win)        { lv_obj_delete(s_tm.win); }
    memset(&s_tm, 0, sizeof(s_tm));
}

/* ---------------------------------------------------------------------------
 * Task row click
 * --------------------------------------------------------------------------- */
STATIC VOID_T __task_row_cb(lv_event_t *e)
{
    INT32_T idx = (INT32_T)(intptr_t)lv_event_get_user_data(e);
    s_tm.sel_task = idx;
    /* Refresh selection highlight */
    UINT32_T n = (UINT32_T)lv_obj_get_child_count(s_tm.task_list);
    for (UINT32_T i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_tm.task_list, (INT32_T)i);
        BOOL_T sel = ((INT32_T)i == idx);
        lv_obj_set_style_bg_color(row, lv_color_hex(
            sel ? WIN95_COLOR_TITLEBAR : 0xFFFFFF), 0);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            lv_color_hex(sel ? WIN95_COLOR_LIGHT : 0x000000), 0);
    }
}

/* ---------------------------------------------------------------------------
 * End Task button
 * --------------------------------------------------------------------------- */
STATIC VOID_T __end_task_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_tm.sel_task < 0 || s_tm.sel_task >= N_TASKS) return;
    /* Mark the task row as "Not Responding" */
    lv_obj_t *row = lv_obj_get_child(s_tm.task_list, s_tm.sel_task);
    if (!row) return;
    lv_obj_t *lbl = lv_obj_get_child(row, 0);
    if (lbl) {
        CHAR_T buf[64];
        snprintf(buf, sizeof(buf), "%s (Not Responding)",
                 s_tasks[s_tm.sel_task].name);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xAA0000), 0);
    }
    s_tm.sel_task = -1;
}

/* ---------------------------------------------------------------------------
 * Performance timer
 * --------------------------------------------------------------------------- */
STATIC VOID_T __perf_tick(lv_timer_t *t)
{
    (VOID_T)t;
    /* Fake CPU: random walk between 5-80% */
    INT32_T delta = (INT32_T)(__tm_rand() % 11) - 5;
    INT32_T cpu   = (INT32_T)s_tm.fake_cpu + delta;
    if (cpu < 5)  cpu = 5;
    if (cpu > 85) cpu = 85;
    s_tm.fake_cpu = (UINT32_T)cpu;

    if (s_tm.cpu_bar) lv_bar_set_value(s_tm.cpu_bar, cpu, LV_ANIM_ON);
    if (s_tm.cpu_lbl) {
        CHAR_T b[16];
        snprintf(b, sizeof(b), "CPU Usage: %d%%", cpu);
        lv_label_set_text(s_tm.cpu_lbl, b);
    }

    /* Fake memory 3-4 MB used out of 8 MB */
    UINT32_T mem_used = 3000 + (__tm_rand() % 1200);
    if (s_tm.mem_bar) lv_bar_set_value(s_tm.mem_bar, (INT32_T)(mem_used / 80), LV_ANIM_ON);
    if (s_tm.mem_lbl) {
        CHAR_T b2[32];
        snprintf(b2, sizeof(b2), "Mem Usage: %uKB / 8192KB", (unsigned)mem_used);
        lv_label_set_text(s_tm.mem_lbl, b2);
    }

    /* Fake handle count */
    if (s_tm.handle_lbl) {
        UINT32_T handles = 320 + (__tm_rand() % 64);
        CHAR_T b3[32];
        snprintf(b3, sizeof(b3), "Handles: %u   Threads: %u",
                 (unsigned)handles, (unsigned)(handles / 8));
        lv_label_set_text(s_tm.handle_lbl, b3);
    }
}

/* ---------------------------------------------------------------------------
 * Build Applications panel
 * --------------------------------------------------------------------------- */
STATIC VOID_T __build_apps(lv_obj_t *panel)
{
    /* Column headers */
    lv_obj_t *h = lv_label_create(panel);
    lv_obj_set_style_text_font(h, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_label_set_text(h, "Task                         Status");
    lv_obj_set_pos(h, 4, 2);

    /* Scrollable task list */
    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_set_size(list, TM_PANEL_W - 4, TM_PANEL_H - 20);
    lv_obj_set_pos(list, 2, 16);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_radius(list, 0, 0);
    s_tm.task_list = list;

    for (INT32_T i = 0; i < N_TASKS; i++) {
        CHAR_T row_txt[64];
        snprintf(row_txt, sizeof(row_txt), "%-28s %s",
                 s_tasks[i].name, s_tasks[i].status);
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, TM_PANEL_W - 24, 14);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_label_set_text(lbl, row_txt);
        lv_obj_set_pos(lbl, 2, 2);
        lv_obj_add_event_cb(row, __task_row_cb, LV_EVENT_CLICKED,
                             (VOID_T *)(intptr_t)i);
    }

    /* End Task button */
    s_tm.end_task_btn = __tm_btn(panel, 4, TM_PANEL_H - 2, 72, 20,
                                  "End Task", __end_task_cb, NULL);
}

/* ---------------------------------------------------------------------------
 * Build Performance panel
 * --------------------------------------------------------------------------- */
STATIC VOID_T __build_perf(lv_obj_t *panel)
{
    INT32_T y = 4;

    lv_obj_t *l1 = lv_label_create(panel);
    lv_obj_set_style_text_font(l1, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l1, lv_color_hex(0x000000), 0);
    lv_label_set_text(l1, "CPU Usage:");
    lv_obj_set_pos(l1, 4, y);

    s_tm.cpu_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(s_tm.cpu_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_tm.cpu_lbl, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_label_set_text(s_tm.cpu_lbl, "CPU Usage:  0%");
    lv_obj_set_pos(s_tm.cpu_lbl, 80, y);

    y += 12;
    s_tm.cpu_bar = lv_bar_create(panel);
    lv_obj_set_size(s_tm.cpu_bar, TM_PANEL_W - 8, 20);
    lv_obj_set_pos(s_tm.cpu_bar, 4, y);
    lv_bar_set_range(s_tm.cpu_bar, 0, 100);
    lv_bar_set_value(s_tm.cpu_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_tm.cpu_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_tm.cpu_bar, lv_color_hex(0x00AA00),
                               LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_tm.cpu_bar, 0, 0);
    lv_obj_set_style_radius(s_tm.cpu_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_tm.cpu_bar, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(s_tm.cpu_bar, 1, 0);

    y += 28;
    s_tm.mem_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(s_tm.mem_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_tm.mem_lbl, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_label_set_text(s_tm.mem_lbl, "Mem Usage: --- KB / 8192 KB");
    lv_obj_set_pos(s_tm.mem_lbl, 4, y);

    y += 12;
    s_tm.mem_bar = lv_bar_create(panel);
    lv_obj_set_size(s_tm.mem_bar, TM_PANEL_W - 8, 20);
    lv_obj_set_pos(s_tm.mem_bar, 4, y);
    lv_bar_set_range(s_tm.mem_bar, 0, 100);
    lv_bar_set_value(s_tm.mem_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_tm.mem_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_tm.mem_bar, lv_color_hex(WIN95_COLOR_TITLEBAR),
                               LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_tm.mem_bar, 0, 0);
    lv_obj_set_style_radius(s_tm.mem_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_tm.mem_bar, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(s_tm.mem_bar, 1, 0);

    y += 32;
    s_tm.handle_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(s_tm.handle_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_tm.handle_lbl, lv_color_hex(0x000000), 0);
    lv_label_set_text(s_tm.handle_lbl, "Handles: ---   Threads: ---");
    lv_obj_set_pos(s_tm.handle_lbl, 4, y);

    y += 20;
    lv_obj_t *sysinfo = lv_label_create(panel);
    lv_obj_set_style_text_font(sysinfo, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(sysinfo, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_label_set_text(sysinfo,
        "Processor:  BK7258 / Tuya T5 AI\n"
        "Total RAM:  8 MB  (PSRAM)\n"
        "System:     TuyaOS 95 v4.00.950");
    lv_obj_set_pos(sysinfo, 4, y);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_taskmgr_open(VOID_T)
{
    if (s_tm.win) {
        if (s_tm.perf_timer) { lv_timer_delete(s_tm.perf_timer); s_tm.perf_timer = NULL; }
        lv_obj_delete(s_tm.win);
        memset(&s_tm, 0, sizeof(s_tm));
    }

    s_tm.fake_seed = (UINT32_T)tal_time_get_posix();
    s_tm.fake_cpu  = 20;
    s_tm.sel_task  = -1;

    lv_obj_t *scr = lv_scr_act();

    /* Window */
    lv_obj_t *w = lv_obj_create(scr);
    lv_obj_remove_style_all(w);
    lv_obj_set_size(w, TM_W, TM_H);
    lv_obj_align(w, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(w, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(w, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(w, 2, 0);
    lv_obj_set_style_radius(w, 0, 0);
    lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(w);
    s_tm.win = w;

    /* Title bar */
    lv_obj_t *tb = lv_obj_create(w);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, TM_W - 4, TM_TITLE_H);
    lv_obj_set_pos(tb, 2, 2);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Windows Task Manager");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_add_event_cb(xb, __tm_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "x");
    lv_obj_center(xl);

    /* Tab bar */
    CONST CHAR_T *tab_names[N_TABS] = {"Applications", "Performance"};
    lv_event_cb_t tab_cbs[N_TABS]   = {__tab0_cb, __tab1_cb};
    INT32_T tab_w = (TM_W - 8) / N_TABS;

    lv_obj_t *tbar2 = lv_obj_create(w);
    lv_obj_remove_style_all(tbar2);
    lv_obj_set_size(tbar2, TM_W - 4, TM_TABBAR_H);
    lv_obj_set_pos(tbar2, 2, TM_TITLE_H + 2);
    lv_obj_set_style_bg_color(tbar2, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(tbar2, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar2, LV_OBJ_FLAG_SCROLLABLE);

    for (INT32_T i = 0; i < N_TABS; i++) {
        lv_obj_t *tbtn = lv_btn_create(tbar2);
        lv_obj_set_size(tbtn, tab_w - 2, TM_TABBAR_H - 2);
        lv_obj_set_pos(tbtn, 2 + i * tab_w, 1);
        lv_obj_set_style_bg_color(tbtn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
        lv_obj_set_style_radius(tbtn, 0, 0);
        lv_obj_set_style_pad_all(tbtn, 0, 0);
        lv_obj_set_style_border_color(tbtn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
        lv_obj_set_style_border_width(tbtn, 1, 0);
        lv_obj_add_event_cb(tbtn, tab_cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *tbtl = lv_label_create(tbtn);
        lv_obj_set_style_text_color(tbtl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(tbtl, &lv_font_unscii_8, 0);
        lv_label_set_text(tbtl, tab_names[i]);
        lv_obj_center(tbtl);
        s_tm.tab_btns[i] = tbtn;
    }

    /* Content panels */
    for (INT32_T i = 0; i < N_TABS; i++) {
        lv_obj_t *p = lv_obj_create(w);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, TM_PANEL_W, TM_PANEL_H + 30);
        lv_obj_set_pos(p, 4, TM_PANEL_Y);
        lv_obj_set_style_bg_color(p, lv_color_hex(WIN95_COLOR_WINDOW), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        s_tm.panels[i] = p;
    }

    __build_apps(s_tm.panels[TAB_APPS]);
    __build_perf(s_tm.panels[TAB_PERF]);
    __tm_switch(TAB_APPS);

    /* Performance update timer: 1s */
    s_tm.perf_timer = lv_timer_create(__perf_tick, 1000, NULL);
}
