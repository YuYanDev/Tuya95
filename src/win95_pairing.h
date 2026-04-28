/**
 * @file win95_pairing.h
 * @brief Tuya IoT pairing lifecycle (BLE + phone-AP) for the 95Simulator desktop
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_PAIRING_H__
#define __WIN95_PAIRING_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

#define KV_KEY_SIM_PID   "sim_pid"
#define KV_KEY_WIFI_SSID "wifi_ssid"
#define KV_KEY_WIFI_PASS "wifi_pass"
#define KV_KEY_TZ_OFFSET "tz_offset"

/**
 * @brief Load PID (KV) and UUID/AUTHKEY (authorize) into the app context.
 *        Updates app->pair_state to PAIR_ST_CREDS_MISSING or PAIR_ST_IDLE.
 * @param[in,out] app app context
 * @return OPRT_OK always (missing creds are not an error)
 */
OPERATE_RET win95_pairing_load_creds(BIOS_APP_CTX_T *app);

/**
 * @brief Save PID to KV and UUID/AUTHKEY via tuya_authorize_write.
 *        NOTE: tuya_authorize_write triggers tal_system_reset() on success,
 *              so this call does not return on success.
 * @param[in] pid       product id (ASCII, <= PID_MAX_LEN)
 * @param[in] uuid      device uuid (20 bytes typical)
 * @param[in] authkey   32-byte authkey
 * @return OPRT_INVALID_PARM on bad input; otherwise whatever
 *         tuya_authorize_write returns (it should reboot).
 */
OPERATE_RET win95_pairing_save_creds(CONST CHAR_T *pid,
                                      CONST CHAR_T *uuid,
                                      CONST CHAR_T *authkey);

/**
 * @brief Start tuya_iot + netmgr + BLE/AP netcfg + spawn the yield thread.
 *        Safe to call only once per boot. If creds are missing, it is a no-op
 *        and app->pair_state stays at PAIR_ST_CREDS_MISSING.
 * @param[in] app app context (owns the PID/UUID/AUTHKEY string buffers)
 * @return OPRT_OK on success
 */
OPERATE_RET win95_pairing_start(BIOS_APP_CTX_T *app);

/**
 * @brief Load WiFi SSID/password from KV into app context.
 * @param[in,out] app app context
 * @return OPRT_OK always (missing is not an error)
 */
OPERATE_RET win95_pairing_load_wifi(BIOS_APP_CTX_T *app);

/**
 * @brief Persist WiFi SSID/password to KV flash.
 * @param[in] ssid WiFi network name
 * @param[in] pass WiFi password (may be empty)
 * @return OPRT_OK on success
 */
OPERATE_RET win95_pairing_save_wifi(CONST CHAR_T *ssid, CONST CHAR_T *pass);

/**
 * @brief Load timezone offset from KV into app context.
 * @param[in,out] app app context
 * @return OPRT_OK always (missing is not an error)
 */
OPERATE_RET win95_pairing_load_tz(BIOS_APP_CTX_T *app);

/**
 * @brief Persist timezone offset (minutes, -720 to +840) to KV flash.
 * @param[in] tz_offset_minutes UTC offset in minutes (e.g. IST = +330)
 * @return OPRT_OK on success
 */
OPERATE_RET win95_pairing_save_tz(INT32_T tz_offset_minutes);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_PAIRING_H__ */
