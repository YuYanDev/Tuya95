/**
 * @file win95_dialup.c
 * @brief Win95 Dial-Up Networking - two tabs:
 *        [Direct]     legacy SSID/password tal_wifi_station_connect path
 *        [Tuya Pair]  save PID/UUID/AUTHKEY then let BLE + phone AP pair
 * @version 2.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_dialup.h"
#include "win95_desktop.h"
#include "win95_pairing.h"
#include "win95_kb.h"

#include "netmgr.h"
#include "netconn_wifi.h"
#include "tal_api.h"
#include "tal_wifi.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define DU_W            BIOS_SCREEN_WIDTH
#define DU_H            BIOS_SCREEN_HEIGHT
#define DU_TITLE_H      18
#define DU_TABBAR_H     20
#define DU_KB_H         130
#define DU_POLL_MS      500
#define DU_TIMEOUT_S    20
#define DU_PAIR_POLL_MS 500

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    DU_TAB_DIRECT = 0,
    DU_TAB_PAIR   = 1,
} DU_TAB_E;

typedef struct {
    lv_obj_t *screen;

    /* Tab bar */
    lv_obj_t *tab_btn_direct;
    lv_obj_t *tab_btn_pair;
    lv_obj_t *tab_direct;
    lv_obj_t *tab_pair;
    DU_TAB_E  cur_tab;

    /* Direct tab */
    lv_obj_t *ta_ssid;
    lv_obj_t *ta_pass;
    lv_obj_t *btn_conn;
    lv_obj_t *btn_disc;
    lv_obj_t *status_lbl;
    lv_obj_t *progress;

    /* Pair tab */
    lv_obj_t *ta_pid;
    lv_obj_t *ta_uuid;
    lv_obj_t *ta_authkey;
    lv_obj_t *btn_save_pair;
    lv_obj_t *pair_status_lbl;

    /* Shared */
    lv_obj_t *kb;
    lv_timer_t *poll_tmr;        /* direct WiFi poll */
    lv_timer_t *pair_poll_tmr;   /* pair state poll */
    UINT32_T poll_count;
} DIALUP_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC DIALUP_CTX_T s_du;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __du_close(VOID_T);
STATIC VOID_T __du_poll_cb(lv_timer_t *timer);
STATIC VOID_T __du_pair_poll_cb(lv_timer_t *timer);
STATIC VOID_T __du_select_tab(DU_TAB_E tab);

STATIC BOOL_T __du_use_netmgr(CONST BIOS_APP_CTX_T *app)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;

    if (app == NULL || app->pair_state == PAIR_ST_CREDS_MISSING) {
        return FALSE;
    }

    return (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &status) == OPRT_OK);
}

STATIC VOID_T __du_sync_wifi_state(BIOS_APP_CTX_T *app)
{
    WF_STATION_STAT_E st = WSS_IDLE;

    if (app == NULL || tal_wifi_station_get_status(&st) != OPRT_OK) {
        return;
    }

    switch (st) {
    case WSS_GOT_IP: {
        NW_IP_S ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        if (tal_wifi_get_ip(WF_STATION, &ip_info) == OPRT_OK) {
            strncpy(app->wifi_ip, ip_info.ip, IP_STR_MAX_LEN - 1);
            app->wifi_ip[IP_STR_MAX_LEN - 1] = '\0';
        }
        app->wifi_state = WIFI_ST_CONNECTED;
        break;
    }
    case WSS_CONNECTING:
    case WSS_CONN_SUCCESS:
        app->wifi_state = WIFI_ST_CONNECTING;
        break;
    case WSS_PASSWD_WRONG:
    case WSS_NO_AP_FOUND:
    case WSS_CONN_FAIL:
    case WSS_DHCP_FAIL:
        app->wifi_state = WIFI_ST_FAILED;
        break;
    case WSS_IDLE:
    default:
        if (app->wifi_state == WIFI_ST_CONNECTED) {
            app->wifi_state = WIFI_ST_DISCONNECTED;
            app->wifi_ip[0] = '\0';
        }
        break;
    }
}

STATIC OPERATE_RET __du_start_connect(BIOS_APP_CTX_T *app)
{
    OPERATE_RET rt = OPRT_OK;

    if (app == NULL) {
        return OPRT_INVALID_PARM;
    }

    if (__du_use_netmgr(app)) {
        netconn_wifi_info_t info;
        memset(&info, 0, sizeof(info));
        strncpy(info.ssid, app->wifi_ssid, sizeof(info.ssid) - 1);
        strncpy(info.pswd, app->wifi_pass, sizeof(info.pswd) - 1);
        return netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &info);
    }

    rt = bios_wifi_legacy_ensure_init();
    if (rt != OPRT_OK) {
        return rt;
    }
    return tal_wifi_station_connect((INT8_T *)app->wifi_ssid, (INT8_T *)app->wifi_pass);
}

STATIC VOID_T __du_stop_connect(BIOS_APP_CTX_T *app)
{
    if (__du_use_netmgr(app)) {
        netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_CLOSE, NULL);
    } else {
        tal_wifi_station_disconnect();
    }
}

/* ---------------------------------------------------------------------------
 * Border helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __du_raised(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_x(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 1, 0);
}

STATIC VOID_T __du_sunken(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
}

/* ---------------------------------------------------------------------------
 * Callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __du_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    __du_close();
}

STATIC VOID_T __du_tab_direct_cb(lv_event_t *e)
{
    (VOID_T)e;
    __du_select_tab(DU_TAB_DIRECT);
}

STATIC VOID_T __du_tab_pair_cb(lv_event_t *e)
{
    (VOID_T)e;
    __du_select_tab(DU_TAB_PAIR);
}

/**
 * @brief Direct tab - Connect button: tal_wifi_station_connect with SSID/PW
 */
STATIC VOID_T __du_connect_cb(lv_event_t *e)
{
    (VOID_T)e;
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    OPERATE_RET rt = OPRT_OK;

    CONST CHAR_T *ssid = lv_textarea_get_text(s_du.ta_ssid);
    CONST CHAR_T *pass = lv_textarea_get_text(s_du.ta_pass);

    if (ssid == NULL || strlen(ssid) == 0) {
        lv_label_set_text(s_du.status_lbl, "Error: Enter WiFi name");
        lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0xFF0000), 0);
        return;
    }

    strncpy(app->wifi_ssid, ssid, SSID_MAX_LEN);
    app->wifi_ssid[SSID_MAX_LEN] = '\0';
    strncpy(app->wifi_pass, pass, PASSWORD_MAX_LEN);
    app->wifi_pass[PASSWORD_MAX_LEN] = '\0';
    app->wifi_state = WIFI_ST_CONNECTING;

    lv_label_set_text(s_du.status_lbl, "Dialing...");
    lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0xFFFF00), 0);

    if (s_du.progress) {
        lv_obj_clear_flag(s_du.progress, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_du.progress, 10, LV_ANIM_ON);
    }

    if (s_du.kb) {
        lv_obj_add_flag(s_du.kb, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(s_du.btn_conn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_du.btn_disc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(s_du.ta_ssid, LV_STATE_DISABLED);
    lv_obj_add_state(s_du.ta_pass, LV_STATE_DISABLED);

    rt = __du_start_connect(app);
    if (rt != OPRT_OK) {
        CHAR_T buf[64];
        snprintf(buf, sizeof(buf), "Connect failed: %d", (int)rt);
        lv_label_set_text(s_du.status_lbl, buf);
        lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0xFF0000), 0);

        if (s_du.progress) {
            lv_bar_set_value(s_du.progress, 0, LV_ANIM_OFF);
            lv_obj_add_flag(s_du.progress, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_clear_flag(s_du.btn_conn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_du.btn_disc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(s_du.ta_ssid, LV_STATE_DISABLED);
        lv_obj_clear_state(s_du.ta_pass, LV_STATE_DISABLED);
        return;
    }

    s_du.poll_count = 0;
    if (s_du.poll_tmr == NULL) {
        s_du.poll_tmr = lv_timer_create(__du_poll_cb, DU_POLL_MS, NULL);
    }
}

STATIC VOID_T __du_disconnect_cb(lv_event_t *e)
{
    (VOID_T)e;
    BIOS_APP_CTX_T *app = bios_app_get_ctx();

    __du_stop_connect(app);
    app->wifi_state = WIFI_ST_DISCONNECTED;
    app->wifi_ip[0] = '\0';

    lv_label_set_text(s_du.status_lbl, "Disconnected.");
    lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0x000000), 0);

    if (s_du.progress) {
        lv_bar_set_value(s_du.progress, 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_du.progress, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(s_du.btn_conn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_du.btn_disc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(s_du.ta_ssid, LV_STATE_DISABLED);
    lv_obj_clear_state(s_du.ta_pass, LV_STATE_DISABLED);

    win95_taskbar_set_net(FALSE);

    if (s_du.poll_tmr) {
        lv_timer_delete(s_du.poll_tmr);
        s_du.poll_tmr = NULL;
    }
}

STATIC VOID_T __du_ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_du.kb) {
        win95_kb_set_textarea(s_du.kb, ta);
        lv_obj_clear_flag(s_du.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Poll WiFi connection status (Direct tab).
 */
STATIC VOID_T __du_poll_cb(lv_timer_t *timer)
{
    (VOID_T)timer;
    BIOS_APP_CTX_T *app = bios_app_get_ctx();

    __du_sync_wifi_state(app);

    s_du.poll_count++;
    UINT32_T pct = (s_du.poll_count * 100) / (DU_TIMEOUT_S * 1000 / DU_POLL_MS);
    if (pct > 95) {
        pct = 95;
    }

    if (app->wifi_state == WIFI_ST_CONNECTED) {
        lv_bar_set_value(s_du.progress, 100, LV_ANIM_ON);
        CHAR_T buf[80];
        snprintf(buf, sizeof(buf), "Connected!  IP: %s", app->wifi_ip);
        lv_label_set_text(s_du.status_lbl, buf);
        lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0x00AA00), 0);
        win95_taskbar_set_net(TRUE);
        win95_pairing_save_wifi(app->wifi_ssid, app->wifi_pass);
        lv_timer_delete(s_du.poll_tmr);
        s_du.poll_tmr = NULL;
        return;
    }

    if (app->wifi_state == WIFI_ST_FAILED) {
        lv_bar_set_value(s_du.progress, 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_du.progress, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_du.status_lbl, "Connection failed!");
        lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0xFF0000), 0);
        lv_obj_clear_flag(s_du.btn_conn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_du.btn_disc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(s_du.ta_ssid, LV_STATE_DISABLED);
        lv_obj_clear_state(s_du.ta_pass, LV_STATE_DISABLED);
        lv_timer_delete(s_du.poll_tmr);
        s_du.poll_tmr = NULL;
        return;
    }

    if (s_du.poll_count >= (DU_TIMEOUT_S * 1000 / DU_POLL_MS)) {
        lv_bar_set_value(s_du.progress, 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_du.progress, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_du.status_lbl, "Timeout. No response.");
        lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0xFF0000), 0);
        lv_obj_clear_flag(s_du.btn_conn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_du.btn_disc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(s_du.ta_ssid, LV_STATE_DISABLED);
        lv_obj_clear_state(s_du.ta_pass, LV_STATE_DISABLED);
        __du_stop_connect(app);
        app->wifi_state = WIFI_ST_IDLE;
        lv_timer_delete(s_du.poll_tmr);
        s_du.poll_tmr = NULL;
        return;
    }

    lv_bar_set_value(s_du.progress, (INT32_T)pct, LV_ANIM_ON);
    CONST CHAR_T *stages[] = {
        "Dialing...",
        "Verifying username...",
        "Verifying password...",
        "Logging on to network...",
        "Registering on network...",
    };
    UINT32_T stage = s_du.poll_count / 4;
    if (stage >= 5) {
        stage = 4;
    }
    lv_label_set_text(s_du.status_lbl, stages[stage]);
}

/**
 * @brief Pair tab - Save button: save creds and reboot via tuya_authorize_write.
 */
STATIC VOID_T __du_save_pair_cb(lv_event_t *e)
{
    (VOID_T)e;

    CONST CHAR_T *pid  = lv_textarea_get_text(s_du.ta_pid);
    CONST CHAR_T *uuid = lv_textarea_get_text(s_du.ta_uuid);
    CONST CHAR_T *akey = lv_textarea_get_text(s_du.ta_authkey);

    if (!pid || !uuid || !akey || pid[0] == '\0' || uuid[0] == '\0' || akey[0] == '\0') {
        lv_label_set_text(s_du.pair_status_lbl, "Fill in PID, UUID and AUTHKEY.");
        lv_obj_set_style_text_color(s_du.pair_status_lbl, lv_color_hex(0xFF0000), 0);
        return;
    }

    lv_label_set_text(s_du.pair_status_lbl, "Saving... device will reboot.");
    lv_obj_set_style_text_color(s_du.pair_status_lbl, lv_color_hex(0xFFFF00), 0);

    /* Push an LVGL refresh before the reboot-on-write. */
    lv_refr_now(NULL);

    OPERATE_RET rt = win95_pairing_save_creds(pid, uuid, akey);
    /* tuya_authorize_write should reboot; if we ever return, show the error. */
    CHAR_T buf[96];
    snprintf(buf, sizeof(buf), "Save failed: %d", (int)rt);
    lv_label_set_text(s_du.pair_status_lbl, buf);
    lv_obj_set_style_text_color(s_du.pair_status_lbl, lv_color_hex(0xFF0000), 0);
}

/**
 * @brief Poll pair_state and update label + taskbar tray.
 */
STATIC VOID_T __du_pair_poll_cb(lv_timer_t *timer)
{
    (VOID_T)timer;
    BIOS_APP_CTX_T *app = bios_app_get_ctx();

    CONST CHAR_T *msg = "Idle.";
    UINT32_T color = 0x000000;
    switch (app->pair_state) {
    case PAIR_ST_IDLE:
        msg = "Idle. Enter creds and Save & Pair.";
        color = 0x000000; break;
    case PAIR_ST_CREDS_MISSING:
        msg = "No credentials - enter PID/UUID/AUTHKEY.";
        color = 0x000000; break;
    case PAIR_ST_INITED:
        msg = "Pairing started - waiting...";
        color = 0xFFFF00; break;
    case PAIR_ST_BIND_START:
        msg = "Ready - use the Tuya app to add device.";
        color = 0xFFFF00; break;
    case PAIR_ST_BIND_TOKEN_ON:
        msg = "Token received - binding...";
        color = 0xFFFF00; break;
    case PAIR_ST_MQTT_CONNECTED:
        msg = "Online! Device is on the cloud.";
        color = 0x00AA00; break;
    case PAIR_ST_MQTT_DISCONNECT:
        msg = "MQTT disconnected - retrying...";
        color = 0xFF8800; break;
    case PAIR_ST_RESET:
        msg = "Reset requested by cloud.";
        color = 0xFF0000; break;
    case PAIR_ST_FAILED:
        msg = "Pairing init failed - check logs.";
        color = 0xFF0000; break;
    default:
        break;
    }
    if (s_du.pair_status_lbl) {
        lv_label_set_text(s_du.pair_status_lbl, msg);
        lv_obj_set_style_text_color(s_du.pair_status_lbl, lv_color_hex(color), 0);
    }
    win95_taskbar_set_net(app->pair_state == PAIR_ST_MQTT_CONNECTED);
}

/* ---------------------------------------------------------------------------
 * Tab switching
 * --------------------------------------------------------------------------- */
STATIC VOID_T __du_style_tab_btn(lv_obj_t *btn, BOOL_T active)
{
    lv_obj_set_style_bg_color(btn,
        lv_color_hex(active ? WIN95_COLOR_LIGHT : WIN95_COLOR_TASKBAR), 0);
}

STATIC VOID_T __du_select_tab(DU_TAB_E tab)
{
    s_du.cur_tab = tab;
    if (tab == DU_TAB_DIRECT) {
        if (s_du.tab_direct) lv_obj_clear_flag(s_du.tab_direct, LV_OBJ_FLAG_HIDDEN);
        if (s_du.tab_pair)   lv_obj_add_flag(s_du.tab_pair, LV_OBJ_FLAG_HIDDEN);
        __du_style_tab_btn(s_du.tab_btn_direct, TRUE);
        __du_style_tab_btn(s_du.tab_btn_pair, FALSE);
    } else {
        if (s_du.tab_direct) lv_obj_add_flag(s_du.tab_direct, LV_OBJ_FLAG_HIDDEN);
        if (s_du.tab_pair)   lv_obj_clear_flag(s_du.tab_pair, LV_OBJ_FLAG_HIDDEN);
        __du_style_tab_btn(s_du.tab_btn_direct, FALSE);
        __du_style_tab_btn(s_du.tab_btn_pair, TRUE);
    }
    /* Hide keyboard on tab switch. */
    if (s_du.kb) {
        lv_obj_add_flag(s_du.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------------------
 * Close & layout
 * --------------------------------------------------------------------------- */
STATIC VOID_T __du_close(VOID_T)
{
    if (s_du.poll_tmr) {
        lv_timer_delete(s_du.poll_tmr);
        s_du.poll_tmr = NULL;
    }
    if (s_du.pair_poll_tmr) {
        lv_timer_delete(s_du.pair_poll_tmr);
        s_du.pair_poll_tmr = NULL;
    }
    if (s_du.screen) {
        lv_obj_delete(s_du.screen);
        s_du.screen = NULL;
    }
    memset(&s_du, 0, sizeof(DIALUP_CTX_T));
}

/**
 * @brief Build the Direct Connect tab widgets inside its container.
 */
STATIC VOID_T __build_direct_tab(lv_obj_t *parent)
{
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    BOOL_T connected = (app->wifi_state == WIFI_ST_CONNECTED);

    /* Modem icon */
    lv_obj_t *ico = lv_obj_create(parent);
    lv_obj_remove_style_all(ico);
    lv_obj_set_size(ico, 36, 28);
    lv_obj_set_pos(ico, 6, 2);
    lv_obj_set_style_bg_color(ico, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_bg_opa(ico, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ico, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(ico, 1, 0);
    lv_obj_clear_flag(ico, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ico_sc = lv_obj_create(ico);
    lv_obj_remove_style_all(ico_sc);
    lv_obj_set_size(ico_sc, 20, 14);
    lv_obj_set_pos(ico_sc, 8, 2);
    lv_obj_set_style_bg_color(ico_sc, lv_color_hex(0x000080), 0);
    lv_obj_set_style_bg_opa(ico_sc, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ico_sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ico_lbl = lv_label_create(ico_sc);
    lv_obj_set_style_text_color(ico_lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(ico_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(ico_lbl, "~");
    lv_obj_center(ico_lbl);

    lv_obj_t *desc = lv_label_create(parent);
    lv_obj_set_style_text_color(desc, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(desc, &lv_font_unscii_8, 0);
    lv_label_set_text(desc, "Connect via WiFi AP");
    lv_obj_set_pos(desc, 48, 8);

    INT32_T fy = 34;
    lv_obj_t *l1 = lv_label_create(parent);
    lv_obj_set_style_text_color(l1, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l1, &lv_font_unscii_8, 0);
    lv_label_set_text(l1, "WiFi Network:");
    lv_obj_set_pos(l1, 6, fy);

    s_du.ta_ssid = lv_textarea_create(parent);
    lv_obj_set_size(s_du.ta_ssid, 210, 22);
    lv_obj_set_pos(s_du.ta_ssid, 6, fy + 10);
    lv_textarea_set_one_line(s_du.ta_ssid, true);
    lv_textarea_set_max_length(s_du.ta_ssid, SSID_MAX_LEN);
    lv_obj_set_style_bg_color(s_du.ta_ssid, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_color(s_du.ta_ssid, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_du.ta_ssid, &lv_font_unscii_8, 0);
    __du_sunken(s_du.ta_ssid);
    lv_obj_add_event_cb(s_du.ta_ssid, __du_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    if (app->wifi_ssid[0] != '\0') {
        lv_textarea_set_text(s_du.ta_ssid, app->wifi_ssid);
    }

    lv_obj_t *l2 = lv_label_create(parent);
    lv_obj_set_style_text_color(l2, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l2, &lv_font_unscii_8, 0);
    lv_label_set_text(l2, "Password:");
    lv_obj_set_pos(l2, 230, fy);

    s_du.ta_pass = lv_textarea_create(parent);
    lv_obj_set_size(s_du.ta_pass, 210, 22);
    lv_obj_set_pos(s_du.ta_pass, 230, fy + 10);
    lv_textarea_set_one_line(s_du.ta_pass, true);
    lv_textarea_set_max_length(s_du.ta_pass, PASSWORD_MAX_LEN);
    lv_textarea_set_password_mode(s_du.ta_pass, true);
    lv_obj_set_style_bg_color(s_du.ta_pass, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_color(s_du.ta_pass, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_du.ta_pass, &lv_font_unscii_8, 0);
    __du_sunken(s_du.ta_pass);
    lv_obj_add_event_cb(s_du.ta_pass, __du_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    if (app->wifi_pass[0] != '\0') {
        lv_textarea_set_text(s_du.ta_pass, app->wifi_pass);
    }

    INT32_T by = fy + 36;
    s_du.btn_conn = lv_btn_create(parent);
    lv_obj_set_size(s_du.btn_conn, 80, 22);
    lv_obj_set_pos(s_du.btn_conn, 6, by);
    lv_obj_set_style_bg_color(s_du.btn_conn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(s_du.btn_conn, 0, 0);
    __du_raised(s_du.btn_conn);
    lv_obj_add_event_cb(s_du.btn_conn, __du_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl1 = lv_label_create(s_du.btn_conn);
    lv_obj_set_style_text_color(cl1, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(cl1, &lv_font_unscii_8, 0);
    lv_label_set_text(cl1, "Connect");
    lv_obj_center(cl1);

    s_du.btn_disc = lv_btn_create(parent);
    lv_obj_set_size(s_du.btn_disc, 90, 22);
    lv_obj_set_pos(s_du.btn_disc, 6, by);
    lv_obj_set_style_bg_color(s_du.btn_disc, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(s_du.btn_disc, 0, 0);
    __du_raised(s_du.btn_disc);
    lv_obj_add_event_cb(s_du.btn_disc, __du_disconnect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl2 = lv_label_create(s_du.btn_disc);
    lv_obj_set_style_text_color(cl2, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(cl2, &lv_font_unscii_8, 0);
    lv_label_set_text(cl2, "Disconnect");
    lv_obj_center(cl2);

    s_du.progress = lv_bar_create(parent);
    lv_obj_set_size(s_du.progress, 220, 10);
    lv_obj_set_pos(s_du.progress, 230, by + 2);
    lv_obj_set_style_bg_color(s_du.progress, lv_color_hex(WIN95_COLOR_SHADOW), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_du.progress, lv_color_hex(WIN95_COLOR_TITLEBAR), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_du.progress, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_du.progress, 0, LV_PART_INDICATOR);
    lv_bar_set_value(s_du.progress, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_du.progress, LV_OBJ_FLAG_HIDDEN);

    s_du.status_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(s_du.status_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_pos(s_du.status_lbl, 230, by + 14);
    lv_obj_set_width(s_du.status_lbl, 220);

    if (connected) {
        CHAR_T buf[80];
        snprintf(buf, sizeof(buf), "Connected!  IP: %s", app->wifi_ip);
        lv_label_set_text(s_du.status_lbl, buf);
        lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0x00AA00), 0);
        lv_obj_add_flag(s_du.btn_conn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_du.btn_disc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_du.status_lbl, "Ready to connect.");
        lv_obj_set_style_text_color(s_du.status_lbl, lv_color_hex(0x000000), 0);
        lv_obj_add_flag(s_du.btn_disc, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Build the Tuya Pair tab widgets inside its container.
 */
STATIC VOID_T __build_pair_tab(lv_obj_t *parent)
{
    BIOS_APP_CTX_T *app = bios_app_get_ctx();

    lv_obj_t *desc = lv_label_create(parent);
    lv_obj_set_style_text_color(desc, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(desc, &lv_font_unscii_8, 0);
    lv_label_set_text(desc, "Tuya IoT pairing  -  BLE + phone app");
    lv_obj_set_pos(desc, 6, 4);

    /* Row 1: PID */
    INT32_T y = 18;
    lv_obj_t *l1 = lv_label_create(parent);
    lv_obj_set_style_text_color(l1, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l1, &lv_font_unscii_8, 0);
    lv_label_set_text(l1, "Product ID:");
    lv_obj_set_pos(l1, 6, y);
    s_du.ta_pid = lv_textarea_create(parent);
    lv_obj_set_size(s_du.ta_pid, 340, 18);
    lv_obj_set_pos(s_du.ta_pid, 100, y - 2);
    lv_textarea_set_one_line(s_du.ta_pid, true);
    lv_textarea_set_max_length(s_du.ta_pid, PID_MAX_LEN);
    lv_obj_set_style_bg_color(s_du.ta_pid, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(s_du.ta_pid, &lv_font_unscii_8, 0);
    __du_sunken(s_du.ta_pid);
    lv_obj_add_event_cb(s_du.ta_pid, __du_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    if (app->product_id[0] != '\0') {
        lv_textarea_set_text(s_du.ta_pid, app->product_id);
    }

    /* Row 2: UUID */
    y += 22;
    lv_obj_t *l2 = lv_label_create(parent);
    lv_obj_set_style_text_color(l2, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l2, &lv_font_unscii_8, 0);
    lv_label_set_text(l2, "UUID:");
    lv_obj_set_pos(l2, 6, y);
    s_du.ta_uuid = lv_textarea_create(parent);
    lv_obj_set_size(s_du.ta_uuid, 340, 18);
    lv_obj_set_pos(s_du.ta_uuid, 100, y - 2);
    lv_textarea_set_one_line(s_du.ta_uuid, true);
    lv_textarea_set_max_length(s_du.ta_uuid, UUID_MAX_LEN);
    lv_obj_set_style_bg_color(s_du.ta_uuid, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(s_du.ta_uuid, &lv_font_unscii_8, 0);
    __du_sunken(s_du.ta_uuid);
    lv_obj_add_event_cb(s_du.ta_uuid, __du_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    if (app->uuid[0] != '\0') {
        lv_textarea_set_text(s_du.ta_uuid, app->uuid);
    }

    /* Row 3: AUTHKEY */
    y += 22;
    lv_obj_t *l3 = lv_label_create(parent);
    lv_obj_set_style_text_color(l3, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l3, &lv_font_unscii_8, 0);
    lv_label_set_text(l3, "AuthKey:");
    lv_obj_set_pos(l3, 6, y);
    s_du.ta_authkey = lv_textarea_create(parent);
    lv_obj_set_size(s_du.ta_authkey, 340, 18);
    lv_obj_set_pos(s_du.ta_authkey, 100, y - 2);
    lv_textarea_set_one_line(s_du.ta_authkey, true);
    lv_textarea_set_max_length(s_du.ta_authkey, AUTHKEY_MAX_LEN);
    lv_obj_set_style_bg_color(s_du.ta_authkey, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(s_du.ta_authkey, &lv_font_unscii_8, 0);
    __du_sunken(s_du.ta_authkey);
    lv_obj_add_event_cb(s_du.ta_authkey, __du_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    if (app->auth_key[0] != '\0') {
        lv_textarea_set_text(s_du.ta_authkey, app->auth_key);
    }

    /* Save & Pair button */
    y += 24;
    s_du.btn_save_pair = lv_btn_create(parent);
    lv_obj_set_size(s_du.btn_save_pair, 110, 22);
    lv_obj_set_pos(s_du.btn_save_pair, 6, y);
    lv_obj_set_style_bg_color(s_du.btn_save_pair, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(s_du.btn_save_pair, 0, 0);
    __du_raised(s_du.btn_save_pair);
    lv_obj_add_event_cb(s_du.btn_save_pair, __du_save_pair_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(s_du.btn_save_pair);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(bl, &lv_font_unscii_8, 0);
    lv_label_set_text(bl, "Save & Pair");
    lv_obj_center(bl);

    /* Status label */
    s_du.pair_status_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(s_du.pair_status_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_pos(s_du.pair_status_lbl, 122, y + 6);
    lv_obj_set_width(s_du.pair_status_lbl, 330);
    lv_label_set_text(s_du.pair_status_lbl, "Ready.");
    lv_obj_set_style_text_color(s_du.pair_status_lbl, lv_color_hex(0x000000), 0);

    /* Immediate first update, then poll. */
    __du_pair_poll_cb(NULL);
    if (s_du.pair_poll_tmr == NULL) {
        s_du.pair_poll_tmr = lv_timer_create(__du_pair_poll_cb, DU_PAIR_POLL_MS, NULL);
    }
}

/**
 * @brief Open the Dial-Up Networking dialog.
 */
VOID_T win95_dialup_open(VOID_T)
{
    if (s_du.screen) {
        __du_close();
    }
    memset(&s_du, 0, sizeof(DIALUP_CTX_T));
    s_du.cur_tab = DU_TAB_DIRECT;

    s_du.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_du.screen);
    lv_obj_set_size(s_du.screen, DU_W, DU_H);
    lv_obj_set_pos(s_du.screen, 0, 0);
    lv_obj_set_style_bg_color(s_du.screen, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_du.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_du.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_du.screen);

    /* Title bar */
    lv_obj_t *tbar = lv_obj_create(s_du.screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, DU_W - 4, DU_TITLE_H);
    lv_obj_set_pos(tbar, 2, 2);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ttl = lv_label_create(tbar);
    lv_obj_set_style_text_color(ttl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_unscii_8, 0);
    lv_label_set_text(ttl, "Dial-Up Networking");
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *xb = lv_btn_create(tbar);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    __du_raised(xb);
    lv_obj_add_event_cb(xb, __du_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);

    /* Tab bar */
    INT32_T ty = DU_TITLE_H + 4;
    s_du.tab_btn_direct = lv_btn_create(s_du.screen);
    lv_obj_set_size(s_du.tab_btn_direct, 80, DU_TABBAR_H);
    lv_obj_set_pos(s_du.tab_btn_direct, 4, ty);
    lv_obj_set_style_bg_color(s_du.tab_btn_direct, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_radius(s_du.tab_btn_direct, 0, 0);
    __du_raised(s_du.tab_btn_direct);
    lv_obj_add_event_cb(s_du.tab_btn_direct, __du_tab_direct_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl1 = lv_label_create(s_du.tab_btn_direct);
    lv_obj_set_style_text_color(tl1, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(tl1, &lv_font_unscii_8, 0);
    lv_label_set_text(tl1, "Direct");
    lv_obj_center(tl1);

    s_du.tab_btn_pair = lv_btn_create(s_du.screen);
    lv_obj_set_size(s_du.tab_btn_pair, 90, DU_TABBAR_H);
    lv_obj_set_pos(s_du.tab_btn_pair, 88, ty);
    lv_obj_set_style_bg_color(s_du.tab_btn_pair, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_radius(s_du.tab_btn_pair, 0, 0);
    __du_raised(s_du.tab_btn_pair);
    lv_obj_add_event_cb(s_du.tab_btn_pair, __du_tab_pair_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl2 = lv_label_create(s_du.tab_btn_pair);
    lv_obj_set_style_text_color(tl2, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(tl2, &lv_font_unscii_8, 0);
    lv_label_set_text(tl2, "Tuya Pair");
    lv_obj_center(tl2);

    /* Tab container area */
    INT32_T cy = ty + DU_TABBAR_H + 2;
    INT32_T ch = DU_H - cy - DU_KB_H;

    s_du.tab_direct = lv_obj_create(s_du.screen);
    lv_obj_remove_style_all(s_du.tab_direct);
    lv_obj_set_size(s_du.tab_direct, DU_W - 8, ch);
    lv_obj_set_pos(s_du.tab_direct, 4, cy);
    lv_obj_set_style_bg_color(s_du.tab_direct, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_du.tab_direct, LV_OPA_COVER, 0);
    __du_sunken(s_du.tab_direct);
    lv_obj_clear_flag(s_du.tab_direct, LV_OBJ_FLAG_SCROLLABLE);

    s_du.tab_pair = lv_obj_create(s_du.screen);
    lv_obj_remove_style_all(s_du.tab_pair);
    lv_obj_set_size(s_du.tab_pair, DU_W - 8, ch);
    lv_obj_set_pos(s_du.tab_pair, 4, cy);
    lv_obj_set_style_bg_color(s_du.tab_pair, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_du.tab_pair, LV_OPA_COVER, 0);
    __du_sunken(s_du.tab_pair);
    lv_obj_clear_flag(s_du.tab_pair, LV_OBJ_FLAG_SCROLLABLE);

    __build_direct_tab(s_du.tab_direct);
    __build_pair_tab(s_du.tab_pair);

    /* Keyboard - shared by both tabs, hidden initially. */
    s_du.kb = win95_kb_create(s_du.screen);
    lv_obj_set_size(s_du.kb, DU_W, DU_KB_H);
    lv_obj_align(s_du.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    win95_kb_set_textarea(s_du.kb, s_du.ta_ssid);
    lv_obj_set_style_bg_color(s_du.kb, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_text_color(s_du.kb, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_du.kb, LV_OBJ_FLAG_HIDDEN);

    __du_select_tab(DU_TAB_DIRECT);
}
