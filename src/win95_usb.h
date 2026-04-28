/**
 * @file win95_usb.h
 * @brief USB HID host — keyboard and mouse support via CherryUSB.
 *        Registers LVGL keypad and pointer input devices backed by USB HID
 *        boot-protocol reports from any connected keyboard/mouse.
 */
#ifndef WIN95_USB_H
#define WIN95_USB_H

#include "tal_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USB HID host and register LVGL input devices.
 *        Safe to call before any USB device is connected — starts a background
 *        thread that polls for /dev/input0 (keyboard) and /dev/input1 (mouse).
 */
VOID_T win95_usb_init(VOID_T);

/**
 * @brief Shut down USB HID host and unregister LVGL input devices.
 */
VOID_T win95_usb_deinit(VOID_T);

#ifdef __cplusplus
}
#endif
#endif /* WIN95_USB_H */
