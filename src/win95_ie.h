/**
 * @file win95_ie.h
 * @brief Win95 Internet Explorer browser declarations
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_IE_H__
#define __WIN95_IE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Open the Internet Explorer browser window
 * @return none
 */
VOID_T win95_ie_open(VOID_T);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_IE_H__ */
