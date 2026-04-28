/**
 * @file win95_kb.c
 * @brief Win95 Smart-ABC style pixel keyboard.
 *
 * Layout (480 × 110 px, positioned at bottom of screen):
 *
 *  Row 0  ` 1 2 3 4 5 6 7 8 9 0 - = [BS]
 *  Row 1  [Tab] q w e r t y u i o p [ ] [\]
 *  Row 2  [Caps] a s d f g h j k l ; ' [Enter]
 *  Row 3  [Shift] z x c v b n m , . / [Shift]
 *  Row 4  [Space]
 *
 * Each key is a plain lv_obj with raised Win95 3D border styling.
 * Shift / Caps toggles relabel all alpha keys between upper and lower case.
 */
#include "win95_kb.h"
#include "win95_desktop.h"    /* WIN95 colour constants */
#include "bios_simulator.h"
#include "lv_vendor.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Dimensions
 * --------------------------------------------------------------------------- */
#define KB_W        BIOS_SCREEN_WIDTH   /* 480 */
#define KB_H        110
#define KEY_H       20
#define ROW_GAP     2
#define KEY_GAP     2
#define MARGIN_X    4
#define MARGIN_Y    2

/* Row Y positions */
#define ROW_Y(r)  (MARGIN_Y + (r) * (KEY_H + ROW_GAP))

/* ---------------------------------------------------------------------------
 * Key type constants
 * --------------------------------------------------------------------------- */
#define KT_CHAR   0
#define KT_BKSP   1
#define KT_ENTER  2
#define KT_SHIFT  3
#define KT_CAPS   4
#define KT_TAB    5
#define KT_SPACE  6

/* ---------------------------------------------------------------------------
 * Key descriptor
 * --------------------------------------------------------------------------- */
typedef struct {
    CONST CHAR_T *label;    /* key face text */
    CONST CHAR_T *normal;   /* inserted char (NULL for special keys) */
    CONST CHAR_T *shifted;  /* shifted insert (NULL = uppercase of normal) */
    INT32_T       type;
    INT32_T       w;        /* pixel width */
} KB_KEY_T;

/* ---------------------------------------------------------------------------
 * Row definitions
 * x position is computed at build time from key widths + gaps, starting at MARGIN_X.
 * Row 0: 13 normal + BS (widths sum to 472 with 13×2 gaps)
 * Row 1: Tab(46) + 12×30 + \(40)  + 13×2 = 46+360+40+26 = 472
 * Row 2: Caps(52) + 11×30 + Enter(60) + 12×2 = 52+330+60+24 = 466 ... adjust
 * Row 3: LShift(64) + 10×30 + RShift(78) + 11×2 = 64+300+78+22 = 464 ... adjust
 * Row 4: Space(472)
 * --------------------------------------------------------------------------- */
STATIC CONST KB_KEY_T s_row0[] = {
    {"`",   "`",  "~",  KT_CHAR, 30},
    {"1",   "1",  "!",  KT_CHAR, 30},
    {"2",   "2",  "@",  KT_CHAR, 30},
    {"3",   "3",  "#",  KT_CHAR, 30},
    {"4",   "4",  "$",  KT_CHAR, 30},
    {"5",   "5",  "%",  KT_CHAR, 30},
    {"6",   "6",  "^",  KT_CHAR, 30},
    {"7",   "7",  "&",  KT_CHAR, 30},
    {"8",   "8",  "*",  KT_CHAR, 30},
    {"9",   "9",  "(",  KT_CHAR, 30},
    {"0",   "0",  ")",  KT_CHAR, 30},
    {"-",   "-",  "_",  KT_CHAR, 30},
    {"=",   "=",  "+",  KT_CHAR, 30},
    {"BS",  NULL, NULL, KT_BKSP, 56},
};
STATIC CONST KB_KEY_T s_row1[] = {
    {"Tab", NULL, NULL, KT_TAB,  46},
    {"q",   "q",  "Q",  KT_CHAR, 30},
    {"w",   "w",  "W",  KT_CHAR, 30},
    {"e",   "e",  "E",  KT_CHAR, 30},
    {"r",   "r",  "R",  KT_CHAR, 30},
    {"t",   "t",  "T",  KT_CHAR, 30},
    {"y",   "y",  "Y",  KT_CHAR, 30},
    {"u",   "u",  "U",  KT_CHAR, 30},
    {"i",   "i",  "I",  KT_CHAR, 30},
    {"o",   "o",  "O",  KT_CHAR, 30},
    {"p",   "p",  "P",  KT_CHAR, 30},
    {"[",   "[",  "{",  KT_CHAR, 30},
    {"]",   "]",  "}",  KT_CHAR, 30},
    {"\\",  "\\", "|",  KT_CHAR, 40},
};
STATIC CONST KB_KEY_T s_row2[] = {
    {"Caps", NULL, NULL, KT_CAPS, 52},
    {"a",   "a",  "A",  KT_CHAR, 30},
    {"s",   "s",  "S",  KT_CHAR, 30},
    {"d",   "d",  "D",  KT_CHAR, 30},
    {"f",   "f",  "F",  KT_CHAR, 30},
    {"g",   "g",  "G",  KT_CHAR, 30},
    {"h",   "h",  "H",  KT_CHAR, 30},
    {"j",   "j",  "J",  KT_CHAR, 30},
    {"k",   "k",  "K",  KT_CHAR, 30},
    {"l",   "l",  "L",  KT_CHAR, 30},
    {";",   ";",  ":",  KT_CHAR, 30},
    {"'",   "'",  "\"", KT_CHAR, 30},
    {"Ent", NULL, NULL, KT_ENTER, 58},
};
STATIC CONST KB_KEY_T s_row3[] = {
    {"Shift", NULL, NULL, KT_SHIFT, 64},
    {"z",   "z",  "Z",  KT_CHAR, 30},
    {"x",   "x",  "X",  KT_CHAR, 30},
    {"c",   "c",  "C",  KT_CHAR, 30},
    {"v",   "v",  "V",  KT_CHAR, 30},
    {"b",   "b",  "B",  KT_CHAR, 30},
    {"n",   "n",  "N",  KT_CHAR, 30},
    {"m",   "m",  "M",  KT_CHAR, 30},
    {",",   ",",  "<",  KT_CHAR, 30},
    {".",   ".",  ">",  KT_CHAR, 30},
    {"/",   "/",  "?",  KT_CHAR, 30},
    {"Shift", NULL, NULL, KT_SHIFT, 76},
};
STATIC CONST KB_KEY_T s_row4[] = {
    {"Space", NULL, NULL, KT_SPACE, 472},
};

typedef struct {
    CONST KB_KEY_T *keys;
    INT32_T         count;
} ROW_T;

STATIC CONST ROW_T s_rows[] = {
    {s_row0, (INT32_T)(sizeof(s_row0)/sizeof(s_row0[0]))},
    {s_row1, (INT32_T)(sizeof(s_row1)/sizeof(s_row1[0]))},
    {s_row2, (INT32_T)(sizeof(s_row2)/sizeof(s_row2[0]))},
    {s_row3, (INT32_T)(sizeof(s_row3)/sizeof(s_row3[0]))},
    {s_row4, (INT32_T)(sizeof(s_row4)/sizeof(s_row4[0]))},
};
#define N_ROWS ((INT32_T)(sizeof(s_rows)/sizeof(s_rows[0])))

/* ---------------------------------------------------------------------------
 * Module state (supports one keyboard instance at a time)
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *s_kb_obj = NULL;  /* keyboard container */
STATIC lv_obj_t *s_kb_ta  = NULL;  /* bound textarea */
STATIC BOOL_T    s_shift   = FALSE;
STATIC BOOL_T    s_caps    = FALSE;

STATIC VOID_T __kb_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_kb_obj) {
        s_kb_obj = NULL;
        s_kb_ta = NULL;
        s_shift = FALSE;
        s_caps = FALSE;
    }
}

/* ---------------------------------------------------------------------------
 * Update key labels for current shift/caps state
 * --------------------------------------------------------------------------- */
STATIC VOID_T __kb_update_labels(VOID_T)
{
    if (!s_kb_obj) return;
    BOOL_T upper = s_shift ^ s_caps;
    INT32_T child_idx = 0;
    for (INT32_T r = 0; r < N_ROWS; r++) {
        for (INT32_T k = 0; k < s_rows[r].count; k++) {
            CONST KB_KEY_T *kd = &s_rows[r].keys[k];
            lv_obj_t *btn = lv_obj_get_child(s_kb_obj, child_idx++);
            if (!btn) return;
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (!lbl) continue;
            if (kd->type == KT_CHAR) {
                CONST CHAR_T *txt = upper ? kd->shifted : kd->label;
                if (txt) lv_label_set_text(lbl, txt);
            } else if (kd->type == KT_SHIFT) {
                /* Highlight shift button when active */
                lv_obj_set_style_bg_color(btn,
                    lv_color_hex(s_shift ? WIN95_COLOR_TITLEBAR : WIN95_COLOR_WINDOW), 0);
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(s_shift ? WIN95_COLOR_LIGHT : 0x000000), 0);
            } else if (kd->type == KT_CAPS) {
                lv_obj_set_style_bg_color(btn,
                    lv_color_hex(s_caps ? WIN95_COLOR_TITLEBAR : WIN95_COLOR_WINDOW), 0);
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(s_caps ? WIN95_COLOR_LIGHT : 0x000000), 0);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Key click handler
 * --------------------------------------------------------------------------- */
STATIC VOID_T __kb_key_cb(lv_event_t *e)
{
    CONST KB_KEY_T *kd = (CONST KB_KEY_T *)lv_event_get_user_data(e);
    if (!kd || !s_kb_ta) return;

    BOOL_T upper = s_shift ^ s_caps;

    switch (kd->type) {
        case KT_CHAR: {
            CONST CHAR_T *ins = upper ? kd->shifted : kd->normal;
            if (ins) lv_textarea_add_text(s_kb_ta, ins);
            if (s_shift) {
                s_shift = FALSE;
                __kb_update_labels();
            }
            break;
        }
        case KT_BKSP:
            lv_textarea_delete_char(s_kb_ta);
            break;
        case KT_ENTER:
            lv_textarea_add_char(s_kb_ta, '\n');
            break;
        case KT_TAB:
            lv_textarea_add_text(s_kb_ta, "    ");
            break;
        case KT_SPACE:
            lv_textarea_add_char(s_kb_ta, ' ');
            break;
        case KT_SHIFT:
            s_shift = !s_shift;
            __kb_update_labels();
            break;
        case KT_CAPS:
            s_caps = !s_caps;
            __kb_update_labels();
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
lv_obj_t *win95_kb_create(lv_obj_t *parent)
{
    if (s_kb_obj) {
        if (lv_obj_is_valid(s_kb_obj)) {
            lv_obj_delete(s_kb_obj);
        }
        s_kb_obj = NULL;
        s_kb_ta = NULL;
    }
    s_shift = FALSE;
    s_caps  = FALSE;
    s_kb_ta = NULL;

    lv_obj_t *kb = lv_obj_create(parent);
    lv_obj_remove_style_all(kb);
    lv_obj_set_size(kb, KB_W, KB_H);
    lv_obj_set_pos(kb, 0, BIOS_SCREEN_HEIGHT - KB_H);
    lv_obj_set_style_bg_color(kb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);   /* hidden by default */
    lv_obj_add_event_cb(kb, __kb_delete_cb, LV_EVENT_DELETE, NULL);
    s_kb_obj = kb;

    /* Build all key buttons */
    for (INT32_T r = 0; r < N_ROWS; r++) {
        INT32_T x = MARGIN_X;
        INT32_T y = ROW_Y(r);
        for (INT32_T k = 0; k < s_rows[r].count; k++) {
            CONST KB_KEY_T *kd = &s_rows[r].keys[k];

            lv_obj_t *btn = lv_obj_create(kb);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, kd->w, KEY_H);
            lv_obj_set_pos(btn, x, y);
            /* Win95 raised button: light top-left, dark bottom-right */
            lv_obj_set_style_bg_color(btn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(WIN95_COLOR_LIGHT), 0);
            lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
            /* Pressed state: invert border sides */
            lv_obj_set_style_border_color(btn, lv_color_hex(WIN95_COLOR_SHADOW),
                                           LV_STATE_PRESSED);
            lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT,
                                          LV_STATE_PRESSED);
            /* Outer border */
            lv_obj_set_style_outline_color(btn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
            lv_obj_set_style_outline_width(btn, 1, 0);
            lv_obj_set_style_radius(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(btn, __kb_key_cb, LV_EVENT_CLICKED,
                                 (VOID_T *)(uintptr_t)(intptr_t)kd);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
            lv_label_set_text(lbl, kd->label);
            lv_obj_center(lbl);

            x += kd->w + KEY_GAP;
        }
    }

    return kb;
}

VOID_T win95_kb_set_textarea(lv_obj_t *kb, lv_obj_t *ta)
{
    (VOID_T)kb;
    s_kb_ta = ta;
}
