/**
 * @file win95_ie.c
 * @brief Win95 Tuya Navigator - HTTP/1.0 + tiny HTML renderer.
 *        Home page defaults to http://info.cern.ch/ (the first website).
 * @version 2.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_ie.h"
#include "win95_desktop.h"
#include "win95_http10.h"
#include "win95_http11.h"
#include "win95_html.h"
#include "win95_cursor.h"
#include "win95_kb.h"

#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define IE_W                BIOS_SCREEN_WIDTH
#define IE_H                BIOS_SCREEN_HEIGHT
#define IE_TITLE_H          18
#define IE_TOOLBAR_H        24
#define IE_STATUS_H         14
#define IE_KB_H             130
#define IE_HTTP_TIMEOUT     10000
#define IE_URL_MAX          256
#define IE_HIST_MAX         8
#define IE_DEFAULT_URL      "http://info.cern.ch/"
#define IE_COOKIE_MAX       12

lv_obj_t *lv_gif_create(lv_obj_t *parent);
void lv_gif_set_src(lv_obj_t *obj, const void *src);

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    CHAR_T host[WIN95_HTTP_HOST_MAX];
    CHAR_T path[WIN95_HTTP_PATH_MAX];
    UINT16_T port;
    BOOL_T is_https;
} IE_URL_T;

typedef struct {
    IE_URL_T url;
    CHAR_T method[8];
    CHAR_T extra_headers[512];
    uint8_t *body;
    UINT32_T body_len;
    UINT32_T req_id;
} IE_REQ_T;

typedef struct {
    BOOL_T used;
    CHAR_T name[48];
    CHAR_T value[96];
    CHAR_T domain[96];
    CHAR_T path[96];
    BOOL_T secure;
} IE_COOKIE_T;

/* Per-image fetch slot — cap at 4 to bound static RAM */
#define IE_IMG_MAX  4
typedef struct {
    lv_obj_t           *widget;    /* placeholder object (NULL = slot free) */
    volatile BOOL_T     done;
    volatile BOOL_T     error;
    THREAD_HANDLE       thread;
    CHAR_T              url[IE_URL_MAX];
    WIN95_HTTP_RESP_T   resp;
    lv_image_dsc_t      dsc;
    UINT8_T             kind;      /* 1=png, 2=gif, 3=jpeg */
    CHAR_T             *img_prebuf; /* pre-allocated body buffer, owned by IE_CTX_T */
} IE_IMG_SLOT_T;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title_lbl;
    lv_obj_t *addr_ta;
    lv_obj_t *content_flex;   /* scrollable flex column */
    lv_obj_t *content_area;
    lv_obj_t *status_lbl;
    lv_obj_t *kb;

    /* HTTP worker */
    THREAD_HANDLE http_thread;
    volatile BOOL_T loading;
    volatile BOOL_T load_done;
    volatile BOOL_T load_error;
    INT32_T last_http_rt;
    WIN95_HTTP_RESP_T resp;
    lv_timer_t *load_tmr;
    UINT32_T req_seq;
    UINT32_T active_req_id;

    /* Current page bookkeeping for relative URL resolution */
    CHAR_T cur_host[WIN95_HTTP_HOST_MAX];
    CHAR_T cur_path[WIN95_HTTP_PATH_MAX];
    UINT16_T cur_port;

    /* History (simple back stack) */
    CHAR_T  hist[IE_HIST_MAX][IE_URL_MAX];
    INT32_T hist_len;

    /* Deferred image fetch slots */
    IE_IMG_SLOT_T img_slots[IE_IMG_MAX];
    lv_timer_t   *img_poll_tmr;

    /* Pre-allocated HTTP body buffers (allocated at open, freed at close) */
    CHAR_T      *page_prebuf;
    UINT32_T     page_prebuf_cap;

    /* Cookies */
    IE_COOKIE_T cookies[IE_COOKIE_MAX];
} IE_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
/* Static HTTP body buffers placed in PSRAM — no runtime tal_malloc, no fragmentation.
 * page: 20 KB for page HTML; img: 8 KB × 4 for inline images. */
STATIC __attribute__((section(".psram.bss"))) CHAR_T s_ie_page_buf[WIN95_HTTP_PAGE_BUF_MAX];
STATIC __attribute__((section(".psram.bss"))) CHAR_T s_ie_img_buf[IE_IMG_MAX][WIN95_HTTP_IMG_BODY_MAX];

STATIC IE_CTX_T s_ie;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_close(VOID_T);
STATIC VOID_T __ie_navigate(CONST CHAR_T *raw_url, BOOL_T push_history);
STATIC VOID_T __ie_apply_js_result(VOID_T);
STATIC VOID_T __ie_start_img_fetches(VOID_T);
STATIC VOID_T __ie_navigate_request(CONST CHAR_T *raw_url, BOOL_T push_history,
                                     CONST CHAR_T *method,
                                     CONST CHAR_T *content_type,
                                     CONST CHAR_T *body);
STATIC VOID_T __ie_on_form_submit(CONST CHAR_T *method, CONST CHAR_T *action,
                                   CONST CHAR_T *enctype, CONST CHAR_T *body,
                                   VOID_T *user_data);
STATIC VOID_T __ie_on_focus(lv_obj_t *obj, VOID_T *user_data);

/* ---------------------------------------------------------------------------
 * Border helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_raised(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_x(obj, 1, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 1, 0);
}

STATIC VOID_T __ie_sunken(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
}

/* ---------------------------------------------------------------------------
 * Status + content helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_set_status(CONST CHAR_T *text)
{
    if (s_ie.status_lbl) {
        lv_label_set_text(s_ie.status_lbl, text);
    }
}

STATIC VOID_T __ie_show_message_page(CONST CHAR_T *html)
{
    if (s_ie.content_flex) {
        win95_html_render(s_ie.content_flex, html, NULL, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * URL helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Resolve a possibly-relative URL against the current page.
 *        Writes an absolute-form http://host:port/path string into `out`.
 */
STATIC VOID_T __ie_resolve_url(CONST CHAR_T *href, CHAR_T *out, UINT32_T cap)
{
    if (!href || !out || cap == 0) return;
    out[0] = '\0';

    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        strncpy(out, href, cap - 1);
        out[cap - 1] = '\0';
        return;
    }

    CHAR_T full_path[WIN95_HTTP_PATH_MAX];
    if (href[0] == '/') {
        strncpy(full_path, href, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        /* Merge with current path: strip last segment of cur_path. */
        strncpy(full_path, s_ie.cur_path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
        CHAR_T *slash = strrchr(full_path, '/');
        if (slash) {
            slash[1] = '\0';
        } else {
            full_path[0] = '/';
            full_path[1] = '\0';
        }
        /* Append href. */
        UINT32_T used = (UINT32_T)strlen(full_path);
        if (used + strlen(href) < sizeof(full_path)) {
            strcat(full_path, href);
        }
    }

    if (s_ie.cur_port == 80 || s_ie.cur_port == 0) {
        snprintf(out, cap, "http://%s%s", s_ie.cur_host, full_path);
    } else if (s_ie.cur_port == 443) {
        snprintf(out, cap, "https://%s%s", s_ie.cur_host, full_path);
    } else {
        snprintf(out, cap, "http://%s:%u%s", s_ie.cur_host,
                 (UINT32_T)s_ie.cur_port, full_path);
    }
}

STATIC BOOL_T __ie_host_matches(CONST CHAR_T *host, CONST CHAR_T *domain)
{
    UINT32_T hl = (UINT32_T)strlen(host);
    UINT32_T dl = (UINT32_T)strlen(domain);
    if (dl == 0 || hl < dl) return FALSE;
    if (strcmp(host + hl - dl, domain) != 0) return FALSE;
    if (hl == dl) return TRUE;
    return host[hl - dl - 1] == '.';
}

STATIC INT32_T __ie_strncasecmp(CONST CHAR_T *a, CONST CHAR_T *b, UINT32_T n)
{
    for (UINT32_T i = 0; i < n; i++) {
        CHAR_T ca = a[i];
        CHAR_T cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (CHAR_T)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (CHAR_T)(cb + 32);
        if (ca != cb) return (UCHAR_T)ca - (UCHAR_T)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

STATIC VOID_T __ie_trim(CHAR_T *s)
{
    if (!s) return;
    CHAR_T *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    INT32_T n = (INT32_T)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

STATIC VOID_T __ie_build_cookie_headers(CONST CHAR_T *host, CONST CHAR_T *path,
                                         BOOL_T is_https, CHAR_T *out, UINT32_T cap)
{
    out[0] = '\0';
    UINT32_T used = 0;
    for (UINT32_T i = 0; i < IE_COOKIE_MAX; i++) {
        IE_COOKIE_T *ck = &s_ie.cookies[i];
        if (!ck->used || !ck->name[0]) continue;
        if (ck->secure && !is_https) continue;
        if (ck->domain[0] && !__ie_host_matches(host, ck->domain)) continue;
        if (ck->path[0] && strncmp(path, ck->path, strlen(ck->path)) != 0) continue;
        if (used == 0) {
            used += (UINT32_T)snprintf(out + used, cap - used, "Cookie: ");
        } else if (used + 2 < cap) {
            out[used++] = ';';
            out[used++] = ' ';
            out[used] = '\0';
        }
        used += (UINT32_T)snprintf(out + used, cap - used, "%s=%s", ck->name, ck->value);
        if (used + 2 >= cap) break;
    }
    if (used > 0 && used + 2 < cap) {
        out[used++] = '\r';
        out[used++] = '\n';
        out[used] = '\0';
    }
}

STATIC VOID_T __ie_store_cookies(CONST WIN95_HTTP_RESP_T *resp, CONST IE_URL_T *url)
{
    if (!resp || !url) return;
    for (UINT32_T i = 0; i < resp->set_cookie_count; i++) {
        CHAR_T line[WIN95_HTTP_COOKIE_LEN];
        strncpy(line, resp->set_cookie[i], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        CHAR_T *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        CHAR_T *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        __ie_trim(line);
        __ie_trim(eq + 1);
        if (!line[0]) continue;

        IE_COOKIE_T *slot = NULL;
        for (UINT32_T k = 0; k < IE_COOKIE_MAX; k++) {
            if (s_ie.cookies[k].used && strcmp(s_ie.cookies[k].name, line) == 0) {
                slot = &s_ie.cookies[k];
                break;
            }
        }
        if (!slot) {
            for (UINT32_T k = 0; k < IE_COOKIE_MAX; k++) {
                if (!s_ie.cookies[k].used) {
                    slot = &s_ie.cookies[k];
                    memset(slot, 0, sizeof(*slot));
                    slot->used = TRUE;
                    break;
                }
            }
        }
        if (!slot) continue;
        strncpy(slot->name, line, sizeof(slot->name) - 1);
        strncpy(slot->value, eq + 1, sizeof(slot->value) - 1);
        strncpy(slot->domain, url->host, sizeof(slot->domain) - 1);
        strncpy(slot->path, "/", sizeof(slot->path) - 1);
        slot->secure = FALSE;

        CONST CHAR_T *attrs = strchr(resp->set_cookie[i], ';');
        while (attrs) {
            attrs++;
            while (*attrs == ' ' || *attrs == '\t') attrs++;
            if (__ie_strncasecmp(attrs, "Domain=", 7) == 0) {
                strncpy(slot->domain, attrs + 7, sizeof(slot->domain) - 1);
                CHAR_T *next = strchr(slot->domain, ';');
                if (next) *next = '\0';
                __ie_trim(slot->domain);
            } else if (__ie_strncasecmp(attrs, "Path=", 5) == 0) {
                strncpy(slot->path, attrs + 5, sizeof(slot->path) - 1);
                CHAR_T *next = strchr(slot->path, ';');
                if (next) *next = '\0';
                __ie_trim(slot->path);
            } else if (__ie_strncasecmp(attrs, "Secure", 6) == 0) {
                slot->secure = TRUE;
            }
            attrs = strchr(attrs, ';');
        }
    }
}

STATIC UINT8_T __ie_guess_image_kind(CONST WIN95_HTTP_RESP_T *resp)
{
    if (!resp || !resp->body || resp->body_len < 4) return 0;
    CONST uint8_t *p = (CONST uint8_t *)resp->body;
    if (resp->content_type[0]) {
        if (strstr(resp->content_type, "png")) return 1;
        if (strstr(resp->content_type, "gif")) return 2;
        if (strstr(resp->content_type, "jpeg") || strstr(resp->content_type, "jpg")) return 3;
    }
    if (resp->body_len >= 8 &&
        p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return 1;
    if (resp->body_len >= 6 &&
        p[0] == 'G' && p[1] == 'I' && p[2] == 'F') return 2;
    if (p[0] == 0xFF && p[1] == 0xD8) return 3;
    return 0;
}

/**
 * @brief User clicked a rendered link.
 */
STATIC VOID_T __ie_on_link(CONST CHAR_T *href, VOID_T *user_data)
{
    (VOID_T)user_data;
    if (!href) return;
    CHAR_T abs[IE_URL_MAX];
    __ie_resolve_url(href, abs, sizeof(abs));
    if (s_ie.addr_ta) {
        lv_textarea_set_text(s_ie.addr_ta, abs);
    }
    __ie_navigate(abs, TRUE);
}

STATIC VOID_T __ie_on_form_submit(CONST CHAR_T *method, CONST CHAR_T *action,
                                   CONST CHAR_T *enctype, CONST CHAR_T *body,
                                   VOID_T *user_data)
{
    (VOID_T)user_data;
    CHAR_T abs[IE_URL_MAX];
    if (action && action[0]) __ie_resolve_url(action, abs, sizeof(abs));
    else if (s_ie.addr_ta) strncpy(abs, lv_textarea_get_text(s_ie.addr_ta), sizeof(abs) - 1);
    else strncpy(abs, IE_DEFAULT_URL, sizeof(abs) - 1);
    abs[sizeof(abs) - 1] = '\0';
    if (s_ie.addr_ta) lv_textarea_set_text(s_ie.addr_ta, abs);
    __ie_navigate_request(abs, TRUE,
                          (method && method[0]) ? method : "GET",
                          (enctype && enctype[0]) ? enctype : "application/x-www-form-urlencoded",
                          body);
}

STATIC VOID_T __ie_on_focus(lv_obj_t *obj, VOID_T *user_data)
{
    (VOID_T)user_data;
    if (!s_ie.kb || !obj) return;
    win95_kb_set_textarea(s_ie.kb, obj);
    lv_obj_clear_flag(s_ie.kb, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------------------------
 * Image fetch worker thread (one per image slot)
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_img_thread(VOID_T *arg)
{
    IE_IMG_SLOT_T *slot = (IE_IMG_SLOT_T *)arg;
    CHAR_T host[WIN95_HTTP_HOST_MAX] = "";
    CHAR_T path[WIN95_HTTP_PATH_MAX] = "/";
    UINT16_T port = 80;
    BOOL_T is_https = FALSE;

    /* Parse URL */
    CONST CHAR_T *u = slot->url;
    if (strncmp(u, "https://", 8) == 0) {
        is_https = TRUE; port = 443; u += 8;
    } else if (strncmp(u, "http://", 7) == 0) {
        u += 7;
    }
    CONST CHAR_T *slash = strchr(u, '/');
    CONST CHAR_T *colon = strchr(u, ':');
    UINT32_T hlen;
    if (colon && (!slash || colon < slash)) {
        hlen = (UINT32_T)(colon - u);
        port = (UINT16_T)atoi(colon + 1);
    } else {
        hlen = slash ? (UINT32_T)(slash - u) : (UINT32_T)strlen(u);
    }
    if (hlen > sizeof(host) - 1) hlen = sizeof(host) - 1;
    memcpy(host, u, hlen);
    host[hlen] = '\0';
    if (slash) { strncpy(path, slash, sizeof(path) - 1); path[sizeof(path)-1] = '\0'; }

    CHAR_T headers[256];
    __ie_build_cookie_headers(host, path, is_https, headers, sizeof(headers));
    /* Preserve pre-allocated buffer; save/restore across the resp memset. */
    CHAR_T *const saved_buf = slot->resp.body_buf;
    UINT32_T const saved_cap = slot->resp.body_buf_cap;
    memset(&slot->resp, 0, sizeof(slot->resp));
    slot->resp.body_buf     = saved_buf;
    slot->resp.body_buf_cap = saved_cap;
    OPERATE_RET rt = win95_http11_request("GET", host, port, path, is_https,
                                          headers[0] ? headers : NULL,
                                          NULL, 0, 5000, &slot->resp);
    if (rt == OPRT_OK && slot->resp.body_len > 0) {
        slot->kind = __ie_guess_image_kind(&slot->resp);
        slot->done = TRUE;
    } else {
        slot->error = TRUE;
    }
    slot->thread = NULL;
    tal_thread_delete(NULL);
}

/* ---------------------------------------------------------------------------
 * Image poll timer: runs in LVGL thread, installs decoded images
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_img_poll_cb(lv_timer_t *t)
{
    (VOID_T)t;
    BOOL_T any_pending = FALSE;

    for (INT32_T i = 0; i < IE_IMG_MAX; i++) {
        IE_IMG_SLOT_T *slot = &s_ie.img_slots[i];
        if (!slot->widget) continue;

        if (slot->done) {
            slot->done = FALSE;
            if (lv_obj_is_valid(slot->widget)) {
                lv_obj_clean(slot->widget);
                if (slot->kind == 2) {
                    lv_obj_t *gif = lv_gif_create(slot->widget);
                    lv_obj_center(gif);
                    memset(&slot->dsc, 0, sizeof(slot->dsc));
                    slot->dsc.data = (const uint8_t *)slot->resp.body;
                    slot->dsc.data_size = slot->resp.body_len;
                    lv_gif_set_src(gif, &slot->dsc);
                } else if (slot->kind == 1 || slot->kind == 3) {
                    lv_obj_t *img = lv_image_create(slot->widget);
                    lv_obj_center(img);
                    memset(&slot->dsc, 0, sizeof(slot->dsc));
                    slot->dsc.data = (const uint8_t *)slot->resp.body;
                    slot->dsc.data_size = slot->resp.body_len;
                    lv_image_set_src(img, &slot->dsc);
                } else {
                    lv_obj_t *lbl = lv_label_create(slot->widget);
                    lv_label_set_text(lbl, "[img fmt]");
                    lv_obj_center(lbl);
                }
            }
            slot->widget = NULL;
        } else if (slot->error) {
            slot->error = FALSE;
            if (lv_obj_is_valid(slot->widget)) {
                lv_obj_t *lbl = lv_obj_get_child(slot->widget, 0);
                if (lbl) lv_label_set_text(lbl, "[img err]");
            }
            win95_http10_free(&slot->resp);
            slot->widget = NULL;
        } else {
            any_pending = TRUE;
        }
    }

    if (!any_pending && s_ie.img_poll_tmr) {
        lv_timer_delete(s_ie.img_poll_tmr);
        s_ie.img_poll_tmr = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Start image fetch workers for all deferred images on the current page
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_start_img_fetches(VOID_T)
{
    CONST WIN95_HTML_IMG_LIST_T *list = win95_html_get_img_list();
    if (!list || list->count == 0) return;

    /* Clean up slots that are safe to free (thread finished or never started).
     * Slots with active threads have widget already NULL'd by navigate_request;
     * leave them alone to avoid a use-after-free race with the running thread. */
    for (INT32_T i = 0; i < IE_IMG_MAX; i++) {
        if (s_ie.img_slots[i].thread == NULL) {
            win95_http10_free(&s_ie.img_slots[i].resp);
            memset(&s_ie.img_slots[i], 0, sizeof(IE_IMG_SLOT_T));
            s_ie.img_slots[i].img_prebuf = s_ie_img_buf[i];
        }
    }
    if (s_ie.img_poll_tmr) {
        lv_timer_delete(s_ie.img_poll_tmr);
        s_ie.img_poll_tmr = NULL;
    }

    THREAD_CFG_T cfg = {4096, 4, "img_fetch"};
    for (UINT8_T i = 0; i < list->count && i < IE_IMG_MAX; i++) {
        CONST WIN95_HTML_IMG_T *hi = &list->imgs[i];
        if (!hi->widget || !hi->src[0]) continue;

        IE_IMG_SLOT_T *slot = &s_ie.img_slots[i];
        if (slot->thread != NULL) continue; /* orphaned thread still running, skip */
        slot->widget = hi->widget;
        /* Wire pre-allocated body buffer — no runtime tal_malloc for image body. */
        slot->resp.body_buf     = slot->img_prebuf;
        slot->resp.body_buf_cap = slot->img_prebuf ? WIN95_HTTP_IMG_BODY_MAX : 0;

        /* Resolve relative URL */
        CHAR_T abs[IE_URL_MAX];
        __ie_resolve_url(hi->src, abs, sizeof(abs));
        strncpy(slot->url, abs, sizeof(slot->url) - 1);
        slot->url[sizeof(slot->url) - 1] = '\0';

        tal_thread_create_and_start(&slot->thread, NULL, NULL,
                                     __ie_img_thread, slot, &cfg);
    }

    if (list->count > 0) {
        s_ie.img_poll_tmr = lv_timer_create(__ie_img_poll_cb, 500, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * HTTP worker
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_http_thread(VOID_T *arg)
{
    IE_REQ_T *req = (IE_REQ_T *)arg;
    THREAD_HANDLE self = s_ie.http_thread;

    /* Preserve pre-allocated buffer pointer across the struct reset. */
    CHAR_T *const saved_buf = s_ie.resp.body_buf;
    UINT32_T const saved_cap = s_ie.resp.body_buf_cap;
    memset(&s_ie.resp, 0, sizeof(s_ie.resp));
    s_ie.resp.body_buf     = saved_buf;
    s_ie.resp.body_buf_cap = saved_cap;
    OPERATE_RET rt = OPRT_COM_ERROR;
    BOOL_T simple_http_get = (!req->url.is_https) &&
                             (strcmp(req->method, "GET") == 0) &&
                             (req->body_len == 0) &&
                             (req->extra_headers[0] == '\0');

    if (simple_http_get) {
        rt = win95_http10_get(req->url.host,
                              req->url.port,
                              req->url.path,
                              IE_HTTP_TIMEOUT,
                              &s_ie.resp);
    }

    if (rt != OPRT_OK || s_ie.resp.body == NULL) {
        win95_http10_free(&s_ie.resp);
        rt = win95_http11_request(req->method,
                                  req->url.host,
                                  req->url.port,
                                  req->url.path,
                                  req->url.is_https,
                                  req->extra_headers[0] ? req->extra_headers : NULL,
                                  req->body,
                                  req->body_len,
                                  IE_HTTP_TIMEOUT,
                                  &s_ie.resp);
    }

    if (req->req_id == s_ie.active_req_id) {
        s_ie.last_http_rt = rt;
        if (rt == OPRT_OK && s_ie.resp.body) {
            s_ie.load_done = TRUE;
        } else {
            s_ie.load_error = TRUE;
        }
        s_ie.loading = FALSE;
        s_ie.http_thread = NULL;
    } else {
        win95_http10_free(&s_ie.resp);
    }

    if (req->body) tal_free(req->body);
    tal_free(req);
    tal_thread_delete(self);
}

/**
 * @brief LVGL-thread poll: picks up result from the HTTP worker, paints UI.
 */
STATIC VOID_T __ie_load_poll(lv_timer_t *timer)
{
    (VOID_T)timer;

    if (s_ie.load_done) {
        s_ie.load_done = FALSE;
        if (s_ie.content_flex && s_ie.resp.body) {
            WIN95_HTML_CALLBACKS_T cb;
            IE_URL_T cur_url;

            memset(&cb, 0, sizeof(cb));
            cb.link_cb = __ie_on_link;
            cb.form_cb = __ie_on_form_submit;
            cb.focus_cb = __ie_on_focus;

            memset(&cur_url, 0, sizeof(cur_url));
            strncpy(cur_url.host, s_ie.cur_host, sizeof(cur_url.host) - 1);
            strncpy(cur_url.path, s_ie.cur_path, sizeof(cur_url.path) - 1);
            cur_url.port = s_ie.cur_port;
            cur_url.is_https = (s_ie.cur_port == 443);

            __ie_store_cookies(&s_ie.resp, &cur_url);
            win95_html_render_ex(s_ie.content_flex, s_ie.resp.body, &cb);
            __ie_apply_js_result();
            __ie_start_img_fetches();
        }
        CHAR_T buf[48];
        snprintf(buf, sizeof(buf), "Done (HTTP %u, %lu bytes)",
                 (UINT32_T)s_ie.resp.status_code,
                 (UINT32_T)s_ie.resp.body_len);
        __ie_set_status(buf);
        win95_http10_free(&s_ie.resp);
        if (s_ie.load_tmr) { lv_timer_delete(s_ie.load_tmr); s_ie.load_tmr = NULL; }
        win95_cursor_set_busy(FALSE);
        return;
    }

    if (s_ie.load_error) {
        s_ie.load_error = FALSE;
        win95_http10_free(&s_ie.resp);
        __ie_show_message_page(
            "<h2>The page cannot be displayed</h2>"
            "<p>The page you are looking for is currently unavailable.</p>"
            "<ul>"
            "<li>Check your network/pairing state in Dial-Up</li>"
            "<li>Verify the address is correct</li>"
            "<li>Try again later</li>"
            "</ul>");
        CHAR_T buf[80];
        snprintf(buf, sizeof(buf), "Error: could not load page (%d)", (int)s_ie.last_http_rt);
        __ie_set_status(buf);
        if (s_ie.load_tmr) { lv_timer_delete(s_ie.load_tmr); s_ie.load_tmr = NULL; }
        win95_cursor_set_busy(FALSE);
        return;
    }
}

/* ---------------------------------------------------------------------------
 * Navigation
 * --------------------------------------------------------------------------- */
STATIC BOOL_T __ie_online(VOID_T)
{
    BIOS_APP_CTX_T *app = bios_app_get_ctx();
    return (app->pair_state == PAIR_ST_MQTT_CONNECTED) ||
           (app->wifi_state == WIFI_ST_CONNECTED);
}

STATIC VOID_T __ie_hist_push(CONST CHAR_T *url)
{
    if (s_ie.hist_len >= IE_HIST_MAX) {
        /* Drop oldest. */
        for (INT32_T i = 1; i < IE_HIST_MAX; i++) {
            strncpy(s_ie.hist[i - 1], s_ie.hist[i], IE_URL_MAX - 1);
            s_ie.hist[i - 1][IE_URL_MAX - 1] = '\0';
        }
        s_ie.hist_len--;
    }
    strncpy(s_ie.hist[s_ie.hist_len], url, IE_URL_MAX - 1);
    s_ie.hist[s_ie.hist_len][IE_URL_MAX - 1] = '\0';
    s_ie.hist_len++;
}

STATIC VOID_T __ie_navigate(CONST CHAR_T *raw_url, BOOL_T push_history)
{
    __ie_navigate_request(raw_url, push_history, "GET", NULL, NULL);
}

STATIC VOID_T __ie_navigate_request(CONST CHAR_T *raw_url, BOOL_T push_history,
                                     CONST CHAR_T *method,
                                     CONST CHAR_T *content_type,
                                     CONST CHAR_T *body)
{
    if (!raw_url || raw_url[0] == '\0') return;

    if (!__ie_online()) {
        __ie_show_message_page(
            "<h2>Not connected</h2>"
            "<p>You are not connected to the Internet.</p>"
            "<p>Open <b>Dial-Up Networking</b> on the desktop - either use the "
            "<i>Direct</i> tab to enter a WiFi SSID, or the <i>Tuya Pair</i> tab "
            "to pair with the Tuya app.</p>");
        __ie_set_status("Error: no connection");
        return;
    }

    if (s_ie.loading) {
        return;
    }

    __ie_set_status("Opening page...");
    __ie_show_message_page("<p><b>Loading...</b></p>");

    IE_REQ_T *req = (IE_REQ_T *)tal_malloc(sizeof(IE_REQ_T));
    if (req == NULL) return;
    memset(req, 0, sizeof(IE_REQ_T));
    strncpy(req->method, (method && method[0]) ? method : "GET", sizeof(req->method) - 1);

    CHAR_T local_host[WIN95_HTTP_HOST_MAX];
    CHAR_T local_path[WIN95_HTTP_PATH_MAX];
    UINT16_T local_port = 80;
    if (win95_http10_parse_url(raw_url,
            local_host, sizeof(local_host),
            &local_port,
            local_path, sizeof(local_path)) != OPRT_OK) {
        tal_free(req);
        __ie_set_status("Invalid URL");
        return;
    }
    BOOL_T is_https = (strncmp(raw_url, "https://", 8) == 0);
    if (is_https && local_port == 80) local_port = 443;

    if (body && body[0] && strcmp(req->method, "GET") == 0) {
        UINT32_T plen = (UINT32_T)strlen(local_path);
        if (plen + strlen(body) + 2 < sizeof(local_path)) {
            strcat(local_path, strchr(local_path, '?') ? "&" : "?");
            strcat(local_path, body);
        }
    }

    strncpy(req->url.host, local_host, sizeof(req->url.host) - 1);
    strncpy(req->url.path, local_path, sizeof(req->url.path) - 1);
    req->url.port     = local_port;
    req->url.is_https = is_https;

    /* Save as current for relative URL resolution. */
    strncpy(s_ie.cur_host, local_host, sizeof(s_ie.cur_host) - 1);
    s_ie.cur_host[sizeof(s_ie.cur_host) - 1] = '\0';
    strncpy(s_ie.cur_path, local_path, sizeof(s_ie.cur_path) - 1);
    s_ie.cur_path[sizeof(s_ie.cur_path) - 1] = '\0';
    s_ie.cur_port = local_port;
    if (push_history) {
        __ie_hist_push(raw_url);
    }

    __ie_build_cookie_headers(local_host, local_path, is_https,
                              req->extra_headers, sizeof(req->extra_headers));
    if (body && body[0] && strcmp(req->method, "GET") != 0) {
        if (content_type && content_type[0]) {
            strncat(req->extra_headers, "Content-Type: ", sizeof(req->extra_headers) - strlen(req->extra_headers) - 1);
            strncat(req->extra_headers, content_type, sizeof(req->extra_headers) - strlen(req->extra_headers) - 1);
            strncat(req->extra_headers, "\r\n", sizeof(req->extra_headers) - strlen(req->extra_headers) - 1);
        }
        req->body_len = (UINT32_T)strlen(body);
        req->body = (uint8_t *)tal_malloc(req->body_len + 1);
        if (!req->body) {
            tal_free(req);
            __ie_set_status("Out of memory");
            return;
        }
        memcpy(req->body, body, req->body_len + 1);
    }

    /* Cancel image fetches from previous page and reclaim memory before new request.
     * Only free slots whose threads have already exited (thread==NULL). Slots with
     * active threads are cancelled by nulling widget; they'll be cleaned next time. */
    if (s_ie.img_poll_tmr) {
        lv_timer_delete(s_ie.img_poll_tmr);
        s_ie.img_poll_tmr = NULL;
    }
    for (INT32_T i = 0; i < IE_IMG_MAX; i++) {
        s_ie.img_slots[i].widget = NULL;
        if (s_ie.img_slots[i].thread == NULL) {
            win95_http10_free(&s_ie.img_slots[i].resp);
            memset(&s_ie.img_slots[i], 0, sizeof(IE_IMG_SLOT_T));
            s_ie.img_slots[i].img_prebuf = s_ie_img_buf[i];
        }
    }
    /* Free any page body that load_poll hasn't consumed yet (prevents stale-resp leak). */
    win95_http10_free(&s_ie.resp);
    /* Wire pre-allocated page buffer so HTTP thread needs no tal_malloc for body. */
    s_ie.resp.body_buf     = s_ie.page_prebuf;
    s_ie.resp.body_buf_cap = s_ie.page_prebuf_cap;

    s_ie.loading = TRUE;
    s_ie.load_done = FALSE;
    s_ie.load_error = FALSE;
    s_ie.last_http_rt = OPRT_OK;
    s_ie.active_req_id = ++s_ie.req_seq;
    req->req_id = s_ie.active_req_id;
    win95_cursor_set_busy(TRUE);

    THREAD_CFG_T cfg = {4096, 4, "ie_http"};
    tal_thread_create_and_start(&s_ie.http_thread, NULL, NULL,
                                 __ie_http_thread, req, &cfg);

    if (s_ie.load_tmr == NULL) {
        s_ie.load_tmr = lv_timer_create(__ie_load_poll, 300, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * JS side-effect handling
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_alert_ok_cb(lv_event_t *e)
{
    lv_obj_t *dlg = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_delete(dlg);
}

STATIC VOID_T __ie_show_alert(CONST CHAR_T *msg)
{
    if (!s_ie.screen || !msg) return;
    lv_obj_t *dlg = lv_obj_create(s_ie.screen);
    lv_obj_set_size(dlg, 300, 96);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 0, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dlg);

    lv_obj_t *tbar = lv_obj_create(dlg);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, 296, 16);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tl = lv_label_create(tbar);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Script Alert");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *ml = lv_label_create(dlg);
    lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ml, 280);
    lv_obj_set_style_text_font(ml, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ml, lv_color_hex(0x000000), 0);
    lv_label_set_text(ml, msg);
    lv_obj_set_pos(ml, 8, 22);

    lv_obj_t *ok = lv_btn_create(dlg);
    lv_obj_set_size(ok, 50, 18);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(ok, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(ok, 0, 0);
    lv_obj_set_style_pad_all(ok, 0, 0);
    lv_obj_set_style_border_color(ok, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(ok, 1, 0);
    lv_obj_add_event_cb(ok, __ie_alert_ok_cb, LV_EVENT_CLICKED, dlg);
    lv_obj_t *ol = lv_label_create(ok);
    lv_obj_set_style_text_color(ol, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(ol, &lv_font_unscii_8, 0);
    lv_label_set_text(ol, "OK");
    lv_obj_center(ol);
}

STATIC VOID_T __ie_apply_js_result(VOID_T)
{
    CONST WIN95_JS_RESULT_T *jr = win95_html_get_js_result();
    if (jr->has_title && s_ie.title_lbl) {
        CHAR_T buf[64];
        snprintf(buf, sizeof(buf), "%.50s - Tuya Navigator", jr->title);
        lv_label_set_text(s_ie.title_lbl, buf);
    }
    if (jr->has_alert) {
        __ie_show_alert(jr->alert_msg);
    }
    if (jr->has_navigate) {
        if (s_ie.addr_ta) lv_textarea_set_text(s_ie.addr_ta, jr->navigate_url);
        __ie_navigate(jr->navigate_url, TRUE);
    }
}

/* ---------------------------------------------------------------------------
 * UI callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_go_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_ie.addr_ta == NULL) return;
    CONST CHAR_T *url = lv_textarea_get_text(s_ie.addr_ta);
    if (url && url[0] != '\0') {
        if (s_ie.kb) lv_obj_add_flag(s_ie.kb, LV_OBJ_FLAG_HIDDEN);
        __ie_navigate(url, TRUE);
    }
}

STATIC VOID_T __ie_home_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_ie.addr_ta) lv_textarea_set_text(s_ie.addr_ta, IE_DEFAULT_URL);
    if (s_ie.kb) lv_obj_add_flag(s_ie.kb, LV_OBJ_FLAG_HIDDEN);
    __ie_navigate(IE_DEFAULT_URL, TRUE);
}

STATIC VOID_T __ie_back_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_ie.hist_len < 2) return;
    s_ie.hist_len--; /* drop current */
    CONST CHAR_T *prev = s_ie.hist[s_ie.hist_len - 1];
    if (s_ie.addr_ta) lv_textarea_set_text(s_ie.addr_ta, prev);
    if (s_ie.kb) lv_obj_add_flag(s_ie.kb, LV_OBJ_FLAG_HIDDEN);
    __ie_navigate(prev, FALSE);
}

STATIC VOID_T __ie_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    __ie_close();
}

STATIC VOID_T __ie_addr_focus_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_ie.kb) {
        win95_kb_set_textarea(s_ie.kb, s_ie.addr_ta);
        lv_obj_clear_flag(s_ie.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */
STATIC VOID_T __ie_close(VOID_T)
{
    s_ie.active_req_id = ++s_ie.req_seq;
    s_ie.loading = FALSE;
    if (s_ie.load_tmr) {
        lv_timer_delete(s_ie.load_tmr);
        s_ie.load_tmr = NULL;
    }
    if (s_ie.img_poll_tmr) {
        lv_timer_delete(s_ie.img_poll_tmr);
        s_ie.img_poll_tmr = NULL;
    }
    for (INT32_T i = 0; i < IE_IMG_MAX; i++) {
        win95_http10_free(&s_ie.img_slots[i].resp);
        /* img_prebuf points into static s_ie_img_buf — do not free */
    }
    win95_http10_free(&s_ie.resp);
    /* page_prebuf points into static s_ie_page_buf — do not free */
    if (s_ie.screen) {
        lv_obj_delete(s_ie.screen);
    }
    memset(&s_ie, 0, sizeof(IE_CTX_T));
}

/* ---------------------------------------------------------------------------
 * Small button factory
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *__ie_toolbar_btn(lv_obj_t *parent, CONST CHAR_T *label,
                                    INT32_T x, INT32_T w, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 18);
    lv_obj_set_pos(b, x, 3);
    lv_obj_set_style_bg_color(b, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    __ie_raised(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    return b;
}

/* ---------------------------------------------------------------------------
 * Open
 * --------------------------------------------------------------------------- */
VOID_T win95_ie_open(VOID_T)
{
    if (s_ie.screen) {
        __ie_close();
    }
    memset(&s_ie, 0, sizeof(IE_CTX_T));

    /* Wire pre-allocated static body buffers — no runtime heap allocation. */
    s_ie.page_prebuf = s_ie_page_buf;
    s_ie.page_prebuf_cap = WIN95_HTTP_PAGE_BUF_MAX;
    for (INT32_T i = 0; i < IE_IMG_MAX; i++) {
        s_ie.img_slots[i].img_prebuf = s_ie_img_buf[i];
    }

    s_ie.screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_ie.screen);
    lv_obj_set_size(s_ie.screen, IE_W, IE_H);
    lv_obj_set_pos(s_ie.screen, 0, 0);
    lv_obj_set_style_bg_color(s_ie.screen, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(s_ie.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ie.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_ie.screen);

    /* Title bar */
    lv_obj_t *tbar = lv_obj_create(s_ie.screen);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, IE_W - 4, IE_TITLE_H);
    lv_obj_set_pos(tbar, 2, 2);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eico = lv_obj_create(tbar);
    lv_obj_remove_style_all(eico);
    lv_obj_set_size(eico, 12, 12);
    lv_obj_align(eico, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(eico, lv_color_hex(0x0066CC), 0);
    lv_obj_set_style_bg_opa(eico, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eico, 6, 0);
    lv_obj_clear_flag(eico, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *e_lbl = lv_label_create(eico);
    lv_obj_set_style_text_color(e_lbl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(e_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(e_lbl, "e");
    lv_obj_center(e_lbl);

    s_ie.title_lbl = lv_label_create(tbar);
    lv_obj_set_style_text_color(s_ie.title_lbl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(s_ie.title_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_ie.title_lbl, "Tuya Navigator 2.0");
    lv_obj_align(s_ie.title_lbl, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *xb = lv_btn_create(tbar);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    __ie_raised(xb);
    lv_obj_add_event_cb(xb, __ie_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);

    /* Toolbar */
    INT32_T tby = IE_TITLE_H + 4;
    lv_obj_t *toolbar = lv_obj_create(s_ie.screen);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, IE_W - 4, IE_TOOLBAR_H);
    lv_obj_set_pos(toolbar, 2, tby);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);

    __ie_toolbar_btn(toolbar, "Back", 2, 40, __ie_back_cb);
    __ie_toolbar_btn(toolbar, "Home", 44, 40, __ie_home_cb);

    lv_obj_t *addr_lbl = lv_label_create(toolbar);
    lv_obj_set_style_text_color(addr_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(addr_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(addr_lbl, "Address:");
    lv_obj_set_pos(addr_lbl, 90, 6);

    s_ie.addr_ta = lv_textarea_create(toolbar);
    lv_obj_set_size(s_ie.addr_ta, IE_W - 236, 18);
    lv_obj_set_pos(s_ie.addr_ta, 148, 3);
    lv_textarea_set_one_line(s_ie.addr_ta, true);
    lv_textarea_set_max_length(s_ie.addr_ta, IE_URL_MAX);
    lv_obj_set_style_bg_color(s_ie.addr_ta, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_color(s_ie.addr_ta, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_ie.addr_ta, &lv_font_unscii_8, 0);
    __ie_sunken(s_ie.addr_ta);
    lv_textarea_set_text(s_ie.addr_ta, IE_DEFAULT_URL);
    lv_obj_add_event_cb(s_ie.addr_ta, __ie_addr_focus_cb, LV_EVENT_FOCUSED, NULL);

    __ie_toolbar_btn(toolbar, "Go", IE_W - 84, 36, __ie_go_cb);

    /* Content area (scrollable flex column) */
    INT32_T cay = tby + IE_TOOLBAR_H + 2;
    INT32_T cah = IE_H - cay - IE_STATUS_H - 2;

    s_ie.content_area = lv_obj_create(s_ie.screen);
    lv_obj_remove_style_all(s_ie.content_area);
    lv_obj_set_size(s_ie.content_area, IE_W - 8, cah);
    lv_obj_set_pos(s_ie.content_area, 4, cay);
    lv_obj_set_style_bg_color(s_ie.content_area, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_bg_opa(s_ie.content_area, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_ie.content_area, 4, 0);
    lv_obj_set_style_radius(s_ie.content_area, 0, 0);
    __ie_sunken(s_ie.content_area);
    lv_obj_set_scrollbar_mode(s_ie.content_area, LV_SCROLLBAR_MODE_AUTO);

    s_ie.content_flex = lv_obj_create(s_ie.content_area);
    lv_obj_remove_style_all(s_ie.content_flex);
    lv_obj_set_size(s_ie.content_flex, IE_W - 16, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_ie.content_flex, 0, 0);
    lv_obj_set_style_bg_opa(s_ie.content_flex, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_ie.content_flex, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ie.content_flex, 2, 0);

    /* Status bar */
    lv_obj_t *sbar = lv_obj_create(s_ie.screen);
    lv_obj_remove_style_all(sbar);
    lv_obj_set_size(sbar, IE_W - 4, IE_STATUS_H);
    lv_obj_align(sbar, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_set_style_bg_color(sbar, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sbar, LV_OBJ_FLAG_SCROLLABLE);
    __ie_sunken(sbar);

    s_ie.status_lbl = lv_label_create(sbar);
    lv_obj_set_style_text_color(s_ie.status_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_ie.status_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_ie.status_lbl, "Done");
    lv_obj_align(s_ie.status_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    /* Keyboard, hidden by default */
    s_ie.kb = win95_kb_create(s_ie.screen);
    lv_obj_set_size(s_ie.kb, IE_W, IE_KB_H);
    lv_obj_align(s_ie.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_ie.kb, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_text_color(s_ie.kb, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_ie.kb, LV_OBJ_FLAG_HIDDEN);

    /* Welcome splash */
    __ie_show_message_page(
        "<h1>Tuya Navigator 2.0</h1>"
        "<p>Welcome to <b>Tuya Navigator</b>.</p>"
        "<p>This browser speaks <i>HTTP/1.0</i> over a raw socket. "
        "Press <b>Home</b> or enter an address above to browse.</p>"
        "<hr>"
        "<p>Try: <a href=\"http://info.cern.ch/\">info.cern.ch</a> "
        "- the first website ever made.</p>"
        "<p>Also try: <a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">TheProject.html</a></p>");
    __ie_set_status("Ready.");
}
