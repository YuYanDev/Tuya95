/**
 * @file win95_dialup.h
 * @brief Win95 Dial-Up Networking dialog declarations
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_DIALUP_H__
#define __WIN95_DIALUP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Open the Dial-Up Networking dialog on the desktop
 * @return none
 */
VOID_T win95_dialup_open(VOID_T);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_DIALUP_H__ */
