/**
 * @file tuya_main.c
 * @brief BiosSimulator entry point with landscape display and WiFi init
 * @version 2.0.0
 * @date 2026-03-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "bios_simulator.h"
#include "win95_pairing.h"

#include "tal_api.h"
#include "tal_wifi.h"
#include "tal_cli.h"
#include "tal_kv.h"
#include "tuya_authorize.h"
#include "tkl_output.h"
#include "tkl_system.h"

#include "lv_vendor.h"
#include "tdd_disp_ili9488.h"
#include "tdd_tp_gt1151.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define APP_DISP_NAME       "display"

#define LCD_CLK_PIN         TUYA_GPIO_NUM_49
#define LCD_CSX_PIN         TUYA_GPIO_NUM_48
#define LCD_SDA_PIN         TUYA_GPIO_NUM_50
#define LCD_DC_PIN          TUYA_GPIO_NUM_MAX
#define LCD_RST_PIN         TUYA_GPIO_NUM_53
#define LCD_BL_PWM          TUYA_PWM_NUM_7
#define LCD_PWR_PIN         TUYA_GPIO_NUM_MAX

#define TP_I2C_PORT         TUYA_I2C_NUM_0
#define TP_SCL_PIN          TUYA_GPIO_NUM_13
#define TP_SDA_PIN          TUYA_GPIO_NUM_15

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC BIOS_APP_CTX_T s_app_ctx;
STATIC BOOL_T s_legacy_wifi_ready = FALSE;
STATIC VOID_T __wifi_event_cb(WF_EVENT_E event, VOID_T *arg);

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Register ILI9488 RGB display with 90-degree CW rotation for landscape
 * @return OPRT_OK on success
 * @note Replaces board_register_hardware() for display/touch only.
 *       The rotation is handled natively by the LVGL port layer which
 *       allocates a rotation buffer and calls lv_display_set_rotation().
 */
STATIC OPERATE_RET __register_display(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;

    DISP_RGB_DEVICE_CFG_T disp_cfg;
    memset(&disp_cfg, 0, sizeof(DISP_RGB_DEVICE_CFG_T));

    disp_cfg.sw_spi_cfg.spi_clk = LCD_CLK_PIN;
    disp_cfg.sw_spi_cfg.spi_sda = LCD_SDA_PIN;
    disp_cfg.sw_spi_cfg.spi_csx = LCD_CSX_PIN;
    disp_cfg.sw_spi_cfg.spi_dc  = LCD_DC_PIN;
    disp_cfg.sw_spi_cfg.spi_rst = LCD_RST_PIN;

    disp_cfg.bl.type              = TUYA_DISP_BL_TP_PWM;
    disp_cfg.bl.pwm.id            = LCD_BL_PWM;
    disp_cfg.bl.pwm.cfg.frequency = 1000;
    disp_cfg.bl.pwm.cfg.duty      = 10000;

    disp_cfg.width     = 320;
    disp_cfg.height    = 480;
    disp_cfg.pixel_fmt = TUYA_PIXEL_FMT_RGB565;
    disp_cfg.rotation  = TUYA_DISPLAY_ROTATION_90;

    disp_cfg.power.pin = LCD_PWR_PIN;

    TUYA_CALL_ERR_RETURN(tdd_disp_rgb_ili9488_register(APP_DISP_NAME, &disp_cfg));

    TDD_TP_GT1151_INFO_T tp_cfg = {
        .i2c_cfg = {
            .port    = TP_I2C_PORT,
            .scl_pin = TP_SCL_PIN,
            .sda_pin = TP_SDA_PIN,
        },
        .tp_cfg = {
            .x_max = 320,
            .y_max = 480,
            .flags = {
                .mirror_x = 0,
                .mirror_y = 0,
                .swap_xy  = 0,
            },
        },
    };

    TUYA_CALL_ERR_RETURN(tdd_tp_i2c_gt1151_register(APP_DISP_NAME, &tp_cfg));

    return rt;
}

/**
 * @brief Get global app context
 * @return pointer to app context
 */
BIOS_APP_CTX_T *bios_app_get_ctx(VOID_T)
{
    return &s_app_ctx;
}

OPERATE_RET bios_wifi_legacy_ensure_init(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;

    if (!s_legacy_wifi_ready) {
        rt = tal_wifi_init(__wifi_event_cb);
        if (rt != OPRT_OK) {
            PR_ERR("tal_wifi_init failed: %d", rt);
            return rt;
        }
        s_legacy_wifi_ready = TRUE;
    }

    rt = tal_wifi_set_work_mode(WWM_STATION);
    if (rt != OPRT_OK) {
        PR_ERR("tal_wifi_set_work_mode failed: %d", rt);
    }
    return rt;
}

/**
 * @brief WiFi event callback - updates app context wifi_state from system thread
 * @param[in] event WiFi event type
 * @param[in] arg event argument (unused)
 * @return none
 */
STATIC VOID_T __wifi_event_cb(WF_EVENT_E event, VOID_T *arg)
{
    (VOID_T)arg;

    switch (event) {
    case WFE_CONNECTED: {
        NW_IP_S ip_info;
        memset(&ip_info, 0, sizeof(NW_IP_S));
        if (tal_wifi_get_ip(WF_STATION, &ip_info) == OPRT_OK) {
            strncpy(s_app_ctx.wifi_ip, ip_info.ip, IP_STR_MAX_LEN - 1);
            s_app_ctx.wifi_ip[IP_STR_MAX_LEN - 1] = '\0';
        }
        s_app_ctx.wifi_state = WIFI_ST_CONNECTED;
        break;
    }
    case WFE_CONNECT_FAILED:
        s_app_ctx.wifi_state = WIFI_ST_FAILED;
        break;
    case WFE_DISCONNECTED:
        s_app_ctx.wifi_state = WIFI_ST_DISCONNECTED;
        s_app_ctx.wifi_ip[0] = '\0';
        break;
    default:
        break;
    }
}

/**
 * @brief Load UUID/AUTH_KEY/PID into app context (legacy wrapper - real work
 *        happens in win95_pairing_load_creds)
 * @return none
 */
STATIC VOID_T __load_authorize(VOID_T)
{
    win95_pairing_load_creds(&s_app_ctx);
    win95_pairing_load_wifi(&s_app_ctx);
    win95_pairing_load_tz(&s_app_ctx);
}

/**
 * @brief Application main logic
 * @return none
 */
STATIC VOID_T user_main(VOID_T)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    memset(&s_app_ctx, 0, sizeof(BIOS_APP_CTX_T));
    s_app_ctx.state = APP_STATE_BIOS;
    s_app_ctx.wifi_state = WIFI_ST_IDLE;
    s_app_ctx.pair_state = PAIR_ST_IDLE;

    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();

    tal_cli_init();
    tuya_authorize_init();

    __load_authorize();

    __register_display();

    /* If Tuya credentials are present, bring up the full IoT stack (netmgr
     * + BLE/AP netcfg + cloud client + yield thread). Otherwise fall back to
     * the legacy tal_wifi path so the Direct Connect tab still works. */
    if (s_app_ctx.pair_state != PAIR_ST_CREDS_MISSING) {
        win95_pairing_start(&s_app_ctx);
    } else {
        bios_wifi_legacy_ensure_init();
    }

    lv_vendor_init(APP_DISP_NAME);

    bios_ui_init();

    lv_vendor_start(5, 1024 * 16);
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
/**
 * @brief Linux main entry
 * @param[in] argc argument count
 * @param[in] argv argument vector
 * @return none
 */
void main(int argc, char *argv[])
{
    user_main();

    while (1) {
        tal_system_sleep(500);
    }
}
#else

STATIC THREAD_HANDLE s_app_thread = NULL;

/**
 * @brief App task thread
 * @param[in] arg task parameter
 * @return none
 */
STATIC VOID_T tuya_app_thread(VOID_T *arg)
{
    (VOID_T)arg;

    user_main();

    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

/**
 * @brief Tuya app main entry
 * @return none
 */
void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {1024 * 4, 4, "tuya_app_main"};
    tal_thread_create_and_start(&s_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
