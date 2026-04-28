/**
 * @file win95_usb.c
 * @brief USB HID host — keyboard and mouse via CherryUSB.
 *
 * On init: opens USB host, registers LVGL keypad + pointer indevs,
 * spawns a reader thread that submits interrupt URBs to /dev/input0
 * (keyboard) and /dev/input1 (mouse) and feeds events into ring buffers.
 *
 * Keyboard: USB HID boot-protocol report — 8 bytes
 *   [0] modifiers (bit0=LCtrl bit1=LShift bit2=LAlt bit3=LGui
 *                  bit4=RCtrl bit5=RShift bit6=RAlt bit7=RGui)
 *   [1] reserved
 *   [2..7] up to 6 simultaneous HID keycodes (0=none)
 *
 * Mouse: USB HID boot-protocol report — 4 bytes
 *   [0] buttons (bit0=left bit1=right bit2=middle)
 *   [1] dx (signed 8-bit)
 *   [2] dy (signed 8-bit)
 *   [3] wheel (signed 8-bit, ignored here)
 */
#include "win95_usb.h"
#include "lv_vendor.h"
#include "tal_api.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Platform USB headers (T5AI / CherryUSB)
 * --------------------------------------------------------------------------- */
#if defined(CONFIG_USBH_HID) && CONFIG_USBH_HID

#include "components/usb.h"
#include "components/cherryusb/usbh_core.h"
#include "components/cherryusb/usbh_hid.h"

/* ---------------------------------------------------------------------------
 * Key event ring buffer
 * --------------------------------------------------------------------------- */
#define KB_BUF_SZ   32

typedef struct {
    UINT32_T key;
    BOOL_T   pressed;
} KB_EV_T;

typedef struct {
    KB_EV_T buf[KB_BUF_SZ];
    INT32_T head, tail;
} KB_RING_T;

/* ---------------------------------------------------------------------------
 * Mouse state
 * --------------------------------------------------------------------------- */
typedef struct {
    INT32_T x, y;
    BOOL_T  lbtn;
} MOUSE_ST_T;

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_indev_t  *kb_indev;
    lv_indev_t  *mouse_indev;
    THREAD_HANDLE reader_thread;
    volatile BOOL_T running;
    KB_RING_T    kb_ring;
    MOUSE_ST_T   mouse;
    UINT8_T      kb_prev[6];   /* previous keycode report for edge detection */
    UINT8_T      kb_buf[8];    /* current keyboard report */
    UINT8_T      ms_buf[4];    /* current mouse report */
} USB_HID_CTX_T;

STATIC USB_HID_CTX_T s_usb;

/* ---------------------------------------------------------------------------
 * Ring buffer helpers (called only from reader thread + indev cb)
 * --------------------------------------------------------------------------- */
STATIC VOID_T __kb_push(UINT32_T key, BOOL_T pressed)
{
    INT32_T next = (s_usb.kb_ring.tail + 1) % KB_BUF_SZ;
    if (next == s_usb.kb_ring.head) return; /* full — drop */
    s_usb.kb_ring.buf[s_usb.kb_ring.tail].key     = key;
    s_usb.kb_ring.buf[s_usb.kb_ring.tail].pressed = pressed;
    s_usb.kb_ring.tail = next;
}

STATIC BOOL_T __kb_pop(UINT32_T *key, BOOL_T *pressed)
{
    if (s_usb.kb_ring.head == s_usb.kb_ring.tail) return FALSE;
    *key     = s_usb.kb_ring.buf[s_usb.kb_ring.head].key;
    *pressed = s_usb.kb_ring.buf[s_usb.kb_ring.head].pressed;
    s_usb.kb_ring.head = (s_usb.kb_ring.head + 1) % KB_BUF_SZ;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * HID keycode → LVGL key mapping
 * Covers printable ASCII plus navigation keys
 * --------------------------------------------------------------------------- */
STATIC UINT32_T __hid_to_lv_key(UINT8_T hid, UINT8_T modifiers)
{
    BOOL_T shift = (modifiers & 0x22u) != 0; /* LShift | RShift */

    if (hid >= 0x04 && hid <= 0x1D) {
        /* a-z */
        CHAR_T base = (CHAR_T)('a' + (hid - 0x04));
        return shift ? (UINT32_T)(base - 32) : (UINT32_T)base;
    }
    if (hid >= 0x1E && hid <= 0x26) {
        /* 1-9 */
        static CONST CHAR_T shifted[] = "!@#$%^&*(";
        CHAR_T base = (CHAR_T)('1' + (hid - 0x1E));
        return shift ? (UINT32_T)(UINT8_T)shifted[hid - 0x1E] : (UINT32_T)base;
    }
    if (hid == 0x27) return shift ? ')' : '0';

    switch (hid) {
    case 0x28: case 0x58: return LV_KEY_ENTER;
    case 0x29: return LV_KEY_ESC;
    case 0x2A: return LV_KEY_BACKSPACE;
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x4F: return LV_KEY_RIGHT;
    case 0x50: return LV_KEY_LEFT;
    case 0x51: return LV_KEY_DOWN;
    case 0x52: return LV_KEY_UP;
    case 0x4A: return LV_KEY_HOME;
    case 0x4D: return LV_KEY_END;
    case 0x4B: return LV_KEY_PREV;
    case 0x4E: return LV_KEY_NEXT;
    case 0x4C: return LV_KEY_DEL;
    default:   return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Process a keyboard report: detect keydown / keyup edges
 * --------------------------------------------------------------------------- */
STATIC VOID_T __process_kb_report(UINT8_T modifiers)
{
    UINT8_T *cur = &s_usb.kb_buf[2];

    /* Key-up: was in prev, not in cur */
    for (INT32_T i = 0; i < 6; i++) {
        UINT8_T k = s_usb.kb_prev[i];
        if (k == 0 || k == 0x01) continue;
        BOOL_T found = FALSE;
        for (INT32_T j = 0; j < 6; j++) {
            if (cur[j] == k) { found = TRUE; break; }
        }
        if (!found) {
            UINT32_T lv = __hid_to_lv_key(k, modifiers);
            if (lv) __kb_push(lv, FALSE);
        }
    }

    /* Key-down: in cur, not in prev */
    for (INT32_T i = 0; i < 6; i++) {
        UINT8_T k = cur[i];
        if (k == 0 || k == 0x01) continue;
        BOOL_T found = FALSE;
        for (INT32_T j = 0; j < 6; j++) {
            if (s_usb.kb_prev[j] == k) { found = TRUE; break; }
        }
        if (!found) {
            UINT32_T lv = __hid_to_lv_key(k, modifiers);
            if (lv) __kb_push(lv, TRUE);
        }
    }

    memcpy(s_usb.kb_prev, cur, 6);
}

/* ---------------------------------------------------------------------------
 * LVGL keyboard indev read callback
 * --------------------------------------------------------------------------- */
STATIC VOID_T __kb_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (VOID_T)indev;
    UINT32_T key;
    BOOL_T pressed;
    if (__kb_pop(&key, &pressed)) {
        data->key   = key;
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = (s_usb.kb_ring.head != s_usb.kb_ring.tail);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ---------------------------------------------------------------------------
 * LVGL mouse pointer indev read callback
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (VOID_T)indev;
    data->point.x = (lv_coord_t)s_usb.mouse.x;
    data->point.y = (lv_coord_t)s_usb.mouse.y;
    data->state   = s_usb.mouse.lbtn ? LV_INDEV_STATE_PRESSED
                                      : LV_INDEV_STATE_RELEASED;
}

/* ---------------------------------------------------------------------------
 * HID reader thread
 * --------------------------------------------------------------------------- */
STATIC VOID_T __usb_reader_thread(VOID_T *arg)
{
    (VOID_T)arg;

    while (s_usb.running) {
        struct usbh_hid *kb = NULL, *ms = NULL;

        /* Try keyboard */
        kb = (struct usbh_hid *)usbh_find_class_instance("/dev/input0");
        if (kb && kb->intin) {
            struct usbh_urb urb;
            memset(&urb, 0, sizeof(urb));
            usbh_int_urb_fill(&urb, kb->intin,
                              s_usb.kb_buf, sizeof(s_usb.kb_buf),
                              50, NULL, NULL);
            INT32_T ret = usbh_submit_urb(&urb);
            if (ret == 0 || ret == -ETIMEDOUT) {
                if (ret == 0 && urb.actual_length >= 3) {
                    __process_kb_report(s_usb.kb_buf[0]);
                }
            }
        }

        /* Try mouse */
        ms = (struct usbh_hid *)usbh_find_class_instance("/dev/input1");
        if (ms && ms->intin) {
            struct usbh_urb urb;
            memset(&urb, 0, sizeof(urb));
            usbh_int_urb_fill(&urb, ms->intin,
                              s_usb.ms_buf, sizeof(s_usb.ms_buf),
                              50, NULL, NULL);
            INT32_T ret = usbh_submit_urb(&urb);
            if (ret == 0 && urb.actual_length >= 3) {
                INT32_T dx = (INT8_T)s_usb.ms_buf[1];
                INT32_T dy = (INT8_T)s_usb.ms_buf[2];
                s_usb.mouse.x += dx;
                s_usb.mouse.y += dy;
                if (s_usb.mouse.x < 0)   s_usb.mouse.x = 0;
                if (s_usb.mouse.y < 0)   s_usb.mouse.y = 0;
                if (s_usb.mouse.x > 479) s_usb.mouse.x = 479;
                if (s_usb.mouse.y > 319) s_usb.mouse.y = 319;
                s_usb.mouse.lbtn = (s_usb.ms_buf[0] & 0x01u) != 0;
            }
        }

        /* If neither device found yet, wait longer */
        if (!kb && !ms) {
            tal_system_sleep(200);
        }
    }

    tal_thread_delete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_usb_init(VOID_T)
{
    if (s_usb.running) return;
    memset(&s_usb, 0, sizeof(s_usb));
    s_usb.mouse.x = 240;   /* start cursor at screen centre */
    s_usb.mouse.y = 160;

    /* USB host should already be open from platform init; call is safe to
     * repeat. Mode 0 = USB_HOST_MODE. */
    bk_usb_open(0u);
    bk_usb_driver_init();

    /* Register LVGL keypad indev */
    s_usb.kb_indev = lv_indev_create();
    lv_indev_set_type(s_usb.kb_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_usb.kb_indev, __kb_read_cb);

    /* Register LVGL pointer indev for mouse */
    s_usb.mouse_indev = lv_indev_create();
    lv_indev_set_type(s_usb.mouse_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_usb.mouse_indev, __mouse_read_cb);

    /* Start reader thread */
    s_usb.running = TRUE;
    THREAD_CFG_T cfg = {4096, 4, "usb_hid"};
    tal_thread_create_and_start(&s_usb.reader_thread, NULL, NULL,
                                __usb_reader_thread, NULL, &cfg);

    PR_DEBUG("USB HID host started");
}

VOID_T win95_usb_deinit(VOID_T)
{
    if (!s_usb.running) return;
    s_usb.running = FALSE;

    if (s_usb.kb_indev) {
        lv_indev_delete(s_usb.kb_indev);
        s_usb.kb_indev = NULL;
    }
    if (s_usb.mouse_indev) {
        lv_indev_delete(s_usb.mouse_indev);
        s_usb.mouse_indev = NULL;
    }
}

#else /* CONFIG_USBH_HID not enabled */

VOID_T win95_usb_init(VOID_T)  { }
VOID_T win95_usb_deinit(VOID_T) { }

#endif /* CONFIG_USBH_HID */
