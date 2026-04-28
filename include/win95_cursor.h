/**
 * @file win95_cursor.h
 * @brief Touch-tracking arrow cursor for Win95 desktop
 */
#ifndef __WIN95_CURSOR_H__
#define __WIN95_CURSOR_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/**
 * @brief Initialise the cursor object and bind it to the touch indev.
 *        Must be called after lv_vendor_init().
 */
VOID_T win95_cursor_init(VOID_T);

/**
 * @brief Show or hide the cursor.
 * @param[in] visible TRUE = visible, FALSE = hidden
 */
VOID_T win95_cursor_set_visible(BOOL_T visible);

/**
 * @brief Switch between the normal arrow and the hourglass busy cursor.
 * @param[in] busy TRUE = hourglass, FALSE = arrow
 */
VOID_T win95_cursor_set_busy(BOOL_T busy);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_CURSOR_H__ */
