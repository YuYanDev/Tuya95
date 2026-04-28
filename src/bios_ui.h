/**
 * @file bios_ui.h
 * @brief BIOS style UI declarations
 * @version 1.1.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __BIOS_UI_H__
#define __BIOS_UI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define BIOS_MENU_ITEM_MAX  6
#define BIOS_TITLE_H        22
#define BIOS_BOTTOM_H       16
#define BIOS_MENU_ITEM_H    20
#define BIOS_MENU_PAD       4

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    BIOS_MENU_SYS_INFO = 0,
    BIOS_MENU_NET_CONFIG,
    BIOS_MENU_AUTH_CONFIG,
    BIOS_MENU_ENTRY_SYSTEM,
    BIOS_MENU_COUNT,
} BIOS_MENU_ITEM_E;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize and show BIOS UI
 * @return none
 */
VOID_T bios_ui_init(VOID_T);

/**
 * @brief Return to BIOS main menu from sub-screen
 * @return none
 */
VOID_T bios_ui_return_main(VOID_T);

#ifdef __cplusplus
}
#endif
#endif /* __BIOS_UI_H__ */
