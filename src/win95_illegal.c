/**
 * @file win95_illegal.c
 * @brief Win95-style "This program has performed an illegal operation" dialog.
 */
#include "win95_illegal.h"
#include "win95_desktop.h"
#include "bios_simulator.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

#define W               BIOS_SCREEN_WIDTH
#define H               BIOS_SCREEN_HEIGHT
#define DLG_W           340
#define DLG_H           160

STATIC lv_obj_t *s_dlg = NULL;

STATIC VOID_T __close_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_dlg) {
        lv_obj_delete(s_dlg);
        s_dlg = NULL;
    }
}

STATIC VOID_T __details_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!s_dlg) return;

    /* Show a second "details" label if not already shown */
    lv_obj_t *det = lv_obj_get_child(s_dlg, 4);  /* rough heuristic */
    if (det) return;

    det = lv_label_create(s_dlg);
    lv_obj_set_style_text_color(det, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(det, &lv_font_unscii_8, 0);
    lv_label_set_text(det,
        "Exception 0E at 0028:C0032A8F\n"
        "EAX=00000000  EBX=00000000\n"
        "CS=0028 DS=0030 EIP=C0032A8F\n"
        "The memory could not be \"read\".");
    lv_obj_set_pos(det, 8, 106);
    lv_obj_set_width(det, DLG_W - 16);
}

VOID_T win95_illegal_op_show(CONST CHAR_T *app_name)
{
    if (s_dlg) {
        lv_obj_delete(s_dlg);
        s_dlg = NULL;
    }

    lv_obj_t *scr = lv_layer_top();

    /* Dialog window */
    s_dlg = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dlg);
    lv_obj_set_size(s_dlg, DLG_W, DLG_H);
    lv_obj_align(s_dlg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_dlg, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dlg, 0, 0);
    lv_obj_set_style_border_color(s_dlg, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(s_dlg, 2, 0);
    lv_obj_set_style_shadow_color(s_dlg, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(s_dlg, 4, 0);
    lv_obj_set_style_shadow_ofs_x(s_dlg, 2, 0);
    lv_obj_set_style_shadow_ofs_y(s_dlg, 2, 0);
    lv_obj_clear_flag(s_dlg, LV_OBJ_FLAG_SCROLLABLE);

    /* Title bar */
    lv_obj_t *tb = lv_obj_create(s_dlg);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, DLG_W - 4, WIN95_WINDOW_TITLE_H);
    lv_obj_set_pos(tb, 2, 2);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Illegal Operation");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_add_event_cb(xb, __close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "x");
    lv_obj_align(xl, LV_ALIGN_CENTER, 0, 0);

    /* Error icon (red stop sign) */
    lv_obj_t *ico = lv_obj_create(s_dlg);
    lv_obj_remove_style_all(ico);
    lv_obj_set_size(ico, 22, 22);
    lv_obj_set_pos(ico, 8, WIN95_WINDOW_TITLE_H + 8);
    lv_obj_set_style_bg_color(ico, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(ico, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ico, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ico, lv_color_hex(0xAA0000), 0);
    lv_obj_set_style_border_width(ico, 1, 0);
    lv_obj_clear_flag(ico, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *icol = lv_label_create(ico);
    lv_obj_set_style_text_color(icol, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(icol, &lv_font_unscii_8, 0);
    lv_label_set_text(icol, "!");
    lv_obj_align(icol, LV_ALIGN_CENTER, 0, 0);

    /* Main message */
    CHAR_T msg[128];
    snprintf(msg, sizeof(msg),
             "%s caused an illegal operation\n"
             "and will be shut down.\n\n"
             "If the problem persists, contact\n"
             "the program vendor.",
             app_name ? app_name : "PROGRAM");

    lv_obj_t *ml = lv_label_create(s_dlg);
    lv_obj_set_style_text_color(ml, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(ml, &lv_font_unscii_8, 0);
    lv_label_set_text(ml, msg);
    lv_obj_set_pos(ml, 36, WIN95_WINDOW_TITLE_H + 6);
    lv_obj_set_width(ml, DLG_W - 44);

    /* Buttons */
    INT32_T by = DLG_H - 28;

    lv_obj_t *dbtn = lv_btn_create(s_dlg);
    lv_obj_set_size(dbtn, 72, 20);
    lv_obj_set_pos(dbtn, DLG_W / 2 - 80, by);
    lv_obj_set_style_bg_color(dbtn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(dbtn, 0, 0);
    lv_obj_set_style_pad_all(dbtn, 0, 0);
    lv_obj_set_style_border_color(dbtn, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(dbtn, 1, 0);
    lv_obj_set_style_shadow_color(dbtn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(dbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_x(dbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_y(dbtn, 1, 0);
    lv_obj_add_event_cb(dbtn, __details_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(dbtn);
    lv_obj_set_style_text_color(dl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(dl, &lv_font_unscii_8, 0);
    lv_label_set_text(dl, "Details >>");
    lv_obj_align(dl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *cbtn = lv_btn_create(s_dlg);
    lv_obj_set_size(cbtn, 72, 20);
    lv_obj_set_pos(cbtn, DLG_W / 2 + 8, by);
    lv_obj_set_style_bg_color(cbtn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(cbtn, 0, 0);
    lv_obj_set_style_pad_all(cbtn, 0, 0);
    lv_obj_set_style_border_color(cbtn, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(cbtn, 1, 0);
    lv_obj_set_style_shadow_color(cbtn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(cbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_x(cbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_y(cbtn, 1, 0);
    lv_obj_add_event_cb(cbtn, __close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cbtn);
    lv_obj_set_style_text_color(cl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(cl, &lv_font_unscii_8, 0);
    lv_label_set_text(cl, "Close");
    lv_obj_align(cl, LV_ALIGN_CENTER, 0, 0);
}

VOID_T win95_illegal_op_dismiss(VOID_T)
{
    if (s_dlg) {
        lv_obj_delete(s_dlg);
        s_dlg = NULL;
    }
}
