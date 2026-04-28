/**
 * @file win95_pairing.c
 * @brief Tuya IoT pairing: BLE + phone-AP, driven from the Dial-Up dialog
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 *
 * This module owns the tuya_iot_client_t and the yield thread. The Win95
 * desktop only reads app->pair_state to drive the tray icon and the Dial-Up
 * status label.
 */
#include "win95_pairing.h"

#include "tal_api.h"
#include "tal_kv.h"
#include "tuya_iot.h"
#include "tuya_authorize.h"
#include "netmgr.h"
#include "netcfg.h"

#include <string.h>

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC tuya_iot_client_t s_client;
STATIC THREAD_HANDLE    s_yield_thread = NULL;
STATIC BOOL_T           s_started      = FALSE;
STATIC BIOS_APP_CTX_T  *s_app          = NULL;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief netmgr network check for tuya_iot
 * @return TRUE if link is up
 */
STATIC bool __pair_network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return (status == NETMGR_LINK_DOWN) ? false : true;
}

/**
 * @brief tuya_iot event handler - only updates pair_state for UI
 * @param[in] client cloud client
 * @param[in] event event info
 * @return none
 */
STATIC VOID_T __pair_event_cb(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    (VOID_T)client;
    if (s_app == NULL) {
        return;
    }

    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        s_app->pair_state = PAIR_ST_BIND_START;
        PR_INFO("Pair: BIND_START (waiting for phone app)");
        break;

    case TUYA_EVENT_BIND_TOKEN_ON:
        s_app->pair_state = PAIR_ST_BIND_TOKEN_ON;
        PR_INFO("Pair: BIND_TOKEN_ON");
        break;

    case TUYA_EVENT_MQTT_CONNECTED:
        s_app->pair_state = PAIR_ST_MQTT_CONNECTED;
        PR_NOTICE("Pair: MQTT_CONNECTED - device is online");
        break;

    case TUYA_EVENT_MQTT_DISCONNECT:
        s_app->pair_state = PAIR_ST_MQTT_DISCONNECT;
        PR_WARN("Pair: MQTT_DISCONNECT");
        break;

    case TUYA_EVENT_RESET:
        s_app->pair_state = PAIR_ST_RESET;
        PR_WARN("Pair: RESET requested");
        break;

    default:
        break;
    }
}

/**
 * @brief Yield thread body - drives the cloud state machine forever.
 * @param[in] arg unused
 * @return none
 */
STATIC VOID_T __pair_yield_thread(VOID_T *arg)
{
    (VOID_T)arg;
    for (;;) {
        tuya_iot_yield(&s_client);
    }
}

/**
 * @brief Check credential validity (non-empty)
 * @param[in] app app context
 * @return TRUE if PID, UUID, AUTHKEY all populated
 */
STATIC BOOL_T __creds_complete(CONST BIOS_APP_CTX_T *app)
{
    return (app->product_id[0] != '\0')
        && (app->uuid[0] != '\0')
        && (app->auth_key[0] != '\0');
}

OPERATE_RET win95_pairing_load_creds(BIOS_APP_CTX_T *app)
{
    if (app == NULL) {
        return OPRT_INVALID_PARM;
    }

    /* PID from KV */
    uint8_t *pid_buf = NULL;
    size_t pid_len = 0;
    if (tal_kv_get(KV_KEY_SIM_PID, &pid_buf, &pid_len) == OPRT_OK && pid_buf && pid_len > 0) {
        size_t copy = (pid_len < PID_MAX_LEN) ? pid_len : PID_MAX_LEN;
        memcpy(app->product_id, pid_buf, copy);
        app->product_id[copy] = '\0';
        tal_kv_free(pid_buf);
    }

    /* UUID + AUTHKEY from authorize KV/OTP */
    tuya_iot_license_t license;
    memset(&license, 0, sizeof(license));
    if (tuya_authorize_read(&license) == OPRT_OK) {
        if (license.uuid) {
            strncpy(app->uuid, license.uuid, UUID_MAX_LEN);
            app->uuid[UUID_MAX_LEN] = '\0';
        }
        if (license.authkey) {
            strncpy(app->auth_key, license.authkey, AUTHKEY_MAX_LEN);
            app->auth_key[AUTHKEY_MAX_LEN] = '\0';
        }
    }

    app->pair_state = __creds_complete(app) ? PAIR_ST_IDLE : PAIR_ST_CREDS_MISSING;
    PR_NOTICE("Pair creds: PID=%s UUID=%.8s... AUTHKEY=%.4s... -> state=%d",
              app->product_id[0] ? app->product_id : "(empty)",
              app->uuid[0] ? app->uuid : "",
              app->auth_key[0] ? app->auth_key : "",
              (int)app->pair_state);
    return OPRT_OK;
}

OPERATE_RET win95_pairing_save_creds(CONST CHAR_T *pid,
                                      CONST CHAR_T *uuid,
                                      CONST CHAR_T *authkey)
{
    if (pid == NULL || uuid == NULL || authkey == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (pid[0] == '\0' || uuid[0] == '\0' || authkey[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    /* Save PID to KV first; if the reboot-after-authorize happens, we want
     * the PID to already be in flash when we come back up. */
    OPERATE_RET rt = tal_kv_set(KV_KEY_SIM_PID, (CONST uint8_t *)pid, strlen(pid));
    if (rt != OPRT_OK) {
        PR_ERR("tal_kv_set(sim_pid) failed: %d", rt);
        return rt;
    }

    /* tuya_authorize_write() reboots on success, so this normally does not
     * return. If it does return, it failed. */
    rt = tuya_authorize_write(uuid, authkey);
    PR_ERR("tuya_authorize_write returned %d (did not reboot)", rt);
    return rt;
}

OPERATE_RET win95_pairing_start(BIOS_APP_CTX_T *app)
{
    if (app == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (s_started) {
        PR_DEBUG("Pair already started");
        return OPRT_OK;
    }
    if (!__creds_complete(app)) {
        app->pair_state = PAIR_ST_CREDS_MISSING;
        PR_WARN("Pair skipped - creds incomplete");
        return OPRT_OK;
    }

    s_app = app;

    tuya_iot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.productkey     = app->product_id;
    cfg.uuid           = app->uuid;
    cfg.authkey        = app->auth_key;
    cfg.software_ver   = PROJECT_VERSION;
    cfg.event_handler  = __pair_event_cb;
    cfg.network_check  = __pair_network_check;

    OPERATE_RET rt = tuya_iot_init(&s_client, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("tuya_iot_init failed: %d", rt);
        app->pair_state = PAIR_ST_FAILED;
        return rt;
    }

    /* Bring up WiFi netmgr + BLE/AP netcfg, same as your_chat_bot. */
    netmgr_init(NETCONN_WIFI);
    netcfg_args_t args;
    memset(&args, 0, sizeof(args));
    args.type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP;
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &args);

    rt = tuya_iot_start(&s_client);
    if (rt != OPRT_OK) {
        PR_ERR("tuya_iot_start failed: %d", rt);
        app->pair_state = PAIR_ST_FAILED;
        return rt;
    }

    THREAD_CFG_T tcfg = {4096, 4, "w95_pair_yield"};
    rt = tal_thread_create_and_start(&s_yield_thread, NULL, NULL,
                                     __pair_yield_thread, NULL, &tcfg);
    if (rt != OPRT_OK) {
        PR_ERR("yield thread create failed: %d", rt);
        app->pair_state = PAIR_ST_FAILED;
        return rt;
    }

    s_started = TRUE;
    app->pair_state = PAIR_ST_INITED;
    PR_NOTICE("Pair started: PID=%s", app->product_id);
    return OPRT_OK;
}

OPERATE_RET win95_pairing_load_wifi(BIOS_APP_CTX_T *app)
{
    if (app == NULL) {
        return OPRT_INVALID_PARM;
    }
    uint8_t *buf = NULL;
    size_t len = 0;

    if (tal_kv_get(KV_KEY_WIFI_SSID, &buf, &len) == OPRT_OK && buf && len > 0) {
        size_t cp = (len < SSID_MAX_LEN) ? len : SSID_MAX_LEN;
        memcpy(app->wifi_ssid, buf, cp);
        app->wifi_ssid[cp] = '\0';
        tal_kv_free(buf);
    }

    buf = NULL; len = 0;
    if (tal_kv_get(KV_KEY_WIFI_PASS, &buf, &len) == OPRT_OK && buf && len > 0) {
        size_t cp = (len < PASSWORD_MAX_LEN) ? len : PASSWORD_MAX_LEN;
        memcpy(app->wifi_pass, buf, cp);
        app->wifi_pass[cp] = '\0';
        tal_kv_free(buf);
    }

    PR_DEBUG("WiFi creds loaded: ssid=%s", app->wifi_ssid[0] ? app->wifi_ssid : "(none)");
    return OPRT_OK;
}

OPERATE_RET win95_pairing_save_wifi(CONST CHAR_T *ssid, CONST CHAR_T *pass)
{
    if (ssid == NULL || pass == NULL) {
        return OPRT_INVALID_PARM;
    }
    OPERATE_RET rt = tal_kv_set(KV_KEY_WIFI_SSID, (CONST uint8_t *)ssid, strlen(ssid));
    if (rt != OPRT_OK) {
        PR_ERR("tal_kv_set(wifi_ssid) failed: %d", rt);
        return rt;
    }
    rt = tal_kv_set(KV_KEY_WIFI_PASS, (CONST uint8_t *)pass, strlen(pass));
    if (rt != OPRT_OK) {
        PR_ERR("tal_kv_set(wifi_pass) failed: %d", rt);
    }
    return rt;
}

OPERATE_RET win95_pairing_load_tz(BIOS_APP_CTX_T *app)
{
    if (app == NULL) {
        return OPRT_INVALID_PARM;
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (tal_kv_get(KV_KEY_TZ_OFFSET, &buf, &len) == OPRT_OK && buf && len == sizeof(INT32_T)) {
        INT32_T val;
        memcpy(&val, buf, sizeof(INT32_T));
        if (val >= -720 && val <= 840) {
            app->tz_offset_minutes = val;
        }
        tal_kv_free(buf);
    }
    return OPRT_OK;
}

OPERATE_RET win95_pairing_save_tz(INT32_T tz_offset_minutes)
{
    return tal_kv_set(KV_KEY_TZ_OFFSET, (CONST uint8_t *)&tz_offset_minutes, sizeof(INT32_T));
}
