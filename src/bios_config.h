/**
 * @file bios_config.h
 * @brief BIOS configuration screens for network and auth
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __BIOS_CONFIG_H__
#define __BIOS_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
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

#ifdef __cplusplus
}
#endif
#endif /* __BIOS_CONFIG_H__ */
