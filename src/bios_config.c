/**
 * @file bios_config.c
 * @brief BIOS config screens - network & auth (480x320 landscape, UNSCII font)
 * @version 2.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "bios_config.h"
#include "bios_ui.h"
#include "win95_kb.h"

#include "tal_api.h"
#include "tuya_authorize.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define CFG_FIELD_W     200
#define CFG_FIELD_H     24
#define CFG_KB_H        140

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    CFG_NET = 0,
    CFG_AUTH,
} CFG_MODE_E;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *ta1;
    lv_obj_t *ta2;
    lv_obj_t *kb;
    lv_obj_t *status;
    CFG_MODE_E mode;
} CFG_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC CFG_CTX_T s_cfg;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Back button handler
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __back_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_cfg.screen) {
        lv_obj_delete(s_cfg.screen);
        s_cfg.screen = NULL;
    }
    bios_ui_return_main();
}

/**
 * @brief Save button handler
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __save_cb(lv_event_t *e)
{
    (VOID_T)e;
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    CONST CHAR_T *v1 = lv_textarea_get_text(s_cfg.ta1);
    CONST CHAR_T *v2 = lv_textarea_get_text(s_cfg.ta2);

    if (s_cfg.mode == CFG_NET) {
        strncpy(app->wifi_ssid, v1, SSID_MAX_LEN);
        app->wifi_ssid[SSID_MAX_LEN] = '\0';
        strncpy(app->wifi_pass, v2, PASSWORD_MAX_LEN);
        app->wifi_pass[PASSWORD_MAX_LEN] = '\0';
        lv_label_set_text(s_cfg.status, "[OK] Network saved!");
    } else {
        INT32_T uuid_len = strlen(v1);
        INT32_T key_len  = strlen(v2);
        if (uuid_len != 20 || key_len != 32) {
            lv_label_set_text(s_cfg.status, "[ERR] UUID=20, KEY=32 chars");
            lv_obj_set_style_text_color(s_cfg.status, lv_color_hex(0xFF5555), 0);
            return;
        }

        lv_label_set_text(s_cfg.status, "Saving... device will reboot.");
        lv_obj_set_style_text_color(s_cfg.status, lv_color_hex(0xFFFF00), 0);
        lv_refr_now(NULL);

        OPERATE_RET rt = tuya_authorize_write(v1, v2);
        /* tuya_authorize_write reboots on success; reaching here means failure. */
        CHAR_T err_buf[48];
        snprintf(err_buf, sizeof(err_buf), "[ERR] Save failed: %d", (int)rt);
        lv_label_set_text(s_cfg.status, err_buf);
        lv_obj_set_style_text_color(s_cfg.status, lv_color_hex(0xFF5555), 0);
        return;
    }
    lv_obj_set_style_text_color(s_cfg.status, lv_color_hex(0x00FF00), 0);
}

/**
 * @brief Textarea focus handler - bind keyboard
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_cfg.kb) {
        win95_kb_set_textarea(s_cfg.kb, ta);
    }
}

/**
 * @brief Build config screen (480x320 landscape)
 * @param[in] mode CFG_NET or CFG_AUTH
 * @return none
 */
STATIC VOID_T __build_cfg(CFG_MODE_E mode)
{
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    s_cfg.mode = mode;

    INT32_T W = BIOS_SCREEN_WIDTH;
    INT32_T H = BIOS_SCREEN_HEIGHT;

    s_cfg.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_cfg.screen);
    lv_obj_set_size(s_cfg.screen, W, H);
    lv_obj_set_style_bg_color(s_cfg.screen, lv_color_hex(BIOS_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_cfg.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_cfg.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_cfg.screen, 0, 0);

    /* Title */
    lv_obj_t *tbar = lv_obj_create(s_cfg.screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, W, 22);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(BIOS_COLOR_TITLE_BG), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tlbl = lv_label_create(tbar);
    lv_obj_set_style_text_color(tlbl, lv_color_hex(BIOS_COLOR_BG), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_unscii_16, 0);
    lv_label_set_text(tlbl, (mode == CFG_NET) ? "Network Config" : "UUID/AUTH_KEY Setup");
    lv_obj_center(tlbl);

    INT32_T y = 28;
    INT32_T col1_x = 10;
    INT32_T col2_x = W / 2 + 4;

    /* Field 1 */
    lv_obj_t *l1 = lv_label_create(s_cfg.screen);
    lv_obj_set_style_text_color(l1, lv_color_hex(BIOS_COLOR_HIGHLIGHT), 0);
    lv_obj_set_style_text_font(l1, &lv_font_unscii_8, 0);
    lv_label_set_text(l1, (mode == CFG_NET) ? "WiFi SSID:" : "UUID:");
    lv_obj_set_pos(l1, col1_x, y);

    s_cfg.ta1 = lv_textarea_create(s_cfg.screen);
    lv_obj_set_size(s_cfg.ta1, CFG_FIELD_W, CFG_FIELD_H);
    lv_obj_set_pos(s_cfg.ta1, col1_x, y + 10);
    lv_textarea_set_one_line(s_cfg.ta1, true);
    lv_textarea_set_max_length(s_cfg.ta1, (mode == CFG_NET) ? SSID_MAX_LEN : UUID_MAX_LEN);
    lv_obj_set_style_bg_color(s_cfg.ta1, lv_color_hex(0x000055), 0);
    lv_obj_set_style_text_color(s_cfg.ta1, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_cfg.ta1, lv_color_hex(BIOS_COLOR_BORDER), 0);
    lv_obj_set_style_text_font(s_cfg.ta1, &lv_font_unscii_8, 0);
    lv_obj_add_event_cb(s_cfg.ta1, __ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    if (mode == CFG_NET && app->wifi_ssid[0] != '\0') {
        lv_textarea_set_text(s_cfg.ta1, app->wifi_ssid);
    } else if (mode == CFG_AUTH && app->uuid[0] != '\0') {
        lv_textarea_set_text(s_cfg.ta1, app->uuid);
    }

    /* Field 2 */
    lv_obj_t *l2 = lv_label_create(s_cfg.screen);
    lv_obj_set_style_text_color(l2, lv_color_hex(BIOS_COLOR_HIGHLIGHT), 0);
    lv_obj_set_style_text_font(l2, &lv_font_unscii_8, 0);
    lv_label_set_text(l2, (mode == CFG_NET) ? "WiFi Password:" : "AUTH_KEY:");
    lv_obj_set_pos(l2, col2_x, y);

    s_cfg.ta2 = lv_textarea_create(s_cfg.screen);
    lv_obj_set_size(s_cfg.ta2, CFG_FIELD_W, CFG_FIELD_H);
    lv_obj_set_pos(s_cfg.ta2, col2_x, y + 10);
    lv_textarea_set_one_line(s_cfg.ta2, true);
    lv_textarea_set_max_length(s_cfg.ta2, (mode == CFG_NET) ? PASSWORD_MAX_LEN : AUTHKEY_MAX_LEN);
    lv_obj_set_style_bg_color(s_cfg.ta2, lv_color_hex(0x000055), 0);
    lv_obj_set_style_text_color(s_cfg.ta2, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_cfg.ta2, lv_color_hex(BIOS_COLOR_BORDER), 0);
    lv_obj_set_style_text_font(s_cfg.ta2, &lv_font_unscii_8, 0);
    lv_obj_add_event_cb(s_cfg.ta2, __ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    if (mode == CFG_NET && app->wifi_pass[0] != '\0') {
        lv_textarea_set_text(s_cfg.ta2, app->wifi_pass);
        lv_textarea_set_password_mode(s_cfg.ta2, true);
    } else if (mode == CFG_AUTH && app->auth_key[0] != '\0') {
        lv_textarea_set_text(s_cfg.ta2, app->auth_key);
    }

    /* Buttons row */
    INT32_T btn_y = y + 10 + CFG_FIELD_H + 6;
    lv_obj_t *bsave = lv_btn_create(s_cfg.screen);
    lv_obj_set_size(bsave, 80, 22);
    lv_obj_set_pos(bsave, col1_x, btn_y);
    lv_obj_set_style_bg_color(bsave, lv_color_hex(0x005500), 0);
    lv_obj_set_style_radius(bsave, 0, 0);
    lv_obj_add_event_cb(bsave, __save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(bsave);
    lv_obj_set_style_text_color(sl, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(sl, &lv_font_unscii_8, 0);
    lv_label_set_text(sl, "Save");
    lv_obj_center(sl);

    lv_obj_t *bback = lv_btn_create(s_cfg.screen);
    lv_obj_set_size(bback, 80, 22);
    lv_obj_set_pos(bback, col1_x + 90, btn_y);
    lv_obj_set_style_bg_color(bback, lv_color_hex(0x550000), 0);
    lv_obj_set_style_radius(bback, 0, 0);
    lv_obj_add_event_cb(bback, __back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bback);
    lv_obj_set_style_text_color(bl, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(bl, &lv_font_unscii_8, 0);
    lv_label_set_text(bl, "Back");
    lv_obj_center(bl);

    /* Status */
    s_cfg.status = lv_label_create(s_cfg.screen);
    lv_obj_set_style_text_color(s_cfg.status, lv_color_hex(BIOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_cfg.status, &lv_font_unscii_8, 0);
    lv_label_set_text(s_cfg.status, "");
    lv_obj_set_pos(s_cfg.status, col2_x, btn_y + 6);

    /* Keyboard at bottom */
    s_cfg.kb = win95_kb_create(s_cfg.screen);
    lv_obj_set_size(s_cfg.kb, W, CFG_KB_H);
    lv_obj_align(s_cfg.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    win95_kb_set_textarea(s_cfg.kb, s_cfg.ta1);
    lv_obj_set_style_bg_color(s_cfg.kb, lv_color_hex(0x333366), 0);
    lv_obj_set_style_text_color(s_cfg.kb, lv_color_hex(BIOS_COLOR_TEXT), 0);
}

/**
 * @brief Show network config screen
 * @return none
 */
VOID_T bios_config_show_network(VOID_T)
{
    __build_cfg(CFG_NET);
}

/**
 * @brief Show UUID/AUTH_KEY config screen
 * @return none
 */
VOID_T bios_config_show_auth(VOID_T)
{
    __build_cfg(CFG_AUTH);
}
