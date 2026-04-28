/**
 * @file win95_desktop.h
 * @brief Windows 95 style desktop UI declarations
 * @version 1.1.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_DESKTOP_H__
#define __WIN95_DESKTOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define WIN95_TASKBAR_H     28
#define WIN95_START_BTN_W   60
#define WIN95_START_BTN_H   22
#define WIN95_START_MENU_W  150
#define WIN95_WINDOW_TITLE_H 18

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize and show Win95 desktop
 * @return none
 */
VOID_T win95_desktop_init(VOID_T);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_DESKTOP_H__ */
