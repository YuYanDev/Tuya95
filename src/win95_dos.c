/**
 * @file win95_dos.c
 * @brief MS-DOS Prompt terminal emulator for Win95 desktop.
 *        Black screen, green UNSCII-8 text. Supports a set of simulated
 *        DOS commands: HELP, VER, CLS, DIR, ECHO, DATE, TIME, SET, MEM,
 *        IPCONFIG, CD, EXIT.
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_dos.h"
#include "win95_ntp.h"
#include "win95_cursor.h"
#include "win95_kb.h"

#include "tal_api.h"
#include "tal_time_service.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define DOS_W          BIOS_SCREEN_WIDTH   /* 480 */
#define DOS_H          BIOS_SCREEN_HEIGHT  /* 320 */
#define DOS_TITLE_H    18
#define DOS_INPUT_H    18
#define DOS_KB_H       120
#define DOS_OUTPUT_H   (DOS_H - DOS_TITLE_H - DOS_INPUT_H - DOS_KB_H)  /* 164 */
#define DOS_BG         0x000000
#define DOS_FG         0x00AA00   /* classic DOS green */
#define DOS_FG_BRIGHT  0x00FF00
#define DOS_OUT_MAX    4096       /* output ring buffer */
#define DOS_CWD_MAX    32

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t  *screen;
    lv_obj_t  *output;       /* scrollable textarea (output history) */
    lv_obj_t  *input;        /* single-line input textarea */
    lv_obj_t  *prompt_lbl;   /* "C:\>" before input */
    lv_obj_t  *enter_btn;
    lv_obj_t  *kb;
    CHAR_T     cwd[DOS_CWD_MAX];
} DOS_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC DOS_CTX_T s_dos;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __dos_close(VOID_T);
STATIC VOID_T __dos_exec(CONST CHAR_T *line);
STATIC VOID_T __dos_input_focus(VOID_T);

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __dos_print(CONST CHAR_T *text)
{
    if (s_dos.output == NULL || text == NULL) {
        return;
    }
    lv_textarea_add_text(s_dos.output, text);
    /* Scroll to the bottom */
    lv_obj_scroll_to_y(s_dos.output, LV_COORD_MAX, LV_ANIM_OFF);
}

STATIC VOID_T __dos_println(CONST CHAR_T *text)
{
    __dos_print(text);
    __dos_print("\n");
}

/* Print the prompt prefix into the output area */
STATIC VOID_T __dos_echo_input(CONST CHAR_T *cmd)
{
    CHAR_T buf[DOS_CWD_MAX + 8];
    snprintf(buf, sizeof(buf), "%s>", s_dos.cwd);
    __dos_print(buf);
    __dos_println(cmd);
}

/* Case-insensitive string compare of first word in line against cmd */
STATIC BOOL_T __cmd_is(CONST CHAR_T *line, CONST CHAR_T *cmd)
{
    UINT32_T clen = (UINT32_T)strlen(cmd);
    for (UINT32_T i = 0; i < clen; i++) {
        CHAR_T a = line[i];
        CHAR_T b = cmd[i];
        if (a >= 'a' && a <= 'z') a = (CHAR_T)(a - 32);
        if (b >= 'a' && b <= 'z') b = (CHAR_T)(b - 32);
        if (a != b) return FALSE;
    }
    return (line[clen] == '\0' || line[clen] == ' ');
}

/* Return pointer to first argument after command word, or "" */
STATIC CONST CHAR_T *__cmd_args(CONST CHAR_T *line)
{
    while (*line && *line != ' ') line++;
    while (*line == ' ') line++;
    return line;
}

/* Upper-case copy of src into dst (bounded) */
STATIC VOID_T __strupr(CHAR_T *dst, CONST CHAR_T *src, UINT32_T n)
{
    UINT32_T i;
    for (i = 0; i < n - 1 && src[i]; i++) {
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? (CHAR_T)(src[i] - 32) : src[i];
    }
    dst[i] = '\0';
}

/* Apply tz_offset_minutes to a UTC POSIX_TM_S, handling day rollover */
STATIC VOID_T __apply_tz(POSIX_TM_S *tm)
{
    INT32_T tz = bios_app_get_ctx()->tz_offset_minutes;
    if (tz == 0) return;

    INT32_T total_min = (INT32_T)tm->tm_hour * 60 + (INT32_T)tm->tm_min + tz;
    INT32_T day_delta = 0;
    if (total_min < 0)       { total_min += 1440; day_delta = -1; }
    else if (total_min >= 1440) { total_min -= 1440; day_delta =  1; }

    tm->tm_hour = total_min / 60;
    tm->tm_min  = total_min % 60;

    if (day_delta == 0) return;

    static const INT32_T dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    INT32_T yr  = (INT32_T)tm->tm_year + 1900;
    INT32_T feb = ((yr % 4 == 0) && (yr % 100 != 0 || yr % 400 == 0)) ? 29 : 28;
    INT32_T mday = (INT32_T)tm->tm_mday + day_delta;
    INT32_T mon  = (INT32_T)tm->tm_mon;
    if (mday < 1) {
        mon--;
        if (mon < 0) { mon = 11; tm->tm_year--; }
        mday = (mon == 1) ? feb : dim[mon];
    } else {
        INT32_T cap = (mon == 1) ? feb : dim[mon];
        if (mday > cap) { mday = 1; mon++; if (mon > 11) { mon = 0; tm->tm_year++; } }
    }
    tm->tm_mday = mday;
    tm->tm_mon  = mon;
}

/* ---------------------------------------------------------------------------
 * Command implementations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __cmd_help(VOID_T)
{
    __dos_println("Available commands:");
    __dos_println("  HELP / ?   This help");
    __dos_println("  VER        OS version");
    __dos_println("  CLS        Clear screen");
    __dos_println("  DIR        Directory listing");
    __dos_println("  ECHO text  Print text");
    __dos_println("  DATE       Show date");
    __dos_println("  TIME       Show time");
    __dos_println("  SET        Environment variables");
    __dos_println("  MEM        Memory info");
    __dos_println("  IPCONFIG   Network info");
    __dos_println("  CD [path]  Change directory");
    __dos_println("  PING host  Ping host");
    __dos_println("  EXIT       Close terminal");
}

STATIC VOID_T __cmd_ver(VOID_T)
{
    __dos_println("MS-DOS Version 6.22");
    __dos_println("TuyaOS 95 [Version 4.00.950]");
}

STATIC VOID_T __cmd_cls(VOID_T)
{
    lv_textarea_set_text(s_dos.output, "");
}

STATIC VOID_T __cmd_dir(VOID_T)
{
    CHAR_T buf[128];
    snprintf(buf, sizeof(buf), " Volume in drive C is TUYA95\n"
             " Directory of %s\n", s_dos.cwd);
    __dos_print(buf);
    __dos_println("");
    __dos_println("COMMAND  COM    93,812  08-24-95  11:11a");
    __dos_println("WIN95    EXE   284,160  08-24-95  11:11a");
    __dos_println("AUTOEXEC BAT       512  08-24-95  11:11a");
    __dos_println("CONFIG   SYS       256  08-24-95  11:11a");
    __dos_println("LVGL     DLL   131,072  04-22-26   9:00a");
    __dos_println("NOTEPAD  EXE    22,016  04-22-26   9:00a");
    __dos_println("        6 File(s)    531,828 bytes");
    __dos_println("  8,126,464 bytes free");
}

STATIC VOID_T __cmd_set(VOID_T)
{
    __dos_println("COMSPEC=C:\\COMMAND.COM");
    __dos_println("PATH=C:\\;C:\\DOS;C:\\WINDOWS");
    __dos_println("PROMPT=$P$G");
    __dos_println("TEMP=C:\\TEMP");
    __dos_println("WINDIR=C:\\WINDOWS");
    __dos_println("BOARD=Tuya T5 AI");
}

STATIC VOID_T __cmd_mem(VOID_T)
{
    __dos_println("Memory Type     Total   Used    Free");
    __dos_println("--------------- ------- ------- -------");
    __dos_println("Conventional    640K    92K     548K");
    __dos_println("Extended (XMS)  16,384K 8,192K  8,192K");
    __dos_println("Total memory    17,024K 8,284K  8,740K");
}

STATIC VOID_T __cmd_ipconfig(VOID_T)
{
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    __dos_println("TuyaOS 95 IP Configuration");
    __dos_println("");
    if (app->wifi_state == WIFI_ST_CONNECTED && app->wifi_ip[0] != '\0') {
        CHAR_T buf[96];
        snprintf(buf, sizeof(buf), "  SSID . . . . : %s", app->wifi_ssid);
        __dos_println(buf);
        snprintf(buf, sizeof(buf), "  IP Address . : %s", app->wifi_ip);
        __dos_println(buf);
        __dos_println("  Subnet Mask  : 255.255.255.0");
        __dos_println("  Gateway . .  : (unknown)");
    } else {
        __dos_println("  Ethernet adapter Dial-Up:");
        __dos_println("    Media State . : Media disconnected");
    }
}

STATIC VOID_T __cmd_date(VOID_T)
{
    CHAR_T buf[48];
    if (win95_ntp_synced()) {
        POSIX_TM_S tm;
        tal_time_get(&tm);
        __apply_tz(&tm);
        snprintf(buf, sizeof(buf), "Current date is %04d-%02d-%02d",
                 (INT32_T)tm.tm_year + 1900, (INT32_T)tm.tm_mon + 1, (INT32_T)tm.tm_mday);
    } else {
        snprintf(buf, sizeof(buf), "Current date is 1995-08-24");
    }
    __dos_println(buf);
}

STATIC VOID_T __cmd_time(VOID_T)
{
    CHAR_T buf[48];
    if (win95_ntp_synced()) {
        POSIX_TM_S tm;
        tal_time_get(&tm);
        __apply_tz(&tm);
        snprintf(buf, sizeof(buf), "Current time is %02d:%02d:%02d",
                 (INT32_T)tm.tm_hour, (INT32_T)tm.tm_min, (INT32_T)tm.tm_sec);
    } else {
        snprintf(buf, sizeof(buf), "Current time is 11:11:00 (NTP not synced)");
    }
    __dos_println(buf);
}

STATIC VOID_T __cmd_ping(CONST CHAR_T *host)
{
    CHAR_T buf[80];
    if (*host == '\0') {
        __dos_println("Usage: PING <hostname>");
        return;
    }
    snprintf(buf, sizeof(buf), "Pinging %s with 32 bytes of data:", host);
    __dos_println(buf);
    snprintf(buf, sizeof(buf), "Reply from %s: bytes=32 time=42ms TTL=128", host);
    __dos_println(buf);
    __dos_println(buf);
    __dos_println(buf);
    __dos_println(buf);
    __dos_println("");
    __dos_println("Ping statistics:");
    snprintf(buf, sizeof(buf), "  Packets: Sent=4 Received=4 Lost=0 (0%% loss)");
    __dos_println(buf);
    __dos_println("Approximate round trip times:");
    __dos_println("  Minimum=42ms  Maximum=42ms  Average=42ms");
}

STATIC VOID_T __cmd_cd(CONST CHAR_T *path)
{
    if (*path == '\0' || (path[0] == '.' && path[1] == '\0')) {
        __dos_println(s_dos.cwd);
        return;
    }
    if (strcmp(path, "..") == 0 || strcmp(path, "..\\") == 0) {
        /* Go up one level (max: back to C:\) */
        CHAR_T *last = strrchr(s_dos.cwd, '\\');
        if (last && last != s_dos.cwd) {
            *last = '\0';
        }
        return;
    }
    /* Descend one level (limit depth to prevent overflow) */
    if (strlen(s_dos.cwd) + strlen(path) + 2 < DOS_CWD_MAX) {
        strcat(s_dos.cwd, "\\");
        CHAR_T upper[DOS_CWD_MAX];
        __strupr(upper, path, DOS_CWD_MAX);
        strcat(s_dos.cwd, upper);
    } else {
        __dos_println("Path too long.");
    }
}

/* ---------------------------------------------------------------------------
 * Command dispatcher
 * --------------------------------------------------------------------------- */
STATIC VOID_T __dos_exec(CONST CHAR_T *line)
{
    /* Skip leading spaces */
    while (*line == ' ') line++;
    if (*line == '\0') {
        return;
    }

    __dos_echo_input(line);

    if (__cmd_is(line, "help") || __cmd_is(line, "?")) {
        __cmd_help();
    } else if (__cmd_is(line, "ver")) {
        __cmd_ver();
    } else if (__cmd_is(line, "cls")) {
        __cmd_cls();
    } else if (__cmd_is(line, "dir")) {
        __cmd_dir();
    } else if (__cmd_is(line, "echo")) {
        __dos_println(__cmd_args(line));
    } else if (__cmd_is(line, "date")) {
        __cmd_date();
    } else if (__cmd_is(line, "time")) {
        __cmd_time();
    } else if (__cmd_is(line, "set")) {
        __cmd_set();
    } else if (__cmd_is(line, "mem")) {
        __cmd_mem();
    } else if (__cmd_is(line, "ipconfig")) {
        __cmd_ipconfig();
    } else if (__cmd_is(line, "cd")) {
        __cmd_cd(__cmd_args(line));
    } else if (__cmd_is(line, "ping")) {
        __cmd_ping(__cmd_args(line));
    } else if (__cmd_is(line, "exit")) {
        __dos_close();
        return;
    } else {
        CHAR_T buf[80];
        CHAR_T uline[40];
        __strupr(uline, line, sizeof(uline));
        /* Show only the command part */
        CHAR_T *sp = strchr(uline, ' ');
        if (sp) *sp = '\0';
        snprintf(buf, sizeof(buf), "Bad command or file name: '%s'", uline);
        __dos_println(buf);
    }

    /* Show next prompt */
    CHAR_T prompt[DOS_CWD_MAX + 2];
    snprintf(prompt, sizeof(prompt), "%s>", s_dos.cwd);
    __dos_print(prompt);
}

/* ---------------------------------------------------------------------------
 * Callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __dos_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    __dos_close();
}

STATIC VOID_T __dos_enter_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_dos.input == NULL) {
        return;
    }
    CONST CHAR_T *txt = lv_textarea_get_text(s_dos.input);
    CHAR_T cmd[128] = {0};
    if (txt && txt[0] != '\0') {
        strncpy(cmd, txt, sizeof(cmd) - 1);
    }
    lv_textarea_set_text(s_dos.input, "");
    __dos_exec(cmd);
    __dos_input_focus();
}

STATIC VOID_T __dos_input_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER) {
        __dos_enter_cb(NULL);
    }
}

STATIC VOID_T __dos_input_focus_cb(lv_event_t *e)
{
    (VOID_T)e;
    __dos_input_focus();
}

STATIC VOID_T __dos_input_defocus_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_dos.kb) {
        lv_obj_add_flag(s_dos.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

STATIC VOID_T __dos_input_focus(VOID_T)
{
    if (s_dos.kb && s_dos.input) {
        win95_kb_set_textarea(s_dos.kb, s_dos.input);
        lv_obj_clear_flag(s_dos.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */
STATIC VOID_T __dos_close(VOID_T)
{
    if (s_dos.screen) {
        lv_obj_delete(s_dos.screen);
    }
    memset(&s_dos, 0, sizeof(DOS_CTX_T));
    win95_cursor_set_visible(TRUE);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------  */
VOID_T win95_dos_open(VOID_T)
{
    if (s_dos.screen) {
        __dos_close();
    }
    memset(&s_dos, 0, sizeof(DOS_CTX_T));
    strncpy(s_dos.cwd, "C:\\", sizeof(s_dos.cwd) - 1);

    s_dos.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_dos.screen);
    lv_obj_set_size(s_dos.screen, DOS_W, DOS_H);
    lv_obj_set_pos(s_dos.screen, 0, 0);
    lv_obj_set_style_bg_color(s_dos.screen, lv_color_hex(DOS_BG), 0);
    lv_obj_set_style_bg_opa(s_dos.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_dos.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_dos.screen);

    /* Title bar */
    lv_obj_t *tbar = lv_obj_create(s_dos.screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, DOS_W - 4, DOS_TITLE_H);
    lv_obj_set_pos(tbar, 2, 2);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(0x000080), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(tbar);
    lv_obj_set_style_text_color(ttl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_unscii_8, 0);
    lv_label_set_text(ttl, "MS-DOS Prompt");
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tbar);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_set_style_border_color(xb, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(xb, 1, 0);
    lv_obj_add_event_cb(xb, __dos_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);

    /* Output textarea */
    s_dos.output = lv_textarea_create(s_dos.screen);
    lv_obj_set_size(s_dos.output, DOS_W - 4, DOS_OUTPUT_H);
    lv_obj_set_pos(s_dos.output, 2, DOS_TITLE_H);
    lv_obj_set_style_bg_color(s_dos.output, lv_color_hex(DOS_BG), 0);
    lv_obj_set_style_bg_color(s_dos.output, lv_color_hex(DOS_BG), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_dos.output, lv_color_hex(DOS_FG), 0);
    lv_obj_set_style_text_font(s_dos.output, &lv_font_unscii_8, 0);
    lv_obj_set_style_radius(s_dos.output, 0, 0);
    lv_obj_set_style_border_width(s_dos.output, 0, 0);
    lv_obj_set_style_pad_all(s_dos.output, 2, 0);
    /* Make cursor invisible */
    lv_obj_set_style_bg_opa(s_dos.output, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_dos.output, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_FOCUSED);
    /* Prevent focus (so keyboard doesn't appear on output tap) */
    lv_obj_clear_flag(s_dos.output, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_textarea_set_placeholder_text(s_dos.output, "");

    /* Input bar */
    lv_obj_t *ibar = lv_obj_create(s_dos.screen);
    lv_obj_remove_style_all(ibar);
    lv_obj_set_size(ibar, DOS_W - 4, DOS_INPUT_H);
    lv_obj_set_pos(ibar, 2, DOS_TITLE_H + DOS_OUTPUT_H);
    lv_obj_set_style_bg_color(ibar, lv_color_hex(DOS_BG), 0);
    lv_obj_set_style_bg_opa(ibar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ibar, lv_color_hex(DOS_FG), 0);
    lv_obj_set_style_border_width(ibar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(ibar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_clear_flag(ibar, LV_OBJ_FLAG_SCROLLABLE);

    /* Prompt label "C:\>" */
    s_dos.prompt_lbl = lv_label_create(ibar);
    lv_obj_set_style_text_color(s_dos.prompt_lbl, lv_color_hex(DOS_FG), 0);
    lv_obj_set_style_text_font(s_dos.prompt_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_dos.prompt_lbl, "C:\\>");
    lv_obj_align(s_dos.prompt_lbl, LV_ALIGN_LEFT_MID, 2, 0);

    INT32_T prompt_w = 32; /* approximate pixel width of "C:\>" in unscii_8 */

    /* Input textarea */
    s_dos.input = lv_textarea_create(ibar);
    lv_obj_set_size(s_dos.input, DOS_W - 4 - prompt_w - 44, DOS_INPUT_H - 2);
    lv_obj_set_pos(s_dos.input, prompt_w + 2, 0);
    lv_obj_set_style_bg_color(s_dos.input, lv_color_hex(DOS_BG), 0);
    lv_obj_set_style_bg_color(s_dos.input, lv_color_hex(DOS_BG), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_dos.input, lv_color_hex(DOS_FG_BRIGHT), 0);
    lv_obj_set_style_text_font(s_dos.input, &lv_font_unscii_8, 0);
    lv_obj_set_style_radius(s_dos.input, 0, 0);
    lv_obj_set_style_border_width(s_dos.input, 0, 0);
    lv_obj_set_style_pad_all(s_dos.input, 1, 0);
    lv_obj_set_style_bg_color(s_dos.input, lv_color_hex(DOS_FG), LV_PART_CURSOR);
    lv_textarea_set_one_line(s_dos.input, true);
    lv_textarea_set_max_length(s_dos.input, 127);
    lv_textarea_set_placeholder_text(s_dos.input, "");
    lv_obj_add_event_cb(s_dos.input, __dos_input_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(s_dos.input, __dos_input_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_dos.input, __dos_input_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    /* Enter button */
    s_dos.enter_btn = lv_btn_create(ibar);
    lv_obj_set_size(s_dos.enter_btn, 40, DOS_INPUT_H - 2);
    lv_obj_align(s_dos.enter_btn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(s_dos.enter_btn, lv_color_hex(0x004400), 0);
    lv_obj_set_style_radius(s_dos.enter_btn, 0, 0);
    lv_obj_set_style_pad_all(s_dos.enter_btn, 0, 0);
    lv_obj_set_style_border_color(s_dos.enter_btn, lv_color_hex(DOS_FG), 0);
    lv_obj_set_style_border_width(s_dos.enter_btn, 1, 0);
    lv_obj_add_event_cb(s_dos.enter_btn, __dos_enter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(s_dos.enter_btn);
    lv_obj_set_style_text_color(el, lv_color_hex(DOS_FG), 0);
    lv_obj_set_style_text_font(el, &lv_font_unscii_8, 0);
    lv_label_set_text(el, "Enter");
    lv_obj_center(el);

    /* Keyboard */
    s_dos.kb = win95_kb_create(s_dos.screen);
    lv_obj_set_size(s_dos.kb, DOS_W, DOS_KB_H);
    lv_obj_align(s_dos.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    win95_kb_set_textarea(s_dos.kb, s_dos.input);
    lv_obj_set_style_bg_color(s_dos.kb, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_text_color(s_dos.kb, lv_color_hex(0x000000), 0);

    /* Welcome banner and first prompt */
    __dos_println("TuyaOS 95");
    __dos_println("(C)Copyright Tuya Inc. 2026.");
    __dos_println("");
    __dos_print("C:\\>");
    __dos_input_focus();

    win95_cursor_set_visible(FALSE);
}
