/**
 * @file win95_html.h
 * @brief Minimal HTML renderer for Tuya Navigator.
 * @version 2.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_HTML_H__
#define __WIN95_HTML_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"
#include "win95_js.h"
#include "lv_vendor.h"

typedef VOID_T (*WIN95_HTML_LINK_CB)(CONST CHAR_T *href, VOID_T *user_data);
typedef VOID_T (*WIN95_HTML_FORM_CB)(CONST CHAR_T *method, CONST CHAR_T *action,
                                      CONST CHAR_T *enctype, CONST CHAR_T *body,
                                      VOID_T *user_data);
typedef VOID_T (*WIN95_HTML_FOCUS_CB)(lv_obj_t *obj, VOID_T *user_data);

typedef struct {
    WIN95_HTML_LINK_CB  link_cb;
    WIN95_HTML_FORM_CB  form_cb;
    WIN95_HTML_FOCUS_CB focus_cb;
    VOID_T             *user_data;
} WIN95_HTML_CALLBACKS_T;

/* Image deferred-fetch record: widget + unresolved src URL */
#define WIN95_HTML_MAX_IMGS  12
typedef struct {
    lv_obj_t *widget;      /* placeholder lv_image widget */
    CHAR_T    src[256];    /* raw src= attribute value (may be relative) */
} WIN95_HTML_IMG_T;

typedef struct {
    WIN95_HTML_IMG_T imgs[WIN95_HTML_MAX_IMGS];
    UINT8_T          count;
} WIN95_HTML_IMG_LIST_T;

typedef struct {
    lv_obj_t *obj;
    CHAR_T    id[48];
    CHAR_T    tag[16];
    CHAR_T    name[48];
    CHAR_T    type[24];
} WIN95_HTML_DOM_INFO_T;

OPERATE_RET win95_html_render_ex(lv_obj_t *container, CONST CHAR_T *html,
                                  CONST WIN95_HTML_CALLBACKS_T *callbacks);

OPERATE_RET win95_html_render(lv_obj_t *container, CONST CHAR_T *html,
                               WIN95_HTML_LINK_CB link_cb, VOID_T *user_data);

/** JS side effects (alert, title, navigate) from the most recent render. */
CONST WIN95_JS_RESULT_T *win95_html_get_js_result(VOID_T);

/** Deferred image list from the most recent render (caller fetches + updates). */
CONST WIN95_HTML_IMG_LIST_T *win95_html_get_img_list(VOID_T);

BOOL_T win95_html_dom_get_info(CONST CHAR_T *id, WIN95_HTML_DOM_INFO_T *out);
BOOL_T win95_html_dom_get_attr(CONST CHAR_T *id, CONST CHAR_T *name,
                                CHAR_T *out, UINT32_T cap);
BOOL_T win95_html_dom_set_attr(CONST CHAR_T *id, CONST CHAR_T *name,
                                CONST CHAR_T *value);
BOOL_T win95_html_dom_get_text(CONST CHAR_T *id, CHAR_T *out, UINT32_T cap);
BOOL_T win95_html_dom_set_text(CONST CHAR_T *id, CONST CHAR_T *text);
BOOL_T win95_html_dom_set_inner_html(CONST CHAR_T *id, CONST CHAR_T *html);
BOOL_T win95_html_dom_submit(CONST CHAR_T *id);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_HTML_H__ */
