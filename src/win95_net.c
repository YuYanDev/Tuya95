/**
 * @file win95_net.c
 * @brief Network Neighborhood: WiFi AP scan + LAN TCP-80 device sweep.
 */
#include "win95_net.h"
#include "tal_api.h"
#include "tal_wifi.h"
#include "tal_network.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define WIN95_COLOR_FACE 0xD4D0C8  /* Win95 3D face (button) colour */
#define NET_W           (BIOS_SCREEN_WIDTH)
#define NET_H           (BIOS_SCREEN_HEIGHT)
#define NET_TITLE_H     18
#define NET_TAB_H       20
#define NET_STATUS_H    14
#define NET_CONTENT_H   (NET_H - NET_TITLE_H - NET_TAB_H - NET_STATUS_H - 2)
#define AP_MAX          20
#define LAN_MAX         20
#define LAN_TIMEOUT_MS  300

/* ---------------------------------------------------------------------------
 * Auth mode strings
 * --------------------------------------------------------------------------- */
STATIC CONST CHAR_T *__auth_str(UINT8_T auth)
{
    switch (auth) {
    case 0: return "Open";
    case 1: return "WEP";
    case 2: return "WPA";
    case 3: return "WPA2";
    case 4: return "WPA/2";
    default: return "?";
    }
}

/* ---------------------------------------------------------------------------
 * Context
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t   *screen;
    lv_obj_t   *tab_btns[2];
    lv_obj_t   *panels[2];      /* [0]=WiFi, [1]=LAN */
    lv_obj_t   *status_lbl;
    INT32_T     active_tab;

    /* WiFi panel */
    lv_obj_t   *wifi_list;

    /* LAN panel */
    lv_obj_t   *lan_list;

    /* Worker thread */
    THREAD_HANDLE scan_thread;
    volatile BOOL_T scan_done;
    volatile INT32_T scan_tab;   /* 0=WiFi, 1=LAN */

    /* Scan results (written by thread, read by timer) */
    CHAR_T ap_ssid[AP_MAX][36];
    INT8_T ap_rssi[AP_MAX];
    UINT8_T ap_ch[AP_MAX];
    UINT8_T ap_auth[AP_MAX];
    UINT32_T ap_count;

    UINT32_T lan_ip[LAN_MAX];
    UINT32_T lan_count;

    lv_timer_t *poll_tmr;
} NET_CTX_T;

STATIC NET_CTX_T *s_net = NULL;

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __net_set_status(CONST CHAR_T *txt)
{
    if (s_net && s_net->status_lbl)
        lv_label_set_text(s_net->status_lbl, txt);
}

STATIC VOID_T __net_switch_tab(INT32_T idx);

/* ---------------------------------------------------------------------------
 * WiFi scan worker
 * --------------------------------------------------------------------------- */
STATIC VOID_T __scan_wifi_thread(VOID_T *arg)
{
    (VOID_T)arg;
    AP_IF_S *ap_ary = NULL;
    uint32_t ap_num = 0;

    OPERATE_RET rt = tal_wifi_all_ap_scan(&ap_ary, &ap_num);
    if (rt == OPRT_OK && ap_ary && ap_num > 0) {
        UINT32_T n = ap_num < AP_MAX ? ap_num : AP_MAX;
        s_net->ap_count = n;
        for (UINT32_T i = 0; i < n; i++) {
            strncpy(s_net->ap_ssid[i], (CHAR_T *)ap_ary[i].ssid, 35);
            s_net->ap_ssid[i][35] = '\0';
            s_net->ap_rssi[i]  = ap_ary[i].rssi;
            s_net->ap_ch[i]    = ap_ary[i].channel;
            s_net->ap_auth[i]  = ap_ary[i].security;
        }
        tal_wifi_release_ap(ap_ary);
    } else {
        s_net->ap_count = 0;
    }
    s_net->scan_tab  = 0;
    s_net->scan_done = TRUE;
    s_net->scan_thread = NULL;
    tal_thread_delete(NULL);
}

/* ---------------------------------------------------------------------------
 * LAN sweep worker — try TCP connect on port 80 per host
 * --------------------------------------------------------------------------- */
STATIC VOID_T __scan_lan_thread(VOID_T *arg)
{
    (VOID_T)arg;
    s_net->lan_count = 0;

    /* Get our IP address (string "A.B.C.D") */
    NW_IP_S ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    if (tal_wifi_get_ip(WF_STATION, &ip_info) != OPRT_OK || ip_info.ip[0] == '\0') {
        s_net->scan_tab  = 1;
        s_net->scan_done = TRUE;
        s_net->scan_thread = NULL;
        tal_thread_delete(NULL);
        return;
    }

    /* Parse base A.B.C from IP string */
    UINT32_T a=0, b=0, c=0, d=0;
    sscanf(ip_info.ip, "%u.%u.%u.%u", &a, &b, &c, &d);

    UINT32_T count = 0;
    for (UINT32_T host = 1; host <= 254 && count < LAN_MAX; host++) {
        if (host == d) continue; /* skip our own IP */
        CHAR_T target[20];
        snprintf(target, sizeof(target), "%u.%u.%u.%u", a, b, c, host);
        TUYA_IP_ADDR_T target_ip = 0;
        if (tal_net_gethostbyname(target, &target_ip) != OPRT_OK || target_ip == 0) continue;
        /* Try TCP connect on port 80 */
        INT32_T fd = tal_net_socket_create(PROTOCOL_TCP);
        if (fd < 0) continue;
        tal_net_set_timeout(fd, LAN_TIMEOUT_MS, TRANS_SEND);
        tal_net_set_timeout(fd, LAN_TIMEOUT_MS, TRANS_RECV);
        if (tal_net_connect(fd, target_ip, 80) == 0) {
            s_net->lan_ip[count++] = (a<<24)|(b<<16)|(c<<8)|host;
        }
        tal_net_close(fd);
        tal_system_sleep(1);
    }
    s_net->lan_count = count;
    s_net->scan_tab  = 1;
    s_net->scan_done = TRUE;
    s_net->scan_thread = NULL;
    tal_thread_delete(NULL);
}

/* ---------------------------------------------------------------------------
 * Poll timer — runs in LVGL thread, picks up scan results
 * --------------------------------------------------------------------------- */
STATIC VOID_T __net_poll_cb(lv_timer_t *t)
{
    (VOID_T)t;
    if (!s_net || !s_net->scan_done) return;
    s_net->scan_done = FALSE;

    if (s_net->scan_tab == 0) {
        /* Populate WiFi list */
        lv_obj_clean(s_net->wifi_list);
        if (s_net->ap_count == 0) {
            lv_obj_t *lbl = lv_label_create(s_net->wifi_list);
            lv_label_set_text(lbl, "No APs found");
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        }
        for (UINT32_T i = 0; i < s_net->ap_count; i++) {
            lv_obj_t *row = lv_obj_create(s_net->wifi_list);
            lv_obj_set_size(row, NET_W - 6, 16);
            lv_obj_set_style_bg_color(row, lv_color_hex(i%2?0xD4D0C8:0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 1, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *lbl = lv_label_create(row);
            CHAR_T buf[64];
            snprintf(buf, sizeof(buf), "%-22s %4d dBm  Ch%2u  %s",
                     s_net->ap_ssid[i][0] ? s_net->ap_ssid[i] : "(hidden)",
                     (INT32_T)s_net->ap_rssi[i],
                     (UINT32_T)s_net->ap_ch[i],
                     __auth_str(s_net->ap_auth[i]));
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        }
        CHAR_T st[32]; snprintf(st, sizeof(st), "%lu AP(s) found", (UINT32_T)s_net->ap_count);
        __net_set_status(st);
    } else {
        /* Populate LAN list */
        lv_obj_clean(s_net->lan_list);
        if (s_net->lan_count == 0) {
            lv_obj_t *lbl = lv_label_create(s_net->lan_list);
            lv_label_set_text(lbl, "No LAN devices found");
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        }
        for (UINT32_T i = 0; i < s_net->lan_count; i++) {
            UINT32_T ip = s_net->lan_ip[i];
            CHAR_T buf[40];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u  (port 80 open)",
                     (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                     (ip >> 8)  & 0xFF,  ip        & 0xFF);
            lv_obj_t *lbl = lv_label_create(s_net->lan_list);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        }
        CHAR_T st[32]; snprintf(st, sizeof(st), "%lu device(s) found", (UINT32_T)s_net->lan_count);
        __net_set_status(st);
    }

    if (s_net->poll_tmr) { lv_timer_delete(s_net->poll_tmr); s_net->poll_tmr = NULL; }
}

/* ---------------------------------------------------------------------------
 * Start a scan
 * --------------------------------------------------------------------------- */
STATIC VOID_T __net_start_scan(INT32_T tab)
{
    if (!s_net || s_net->scan_thread) return;
    __net_set_status("Scanning...");
    s_net->scan_done = FALSE;
    THREAD_CFG_T cfg = {4096, 4, "net_scan"};
    THREAD_HANDLE h = NULL;
    if (tab == 0) {
        tal_thread_create_and_start(&h, NULL, NULL, __scan_wifi_thread, NULL, &cfg);
    } else {
        tal_thread_create_and_start(&h, NULL, NULL, __scan_lan_thread, NULL, &cfg);
    }
    s_net->scan_thread = h;
    if (s_net->poll_tmr == NULL) {
        s_net->poll_tmr = lv_timer_create(__net_poll_cb, 500, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Tab switching
 * --------------------------------------------------------------------------- */
STATIC VOID_T __net_switch_tab(INT32_T idx)
{
    if (!s_net) return;
    s_net->active_tab = idx;
    for (INT32_T i = 0; i < 2; i++) {
        UINT32_T bg = (i == idx) ? WIN95_COLOR_WINDOW : WIN95_COLOR_FACE;
        lv_obj_set_style_bg_color(s_net->tab_btns[i], lv_color_hex(bg), 0);
        if (s_net->panels[i]) {
            if (i == idx) lv_obj_clear_flag(s_net->panels[i], LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(s_net->panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Tab button callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __tab_wifi_cb(lv_event_t *e)  { (VOID_T)e; __net_switch_tab(0); }
STATIC VOID_T __tab_lan_cb(lv_event_t *e)   { (VOID_T)e; __net_switch_tab(1); }

STATIC VOID_T __refresh_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_net) __net_start_scan(s_net->active_tab);
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */
STATIC VOID_T __net_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!s_net) return;
    if (s_net->poll_tmr) { lv_timer_delete(s_net->poll_tmr); s_net->poll_tmr = NULL; }
    if (s_net->screen) { lv_obj_delete(s_net->screen); s_net->screen = NULL; }
    tal_free(s_net); s_net = NULL;
}

/* ---------------------------------------------------------------------------
 * Build content panel
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *__make_list_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, NET_W - 4, NET_CONTENT_H - 22);
    lv_obj_set_pos(p, 2, NET_TITLE_H + NET_TAB_H + 2);
    lv_obj_set_style_bg_color(p, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 2, 0);
    lv_obj_set_layout(p, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return p;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_net_open(lv_obj_t *parent)
{
    if (s_net) return;

    s_net = (NET_CTX_T *)tal_malloc(sizeof(NET_CTX_T));
    if (!s_net) return;
    memset(s_net, 0, sizeof(NET_CTX_T));

    /* Window screen */
    s_net->screen = lv_obj_create(parent);
    lv_obj_set_size(s_net->screen, NET_W, NET_H);
    lv_obj_set_pos(s_net->screen, 0, 0);
    lv_obj_set_style_bg_color(s_net->screen, lv_color_hex(WIN95_COLOR_FACE), 0);
    lv_obj_set_style_bg_opa(s_net->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_net->screen, 2, 0);
    lv_obj_set_style_border_color(s_net->screen, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_radius(s_net->screen, 0, 0);
    lv_obj_set_style_pad_all(s_net->screen, 0, 0);
    lv_obj_clear_flag(s_net->screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Title bar */
    lv_obj_t *tbar = lv_obj_create(s_net->screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, NET_W - 4, NET_TITLE_H);
    lv_obj_set_pos(tbar, 2, 2);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tbar);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Network Neighborhood");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xbtn = lv_btn_create(tbar);
    lv_obj_set_size(xbtn, 14, 12);
    lv_obj_align(xbtn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xbtn, lv_color_hex(WIN95_COLOR_FACE), 0);
    lv_obj_set_style_radius(xbtn, 0, 0);
    lv_obj_set_style_pad_all(xbtn, 0, 0);
    lv_obj_set_style_border_color(xbtn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(xbtn, 1, 0);
    lv_obj_add_event_cb(xbtn, __net_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xbtn);
    lv_label_set_text(xl, "X");
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_center(xl);

    /* Tab buttons */
    CONST CHAR_T *tab_names[] = {"WiFi APs", "LAN Devices"};
    lv_event_cb_t tab_cbs[] = {__tab_wifi_cb, __tab_lan_cb};
    for (INT32_T i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_btn_create(s_net->screen);
        lv_obj_set_size(btn, 96, NET_TAB_H);
        lv_obj_set_pos(btn, 2 + i * 98, NET_TITLE_H + 2);
        lv_obj_set_style_bg_color(btn, lv_color_hex(i==0?WIN95_COLOR_WINDOW:WIN95_COLOR_FACE), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_event_cb(btn, tab_cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_center(lbl);
        s_net->tab_btns[i] = btn;
    }

    /* Refresh button */
    lv_obj_t *rbtn = lv_btn_create(s_net->screen);
    lv_obj_set_size(rbtn, 80, NET_TAB_H);
    lv_obj_set_pos(rbtn, NET_W - 84, NET_TITLE_H + 2);
    lv_obj_set_style_bg_color(rbtn, lv_color_hex(WIN95_COLOR_FACE), 0);
    lv_obj_set_style_radius(rbtn, 0, 0);
    lv_obj_set_style_pad_all(rbtn, 0, 0);
    lv_obj_set_style_border_color(rbtn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(rbtn, 1, 0);
    lv_obj_add_event_cb(rbtn, __refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rbtn);
    lv_label_set_text(rl, "[Refresh]");
    lv_obj_set_style_text_font(rl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(rl, lv_color_hex(0x000000), 0);
    lv_obj_center(rl);

    /* Content panels */
    s_net->panels[0] = __make_list_panel(s_net->screen);
    s_net->wifi_list = s_net->panels[0];
    s_net->panels[1] = __make_list_panel(s_net->screen);
    s_net->lan_list  = s_net->panels[1];
    lv_obj_add_flag(s_net->panels[1], LV_OBJ_FLAG_HIDDEN);

    /* Status bar */
    s_net->status_lbl = lv_label_create(s_net->screen);
    lv_label_set_text(s_net->status_lbl, "Ready");
    lv_obj_set_style_text_font(s_net->status_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_net->status_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(s_net->status_lbl, 4, NET_H - NET_STATUS_H - 2);

    /* Auto-scan WiFi on open */
    __net_start_scan(0);
}
