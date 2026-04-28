/**
 * @file bios_simulator.h
 * @brief BiosSimulator - BIOS + Win95 Desktop for Tuya T5
 * @version 2.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __BIOS_SIMULATOR_H__
#define __BIOS_SIMULATOR_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"
#include "lvgl.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define BIOS_SCREEN_WIDTH   480
#define BIOS_SCREEN_HEIGHT  320

#define BIOS_COLOR_BG       0x0000AA
#define BIOS_COLOR_TITLE_BG 0xAAAAAA
#define BIOS_COLOR_TEXT      0xFFFFFF
#define BIOS_COLOR_HIGHLIGHT 0xFFFF00
#define BIOS_COLOR_SELECT_BG 0xAAAAAA
#define BIOS_COLOR_SELECT_FG 0x000000
#define BIOS_COLOR_BORDER    0xFFFFFF

#define WIN95_COLOR_DESKTOP  0x008080
#define WIN95_COLOR_TASKBAR  0xC0C0C0
#define WIN95_COLOR_START_BG 0xC0C0C0
#define WIN95_COLOR_START_FG 0x000000
#define WIN95_COLOR_TITLEBAR 0x000080
#define WIN95_COLOR_WINDOW   0xC0C0C0
#define WIN95_COLOR_SHADOW   0x808080
#define WIN95_COLOR_LIGHT    0xFFFFFF

#define UUID_MAX_LEN         25
#define AUTHKEY_MAX_LEN      32
#define SSID_MAX_LEN         64
#define PASSWORD_MAX_LEN     64
#define IP_STR_MAX_LEN       16
#define PID_MAX_LEN          16

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    APP_STATE_BIOS = 0,
    APP_STATE_DESKTOP,
} APP_STATE_E;

typedef enum {
    WIFI_ST_IDLE = 0,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,
    WIFI_ST_DISCONNECTED,
} WIFI_STATE_E;

/* Tuya pairing state, updated from the cloud event handler */
typedef enum {
    PAIR_ST_IDLE = 0,        /* not started */
    PAIR_ST_CREDS_MISSING,   /* PID/UUID/AUTHKEY not all present */
    PAIR_ST_INITED,          /* tuya_iot_init + netmgr started, no events yet */
    PAIR_ST_BIND_START,      /* BLE/AP waiting for phone app */
    PAIR_ST_BIND_TOKEN_ON,
    PAIR_ST_MQTT_CONNECTED,  /* fully online */
    PAIR_ST_MQTT_DISCONNECT,
    PAIR_ST_RESET,
    PAIR_ST_FAILED,
} PAIR_STATE_E;

typedef struct {
    CHAR_T product_id[PID_MAX_LEN + 1];
    CHAR_T uuid[UUID_MAX_LEN + 1];
    CHAR_T auth_key[AUTHKEY_MAX_LEN + 1];
    CHAR_T wifi_ssid[SSID_MAX_LEN + 1];
    CHAR_T wifi_pass[PASSWORD_MAX_LEN + 1];
    CHAR_T wifi_ip[IP_STR_MAX_LEN];
    INT32_T tz_offset_minutes;
    APP_STATE_E state;
    volatile WIFI_STATE_E wifi_state;
    volatile PAIR_STATE_E pair_state;
} BIOS_APP_CTX_T;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Get global app context
 * @return pointer to app context
 */
BIOS_APP_CTX_T *bios_app_get_ctx(VOID_T);

/**
 * @brief Ensure the legacy tal_wifi station path is initialized.
 * @return OPRT_OK on success
 */
OPERATE_RET bios_wifi_legacy_ensure_init(VOID_T);

/**
 * @brief Initialize and show BIOS UI
 * @return none
 */
VOID_T bios_ui_init(VOID_T);

/**
 * @brief Initialize and show Win95 desktop
 * @return none
 */
VOID_T win95_desktop_init(VOID_T);

/**
 * @brief Show BIOS network config screen
 * @return none
 */
VOID_T bios_config_show_network(VOID_T);

/**
 * @brief Show BIOS UUID/AUTH_KEY config screen
 * @return none
 */
VOID_T bios_config_show_auth(VOID_T);

/**
 * @brief Open Win95 Dial-Up Networking dialog
 * @return none
 */
VOID_T win95_dialup_open(VOID_T);

/**
 * @brief Open Win95 Internet Explorer browser
 * @return none
 */
VOID_T win95_ie_open(VOID_T);

/**
 * @brief Open Win95 Notepad text editor
 * @return none
 */
VOID_T win95_notepad_open(VOID_T);

/**
 * @brief Open Win95 MS-DOS Prompt terminal
 * @return none
 */
VOID_T win95_dos_open(VOID_T);

/**
 * @brief Open Win95 My Computer window
 * @return none
 */
VOID_T win95_mypc_open(VOID_T);

/**
 * @brief Open Win95 Recycle Bin window
 * @return none
 */
VOID_T win95_recycle_open(VOID_T);

/**
 * @brief Update taskbar network indicator
 * @param[in] connected TRUE shows icon, FALSE hides it
 * @return none
 */
VOID_T win95_taskbar_set_net(BOOL_T connected);

#ifdef __cplusplus
}
#endif
#endif /* __BIOS_SIMULATOR_H__ */
