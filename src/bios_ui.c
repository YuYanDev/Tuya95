/**
 * @file bios_ui.c
 * @brief BIOS style main UI (480x320 landscape, UNSCII pixel font)
 * @version 2.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "bios_ui.h"
#include "bios_config.h"
#include "win95_desktop.h"
#include "win95_cursor.h"
#include "win95_logos.h"

#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define POST_DELAY_MS       2000
#define W                   BIOS_SCREEN_WIDTH
#define H                   BIOS_SCREEN_HEIGHT

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *s_scr_bios = NULL;
STATIC lv_obj_t *s_scr_root = NULL;
STATIC lv_obj_t *s_menu_items[BIOS_MENU_COUNT];
STATIC INT32_T   s_menu_sel = 0;

STATIC CONST CHAR_T *s_menu_labels[BIOS_MENU_COUNT] = {
    " System Information",
    " Network Configuration",
    " UUID / AUTH_KEY Setup",
    " Entry System          >>>",
};

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Create a BIOS-style bordered box
 * @param[in] parent parent object
 * @param[in] x x position
 * @param[in] y y position
 * @param[in] w width
 * @param[in] h height
 * @return created box object
 */
STATIC lv_obj_t *__bios_box(lv_obj_t *parent, INT32_T x, INT32_T y, INT32_T w, INT32_T h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(BIOS_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

/**
 * @brief Update menu highlight state
 * @return none
 */
STATIC VOID_T __update_highlight(VOID_T)
{
    for (INT32_T i = 0; i < BIOS_MENU_COUNT; i++) {
        if (i == s_menu_sel) {
            lv_obj_set_style_bg_color(s_menu_items[i], lv_color_hex(BIOS_COLOR_SELECT_BG), 0);
            lv_obj_set_style_bg_opa(s_menu_items[i], LV_OPA_COVER, 0);
            lv_obj_t *lbl = lv_obj_get_child(s_menu_items[i], 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(BIOS_COLOR_SELECT_FG), 0);
            }
        } else {
            lv_obj_set_style_bg_opa(s_menu_items[i], LV_OPA_TRANSP, 0);
            lv_obj_t *lbl = lv_obj_get_child(s_menu_items[i], 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(BIOS_COLOR_TEXT), 0);
            }
        }
    }
}

/**
 * @brief Handle menu item click
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __menu_click_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);

    for (INT32_T i = 0; i < BIOS_MENU_COUNT; i++) {
        if (s_menu_items[i] == target || lv_obj_get_parent(target) == s_menu_items[i]) {
            s_menu_sel = i;
            __update_highlight();
            break;
        }
    }

    switch (s_menu_sel) {
    case BIOS_MENU_NET_CONFIG:
        bios_config_show_network();
        break;
    case BIOS_MENU_AUTH_CONFIG:
        bios_config_show_auth();
        break;
    case BIOS_MENU_ENTRY_SYSTEM:
        win95_desktop_init();
        break;
    default:
        break;
    }
}

/**
 * @brief Build POST screen (480x320 landscape)
 * @param[in] done_cb timer callback when POST finishes
 * @return none
 */
STATIC VOID_T __build_post_screen(lv_timer_cb_t done_cb)
{
    lv_obj_t *scr = lv_obj_create(s_scr_root);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, W, H);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(scr, 0, 0);

    lv_obj_t *logo = lv_label_create(scr);
    lv_obj_set_style_text_color(logo, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(logo, &lv_font_unscii_16, 0);
    lv_label_set_text(logo,
        "TuyaBIOS 4.0 Release 6.1\n"
        "Copyright 2024-2026\n"
        "Tuya Inc.");
    lv_obj_set_pos(logo, 8, 8);

    lv_obj_t *info = lv_label_create(scr);
    lv_obj_set_style_text_color(info, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(info, &lv_font_unscii_8, 0);
    lv_label_set_text(info,
        "Tuya T5 AI Board Rev 1.0\n"
        "CPU : BK7236 @ 320MHz\n"
        "Mem : 8192KB OK\n"
        "PSRAM : 8MB Detected\n\n"
        "Detecting Display.. ILI9488 480x320 OK\n"
        "Detecting Touch.... GT1151 OK\n"
        "Detecting WiFi..... BK7236 OK\n"
        "Detecting BLE...... OK\n\n"
        "Touch screen to enter SETUP...");
    lv_obj_set_pos(info, 8, 70);

    lv_obj_t *energy = lv_image_create(scr);
    lv_image_set_src(energy, &g_win95_energy_yellow);
    lv_obj_set_pos(energy,
                   W - (INT32_T)g_win95_energy_yellow.header.w - 10,
                   H - (INT32_T)g_win95_energy_yellow.header.h - 8);
    lv_obj_set_style_img_recolor(energy, lv_color_hex(0xF0C300), 0);
    lv_obj_set_style_img_recolor_opa(energy, LV_OPA_COVER, 0);
    lv_obj_clear_flag(energy, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(energy, LV_OBJ_FLAG_SCROLLABLE);

    lv_timer_create(done_cb, POST_DELAY_MS, scr);
}

/**
 * @brief Build BIOS main setup screen (480x320 landscape, left menu + right info)
 * @return none
 */
STATIC VOID_T __build_bios_setup(VOID_T)
{
    s_scr_bios = lv_obj_create(s_scr_root);
    lv_obj_remove_style_all(s_scr_bios);
    lv_obj_set_size(s_scr_bios, W, H);
    lv_obj_set_style_bg_color(s_scr_bios, lv_color_hex(BIOS_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr_bios, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr_bios, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_scr_bios, 0, 0);

    /* Title bar */
    lv_obj_t *tbar = lv_obj_create(s_scr_bios);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, W, BIOS_TITLE_H);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(BIOS_COLOR_TITLE_BG), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tlbl = lv_label_create(tbar);
    lv_obj_set_style_text_color(tlbl, lv_color_hex(BIOS_COLOR_BG), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_unscii_16, 0);
    lv_label_set_text(tlbl, "TuyaBIOS Setup Utility");
    lv_obj_center(tlbl);

    INT32_T body_y = BIOS_TITLE_H + 4;
    INT32_T body_h = H - BIOS_TITLE_H - BIOS_BOTTOM_H - 8;
    INT32_T left_w = 240;
    INT32_T right_w = W - left_w - 20;

    /* Left panel - Main Menu */
    INT32_T mh = BIOS_MENU_COUNT * BIOS_MENU_ITEM_H + BIOS_MENU_PAD * 2 + 14;
    lv_obj_t *mbox = __bios_box(s_scr_bios, 6, body_y, left_w, mh);

    lv_obj_t *mtitle = lv_label_create(mbox);
    lv_obj_set_style_text_color(mtitle, lv_color_hex(BIOS_COLOR_HIGHLIGHT), 0);
    lv_obj_set_style_text_font(mtitle, &lv_font_unscii_8, 0);
    lv_label_set_text(mtitle, " Main Menu");
    lv_obj_set_pos(mtitle, 2, 2);

    for (INT32_T i = 0; i < BIOS_MENU_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(mbox);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, left_w - 6, BIOS_MENU_ITEM_H);
        lv_obj_set_pos(item, 3, 14 + BIOS_MENU_PAD + i * BIOS_MENU_ITEM_H);
        lv_obj_set_style_radius(item, 0, 0);
        lv_obj_set_style_pad_all(item, 2, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item, __menu_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(item);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_label_set_text(lbl, s_menu_labels[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(lbl, __menu_click_cb, LV_EVENT_CLICKED, NULL);

        s_menu_items[i] = item;
    }
    s_menu_sel = 0;
    __update_highlight();

    /* Right top - System Information */
    INT32_T rx = left_w + 12;
    INT32_T info_h = 110;
    lv_obj_t *ibox = __bios_box(s_scr_bios, rx, body_y, right_w, info_h);

    lv_obj_t *ititle = lv_label_create(ibox);
    lv_obj_set_style_text_color(ititle, lv_color_hex(BIOS_COLOR_HIGHLIGHT), 0);
    lv_obj_set_style_text_font(ititle, &lv_font_unscii_8, 0);
    lv_label_set_text(ititle, " System Information");
    lv_obj_set_pos(ititle, 2, 2);

    BIOS_APP_CTX_T *ctx = bios_app_get_ctx();
    CHAR_T buf[256];
    snprintf(buf, sizeof(buf),
        "Board : Tuya T5 AI\n"
        "CPU   : BK7236 @ 320MHz\n"
        "LCD   : 480x320 ILI9488\n"
        "Touch : GT1151\n"
        "UUID  : %.16s%s\n"
        "Auth  : %.8s%s",
        (ctx->uuid[0] != '\0') ? ctx->uuid : "<not set>",
        (strlen(ctx->uuid) > 16) ? ".." : "",
        (ctx->auth_key[0] != '\0') ? ctx->auth_key : "<not set>",
        (strlen(ctx->auth_key) > 8) ? ".." : "");

    lv_obj_t *ilbl = lv_label_create(ibox);
    lv_obj_set_style_text_color(ilbl, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(ilbl, &lv_font_unscii_8, 0);
    lv_label_set_text(ilbl, buf);
    lv_obj_set_pos(ilbl, 4, 14);
    lv_obj_set_width(ilbl, right_w - 10);

    /* Right bottom - Help */
    INT32_T help_y = body_y + info_h + 4;
    INT32_T help_h = body_h - info_h - 4;
    lv_obj_t *hbox = __bios_box(s_scr_bios, rx, help_y, right_w, help_h);

    lv_obj_t *htitle = lv_label_create(hbox);
    lv_obj_set_style_text_color(htitle, lv_color_hex(BIOS_COLOR_HIGHLIGHT), 0);
    lv_obj_set_style_text_font(htitle, &lv_font_unscii_8, 0);
    lv_label_set_text(htitle, " Help");
    lv_obj_set_pos(htitle, 2, 2);

    lv_obj_t *hlbl = lv_label_create(hbox);
    lv_obj_set_style_text_color(hlbl, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(hlbl, &lv_font_unscii_8, 0);
    lv_label_set_text(hlbl,
        "Touch a menu item to\n"
        "select it.\n"
        "\"Entry System\" boots\n"
        "TuyaOS 95 desktop.\n"
        "Set network and auth\n"
        "before first boot.");
    lv_obj_set_pos(hlbl, 4, 14);
    lv_obj_set_width(hlbl, right_w - 10);

    /* Bottom bar */
    lv_obj_t *blbl = lv_label_create(s_scr_bios);
    lv_obj_set_style_text_color(blbl, lv_color_hex(BIOS_COLOR_HIGHLIGHT), 0);
    lv_obj_set_style_text_font(blbl, &lv_font_unscii_8, 0);
    lv_label_set_text(blbl, "TuyaBIOS v1.0.0   F1:Help  F10:Save & Exit");
    lv_obj_align(blbl, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/**
 * @brief POST done callback
 * @param[in] timer LVGL timer
 * @return none
 */
STATIC VOID_T __post_done_cb(lv_timer_t *timer)
{
    lv_obj_t *scr = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (scr) {
        lv_obj_delete(scr);
    }
    lv_timer_delete(timer);
    __build_bios_setup();
}

/**
 * @brief Initialize and show BIOS UI
 * @return none
 */
VOID_T bios_ui_init(VOID_T)
{
    lv_obj_t *old_root = s_scr_root;

    s_scr_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr_root);
    lv_obj_set_size(s_scr_root, W, H);
    lv_obj_set_style_bg_color(s_scr_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_scr_root);

    if (old_root && old_root != s_scr_root && lv_obj_is_valid(old_root)) {
        lv_obj_delete(old_root);
    }

    s_scr_bios = NULL;
    win95_cursor_init();
    win95_cursor_set_visible(FALSE);
    __build_post_screen(__post_done_cb);
}

/**
 * @brief Return to BIOS main menu
 * @return none
 */
VOID_T bios_ui_return_main(VOID_T)
{
    if (s_scr_bios) {
        lv_obj_delete(s_scr_bios);
        s_scr_bios = NULL;
    }
    win95_cursor_set_visible(FALSE);
    __build_bios_setup();
}
