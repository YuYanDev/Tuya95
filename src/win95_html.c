/**
 * @file win95_html.c
 * @brief Minimal HTML renderer for Tuya Navigator. Parses h1-h6, p, br, hr,
 *        ul/ol/li, a, b/strong, i/em, font color, img alt, pre/code,
 *        table/tr/td/th, div/center, blockquote, title. JavaScript blocks are
 *        preprocessed via win95_js: document.write() output is injected
 *        in-place and side effects (alert, title, navigate) are captured.
 * @version 2.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_html.h"
#include "win95_js.h"
#include "tal_api.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define INLINE_BUF_MAX      4096
#define MAX_EMIT_WIDGETS    512
#define LINE_WRAP_WIDTH     444
#define WIN95_HTML_DOM_MAX  96
#define WIN95_HTML_ATTR_MAX 8
#define WIN95_HTML_FORM_MAX 12
#define WIN95_HTML_STACK_MAX 8
#define WIN95_HTML_TEXT_MAX 256

#define COLOR_TEXT      0x000000
#define COLOR_LINK      0x0000C0
#define COLOR_HEADING   0x000080
#define COLOR_ITALIC    0x404040
#define COLOR_PRE_BG    0xE8E8E8
#define COLOR_HR        0x808080

/* ---------------------------------------------------------------------------
 * CSS support
 * --------------------------------------------------------------------------- */
#define CSS_SEL_MAX   48
#define CSS_MAX_RULES 32

#define CSS_F_COLOR  0x01u   /* color */
#define CSS_F_BGCLR  0x02u   /* background-color */
#define CSS_F_BOLD   0x04u   /* font-weight:bold  (no bold font; kept for future) */
#define CSS_F_ITALIC 0x08u   /* font-style:italic */
#define CSS_F_LARGE  0x10u   /* font-size >= 16px  → lv_font_unscii_16 */
#define CSS_F_NONE   0x20u   /* display:none */
#define CSS_F_CENTER 0x40u   /* text-align:center */
#define CSS_F_UNDERLINE 0x80u /* text-decoration:underline */
/* Extended flags (flags2 field) */
#define CSS_F2_RIGHT   0x0001u  /* text-align:right */
#define CSS_F2_SMALL   0x0002u  /* font-size < 12px */
#define CSS_F2_STRIKE  0x0004u  /* text-decoration:line-through */
#define CSS_F2_PAD     0x0008u  /* padding specified */
#define CSS_F2_MARGIN  0x0010u  /* margin specified */
#define CSS_F2_LSPACE  0x0020u  /* line-height specified */
#define CSS_F2_WIDTH   0x0040u  /* width specified */
#define CSS_F2_HEIGHT  0x0080u  /* height specified */
#define CSS_F2_BORDER  0x0100u  /* border width/color specified */
#define CSS_F2_ABS     0x0200u  /* position:absolute */

typedef struct {
    CHAR_T   sel[CSS_SEL_MAX]; /* "p", ".cls", "#id" */
    UINT8_T  flags;
    UINT16_T flags2;          /* extended flags */
    UINT32_T color;
    UINT32_T bg_color;
    UINT32_T border_color;
    UINT8_T  padding_px;      /* CSS padding in pixels */
    UINT8_T  margin_px;       /* CSS margin in pixels */
    UINT16_T width_px;
    UINT16_T height_px;
    UINT8_T  border_px;
    INT8_T   line_space;      /* extra line spacing (-1 = unset) */
    INT16_T  left_px;
    INT16_T  top_px;
} CSS_RULE_T;

typedef struct {
    CHAR_T name[24];
    CHAR_T value[96];
} HTML_ATTR_T;

typedef struct {
    BOOL_T   used;
    lv_obj_t *obj;
    INT16_T  form_idx;
    UINT8_T  is_container;
    UINT8_T  attr_count;
    CHAR_T   id[48];
    CHAR_T   tag[16];
    CHAR_T   name[48];
    CHAR_T   type[24];
    CHAR_T   text[WIN95_HTML_TEXT_MAX];
    HTML_ATTR_T attrs[WIN95_HTML_ATTR_MAX];
} HTML_DOM_NODE_T;

typedef struct {
    BOOL_T   used;
    lv_obj_t *obj;
    CHAR_T   id[48];
    CHAR_T   method[8];
    CHAR_T   action[256];
    CHAR_T   enctype[48];
} HTML_FORM_T;

typedef struct {
    lv_obj_t            *container;
    WIN95_HTML_CALLBACKS_T callbacks;

    CHAR_T              *inline_buf;
    UINT32_T             inline_len;

    UINT8_T              in_script;
    UINT8_T              in_style;
    UINT8_T              in_title;
    UINT8_T              in_head;

    UINT8_T              heading_level;
    UINT8_T              in_pre;
    UINT8_T              in_li;
    UINT8_T              list_ord;
    UINT8_T              list_idx;
    UINT8_T              in_anchor;
    CHAR_T               anchor_href[256];
    CHAR_T               cur_name[48];
    CHAR_T               cur_type[24];

    /* Inline style state */
    UINT32_T             text_color;
    UINT8_T              italic;
    UINT8_T              in_bold;
    UINT8_T              in_underline;
    UINT32_T             font_clr_stk[4];
    UINT8_T              font_depth;

    /* CSS rule table (populated from <style> blocks) */
    CSS_RULE_T           css_rules[CSS_MAX_RULES];
    UINT32_T             css_rule_count;
    CHAR_T              *style_buf;       /* accumulates <style> block text */
    UINT32_T             style_len;

    /* Current element CSS state */
    UINT32_T             bg_color;        /* background-color from CSS */
    UINT8_T              display_none;    /* skip widget emission */
    UINT8_T              use_large_font;  /* font-size >= 16px */
    UINT8_T              css_center;      /* text-align:center */
    UINT8_T              css_underline;   /* text-decoration:underline from CSS */
    UINT8_T              css_right;       /* text-align:right */
    UINT8_T              css_strike_css;  /* text-decoration:line-through from CSS */
    UINT8_T              css_padding;     /* padding in px (0 = none) */
    UINT8_T              css_margin;      /* margin in px (0 = none) */
    INT8_T               css_line_space;  /* extra line space in px (-1 = unset) */
    UINT16_T             css_width;       /* width in px (0 = unset) */
    UINT16_T             css_height;      /* height in px (0 = unset) */
    UINT8_T              css_border;      /* border width in px */
    UINT32_T             css_border_color;
    UINT8_T              css_abs_pos;     /* position:absolute */
    INT16_T              css_left;
    INT16_T              css_top;
    CHAR_T               cur_class[48];   /* class= of current element */
    CHAR_T               cur_id[48];      /* id= of current element */

    UINT32_T             emit_count;

    /* TABLE layout stack */
    lv_obj_t            *tbl_stack[6];
    UINT8_T              tbl_depth;

    /* MARQUEE state */
    UINT8_T              in_marquee;

    /* SELECT/OPTION state */
    UINT8_T              in_select;
    lv_obj_t            *select_obj;
    CHAR_T               select_opts[256];
    INT16_T              select_dom_idx;

    /* Strikethrough */
    UINT8_T              in_strike;

    /* Body-level colors (from <body> attrs) */
    UINT32_T             body_link_color;

    /* DOM/form tracking */
    INT16_T              cur_form_idx;
    INT16_T              textarea_dom_idx;
    INT16_T              button_dom_idx;
    lv_obj_t            *button_label;
    lv_obj_t            *block_stack[WIN95_HTML_STACK_MAX];
    UINT8_T              block_depth;
} HTML_CTX_T;

STATIC WIN95_JS_RESULT_T      s_js_result;
STATIC WIN95_HTML_IMG_LIST_T  s_img_list;
STATIC WIN95_HTML_CALLBACKS_T s_callbacks;
STATIC HTML_DOM_NODE_T        s_dom_nodes[WIN95_HTML_DOM_MAX];
STATIC HTML_FORM_T            s_forms[WIN95_HTML_FORM_MAX];
STATIC UINT32_T               s_dom_count;
STATIC UINT32_T               s_form_count;

/* Static href pool — avoids lv_malloc per link (never freed by LVGL on delete) */
#define HREF_POOL_MAX  64
#define HREF_URL_MAX   256
STATIC __attribute__((section(".psram.bss"))) CHAR_T   s_href_pool[HREF_POOL_MAX][HREF_URL_MAX];
STATIC UINT32_T  s_href_pool_idx = 0;

/* Static inline accumulation buffer — avoids per-render tal_malloc */
STATIC __attribute__((section(".psram.bss"))) CHAR_T   s_inline_buf[INLINE_BUF_MAX];

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __flush_inline(HTML_CTX_T *ctx);
STATIC OPERATE_RET __render_html(lv_obj_t *container, CONST CHAR_T *html,
                                  CONST WIN95_HTML_CALLBACKS_T *callbacks,
                                  BOOL_T reset_state, BOOL_T preprocess_js);
STATIC BOOL_T __submit_form_idx(INT16_T form_idx);

/* ---------------------------------------------------------------------------
 * Public accessor
 * --------------------------------------------------------------------------- */
CONST WIN95_JS_RESULT_T *win95_html_get_js_result(VOID_T)
{
    return &s_js_result;
}

CONST WIN95_HTML_IMG_LIST_T *win95_html_get_img_list(VOID_T)
{
    return &s_img_list;
}

STATIC BOOL_T __obj_in_tree(lv_obj_t *obj, lv_obj_t *ancestor)
{
    while (obj) {
        if (obj == ancestor) return TRUE;
        obj = lv_obj_get_parent(obj);
    }
    return FALSE;
}

STATIC INT32_T __dom_alloc_slot(VOID_T)
{
    for (UINT32_T i = 0; i < s_dom_count; i++) {
        if (!s_dom_nodes[i].used) {
            memset(&s_dom_nodes[i], 0, sizeof(s_dom_nodes[i]));
            s_dom_nodes[i].used = TRUE;
            return (INT32_T)i;
        }
    }
    if (s_dom_count >= WIN95_HTML_DOM_MAX) {
        return -1;
    }
    memset(&s_dom_nodes[s_dom_count], 0, sizeof(s_dom_nodes[s_dom_count]));
    s_dom_nodes[s_dom_count].used = TRUE;
    return (INT32_T)s_dom_count++;
}

STATIC INT32_T __form_alloc_slot(VOID_T)
{
    for (UINT32_T i = 0; i < s_form_count; i++) {
        if (!s_forms[i].used) {
            memset(&s_forms[i], 0, sizeof(s_forms[i]));
            s_forms[i].used = TRUE;
            return (INT32_T)i;
        }
    }
    if (s_form_count >= WIN95_HTML_FORM_MAX) {
        return -1;
    }
    memset(&s_forms[s_form_count], 0, sizeof(s_forms[s_form_count]));
    s_forms[s_form_count].used = TRUE;
    return (INT32_T)s_form_count++;
}

STATIC HTML_DOM_NODE_T *__dom_find_by_id(CONST CHAR_T *id)
{
    if (!id || !id[0]) return NULL;
    for (INT32_T i = (INT32_T)s_dom_count - 1; i >= 0; i--) {
        if (s_dom_nodes[i].used && strcmp(s_dom_nodes[i].id, id) == 0) {
            return &s_dom_nodes[i];
        }
    }
    return NULL;
}

STATIC VOID_T __dom_store_attr(HTML_DOM_NODE_T *node, CONST CHAR_T *name, CONST CHAR_T *value)
{
    if (!node || !name || !name[0] || !value) return;
    for (UINT8_T i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, name) == 0) {
            strncpy(node->attrs[i].value, value, sizeof(node->attrs[i].value) - 1);
            node->attrs[i].value[sizeof(node->attrs[i].value) - 1] = '\0';
            return;
        }
    }
    if (node->attr_count >= WIN95_HTML_ATTR_MAX) return;
    strncpy(node->attrs[node->attr_count].name, name,
            sizeof(node->attrs[node->attr_count].name) - 1);
    node->attrs[node->attr_count].name[sizeof(node->attrs[node->attr_count].name) - 1] = '\0';
    strncpy(node->attrs[node->attr_count].value, value,
            sizeof(node->attrs[node->attr_count].value) - 1);
    node->attrs[node->attr_count].value[sizeof(node->attrs[node->attr_count].value) - 1] = '\0';
    node->attr_count++;
}

STATIC CONST CHAR_T *__dom_get_attr_local(CONST HTML_DOM_NODE_T *node, CONST CHAR_T *name)
{
    if (!node || !name) return NULL;
    for (UINT8_T i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, name) == 0) {
            return node->attrs[i].value;
        }
    }
    return NULL;
}

STATIC INT32_T __dom_register(lv_obj_t *obj, CONST CHAR_T *tag, CONST CHAR_T *id,
                               CONST CHAR_T *name, CONST CHAR_T *type,
                               INT16_T form_idx, BOOL_T is_container)
{
    INT32_T idx = __dom_alloc_slot();
    if (idx < 0) return -1;
    HTML_DOM_NODE_T *node = &s_dom_nodes[idx];
    node->obj = obj;
    node->form_idx = form_idx;
    node->is_container = is_container ? 1 : 0;
    if (tag) {
        strncpy(node->tag, tag, sizeof(node->tag) - 1);
        node->tag[sizeof(node->tag) - 1] = '\0';
    }
    if (id) {
        strncpy(node->id, id, sizeof(node->id) - 1);
        node->id[sizeof(node->id) - 1] = '\0';
    }
    if (name) {
        strncpy(node->name, name, sizeof(node->name) - 1);
        node->name[sizeof(node->name) - 1] = '\0';
    }
    if (type) {
        strncpy(node->type, type, sizeof(node->type) - 1);
        node->type[sizeof(node->type) - 1] = '\0';
    }
    return idx;
}

STATIC VOID_T __dom_prune_container(lv_obj_t *container)
{
    if (!container) return;
    for (UINT32_T i = 0; i < s_dom_count; i++) {
        if (s_dom_nodes[i].used && s_dom_nodes[i].obj &&
            __obj_in_tree(s_dom_nodes[i].obj, container)) {
            memset(&s_dom_nodes[i], 0, sizeof(s_dom_nodes[i]));
        }
    }
}

STATIC VOID_T __block_push(HTML_CTX_T *ctx, lv_obj_t *obj)
{
    if (ctx->block_depth < WIN95_HTML_STACK_MAX) {
        ctx->block_stack[ctx->block_depth++] = ctx->container;
        ctx->container = obj;
    }
}

STATIC VOID_T __block_pop(HTML_CTX_T *ctx)
{
    if (ctx->block_depth > 0) {
        ctx->container = ctx->block_stack[--ctx->block_depth];
    }
}

/* ---------------------------------------------------------------------------
 * Inline buffer helpers
 * --------------------------------------------------------------------------- */
STATIC INT32_T __tag_eq(CONST CHAR_T *tag, UINT32_T tag_len, CONST CHAR_T *name)
{
    UINT32_T nlen = (UINT32_T)strlen(name);
    if (tag_len != nlen) return 0;
    for (UINT32_T i = 0; i < nlen; i++) {
        CHAR_T a = tag[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (CHAR_T)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (CHAR_T)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

STATIC VOID_T __inline_append(HTML_CTX_T *ctx, CHAR_T c)
{
    if (ctx->inline_buf == NULL) return;
    if (ctx->inline_len + 1 >= INLINE_BUF_MAX) return;
    if (!ctx->in_pre && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
        if (ctx->inline_len == 0) return;
        CHAR_T last = ctx->inline_buf[ctx->inline_len - 1];
        if (last == ' ' || last == '\n') return;
        c = ' ';
    }
    ctx->inline_buf[ctx->inline_len++] = c;
    ctx->inline_buf[ctx->inline_len] = '\0';
}

STATIC VOID_T __inline_clear(HTML_CTX_T *ctx)
{
    if (ctx->inline_buf) ctx->inline_buf[0] = '\0';
    ctx->inline_len = 0;
}

STATIC VOID_T __trim_inline(HTML_CTX_T *ctx)
{
    while (ctx->inline_len > 0) {
        CHAR_T c = ctx->inline_buf[ctx->inline_len - 1];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') ctx->inline_len--;
        else break;
    }
    if (ctx->inline_buf) ctx->inline_buf[ctx->inline_len] = '\0';
    if (ctx->inline_len > 0 && ctx->inline_buf) {
        UINT32_T i = 0;
        while (i < ctx->inline_len) {
            CHAR_T c = ctx->inline_buf[i];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') i++;
            else break;
        }
        if (i > 0) {
            memmove(ctx->inline_buf, &ctx->inline_buf[i], ctx->inline_len - i + 1);
            ctx->inline_len -= i;
        }
    }
}

/* ---------------------------------------------------------------------------
 * LVGL widget emitters
 * --------------------------------------------------------------------------- */
STATIC BOOL_T __can_emit(HTML_CTX_T *ctx)
{
    return ctx->emit_count < MAX_EMIT_WIDGETS;
}

STATIC VOID_T __apply_box_style(HTML_CTX_T *ctx, lv_obj_t *obj)
{
    if (!ctx || !obj) return;
    if (ctx->css_width > 0) {
        lv_obj_set_width(obj, (lv_coord_t)ctx->css_width);
    }
    if (ctx->css_height > 0) {
        lv_obj_set_height(obj, (lv_coord_t)ctx->css_height);
    }
    if (ctx->css_border > 0) {
        lv_obj_set_style_border_width(obj, (lv_coord_t)ctx->css_border, 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(ctx->css_border_color), 0);
    }
    if (ctx->css_abs_pos) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_pos(obj, ctx->css_left, ctx->css_top);
    }
}

STATIC INT32_T __register_current_widget(HTML_CTX_T *ctx, lv_obj_t *obj,
                                          CONST CHAR_T *tag, CONST CHAR_T *text,
                                          BOOL_T is_container)
{
    if ((!ctx->cur_id[0] && !ctx->cur_name[0]) || !obj) {
        return -1;
    }
    INT32_T idx = __dom_register(obj, tag, ctx->cur_id, ctx->cur_name,
                                 ctx->cur_type, ctx->cur_form_idx, is_container);
    if (idx >= 0 && text) {
        strncpy(s_dom_nodes[idx].text, text, sizeof(s_dom_nodes[idx].text) - 1);
        s_dom_nodes[idx].text[sizeof(s_dom_nodes[idx].text) - 1] = '\0';
    }
    return idx;
}

STATIC lv_obj_t *__make_label(lv_obj_t *parent, CONST CHAR_T *text,
                                CONST lv_font_t *font, UINT32_T color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LINE_WRAP_WIDTH);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

STATIC VOID_T __emit_paragraph(HTML_CTX_T *ctx, CONST CHAR_T *text)
{
    if (!__can_emit(ctx)) return;
    if (text == NULL || text[0] == '\0') return;
    if (ctx->display_none) return;
    UINT32_T color = ctx->text_color;
    /* Italic: teal color + underline decoration as visual stand-in */
    if (ctx->italic)      color = 0x006060;
    /* Bold: dark navy color */
    if (ctx->in_bold)     color = 0x000060;
    CONST lv_font_t *font = ctx->use_large_font ? &lv_font_unscii_16 : &lv_font_unscii_8;
    lv_obj_t *l = __make_label(ctx->container, text, font, color);
    if (ctx->bg_color) {
        lv_obj_set_style_bg_color(l, lv_color_hex(ctx->bg_color), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    }
    if (ctx->css_center) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    } else if (ctx->css_right) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    /* Text decorations */
    lv_text_decor_t decor = LV_TEXT_DECOR_NONE;
    if (ctx->italic)        decor |= (lv_text_decor_t)LV_TEXT_DECOR_UNDERLINE;
    if (ctx->in_underline)  decor |= (lv_text_decor_t)LV_TEXT_DECOR_UNDERLINE;
    if (ctx->css_underline) decor |= (lv_text_decor_t)LV_TEXT_DECOR_UNDERLINE;
    if (ctx->in_strike)     decor |= (lv_text_decor_t)LV_TEXT_DECOR_STRIKETHROUGH;
    if (ctx->css_strike_css) decor |= (lv_text_decor_t)LV_TEXT_DECOR_STRIKETHROUGH;
    if (decor != LV_TEXT_DECOR_NONE)
        lv_obj_set_style_text_decor(l, decor, 0);
    /* Padding / margin / line-height */
    if (ctx->css_padding > 0)
        lv_obj_set_style_pad_all(l, (lv_coord_t)ctx->css_padding, 0);
    if (ctx->css_margin > 0)
        lv_obj_set_style_margin_all(l, (lv_coord_t)ctx->css_margin, 0);
    if (ctx->css_line_space >= 0)
        lv_obj_set_style_text_line_space(l, (lv_coord_t)ctx->css_line_space, 0);
    __apply_box_style(ctx, l);
    __register_current_widget(ctx, l, "p", text, FALSE);
    ctx->emit_count++;
}

STATIC VOID_T __emit_heading(HTML_CTX_T *ctx, UINT8_T level, CONST CHAR_T *text)
{
    if (!__can_emit(ctx)) return;
    if (text == NULL || text[0] == '\0') return;
    if (ctx->display_none) return;
    CONST lv_font_t *font = (level <= 2 || ctx->use_large_font)
                            ? &lv_font_unscii_16 : &lv_font_unscii_8;
    UINT32_T hcol = ctx->text_color != (UINT32_T)COLOR_TEXT ? ctx->text_color : (UINT32_T)COLOR_HEADING;
    lv_obj_t *l = __make_label(ctx->container, text, font, hcol);
    lv_obj_set_style_text_letter_space(l, 0, 0);
    if (ctx->bg_color) {
        lv_obj_set_style_bg_color(l, lv_color_hex(ctx->bg_color), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    }
    if (ctx->css_center) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    } else if (ctx->css_right) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_text_decor_t decor = (level <= 3) ? (lv_text_decor_t)LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE;
    if (ctx->in_underline)  decor |= (lv_text_decor_t)LV_TEXT_DECOR_UNDERLINE;
    if (ctx->css_underline) decor |= (lv_text_decor_t)LV_TEXT_DECOR_UNDERLINE;
    if (decor != LV_TEXT_DECOR_NONE)
        lv_obj_set_style_text_decor(l, decor, 0);
    if (ctx->css_padding > 0)
        lv_obj_set_style_pad_all(l, (lv_coord_t)ctx->css_padding, 0);
    if (ctx->css_margin > 0)
        lv_obj_set_style_margin_all(l, (lv_coord_t)ctx->css_margin, 0);
    if (ctx->css_line_space >= 0)
        lv_obj_set_style_text_line_space(l, (lv_coord_t)ctx->css_line_space, 0);
    __apply_box_style(ctx, l);
    __register_current_widget(ctx, l, "h", text, FALSE);
    ctx->emit_count++;
}

STATIC VOID_T __emit_list_item(HTML_CTX_T *ctx, CONST CHAR_T *text)
{
    if (!__can_emit(ctx)) return;
    if (text == NULL || text[0] == '\0') return;
    if (ctx->display_none) return;
    CHAR_T buf[INLINE_BUF_MAX + 16];
    if (ctx->list_ord) {
        snprintf(buf, sizeof(buf), "%u. %s", (UINT32_T)ctx->list_idx, text);
    } else {
        snprintf(buf, sizeof(buf), "* %s", text);
    }
    lv_obj_t *l = __make_label(ctx->container, buf, &lv_font_unscii_8, COLOR_TEXT);
    __apply_box_style(ctx, l);
    __register_current_widget(ctx, l, "li", buf, FALSE);
    ctx->emit_count++;
}

STATIC VOID_T __emit_pre(HTML_CTX_T *ctx, CONST CHAR_T *text)
{
    if (!__can_emit(ctx)) return;
    if (text == NULL || text[0] == '\0') return;
    lv_obj_t *l = __make_label(ctx->container, text, &lv_font_unscii_8, COLOR_TEXT);
    lv_obj_set_style_bg_color(l, lv_color_hex(COLOR_PRE_BG), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(l, 2, 0);
    __apply_box_style(ctx, l);
    __register_current_widget(ctx, l, "pre", text, FALSE);
    ctx->emit_count++;
}

STATIC VOID_T __emit_hr(HTML_CTX_T *ctx)
{
    if (!__can_emit(ctx)) return;
    lv_obj_t *hr = lv_obj_create(ctx->container);
    lv_obj_remove_style_all(hr);
    lv_obj_set_size(hr, LINE_WRAP_WIDTH, 1);
    lv_obj_set_style_bg_color(hr, lv_color_hex(COLOR_HR), 0);
    lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);
    __apply_box_style(ctx, hr);
    __register_current_widget(ctx, hr, "hr", "", FALSE);
    ctx->emit_count++;
}

/* Table container stack helpers */
STATIC VOID_T __tbl_push(HTML_CTX_T *ctx, lv_obj_t *new_cont)
{
    if (ctx->tbl_depth < 6)
        ctx->tbl_stack[ctx->tbl_depth++] = ctx->container;
    ctx->container = new_cont;
}

STATIC VOID_T __tbl_pop(HTML_CTX_T *ctx)
{
    if (ctx->tbl_depth > 0)
        ctx->container = ctx->tbl_stack[--ctx->tbl_depth];
}

/* Marquee animated label */
STATIC VOID_T __emit_marquee(HTML_CTX_T *ctx, CONST CHAR_T *text)
{
    if (!__can_emit(ctx) || !text || !text[0]) return;
    lv_obj_t *box = lv_obj_create(ctx->container);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LINE_WRAP_WIDTH, 14);
    lv_obj_set_style_clip_corner(box, TRUE, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = lv_label_create(box);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_obj_set_pos(lbl, LINE_WRAP_WIDTH, 1);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, LINE_WRAP_WIDTH, -(INT32_T)(strlen(text) * 6 + 10));
    lv_anim_set_duration(&a, 4000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    __apply_box_style(ctx, box);
    __register_current_widget(ctx, box, "marquee", text, TRUE);
    ctx->emit_count++;
}

STATIC VOID_T __link_clicked_cb(lv_event_t *e)
{
    CONST CHAR_T *href = (CONST CHAR_T *)lv_event_get_user_data(e);
    if (s_callbacks.link_cb && href) {
        s_callbacks.link_cb(href, s_callbacks.user_data);
    }
}

STATIC VOID_T __emit_link(HTML_CTX_T *ctx, CONST CHAR_T *text, CONST CHAR_T *href)
{
    if (!__can_emit(ctx)) return;
    if (text == NULL || text[0] == '\0') return;
    UINT32_T lcolor = ctx->body_link_color ? ctx->body_link_color : (UINT32_T)COLOR_LINK;
    lv_obj_t *l = __make_label(ctx->container, text, &lv_font_unscii_8, lcolor);
    lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (href && href[0] && s_href_pool_idx < HREF_POOL_MAX) {
        strncpy(s_href_pool[s_href_pool_idx], href, HREF_URL_MAX - 1);
        s_href_pool[s_href_pool_idx][HREF_URL_MAX - 1] = '\0';
        lv_obj_add_event_cb(l, __link_clicked_cb, LV_EVENT_CLICKED,
                            s_href_pool[s_href_pool_idx]);
        s_href_pool_idx++;
    }
    __apply_box_style(ctx, l);
    __register_current_widget(ctx, l, "a", text, FALSE);
    ctx->emit_count++;
}

STATIC VOID_T __flush_inline(HTML_CTX_T *ctx)
{
    if (ctx->inline_len == 0) return;
    if (!ctx->in_pre) __trim_inline(ctx);
    if (ctx->inline_len == 0) return;

    if (ctx->in_anchor && ctx->anchor_href[0] != '\0') {
        __emit_link(ctx, ctx->inline_buf, ctx->anchor_href);
    } else if (ctx->in_pre) {
        __emit_pre(ctx, ctx->inline_buf);
    } else if (ctx->heading_level > 0) {
        __emit_heading(ctx, ctx->heading_level, ctx->inline_buf);
    } else if (ctx->in_li) {
        __emit_list_item(ctx, ctx->inline_buf);
    } else {
        __emit_paragraph(ctx, ctx->inline_buf);
    }
    __inline_clear(ctx);
}

/* ---------------------------------------------------------------------------
 * Entity decoding
 * --------------------------------------------------------------------------- */
STATIC UINT32_T __decode_entity(CONST CHAR_T *p, CHAR_T *out)
{
    if (p[0] != '&') { *out = p[0]; return 1; }
    CONST CHAR_T *end = p + 1;
    while (*end && *end != ';' && (end - p) < 10) end++;
    if (*end != ';') { *out = '&'; return 1; }
    UINT32_T elen = (UINT32_T)(end - p) + 1;

    if (strncmp(p, "&amp;", 5) == 0)   { *out = '&';  return elen; }
    if (strncmp(p, "&lt;",  4) == 0)   { *out = '<';  return elen; }
    if (strncmp(p, "&gt;",  4) == 0)   { *out = '>';  return elen; }
    if (strncmp(p, "&quot;",6) == 0)   { *out = '"';  return elen; }
    if (strncmp(p, "&apos;",6) == 0)   { *out = '\''; return elen; }
    if (strncmp(p, "&nbsp;",6) == 0)   { *out = ' ';  return elen; }
    if (strncmp(p, "&copy;",6) == 0)   { *out = '(';  return elen; } /* (c) */
    if (strncmp(p, "&reg;", 5) == 0)   { *out = '(';  return elen; } /* (r) */
    if (strncmp(p, "&trade;",7) == 0)  { *out = '(';  return elen; } /* (tm) */
    if (strncmp(p, "&mdash;",7) == 0)  { *out = '-';  return elen; } /* em dash */
    if (strncmp(p, "&ndash;",7) == 0)  { *out = '-';  return elen; } /* en dash */
    if (strncmp(p, "&hellip;",8)== 0)  { *out = '.';  return elen; } /* ... */
    if (strncmp(p, "&laquo;",7) == 0)  { *out = '<';  return elen; }
    if (strncmp(p, "&raquo;",7) == 0)  { *out = '>';  return elen; }
    if (strncmp(p, "&bull;", 6) == 0)  { *out = '*';  return elen; }
    if (strncmp(p, "&middot;",8)== 0)  { *out = '.';  return elen; }
    if (strncmp(p, "&minus;",7) == 0)  { *out = '-';  return elen; }
    if (strncmp(p, "&times;",7) == 0)  { *out = 'x';  return elen; }
    if (strncmp(p, "&divide;",8)== 0)  { *out = '/';  return elen; }
    if (strncmp(p, "&plusmn;",8)== 0)  { *out = '+';  return elen; }
    if (strncmp(p, "&deg;",  5) == 0)  { *out = 'o';  return elen; }
    if (strncmp(p, "&sect;", 6) == 0)  { *out = 'S';  return elen; }
    if (strncmp(p, "&para;", 6) == 0)  { *out = 'P';  return elen; }
    if (strncmp(p, "&pound;",7) == 0)  { *out = 'L';  return elen; }
    if (strncmp(p, "&euro;", 6) == 0)  { *out = 'E';  return elen; }
    if (strncmp(p, "&yen;",  5) == 0)  { *out = 'Y';  return elen; }
    if (strncmp(p, "&cent;", 6) == 0)  { *out = 'c';  return elen; }
    if (strncmp(p, "&frac12;",8)== 0)  { *out = '?';  return elen; } /* 1/2 */
    if (strncmp(p, "&frac14;",8)== 0)  { *out = '?';  return elen; } /* 1/4 */
    if (strncmp(p, "&sup2;", 6) == 0)  { *out = '2';  return elen; }
    if (strncmp(p, "&sup3;", 6) == 0)  { *out = '3';  return elen; }
    if (strncmp(p, "&lsquo;",7) == 0)  { *out = '\''; return elen; }
    if (strncmp(p, "&rsquo;",7) == 0)  { *out = '\''; return elen; }
    if (strncmp(p, "&ldquo;",7) == 0)  { *out = '"';  return elen; }
    if (strncmp(p, "&rdquo;",7) == 0)  { *out = '"';  return elen; }
    if (strncmp(p, "&sbquo;",7) == 0)  { *out = ',';  return elen; }
    if (strncmp(p, "&prime;",7) == 0)  { *out = '\''; return elen; }
    if (strncmp(p, "&Prime;",7) == 0)  { *out = '"';  return elen; }
    if (p[1] == '#') {
        UINT32_T val = 0;
        CONST CHAR_T *np = p + 2;
        if (*np == 'x' || *np == 'X') {
            np++;
            while (np < end) {
                CHAR_T c = *np++;
                if (c >= '0' && c <= '9') val = val * 16 + (UINT32_T)(c - '0');
                else if (c >= 'a' && c <= 'f') val = val * 16 + (UINT32_T)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') val = val * 16 + (UINT32_T)(c - 'A' + 10);
                else break;
            }
        } else {
            while (np < end && *np >= '0' && *np <= '9') {
                val = val * 10 + (UINT32_T)(*np - '0');
                np++;
            }
        }
        *out = (val < 128) ? (CHAR_T)val : '?';
        return elen;
    }
    *out = '&';
    return 1;
}

/* ---------------------------------------------------------------------------
 * Attribute extraction
 * --------------------------------------------------------------------------- */
STATIC VOID_T __extract_href(CONST CHAR_T *attrs, UINT32_T attrs_len,
                              CHAR_T *out, UINT32_T cap)
{
    out[0] = '\0';
    for (UINT32_T i = 0; i + 4 <= attrs_len; i++) {
        if ((attrs[i]   == 'h' || attrs[i]   == 'H') &&
            (attrs[i+1] == 'r' || attrs[i+1] == 'R') &&
            (attrs[i+2] == 'e' || attrs[i+2] == 'E') &&
            (attrs[i+3] == 'f' || attrs[i+3] == 'F')) {
            UINT32_T j = i + 4;
            while (j < attrs_len && (attrs[j] == ' ' || attrs[j] == '\t')) j++;
            if (j >= attrs_len || attrs[j] != '=') continue;
            j++;
            while (j < attrs_len && (attrs[j] == ' ' || attrs[j] == '\t')) j++;
            CHAR_T quote = 0;
            if (j < attrs_len && (attrs[j] == '"' || attrs[j] == '\'')) {
                quote = attrs[j++];
            }
            UINT32_T n = 0;
            while (j < attrs_len && n + 1 < cap) {
                CHAR_T c = attrs[j];
                if (quote) { if (c == quote) break; }
                else       { if (c == ' ' || c == '\t' || c == '>') break; }
                out[n++] = c; j++;
            }
            out[n] = '\0';
            return;
        }
    }
}

STATIC VOID_T __extract_attr(CONST CHAR_T *attrs, UINT32_T alen,
                               CONST CHAR_T *attr_name,
                               CHAR_T *out, UINT32_T cap)
{
    out[0] = '\0';
    UINT32_T klen = (UINT32_T)strlen(attr_name);
    for (UINT32_T i = 0; i + klen <= alen; i++) {
        BOOL_T match = TRUE;
        for (UINT32_T k = 0; k < klen; k++) {
            CHAR_T a = attrs[i + k], b = attr_name[k];
            if (a >= 'A' && a <= 'Z') a = (CHAR_T)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (CHAR_T)(b + 32);
            if (a != b) { match = FALSE; break; }
        }
        if (!match) continue;
        UINT32_T j = i + klen;
        while (j < alen && (attrs[j] == ' ' || attrs[j] == '\t')) j++;
        if (j >= alen || attrs[j] != '=') continue;
        j++;
        while (j < alen && (attrs[j] == ' ' || attrs[j] == '\t')) j++;
        CHAR_T q = 0;
        if (j < alen && (attrs[j] == '"' || attrs[j] == '\'')) { q = attrs[j++]; }
        UINT32_T n = 0;
        while (j < alen && n + 1 < cap) {
            CHAR_T c = attrs[j];
            if (q ? c == q : (c == ' ' || c == '\t' || c == '>')) break;
            out[n++] = c; j++;
        }
        out[n] = '\0';
        return;
    }
}

STATIC UINT32_T __parse_color(CONST CHAR_T *s)
{
    if (!s || !*s) return (UINT32_T)COLOR_TEXT;
    if (*s == '#') {
        s++;
        UINT32_T v = 0;
        for (INT32_T i = 0; i < 6 && *s; i++, s++) {
            CHAR_T c = *s;
            if (c >= '0' && c <= '9') v = v * 16 + (UINT32_T)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (UINT32_T)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (UINT32_T)(c - 'A' + 10);
            else break;
        }
        return v;
    }
    /* Named colors (IE4 palette + CSS1 extensions) */
    if (strncmp(s, "red",         3) == 0) return 0xFF0000U;
    if (strncmp(s, "green",       5) == 0) return 0x008000U;
    if (strncmp(s, "blue",        4) == 0) return 0x0000FFU;
    if (strncmp(s, "white",       5) == 0) return 0xFFFFFFU;
    if (strncmp(s, "black",       5) == 0) return 0x000000U;
    if (strncmp(s, "yellow",      6) == 0) return 0xFFFF00U;
    if (strncmp(s, "orange",      6) == 0) return 0xFF8000U;
    if (strncmp(s, "purple",      6) == 0) return 0x800080U;
    if (strncmp(s, "gray",        4) == 0 ||
        strncmp(s, "grey",        4) == 0) return 0x808080U;
    if (strncmp(s, "cyan",        4) == 0 ||
        strncmp(s, "aqua",        4) == 0) return 0x00FFFFU;
    if (strncmp(s, "lime",        4) == 0) return 0x00FF00U;
    if (strncmp(s, "maroon",      6) == 0) return 0x800000U;
    if (strncmp(s, "navy",        4) == 0) return 0x000080U;
    if (strncmp(s, "silver",      6) == 0) return 0xC0C0C0U;
    if (strncmp(s, "teal",        4) == 0) return 0x008080U;
    if (strncmp(s, "magenta",     7) == 0 ||
        strncmp(s, "fuchsia",     7) == 0) return 0xFF00FFU;
    if (strncmp(s, "olive",       5) == 0) return 0x808000U;
    if (strncmp(s, "coral",       5) == 0) return 0xFF6347U;
    if (strncmp(s, "salmon",      6) == 0) return 0xFA8072U;
    if (strncmp(s, "khaki",       5) == 0) return 0xF0E68CU;
    if (strncmp(s, "turquoise",   9) == 0) return 0x40E0D0U;
    if (strncmp(s, "violet",      6) == 0) return 0xEE82EEU;
    if (strncmp(s, "indigo",      6) == 0) return 0x4B0082U;
    if (strncmp(s, "brown",       5) == 0) return 0xA52A2AU;
    if (strncmp(s, "pink",        4) == 0) return 0xFFC0CBU;
    if (strncmp(s, "beige",       5) == 0) return 0xF5F5DCU;
    if (strncmp(s, "tan",         3) == 0) return 0xD2B48CU;
    if (strncmp(s, "lavender",    8) == 0) return 0xE6E6FAU;
    if (strncmp(s, "crimson",     7) == 0) return 0xDC143CU;
    if (strncmp(s, "gold",        4) == 0) return 0xFFD700U;
    if (strncmp(s, "lightblue",   9) == 0) return 0xADD8E6U;
    if (strncmp(s, "lightgreen", 10) == 0) return 0x90EE90U;
    if (strncmp(s, "lightgray",   9) == 0 ||
        strncmp(s, "lightgrey",   9) == 0) return 0xD3D3D3U;
    if (strncmp(s, "darkblue",    8) == 0) return 0x00008BU;
    if (strncmp(s, "darkgreen",   9) == 0) return 0x006400U;
    if (strncmp(s, "darkred",     7) == 0) return 0x8B0000U;
    if (strncmp(s, "darkgray",    8) == 0 ||
        strncmp(s, "darkgrey",    8) == 0) return 0xA9A9A9U;
    if (strncmp(s, "darkorange", 10) == 0) return 0xFF8C00U;
    if (strncmp(s, "hotpink",     7) == 0) return 0xFF69B4U;
    if (strncmp(s, "skyblue",     7) == 0) return 0x87CEEBU;
    if (strncmp(s, "steelblue",   9) == 0) return 0x4682B4U;
    if (strncmp(s, "transparent", 11) == 0) return 0xFFFFFFU;
    return (UINT32_T)COLOR_TEXT;
}

/* ---------------------------------------------------------------------------
 * CSS helpers
 * --------------------------------------------------------------------------- */

/* Skip whitespace */
STATIC CONST CHAR_T *__css_skip_ws(CONST CHAR_T *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Case-insensitive prefix match: does haystack start with needle? */
STATIC BOOL_T __css_pfx(CONST CHAR_T *hay, CONST CHAR_T *needle)
{
    while (*needle) {
        CHAR_T a = *hay, b = *needle;
        if (a >= 'A' && a <= 'Z') a = (CHAR_T)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (CHAR_T)(b + 32);
        if (a != b) return FALSE;
        hay++; needle++;
    }
    return TRUE;
}

/* Parse CSS property block text into widget/text style fields. */
STATIC VOID_T __css_parse_props(UINT8_T *flags, UINT16_T *flags2,
                                  UINT32_T *color, UINT32_T *bg_color,
                                  UINT32_T *border_color,
                                  UINT8_T *padding_out, UINT8_T *margin_out,
                                  UINT16_T *width_out, UINT16_T *height_out,
                                  UINT8_T *border_out,
                                  INT16_T *left_out, INT16_T *top_out,
                                  INT8_T *line_space_out,
                                  CONST CHAR_T *text)
{
    CONST CHAR_T *p = text;
    while (*p) {
        p = __css_skip_ws(p);
        if (*p == '\0' || *p == '}') break;

        /* Collect key */
        CONST CHAR_T *key = p;
        while (*p && *p != ':' && *p != ';' && *p != '}') p++;
        if (*p != ':') { while (*p && *p != ';') p++; if (*p) p++; continue; }
        UINT32_T klen = (UINT32_T)(p - key);
        p++; /* skip ':' */
        p = __css_skip_ws(p);

        /* Collect value (up to ';' or '}') */
        CONST CHAR_T *val = p;
        while (*p && *p != ';' && *p != '}') p++;
        UINT32_T vlen = (UINT32_T)(p - val);
        if (*p == ';') p++;

        /* Match key */
        CHAR_T kbuf[32];
        if (klen >= sizeof(kbuf)) continue;
        for (UINT32_T i = 0; i < klen; i++) {
            CHAR_T c = key[i];
            kbuf[i] = (c >= 'A' && c <= 'Z') ? (CHAR_T)(c + 32) : c;
        }
        kbuf[klen] = '\0';

        /* Value as trimmed C-string */
        CHAR_T vbuf[64];
        if (vlen >= sizeof(vbuf)) continue;
        UINT32_T vi = 0;
        /* trim leading */
        CONST CHAR_T *vp = val;
        while (vi == 0 && vlen > 0 &&
               (*vp == ' ' || *vp == '\t')) { vp++; vlen--; }
        for (UINT32_T i = 0; i < vlen; i++) {
            CHAR_T c = vp[i];
            vbuf[i] = (c >= 'A' && c <= 'Z') ? (CHAR_T)(c + 32) : c;
        }
        vbuf[vlen] = '\0';
        /* trim trailing */
        INT32_T tl = (INT32_T)vlen - 1;
        while (tl >= 0 && (vbuf[tl] == ' ' || vbuf[tl] == '\t')) tl--;
        if (tl >= 0) vbuf[tl + 1] = '\0';

        if (strcmp(kbuf, "color") == 0) {
            *flags |= CSS_F_COLOR;
            *color = __parse_color(vbuf);
        } else if (strcmp(kbuf, "background-color") == 0 ||
                   strcmp(kbuf, "background") == 0) {
            *flags |= CSS_F_BGCLR;
            *bg_color = __parse_color(vbuf);
        } else if (strcmp(kbuf, "font-weight") == 0) {
            if (strcmp(vbuf, "bold") == 0) *flags |= CSS_F_BOLD;
        } else if (strcmp(kbuf, "font-style") == 0) {
            if (strcmp(vbuf, "italic") == 0 || strcmp(vbuf, "oblique") == 0)
                *flags |= CSS_F_ITALIC;
        } else if (strcmp(kbuf, "font-size") == 0) {
            INT32_T sz = 0;
            for (UINT32_T i = 0; vbuf[i] >= '0' && vbuf[i] <= '9'; i++)
                sz = sz * 10 + (vbuf[i] - '0');
            if (strcmp(vbuf, "small") == 0 || strcmp(vbuf, "x-small") == 0) {
                if (flags2) *flags2 |= CSS_F2_SMALL;
            } else if (sz >= 16 || strcmp(vbuf, "large") == 0 ||
                       strcmp(vbuf, "x-large") == 0 || strcmp(vbuf, "xx-large") == 0) {
                *flags |= CSS_F_LARGE;
            }
        } else if (strcmp(kbuf, "display") == 0) {
            if (strcmp(vbuf, "none") == 0) *flags |= CSS_F_NONE;
        } else if (strcmp(kbuf, "text-align") == 0) {
            if (strcmp(vbuf, "center") == 0)      *flags |= CSS_F_CENTER;
            else if (strcmp(vbuf, "right") == 0)  { if (flags2) *flags2 |= CSS_F2_RIGHT; }
        } else if (strcmp(kbuf, "text-decoration") == 0) {
            if (strstr(vbuf, "underline"))    *flags |= CSS_F_UNDERLINE;
            if (strstr(vbuf, "line-through")) { if (flags2) *flags2 |= CSS_F2_STRIKE; }
        } else if (strcmp(kbuf, "width") == 0) {
            INT32_T pv = atoi(vbuf);
            if (pv > 0 && width_out) {
                *width_out = (UINT16_T)(pv > 65535 ? 65535 : pv);
                if (flags2) *flags2 |= CSS_F2_WIDTH;
            }
        } else if (strcmp(kbuf, "height") == 0) {
            INT32_T pv = atoi(vbuf);
            if (pv > 0 && height_out) {
                *height_out = (UINT16_T)(pv > 65535 ? 65535 : pv);
                if (flags2) *flags2 |= CSS_F2_HEIGHT;
            }
        } else if (strcmp(kbuf, "left") == 0) {
            if (left_out) *left_out = (INT16_T)atoi(vbuf);
        } else if (strcmp(kbuf, "top") == 0) {
            if (top_out) *top_out = (INT16_T)atoi(vbuf);
        } else if (strcmp(kbuf, "position") == 0) {
            if (strcmp(vbuf, "absolute") == 0 && flags2) *flags2 |= CSS_F2_ABS;
        } else if (strcmp(kbuf, "border") == 0 ||
                   strcmp(kbuf, "border-width") == 0) {
            INT32_T pv = atoi(vbuf);
            if (pv <= 0 && strstr(vbuf, "solid")) pv = 1;
            if (pv > 0 && border_out) {
                *border_out = (UINT8_T)(pv > 255 ? 255 : pv);
                if (flags2) *flags2 |= CSS_F2_BORDER;
            }
            if (strchr(vbuf, '#') || __css_pfx(vbuf, "red") || __css_pfx(vbuf, "green") ||
                __css_pfx(vbuf, "blue") || __css_pfx(vbuf, "black") || __css_pfx(vbuf, "white") ||
                __css_pfx(vbuf, "gray") || __css_pfx(vbuf, "silver") || __css_pfx(vbuf, "navy")) {
                if (border_color) *border_color = __parse_color(vbuf);
            }
        } else if (strcmp(kbuf, "border-color") == 0) {
            if (border_color) *border_color = __parse_color(vbuf);
            if (flags2) *flags2 |= CSS_F2_BORDER;
        } else if (strcmp(kbuf, "padding") == 0 ||
                   strcmp(kbuf, "padding-top") == 0 || strcmp(kbuf, "padding-bottom") == 0 ||
                   strcmp(kbuf, "padding-left") == 0 || strcmp(kbuf, "padding-right") == 0 ||
                   strcmp(kbuf, "padding-all") == 0) {
            INT32_T pv = 0;
            for (UINT32_T i = 0; vbuf[i] >= '0' && vbuf[i] <= '9'; i++)
                pv = pv * 10 + (vbuf[i] - '0');
            if (pv > 0 && padding_out) { *padding_out = (UINT8_T)(pv > 255 ? 255 : pv); if (flags2) *flags2 |= CSS_F2_PAD; }
        } else if (strcmp(kbuf, "margin") == 0 ||
                   strcmp(kbuf, "margin-top") == 0 || strcmp(kbuf, "margin-bottom") == 0 ||
                   strcmp(kbuf, "margin-left") == 0 || strcmp(kbuf, "margin-right") == 0 ||
                   strcmp(kbuf, "margin-all") == 0) {
            INT32_T mv = 0;
            for (UINT32_T i = 0; vbuf[i] >= '0' && vbuf[i] <= '9'; i++)
                mv = mv * 10 + (vbuf[i] - '0');
            if (mv > 0 && margin_out) { *margin_out = (UINT8_T)(mv > 255 ? 255 : mv); if (flags2) *flags2 |= CSS_F2_MARGIN; }
        } else if (strcmp(kbuf, "line-height") == 0) {
            /* Parse em multiplier (1.5) or px value (24px); store extra pixels */
            INT32_T lv = 0; BOOL_T is_float = FALSE;
            UINT32_T ii = 0; INT32_T frac = 0, fdiv = 1;
            while (vbuf[ii] >= '0' && vbuf[ii] <= '9') lv = lv * 10 + (vbuf[ii++] - '0');
            if (vbuf[ii] == '.') { ii++;
                while (vbuf[ii] >= '0' && vbuf[ii] <= '9') { frac = frac*10+(vbuf[ii++]-'0'); fdiv*=10; }
                is_float = TRUE;
            }
            INT32_T lsp;
            if (strstr(vbuf, "px") || lv > 3) {
                /* pixel value: extra space = value - 8 (font height) */
                lsp = lv > 8 ? lv - 8 : 0;
            } else {
                /* em multiplier: extra space = (multiplier - 1) * 8 */
                lsp = (INT32_T)((lv - 1) * 8 + (is_float ? frac * 8 / fdiv : 0));
            }
            if (lsp >= 0 && lsp <= 32 && line_space_out) {
                *line_space_out = (INT8_T)lsp;
                if (flags2) *flags2 |= CSS_F2_LSPACE;
            }
        }
    }
}

/* Parse a full <style> block: "sel { props } sel2 { props2 } ..." */
STATIC VOID_T __parse_style_block(HTML_CTX_T *ctx, CONST CHAR_T *css)
{
    CONST CHAR_T *p = css;
    while (*p) {
        p = __css_skip_ws(p);
        if (*p == '\0') break;

        /* Collect selector (up to '{') */
        CONST CHAR_T *sel_start = p;
        while (*p && *p != '{') p++;
        if (*p != '{') break;
        UINT32_T slen = (UINT32_T)(p - sel_start);
        /* trim trailing whitespace from selector */
        while (slen > 0 && (sel_start[slen - 1] == ' ' ||
               sel_start[slen - 1] == '\t' || sel_start[slen - 1] == '\n')) slen--;
        p++; /* skip '{' */

        /* Collect property block (up to '}') */
        CONST CHAR_T *props_start = p;
        while (*p && *p != '}') p++;
        UINT32_T plen = (UINT32_T)(p - props_start);
        if (*p == '}') p++;

        CHAR_T pbuf[256];
        if (plen >= sizeof(pbuf)) continue;
        memcpy(pbuf, props_start, plen);
        pbuf[plen] = '\0';

        UINT8_T  fl = 0;
        UINT16_T fl2 = 0;
        UINT32_T col = 0, bg = 0, border_col = 0x808080;
        UINT8_T  pad = 0, marg = 0, border_px = 0;
        UINT16_T width_px = 0, height_px = 0;
        INT16_T left_px = 0, top_px = 0;
        INT8_T   lsp = -1;
        __css_parse_props(&fl, &fl2, &col, &bg, &border_col,
                          &pad, &marg, &width_px, &height_px, &border_px,
                          &left_px, &top_px, &lsp, pbuf);
        if (fl == 0 && fl2 == 0) continue;

        /* Selector may be comma-separated: "h1, .big, #main" */
        CONST CHAR_T *sp = sel_start;
        UINT32_T      rem = slen;
        while (rem > 0 && ctx->css_rule_count < CSS_MAX_RULES) {
            /* Find next comma */
            UINT32_T seg = rem;
            for (UINT32_T i = 0; i < rem; i++) {
                if (sp[i] == ',') { seg = i; break; }
            }
            /* Trim single selector segment */
            CONST CHAR_T *s2 = sp;
            UINT32_T l2 = seg;
            while (l2 > 0 && (*s2 == ' ' || *s2 == '\t' || *s2 == '\n'))
                { s2++; l2--; }
            while (l2 > 0 && (s2[l2-1] == ' ' || s2[l2-1] == '\t' || s2[l2-1] == '\n'))
                l2--;
            if (l2 > 0 && l2 < CSS_SEL_MAX) {
                CSS_RULE_T *r = &ctx->css_rules[ctx->css_rule_count++];
                for (UINT32_T i = 0; i < l2; i++) {
                    CHAR_T c = s2[i];
                    r->sel[i] = (c >= 'A' && c <= 'Z') ? (CHAR_T)(c + 32) : c;
                }
                r->sel[l2] = '\0';
                r->flags      = fl;
                r->flags2     = fl2;
                r->color      = col;
                r->bg_color   = bg;
                r->border_color = border_col;
                r->padding_px = pad;
                r->margin_px  = marg;
                r->width_px   = width_px;
                r->height_px  = height_px;
                r->border_px  = border_px;
                r->line_space = lsp;
                r->left_px    = left_px;
                r->top_px     = top_px;
            }
            sp  += seg + (seg < rem ? 1u : 0u);
            rem -= seg + (seg < rem ? 1u : 0u);
        }
    }
}

/* Apply inline style="..." to ctx directly. */
STATIC VOID_T __apply_inline_style(HTML_CTX_T *ctx, CONST CHAR_T *style_attr)
{
    if (!style_attr || !style_attr[0]) return;
    UINT8_T  fl = 0;
    UINT16_T fl2 = 0;
    UINT32_T col = 0, bg = 0, border_col = 0x808080;
    UINT8_T  pad = 0, marg = 0, border_px = 0;
    UINT16_T width_px = 0, height_px = 0;
    INT16_T  left_px = 0, top_px = 0;
    INT8_T   lsp = -1;
    __css_parse_props(&fl, &fl2, &col, &bg, &border_col,
                      &pad, &marg, &width_px, &height_px, &border_px,
                      &left_px, &top_px, &lsp, style_attr);
    if (fl & CSS_F_COLOR)     { ctx->text_color = col; }
    if (fl & CSS_F_BGCLR)     { ctx->bg_color   = bg;  }
    if (fl & CSS_F_ITALIC)    { ctx->italic++;           }
    if (fl & CSS_F_LARGE)     { ctx->use_large_font = 1; }
    if (fl & CSS_F_CENTER)    { ctx->css_center = 1;     }
    if (fl & CSS_F_NONE)      { ctx->display_none = 1;   }
    if (fl & CSS_F_UNDERLINE) { ctx->css_underline = 1;  }
    if (fl2 & CSS_F2_RIGHT)   { ctx->css_right = 1;      }
    if (fl2 & CSS_F2_STRIKE)  { ctx->css_strike_css = 1; }
    if (fl2 & CSS_F2_PAD)     { ctx->css_padding = pad;  }
    if (fl2 & CSS_F2_MARGIN)  { ctx->css_margin  = marg; }
    if (fl2 & CSS_F2_LSPACE)  { ctx->css_line_space = lsp; }
    if (fl2 & CSS_F2_WIDTH)   { ctx->css_width = width_px; }
    if (fl2 & CSS_F2_HEIGHT)  { ctx->css_height = height_px; }
    if (fl2 & CSS_F2_BORDER)  { ctx->css_border = border_px ? border_px : 1; ctx->css_border_color = border_col; }
    if (fl2 & CSS_F2_ABS)     { ctx->css_abs_pos = 1; ctx->css_left = left_px; ctx->css_top = top_px; }
}

/* Apply matching CSS rules for element tag, .class, #id to ctx. */
STATIC VOID_T __apply_element_css(HTML_CTX_T *ctx, CONST CHAR_T *tag, UINT32_T nlen)
{
    CHAR_T tbuf[32];
    if (nlen >= sizeof(tbuf)) return;
    for (UINT32_T i = 0; i < nlen; i++) {
        CHAR_T c = tag[i];
        tbuf[i] = (c >= 'A' && c <= 'Z') ? (CHAR_T)(c + 32) : c;
    }
    tbuf[nlen] = '\0';

    for (UINT32_T i = 0; i < ctx->css_rule_count; i++) {
        CONST CSS_RULE_T *r = &ctx->css_rules[i];
        BOOL_T match = FALSE;
        if (r->sel[0] == '.') {
            /* class selector */
            if (ctx->cur_class[0] && strcmp(r->sel + 1, ctx->cur_class) == 0) match = TRUE;
        } else if (r->sel[0] == '#') {
            /* id selector */
            if (ctx->cur_id[0] && strcmp(r->sel + 1, ctx->cur_id) == 0) match = TRUE;
        } else {
            /* element selector */
            if (strcmp(r->sel, tbuf) == 0) match = TRUE;
        }
        if (!match) continue;

        if (r->flags & CSS_F_COLOR)     ctx->text_color    = r->color;
        if (r->flags & CSS_F_BGCLR)     ctx->bg_color      = r->bg_color;
        if (r->flags & CSS_F_ITALIC)    ctx->italic++;
        if (r->flags & CSS_F_LARGE)     ctx->use_large_font = 1;
        if (r->flags & CSS_F_CENTER)    ctx->css_center     = 1;
        if (r->flags & CSS_F_NONE)      ctx->display_none   = 1;
        if (r->flags & CSS_F_UNDERLINE) ctx->css_underline  = 1;
        if (r->flags2 & CSS_F2_RIGHT)   ctx->css_right      = 1;
        if (r->flags2 & CSS_F2_STRIKE)  ctx->css_strike_css = 1;
        if (r->flags2 & CSS_F2_PAD)     ctx->css_padding    = r->padding_px;
        if (r->flags2 & CSS_F2_MARGIN)  ctx->css_margin     = r->margin_px;
        if (r->flags2 & CSS_F2_LSPACE)  ctx->css_line_space = r->line_space;
        if (r->flags2 & CSS_F2_WIDTH)   ctx->css_width      = r->width_px;
        if (r->flags2 & CSS_F2_HEIGHT)  ctx->css_height     = r->height_px;
        if (r->flags2 & CSS_F2_BORDER)  { ctx->css_border = r->border_px ? r->border_px : 1; ctx->css_border_color = r->border_color; }
        if (r->flags2 & CSS_F2_ABS)     { ctx->css_abs_pos = 1; ctx->css_left = r->left_px; ctx->css_top = r->top_px; }
    }
}

STATIC VOID_T __focus_evt_cb(lv_event_t *e)
{
    if (s_callbacks.focus_cb) {
        s_callbacks.focus_cb(lv_event_get_target(e), s_callbacks.user_data);
    }
}

STATIC VOID_T __submit_evt_cb(lv_event_t *e)
{
    HTML_DOM_NODE_T *node = (HTML_DOM_NODE_T *)lv_event_get_user_data(e);
    if (!node) return;
    if (node->id[0]) {
        win95_html_dom_submit(node->id);
        return;
    }
    __submit_form_idx(node->form_idx);
}

/* ---------------------------------------------------------------------------
 * Tag handling
 * --------------------------------------------------------------------------- */
STATIC VOID_T __open_tag(HTML_CTX_T *ctx, CONST CHAR_T *name, UINT32_T nlen,
                          CONST CHAR_T *attrs, UINT32_T attrs_len)
{
    if (__tag_eq(name, nlen, "script")) { ctx->in_script++; return; }
    if (__tag_eq(name, nlen, "style")) {
        ctx->in_style++;
        if (!ctx->style_buf) {
            ctx->style_buf = (CHAR_T *)tal_malloc(4096);
            if (ctx->style_buf) { ctx->style_buf[0] = '\0'; ctx->style_len = 0; }
        }
        return;
    }
    if (__tag_eq(name, nlen, "head"))   { ctx->in_head++;   return; }
    if (__tag_eq(name, nlen, "title"))  { ctx->in_title++;  return; }

    /* <body>: set page-level colors / background */
    if (__tag_eq(name, nlen, "body")) {
        CHAR_T bg[32]="", tc[32]="", lc[32]="", style_val[256]="";
        __extract_attr(attrs, attrs_len, "bgcolor", bg, sizeof(bg));
        __extract_attr(attrs, attrs_len, "text",    tc, sizeof(tc));
        __extract_attr(attrs, attrs_len, "link",    lc, sizeof(lc));
        __extract_attr(attrs, attrs_len, "style",   style_val, sizeof(style_val));
        if (bg[0]) {
            lv_obj_set_style_bg_color(ctx->container, lv_color_hex(__parse_color(bg)), 0);
            lv_obj_set_style_bg_opa(ctx->container, LV_OPA_COVER, 0);
        }
        if (tc[0]) ctx->text_color      = __parse_color(tc);
        if (lc[0]) ctx->body_link_color = __parse_color(lc);
        if (style_val[0]) __apply_inline_style(ctx, style_val);
        return;
    }

    /* Extract class/id for CSS matching */
    __extract_attr(attrs, attrs_len, "class", ctx->cur_class, sizeof(ctx->cur_class));
    __extract_attr(attrs, attrs_len, "id",    ctx->cur_id,    sizeof(ctx->cur_id));
    __extract_attr(attrs, attrs_len, "name",  ctx->cur_name,  sizeof(ctx->cur_name));
    __extract_attr(attrs, attrs_len, "type",  ctx->cur_type,  sizeof(ctx->cur_type));

    /* Apply stylesheet rules for this element */
    __apply_element_css(ctx, name, nlen);

    /* Apply inline style attribute (overrides stylesheet) */
    CHAR_T style_val[256] = "";
    __extract_attr(attrs, attrs_len, "style", style_val, sizeof(style_val));
    if (style_val[0]) __apply_inline_style(ctx, style_val);

    if (__tag_eq(name, nlen, "form")) {
        CHAR_T method[8] = "GET";
        CHAR_T action[256] = "";
        CHAR_T enctype[48] = "application/x-www-form-urlencoded";
        __extract_attr(attrs, attrs_len, "method", method, sizeof(method));
        __extract_attr(attrs, attrs_len, "action", action, sizeof(action));
        __extract_attr(attrs, attrs_len, "enctype", enctype, sizeof(enctype));
        INT32_T form_idx = __form_alloc_slot();
        if (form_idx >= 0) {
            HTML_FORM_T *form = &s_forms[form_idx];
            form->obj = ctx->container;
            strncpy(form->id, ctx->cur_id, sizeof(form->id) - 1);
            strncpy(form->method, method, sizeof(form->method) - 1);
            strncpy(form->action, action, sizeof(form->action) - 1);
            strncpy(form->enctype, enctype, sizeof(form->enctype) - 1);
            lv_obj_t *box = lv_obj_create(ctx->container);
            lv_obj_remove_style_all(box);
            lv_obj_set_width(box, lv_pct(100));
            lv_obj_set_height(box, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
            lv_obj_set_layout(box, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(box, 2, 0);
            __apply_box_style(ctx, box);
            form->obj = box;
            ctx->cur_form_idx = (INT16_T)form_idx;
            __dom_register(box, "form", ctx->cur_id, ctx->cur_name, "form",
                           ctx->cur_form_idx, TRUE);
            __block_push(ctx, box);
        }
        return;
    }

    if (__tag_eq(name, nlen, "div")) {
        lv_obj_t *box = lv_obj_create(ctx->container);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, lv_pct(100));
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(box, ctx->bg_color ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (ctx->bg_color) {
            lv_obj_set_style_bg_color(box, lv_color_hex(ctx->bg_color), 0);
        }
        lv_obj_set_layout(box, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box, 2, 0);
        __apply_box_style(ctx, box);
        __dom_register(box, "div", ctx->cur_id, ctx->cur_name, ctx->cur_type,
                       ctx->cur_form_idx, TRUE);
        __block_push(ctx, box);
        return;
    }

    /* Inline formatting */
    if (__tag_eq(name, nlen, "b") || __tag_eq(name, nlen, "strong")) {
        __flush_inline(ctx);
        ctx->in_bold++;
        return;
    }
    if (__tag_eq(name, nlen, "i") || __tag_eq(name, nlen, "em")) {
        __flush_inline(ctx);
        ctx->italic++;
        return;
    }
    if (__tag_eq(name, nlen, "u")) {
        __flush_inline(ctx);
        ctx->in_underline++;
        return;
    }
    if (__tag_eq(name, nlen, "tt") ||
        __tag_eq(name, nlen, "small") || __tag_eq(name, nlen, "big") ||
        __tag_eq(name, nlen, "span")) {
        return;
    }
    if (__tag_eq(name, nlen, "s")      || __tag_eq(name, nlen, "strike") ||
        __tag_eq(name, nlen, "del")) {
        __flush_inline(ctx); ctx->in_strike++; return;
    }
    /* Pass-through inline semantic tags */
    if (__tag_eq(name, nlen, "kbd")     || __tag_eq(name, nlen, "samp")    ||
        __tag_eq(name, nlen, "var")     || __tag_eq(name, nlen, "cite")    ||
        __tag_eq(name, nlen, "abbr")    || __tag_eq(name, nlen, "acronym") ||
        __tag_eq(name, nlen, "dfn")     || __tag_eq(name, nlen, "bdo")     ||
        __tag_eq(name, nlen, "ins")     || __tag_eq(name, nlen, "sub")     ||
        __tag_eq(name, nlen, "sup")     || __tag_eq(name, nlen, "bdi")     ||
        __tag_eq(name, nlen, "nobr")    || __tag_eq(name, nlen, "wbr")) {
        return;
    }
    if (__tag_eq(name, nlen, "q")) {
        /* Opening quote */
        __inline_append(ctx, '"');
        return;
    }
    if (__tag_eq(name, nlen, "font")) {
        CHAR_T col[32] = "";
        __extract_attr(attrs, attrs_len, "color", col, sizeof(col));
        if (col[0] != '\0' && ctx->font_depth < 4) {
            ctx->font_clr_stk[ctx->font_depth] = ctx->text_color;
            ctx->font_depth++;
            ctx->text_color = __parse_color(col);
        }
        CHAR_T sz[8] = "";
        __extract_attr(attrs, attrs_len, "size", sz, sizeof(sz));
        if (sz[0]) {
            INT32_T s = 0;
            CONST CHAR_T *sp = sz;
            if (*sp == '+') { sp++; s = 3 + (INT32_T)(*sp - '0'); }
            else if (*sp == '-') { sp++; s = 3 - (INT32_T)(*sp - '0'); }
            else { s = (INT32_T)(*sp - '0'); }
            ctx->use_large_font = (s >= 4) ? 1 : 0;
        }
        return;
    }

    /* Image: create placeholder, register for deferred fetch */
    if (__tag_eq(name, nlen, "img")) {
        CHAR_T src[256] = "";
        CHAR_T alt[64]  = "";
        CHAR_T wstr[16] = "";
        CHAR_T hstr[16] = "";
        __extract_attr(attrs, attrs_len, "src",    src,  sizeof(src));
        __extract_attr(attrs, attrs_len, "alt",    alt,  sizeof(alt));
        __extract_attr(attrs, attrs_len, "width",  wstr, sizeof(wstr));
        __extract_attr(attrs, attrs_len, "height", hstr, sizeof(hstr));
        lv_coord_t iw = (lv_coord_t)(wstr[0] ? atoi(wstr) : 160);
        lv_coord_t ih = (lv_coord_t)(hstr[0] ? atoi(hstr) : 120);
        if (iw < 8 || iw > 440) iw = 160;
        if (ih < 8 || ih > 300) ih = 120;
        __flush_inline(ctx);
        if (!ctx->display_none && __can_emit(ctx)) {
            if (src[0] != '\0' && s_img_list.count < WIN95_HTML_MAX_IMGS) {
                /* Placeholder box — will be replaced with actual JPEG on fetch */
                lv_obj_t *ph = lv_obj_create(ctx->container);
                lv_obj_set_size(ph, iw, ih);
                lv_obj_set_style_bg_color(ph, lv_color_hex(0xCCCCCC), 0);
                lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(ph, 1, 0);
                lv_obj_set_style_border_color(ph, lv_color_hex(0x888888), 0);
                lv_obj_set_style_radius(ph, 0, 0);
                lv_obj_set_style_pad_all(ph, 2, 0);
                lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
                __apply_box_style(ctx, ph);
                lv_obj_t *lbl = lv_label_create(ph);
                lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
                lv_label_set_text(lbl, alt[0] ? alt : "[img]");
                lv_obj_center(lbl);
                INT32_T idx = __dom_register(ph, "img", ctx->cur_id, ctx->cur_name,
                                             "img", ctx->cur_form_idx, TRUE);
                if (idx >= 0) {
                    __dom_store_attr(&s_dom_nodes[idx], "src", src);
                    __dom_store_attr(&s_dom_nodes[idx], "alt", alt);
                }

                WIN95_HTML_IMG_T *img = &s_img_list.imgs[s_img_list.count++];
                img->widget = ph;
                strncpy(img->src, src, sizeof(img->src) - 1);
                img->src[sizeof(img->src) - 1] = '\0';
            } else {
                __emit_paragraph(ctx, alt[0] ? alt : "[image]");
            }
            ctx->emit_count++;
        }
        return;
    }

    /* Block elements flush pending inline. */
    if (__tag_eq(name, nlen, "p")         ||
        __tag_eq(name, nlen, "div")       ||
        __tag_eq(name, nlen, "blockquote") ||
        __tag_eq(name, nlen, "address")) {
        __flush_inline(ctx);
        CHAR_T al[16] = "";
        __extract_attr(attrs, attrs_len, "align", al, sizeof(al));
        if (al[0]) ctx->css_center = __tag_eq(al, (UINT32_T)strlen(al), "center") ? 1 : 0;
        return;
    }
    if (__tag_eq(name, nlen, "br")) {
        /* Insert a hard line break into the inline buffer */
        if (ctx->inline_buf && ctx->inline_len < INLINE_BUF_MAX - 1) {
            ctx->inline_buf[ctx->inline_len++] = '\n';
            ctx->inline_buf[ctx->inline_len]   = '\0';
        } else {
            __flush_inline(ctx);
        }
        return;
    }

    /* <center>: flush + enable centering */
    if (__tag_eq(name, nlen, "center")) {
        __flush_inline(ctx);
        ctx->css_center = 1;
        return;
    }

    /* <marquee>: set flag, text accumulates in inline_buf */
    if (__tag_eq(name, nlen, "marquee")) {
        __flush_inline(ctx);
        ctx->in_marquee = 1;
        return;
    }

    /* TABLE layout */
    if (__tag_eq(name, nlen, "table")) {
        __flush_inline(ctx);
        CHAR_T wstr[16]="", bstr[8]="", bgstr[32]="", cpstr[8]="";
        __extract_attr(attrs, attrs_len, "width",       wstr,  sizeof(wstr));
        __extract_attr(attrs, attrs_len, "border",      bstr,  sizeof(bstr));
        __extract_attr(attrs, attrs_len, "bgcolor",     bgstr, sizeof(bgstr));
        __extract_attr(attrs, attrs_len, "cellpadding", cpstr, sizeof(cpstr));
        lv_coord_t tw = wstr[0] ? (lv_coord_t)atoi(wstr) : (lv_coord_t)LINE_WRAP_WIDTH;
        if (tw < 50 || tw > LINE_WRAP_WIDTH) tw = (lv_coord_t)LINE_WRAP_WIDTH;
        INT32_T tborder = bstr[0] ? atoi(bstr) : 1;
        lv_obj_t *tbl = lv_obj_create(ctx->container);
        lv_obj_set_width(tbl, tw);
        lv_obj_set_height(tbl, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(tbl, 0, 0);
        lv_obj_set_style_border_width(tbl, tborder > 0 ? (lv_coord_t)tborder : 0, 0);
        lv_obj_set_style_border_color(tbl, lv_color_hex(0x808080), 0);
        lv_obj_set_style_radius(tbl, 0, 0);
        lv_obj_clear_flag(tbl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(tbl, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(tbl, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tbl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        if (bgstr[0]) {
            lv_obj_set_style_bg_color(tbl, lv_color_hex(__parse_color(bgstr)), 0);
            lv_obj_set_style_bg_opa(tbl, LV_OPA_COVER, 0);
        }
        (void)cpstr;
        __tbl_push(ctx, tbl);
        return;
    }
    if (__tag_eq(name, nlen, "tr")) {
        __flush_inline(ctx);
        CHAR_T bgstr[32]="";
        __extract_attr(attrs, attrs_len, "bgcolor", bgstr, sizeof(bgstr));
        lv_obj_t *row = lv_obj_create(ctx->container);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        if (bgstr[0]) {
            lv_obj_set_style_bg_color(row, lv_color_hex(__parse_color(bgstr)), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }
        __tbl_push(ctx, row);
        return;
    }
    if (__tag_eq(name, nlen, "td") || __tag_eq(name, nlen, "th")) {
        __flush_inline(ctx);
        CHAR_T wstr[16]="", bgstr[32]="", alstr[16]="";
        __extract_attr(attrs, attrs_len, "width",  wstr,  sizeof(wstr));
        __extract_attr(attrs, attrs_len, "bgcolor", bgstr, sizeof(bgstr));
        __extract_attr(attrs, attrs_len, "align",   alstr, sizeof(alstr));
        lv_obj_t *cell = lv_obj_create(ctx->container);
        lv_obj_set_style_pad_all(cell, 2, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(0xC0C0C0), 0);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(cell, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        if (wstr[0]) {
            lv_obj_set_width(cell, (lv_coord_t)atoi(wstr));
            lv_obj_set_flex_grow(cell, 0);
        } else {
            lv_obj_set_flex_grow(cell, 1);
        }
        if (__tag_eq(name, nlen, "th")) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(0xD0D0D0), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        }
        if (bgstr[0]) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(__parse_color(bgstr)), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        }
        if (alstr[0] && __tag_eq(alstr, (UINT32_T)strlen(alstr), "center"))
            ctx->css_center = 1;
        __tbl_push(ctx, cell);
        return;
    }

    /* <input>: interactive controls */
    if (__tag_eq(name, nlen, "input")) {
        CHAR_T tp[16] = "", val[64] = "";
        __extract_attr(attrs, attrs_len, "type",  tp,  sizeof(tp));
        __extract_attr(attrs, attrs_len, "value", val, sizeof(val));
        BOOL_T is_text = (tp[0] == '\0' || __tag_eq(tp, (UINT32_T)strlen(tp), "text") ||
                          __tag_eq(tp, (UINT32_T)strlen(tp), "password"));
        BOOL_T is_submit = __tag_eq(tp, (UINT32_T)strlen(tp), "submit");
        BOOL_T is_button = __tag_eq(tp, (UINT32_T)strlen(tp), "button");
        BOOL_T is_hidden = __tag_eq(tp, (UINT32_T)strlen(tp), "hidden");
        if (is_hidden) {
            INT32_T idx = __dom_register(NULL, "input", ctx->cur_id, ctx->cur_name,
                                         tp[0] ? tp : "hidden", ctx->cur_form_idx, FALSE);
            if (idx >= 0) {
                strncpy(s_dom_nodes[idx].text, val, sizeof(s_dom_nodes[idx].text) - 1);
                __dom_store_attr(&s_dom_nodes[idx], "value", val);
            }
            return;
        }
        if (is_text && !ctx->display_none && __can_emit(ctx)) {
            __flush_inline(ctx);
            lv_obj_t *ta = lv_textarea_create(ctx->container);
            lv_obj_set_size(ta, ctx->css_width ? ctx->css_width : 160,
                            ctx->css_height ? ctx->css_height : 24);
            lv_textarea_set_one_line(ta, TRUE);
            lv_textarea_set_text(ta, val);
            lv_obj_set_style_text_font(ta, &lv_font_unscii_8, 0);
            if (__tag_eq(tp, (UINT32_T)strlen(tp), "password")) {
                lv_textarea_set_password_mode(ta, TRUE);
            }
            __apply_box_style(ctx, ta);
            lv_obj_add_event_cb(ta, __focus_evt_cb, LV_EVENT_FOCUSED, NULL);
            INT32_T idx = __dom_register(ta, "input", ctx->cur_id, ctx->cur_name,
                                         tp[0] ? tp : "text", ctx->cur_form_idx, FALSE);
            if (idx >= 0) {
                strncpy(s_dom_nodes[idx].text, val, sizeof(s_dom_nodes[idx].text) - 1);
                __dom_store_attr(&s_dom_nodes[idx], "value", val);
            }
            ctx->emit_count++;
        } else if ((is_submit || is_button) && !ctx->display_none && __can_emit(ctx)) {
            __flush_inline(ctx);
            lv_obj_t *btn = lv_btn_create(ctx->container);
            lv_obj_set_size(btn, ctx->css_width ? ctx->css_width : 80,
                            ctx->css_height ? ctx->css_height : 20);
            __apply_box_style(ctx, btn);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
            lv_label_set_text(lbl, val[0] ? val : (is_submit ? "Submit" : "Button"));
            lv_obj_center(lbl);
            INT32_T idx = __dom_register(btn, "input", ctx->cur_id, ctx->cur_name,
                                         is_submit ? "submit" : "button",
                                         ctx->cur_form_idx, FALSE);
            if (idx >= 0) {
                strncpy(s_dom_nodes[idx].text, val[0] ? val : (is_submit ? "Submit" : "Button"),
                        sizeof(s_dom_nodes[idx].text) - 1);
                __dom_store_attr(&s_dom_nodes[idx], "value", s_dom_nodes[idx].text);
                if (ctx->cur_form_idx >= 0) {
                    lv_obj_add_event_cb(btn, __submit_evt_cb, LV_EVENT_CLICKED, &s_dom_nodes[idx]);
                }
            }
            ctx->emit_count++;
        }
        return;
    }

    /* <select>: dropdown */
    if (__tag_eq(name, nlen, "select")) {
        __flush_inline(ctx);
        if (!ctx->display_none && __can_emit(ctx)) {
            ctx->select_obj = lv_dropdown_create(ctx->container);
            lv_obj_set_width(ctx->select_obj, ctx->css_width ? ctx->css_width : 180);
            lv_obj_set_style_text_font(ctx->select_obj, &lv_font_unscii_8, 0);
            lv_dropdown_set_options(ctx->select_obj, "");
            __apply_box_style(ctx, ctx->select_obj);
            ctx->in_select = 1;
            ctx->select_opts[0] = '\0';
            ctx->select_dom_idx = __dom_register(ctx->select_obj, "select", ctx->cur_id,
                                                 ctx->cur_name, "select", ctx->cur_form_idx, FALSE);
            ctx->emit_count++;
        }
        return;
    }

    /* <option>: flush current inline, text will accumulate for </option> */
    if (__tag_eq(name, nlen, "option")) {
        __flush_inline(ctx);
        return;
    }

    /* <meta>: handle http-equiv=refresh */
    if (__tag_eq(name, nlen, "meta")) {
        CHAR_T equiv[32] = "", content[256] = "";
        __extract_attr(attrs, attrs_len, "http-equiv", equiv, sizeof(equiv));
        __extract_attr(attrs, attrs_len, "content",    content, sizeof(content));
        if (equiv[0] && content[0]) {
            /* case-insensitive check for "refresh" */
            BOOL_T is_ref = __tag_eq(equiv, (UINT32_T)strlen(equiv), "refresh");
            if (is_ref && !s_js_result.has_navigate) {
                CHAR_T *semi = strchr(content, ';');
                if (semi) {
                    CHAR_T *urlp = semi + 1;
                    while (*urlp == ' ' || *urlp == '\t') urlp++;
                    if ((urlp[0]=='u'||urlp[0]=='U') && (urlp[1]=='r'||urlp[1]=='R') &&
                        (urlp[2]=='l'||urlp[2]=='L') && urlp[3]=='=') {
                        urlp += 4;
                        if (*urlp == '"' || *urlp == '\'') urlp++;
                        s_js_result.has_navigate = TRUE;
                        strncpy(s_js_result.navigate_url, urlp, JS_URL_MAX - 1);
                        s_js_result.navigate_url[JS_URL_MAX - 1] = '\0';
                        /* strip trailing quote */
                        UINT32_T ul = (UINT32_T)strlen(s_js_result.navigate_url);
                        if (ul > 0) {
                            CHAR_T last = s_js_result.navigate_url[ul - 1];
                            if (last == '"' || last == '\'')
                                s_js_result.navigate_url[ul - 1] = '\0';
                        }
                    }
                }
            }
        }
        return;
    }

    if (nlen == 2 && (name[0] == 'h' || name[0] == 'H') &&
        name[1] >= '1' && name[1] <= '6') {
        __flush_inline(ctx);
        ctx->heading_level = (UINT8_T)(name[1] - '0');
        CHAR_T al[16] = "";
        __extract_attr(attrs, attrs_len, "align", al, sizeof(al));
        if (al[0]) ctx->css_center = __tag_eq(al, (UINT32_T)strlen(al), "center") ? 1 : 0;
        return;
    }

    if (__tag_eq(name, nlen, "hr")) {
        __flush_inline(ctx);
        __emit_hr(ctx);
        return;
    }

    if (__tag_eq(name, nlen, "ul")) { __flush_inline(ctx); ctx->list_ord = 0; ctx->list_idx = 0; return; }
    if (__tag_eq(name, nlen, "ol")) { __flush_inline(ctx); ctx->list_ord = 1; ctx->list_idx = 0; return; }
    if (__tag_eq(name, nlen, "li")) {
        __flush_inline(ctx);
        ctx->in_li = 1;
        if (ctx->list_ord) ctx->list_idx++;
        return;
    }
    if (__tag_eq(name, nlen, "pre") || __tag_eq(name, nlen, "code")) {
        __flush_inline(ctx);
        ctx->in_pre = 1;
        return;
    }

    if (__tag_eq(name, nlen, "a")) {
        __flush_inline(ctx);
        __extract_href(attrs, attrs_len, ctx->anchor_href, sizeof(ctx->anchor_href));
        if (ctx->anchor_href[0] != '\0') ctx->in_anchor = 1;
        return;
    }

    /* Definition list */
    if (__tag_eq(name, nlen, "dl")) {
        __flush_inline(ctx); ctx->list_ord = 0; ctx->list_idx = 0; return;
    }
    if (__tag_eq(name, nlen, "dt")) {
        __flush_inline(ctx); ctx->in_bold++; return;
    }
    if (__tag_eq(name, nlen, "dd")) {
        __flush_inline(ctx);
        /* Two-space indent for definition */
        __inline_append(ctx, ' ');
        __inline_append(ctx, ' ');
        return;
    }

    /* Form elements / structural wrappers — flush context, content passes through */
    if (__tag_eq(name, nlen, "fieldset") || __tag_eq(name, nlen, "legend")   ||
        __tag_eq(name, nlen, "label")    || __tag_eq(name, nlen, "colgroup") ||
        __tag_eq(name, nlen, "col")      || __tag_eq(name, nlen, "caption")  ||
        __tag_eq(name, nlen, "thead")    || __tag_eq(name, nlen, "tbody")    ||
        __tag_eq(name, nlen, "tfoot")    || __tag_eq(name, nlen, "nav")      ||
        __tag_eq(name, nlen, "article")  || __tag_eq(name, nlen, "section")  ||
        __tag_eq(name, nlen, "header")   || __tag_eq(name, nlen, "footer")   ||
        __tag_eq(name, nlen, "aside")    || __tag_eq(name, nlen, "main")     ||
        __tag_eq(name, nlen, "map")      || __tag_eq(name, nlen, "area")     ||
        __tag_eq(name, nlen, "iframe")) {
        __flush_inline(ctx); return;
    }

    if (__tag_eq(name, nlen, "button")) {
        __flush_inline(ctx);
        if (!ctx->display_none && __can_emit(ctx)) {
            lv_obj_t *btn = lv_btn_create(ctx->container);
            lv_obj_set_size(btn, ctx->css_width ? ctx->css_width : 80,
                            ctx->css_height ? ctx->css_height : 20);
            __apply_box_style(ctx, btn);
            ctx->button_label = lv_label_create(btn);
            lv_obj_set_style_text_font(ctx->button_label, &lv_font_unscii_8, 0);
            lv_label_set_text(ctx->button_label, "Button");
            lv_obj_center(ctx->button_label);
            ctx->button_dom_idx = __dom_register(btn, "button", ctx->cur_id, ctx->cur_name,
                                                 "button", ctx->cur_form_idx, FALSE);
            if (ctx->button_dom_idx >= 0 && ctx->cur_form_idx >= 0) {
                lv_obj_add_event_cb(btn, __submit_evt_cb, LV_EVENT_CLICKED,
                                    &s_dom_nodes[ctx->button_dom_idx]);
            }
            ctx->emit_count++;
        }
        return;
    }

    /* <textarea>: interactive multi-line control */
    if (__tag_eq(name, nlen, "textarea")) {
        __flush_inline(ctx);
        if (!ctx->display_none && __can_emit(ctx)) {
            lv_obj_t *ta = lv_textarea_create(ctx->container);
            lv_obj_set_size(ta, ctx->css_width ? ctx->css_width : 200,
                            ctx->css_height ? ctx->css_height : 48);
            lv_obj_set_style_text_font(ta, &lv_font_unscii_8, 0);
            __apply_box_style(ctx, ta);
            lv_obj_add_event_cb(ta, __focus_evt_cb, LV_EVENT_FOCUSED, NULL);
            ctx->textarea_dom_idx = __dom_register(ta, "textarea", ctx->cur_id,
                                                   ctx->cur_name, "textarea",
                                                   ctx->cur_form_idx, FALSE);
            ctx->emit_count++;
        }
        return;
    }
}

STATIC VOID_T __close_tag(HTML_CTX_T *ctx, CONST CHAR_T *name, UINT32_T nlen)
{
    if (__tag_eq(name, nlen, "script")) { if (ctx->in_script) ctx->in_script--; return; }
    if (__tag_eq(name, nlen, "style")) {
        if (ctx->in_style) ctx->in_style--;
        if (ctx->style_buf && ctx->style_len > 0) {
            __parse_style_block(ctx, ctx->style_buf);
        }
        if (ctx->style_buf) { tal_free(ctx->style_buf); ctx->style_buf = NULL; }
        ctx->style_len = 0;
        return;
    }
    if (__tag_eq(name, nlen, "head"))   { if (ctx->in_head)   ctx->in_head--;   return; }
    if (__tag_eq(name, nlen, "title")) {
        if (ctx->in_title) ctx->in_title--;
        /* Capture title text (if not already set by JS) */
        if (ctx->inline_len > 0 && !s_js_result.has_title) {
            __trim_inline(ctx);
            s_js_result.has_title = TRUE;
            strncpy(s_js_result.title, ctx->inline_buf, JS_TITLE_MAX - 1);
            s_js_result.title[JS_TITLE_MAX - 1] = '\0';
        }
        __inline_clear(ctx);
        return;
    }

    /* Inline formatting */
    if (__tag_eq(name, nlen, "i") || __tag_eq(name, nlen, "em")) {
        __flush_inline(ctx);
        if (ctx->italic) ctx->italic--;
        return;
    }
    if (__tag_eq(name, nlen, "b") || __tag_eq(name, nlen, "strong")) {
        __flush_inline(ctx);
        if (ctx->in_bold) ctx->in_bold--;
        return;
    }
    if (__tag_eq(name, nlen, "u")) {
        __flush_inline(ctx);
        if (ctx->in_underline) ctx->in_underline--;
        return;
    }
    if (__tag_eq(name, nlen, "s")      || __tag_eq(name, nlen, "strike") ||
        __tag_eq(name, nlen, "del")) {
        __flush_inline(ctx);
        if (ctx->in_strike) ctx->in_strike--;
        return;
    }
    if (__tag_eq(name, nlen, "q")) {
        __inline_append(ctx, '"');
        return;
    }
    if (__tag_eq(name, nlen, "tt")     ||
        __tag_eq(name, nlen, "small") || __tag_eq(name, nlen, "big") ||
        __tag_eq(name, nlen, "span")) {
        return;
    }
    /* Pass-through inline semantic close tags */
    if (__tag_eq(name, nlen, "kbd")     || __tag_eq(name, nlen, "samp")    ||
        __tag_eq(name, nlen, "var")     || __tag_eq(name, nlen, "cite")    ||
        __tag_eq(name, nlen, "abbr")    || __tag_eq(name, nlen, "acronym") ||
        __tag_eq(name, nlen, "dfn")     || __tag_eq(name, nlen, "bdo")     ||
        __tag_eq(name, nlen, "ins")     || __tag_eq(name, nlen, "sub")     ||
        __tag_eq(name, nlen, "sup")     || __tag_eq(name, nlen, "bdi")     ||
        __tag_eq(name, nlen, "nobr")    || __tag_eq(name, nlen, "wbr")) {
        return;
    }
    if (__tag_eq(name, nlen, "font")) {
        if (ctx->font_depth > 0) {
            ctx->font_depth--;
            ctx->text_color = ctx->font_clr_stk[ctx->font_depth];
        }
        ctx->use_large_font = 0;
        return;
    }

    if (__tag_eq(name, nlen, "a")) {
        __flush_inline(ctx);
        ctx->in_anchor = 0;
        ctx->anchor_href[0] = '\0';
        return;
    }

    if (nlen == 2 && (name[0] == 'h' || name[0] == 'H') &&
        name[1] >= '1' && name[1] <= '6') {
        __flush_inline(ctx);
        ctx->heading_level = 0;
        ctx->display_none = 0; ctx->use_large_font = 0;
        ctx->css_center = 0;   ctx->css_right = 0;  ctx->bg_color = 0;
        ctx->css_underline = 0; ctx->css_strike_css = 0;
        ctx->css_padding = 0;  ctx->css_margin = 0; ctx->css_line_space = -1;
        ctx->css_width = 0;    ctx->css_height = 0; ctx->css_border = 0;
        ctx->css_border_color = 0x808080; ctx->css_abs_pos = 0;
        ctx->css_left = 0;     ctx->css_top = 0;
        ctx->cur_class[0] = '\0'; ctx->cur_id[0] = '\0';
        ctx->cur_name[0] = '\0'; ctx->cur_type[0] = '\0';
        return;
    }

    if (__tag_eq(name, nlen, "p")     || __tag_eq(name, nlen, "div")   ||
        __tag_eq(name, nlen, "blockquote")) {
        __flush_inline(ctx);
        if (__tag_eq(name, nlen, "div")) {
            __block_pop(ctx);
        }
        ctx->display_none = 0; ctx->use_large_font = 0;
        ctx->css_center = 0;   ctx->css_right = 0;  ctx->bg_color = 0;
        ctx->css_underline = 0; ctx->css_strike_css = 0;
        ctx->css_padding = 0;  ctx->css_margin = 0; ctx->css_line_space = -1;
        ctx->css_width = 0;    ctx->css_height = 0; ctx->css_border = 0;
        ctx->css_border_color = 0x808080; ctx->css_abs_pos = 0;
        ctx->css_left = 0;     ctx->css_top = 0;
        ctx->cur_class[0] = '\0'; ctx->cur_id[0] = '\0';
        ctx->cur_name[0] = '\0'; ctx->cur_type[0] = '\0';
        return;
    }

    /* <center>: flush + reset centering */
    if (__tag_eq(name, nlen, "center")) {
        __flush_inline(ctx);
        ctx->css_center = 0;   ctx->css_right = 0;
        ctx->display_none = 0; ctx->use_large_font = 0;
        ctx->css_underline = 0; ctx->css_strike_css = 0;
        ctx->css_padding = 0;  ctx->css_margin = 0; ctx->css_line_space = -1;
        ctx->css_width = 0;    ctx->css_height = 0; ctx->css_border = 0;
        ctx->css_border_color = 0x808080; ctx->css_abs_pos = 0;
        ctx->css_left = 0;     ctx->css_top = 0;
        ctx->bg_color = 0; ctx->cur_class[0] = '\0'; ctx->cur_id[0] = '\0';
        ctx->cur_name[0] = '\0'; ctx->cur_type[0] = '\0';
        return;
    }

    /* <marquee>: emit animated label from inline_buf */
    if (__tag_eq(name, nlen, "marquee")) {
        __trim_inline(ctx);
        if (ctx->inline_len > 0 && !ctx->display_none)
            __emit_marquee(ctx, ctx->inline_buf);
        __inline_clear(ctx);
        ctx->in_marquee = 0;
        return;
    }

    /* TABLE close: pop container stack */
    if (__tag_eq(name, nlen, "td") || __tag_eq(name, nlen, "th")) {
        __flush_inline(ctx);
        ctx->display_none = 0; ctx->use_large_font = 0;
        ctx->css_center = 0;   ctx->bg_color = 0;
        ctx->css_width = 0;    ctx->css_height = 0; ctx->css_border = 0;
        ctx->css_border_color = 0x808080; ctx->css_abs_pos = 0;
        ctx->css_left = 0;     ctx->css_top = 0;
        ctx->cur_class[0] = '\0'; ctx->cur_id[0] = '\0';
        ctx->cur_name[0] = '\0'; ctx->cur_type[0] = '\0';
        __tbl_pop(ctx);
        return;
    }
    if (__tag_eq(name, nlen, "tr")) {
        __flush_inline(ctx);
        __tbl_pop(ctx);
        return;
    }
    if (__tag_eq(name, nlen, "table")) {
        __flush_inline(ctx);
        __tbl_pop(ctx);
        return;
    }

    /* <option>: harvest text into select_opts */
    if (__tag_eq(name, nlen, "option")) {
        __trim_inline(ctx);
        if (ctx->in_select && ctx->inline_len > 0) {
            UINT32_T ol = (UINT32_T)strlen(ctx->select_opts);
            if (ol > 0 && ol + 1 < sizeof(ctx->select_opts)) {
                ctx->select_opts[ol++] = '\n';
                ctx->select_opts[ol] = '\0';
            }
            UINT32_T avail = (UINT32_T)sizeof(ctx->select_opts) - 1 - (UINT32_T)strlen(ctx->select_opts);
            strncat(ctx->select_opts, ctx->inline_buf,
                    ctx->inline_len < avail ? ctx->inline_len : avail);
        }
        __inline_clear(ctx);
        return;
    }

    /* <select>: apply accumulated options */
    if (__tag_eq(name, nlen, "select")) {
        if (ctx->select_obj && ctx->select_opts[0])
            lv_dropdown_set_options(ctx->select_obj, ctx->select_opts);
        if (ctx->select_dom_idx >= 0) {
            __dom_store_attr(&s_dom_nodes[ctx->select_dom_idx], "options", ctx->select_opts);
        }
        ctx->in_select   = 0;
        ctx->select_obj  = NULL;
        ctx->select_opts[0] = '\0';
        ctx->select_dom_idx = -1;
        return;
    }

    if (__tag_eq(name, nlen, "li")) {
        __flush_inline(ctx); ctx->in_li = 0;
        ctx->display_none = 0; ctx->use_large_font = 0;
        ctx->css_center = 0;   ctx->bg_color = 0;
        ctx->css_width = 0;    ctx->css_height = 0; ctx->css_border = 0;
        ctx->css_border_color = 0x808080; ctx->css_abs_pos = 0;
        ctx->css_left = 0;     ctx->css_top = 0;
        ctx->cur_class[0] = '\0'; ctx->cur_id[0] = '\0';
        ctx->cur_name[0] = '\0'; ctx->cur_type[0] = '\0';
        return;
    }
    if (__tag_eq(name, nlen, "ul") || __tag_eq(name, nlen, "ol")) {
        __flush_inline(ctx); ctx->list_ord = 0; ctx->list_idx = 0; return;
    }
    if (__tag_eq(name, nlen, "dt")) {
        __flush_inline(ctx); if (ctx->in_bold) ctx->in_bold--; return;
    }
    if (__tag_eq(name, nlen, "dd") || __tag_eq(name, nlen, "dl")) {
        __flush_inline(ctx); return;
    }
    if (__tag_eq(name, nlen, "pre") || __tag_eq(name, nlen, "code")) {
        __flush_inline(ctx); ctx->in_pre = 0; return;
    }
    if (__tag_eq(name, nlen, "button")) {
        __trim_inline(ctx);
        if (ctx->button_dom_idx >= 0) {
            if (ctx->inline_len > 0) {
                strncpy(s_dom_nodes[ctx->button_dom_idx].text, ctx->inline_buf,
                        sizeof(s_dom_nodes[ctx->button_dom_idx].text) - 1);
                if (ctx->button_label) {
                    lv_label_set_text(ctx->button_label, ctx->inline_buf);
                    lv_obj_center(ctx->button_label);
                }
            }
            __dom_store_attr(&s_dom_nodes[ctx->button_dom_idx], "value",
                             s_dom_nodes[ctx->button_dom_idx].text);
        }
        __inline_clear(ctx);
        ctx->button_dom_idx = -1;
        ctx->button_label = NULL;
        return;
    }

    if (__tag_eq(name, nlen, "textarea")) {
        __trim_inline(ctx);
        if (ctx->textarea_dom_idx >= 0) {
            HTML_DOM_NODE_T *node = &s_dom_nodes[ctx->textarea_dom_idx];
            strncpy(node->text, ctx->inline_buf, sizeof(node->text) - 1);
            if (node->obj) {
                lv_textarea_set_text(node->obj, ctx->inline_buf);
            }
            __dom_store_attr(node, "value", node->text);
        }
        __inline_clear(ctx);
        ctx->textarea_dom_idx = -1;
        return;
    }

    /* Form / structural close tags — just flush */
    if (__tag_eq(name, nlen, "form")     || __tag_eq(name, nlen, "fieldset") ||
        __tag_eq(name, nlen, "legend")   || __tag_eq(name, nlen, "label")    ||
        __tag_eq(name, nlen, "colgroup") ||
        __tag_eq(name, nlen, "thead")    || __tag_eq(name, nlen, "tbody")    ||
        __tag_eq(name, nlen, "tfoot")    || __tag_eq(name, nlen, "nav")      ||
        __tag_eq(name, nlen, "article")  || __tag_eq(name, nlen, "section")  ||
        __tag_eq(name, nlen, "header")   || __tag_eq(name, nlen, "footer")   ||
        __tag_eq(name, nlen, "aside")    || __tag_eq(name, nlen, "main")     ||
        __tag_eq(name, nlen, "iframe")   ||
        __tag_eq(name, nlen, "body")     || __tag_eq(name, nlen, "html")) {
        __flush_inline(ctx);
        if (__tag_eq(name, nlen, "form")) {
            ctx->cur_form_idx = -1;
            __block_pop(ctx);
        }
        return;
    }
}

/* ---------------------------------------------------------------------------
 * JS preprocessing: strip <script>...</script>, run each via win95_js, inject
 * document.write() output in-place, accumulate side effects into s_js_result.
 * Returns heap-allocated modified HTML; caller must tal_free().
 * --------------------------------------------------------------------------- */
STATIC CHAR_T *__js_preprocess(CONST CHAR_T *html)
{
    UINT32_T in_len = (UINT32_T)strlen(html);
    UINT32_T cap    = in_len + JS_WBUF_MAX + 64;
    CHAR_T  *out    = (CHAR_T *)tal_malloc(cap);
    if (!out) return NULL;

    memset(&s_js_result, 0, sizeof(s_js_result));

    UINT32_T     wi = 0;
    CONST CHAR_T *p = html;

    while (*p) {
        /* Detect <script (case-insensitive) */
        if (p[0] == '<' &&
            (p[1] == 's' || p[1] == 'S') &&
            (p[2] == 'c' || p[2] == 'C') &&
            (p[3] == 'r' || p[3] == 'R') &&
            (p[4] == 'i' || p[4] == 'I') &&
            (p[5] == 'p' || p[5] == 'P') &&
            (p[6] == 't' || p[6] == 'T') &&
            (p[7] == '>' || p[7] == ' '  || p[7] == '\t' ||
             p[7] == '\r'|| p[7] == '\n' || p[7] == '/')) {

            /* Advance past opening tag */
            CONST CHAR_T *tag_end = p + 7;
            while (*tag_end && *tag_end != '>') tag_end++;
            if (*tag_end != '>') { if (wi < cap - 1) out[wi++] = *p++; continue; }
            CONST CHAR_T *body = tag_end + 1;

            /* Find </script> (case-insensitive) */
            CONST CHAR_T *close = body;
            while (*close) {
                if (close[0] == '<' && close[1] == '/' &&
                    (close[2] == 's' || close[2] == 'S') &&
                    (close[3] == 'c' || close[3] == 'C') &&
                    (close[4] == 'r' || close[4] == 'R') &&
                    (close[5] == 'i' || close[5] == 'I') &&
                    (close[6] == 'p' || close[6] == 'P') &&
                    (close[7] == 't' || close[7] == 'T')) break;
                close++;
            }

            /* Execute the script block */
            UINT32_T slen = (UINT32_T)(close - body);
            CHAR_T  *scr  = (CHAR_T *)tal_malloc(slen + 1);
            if (scr) {
                memcpy(scr, body, slen);
                scr[slen] = '\0';

                WIN95_JS_RESULT_T jsr;
                memset(&jsr, 0, sizeof(jsr));
                win95_js_run(scr, &jsr);
                tal_free(scr);

                if (jsr.write_buf && jsr.write_len > 0 &&
                    wi + jsr.write_len < cap) {
                    memcpy(&out[wi], jsr.write_buf, jsr.write_len);
                    wi += jsr.write_len;
                }
                if (jsr.has_alert && !s_js_result.has_alert) {
                    s_js_result.has_alert = TRUE;
                    strncpy(s_js_result.alert_msg, jsr.alert_msg,
                            JS_ALERT_MAX - 1);
                    s_js_result.alert_msg[JS_ALERT_MAX - 1] = '\0';
                }
                if (jsr.has_title && !s_js_result.has_title) {
                    s_js_result.has_title = TRUE;
                    strncpy(s_js_result.title, jsr.title, JS_TITLE_MAX - 1);
                    s_js_result.title[JS_TITLE_MAX - 1] = '\0';
                }
                if (jsr.has_navigate && !s_js_result.has_navigate) {
                    s_js_result.has_navigate = TRUE;
                    strncpy(s_js_result.navigate_url, jsr.navigate_url,
                            JS_URL_MAX - 1);
                    s_js_result.navigate_url[JS_URL_MAX - 1] = '\0';
                }
                win95_js_result_free(&jsr);
            }

            /* Skip past </script> closing tag */
            if (*close) {
                CONST CHAR_T *end_close = strchr(close, '>');
                p = end_close ? end_close + 1 : close;
            } else {
                p = close;
            }
            continue;
        }

        if (wi < cap - 1) out[wi++] = *p;
        p++;
    }
    out[wi] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Top-level parse loop
 * --------------------------------------------------------------------------- */
STATIC BOOL_T __dom_get_text_local(HTML_DOM_NODE_T *node, CHAR_T *out, UINT32_T cap)
{
    if (!node || !out || cap == 0) return FALSE;
    out[0] = '\0';
    if (node->obj) {
        if (strcmp(node->tag, "input") == 0 || strcmp(node->tag, "textarea") == 0) {
            CONST CHAR_T *txt = lv_textarea_get_text(node->obj);
            if (txt) {
                strncpy(out, txt, cap - 1);
                out[cap - 1] = '\0';
                return TRUE;
            }
        } else if (strcmp(node->tag, "select") == 0) {
            lv_dropdown_get_selected_str(node->obj, out, cap);
            return TRUE;
        } else if (strcmp(node->tag, "button") == 0) {
            lv_obj_t *child = lv_obj_get_child(node->obj, 0);
            if (child) {
                CONST CHAR_T *txt = lv_label_get_text(child);
                if (txt) {
                    strncpy(out, txt, cap - 1);
                    out[cap - 1] = '\0';
                    return TRUE;
                }
            }
        } else if (strcmp(node->tag, "p") == 0 || strcmp(node->tag, "a") == 0 ||
                   strcmp(node->tag, "li") == 0 || strcmp(node->tag, "pre") == 0 ||
                   strcmp(node->tag, "h") == 0 || strcmp(node->tag, "marquee") == 0) {
            CONST CHAR_T *txt = lv_label_get_text(node->obj);
            if (txt) {
                strncpy(out, txt, cap - 1);
                out[cap - 1] = '\0';
                return TRUE;
            }
        }
    }
    strncpy(out, node->text, cap - 1);
    out[cap - 1] = '\0';
    return TRUE;
}

STATIC BOOL_T __dom_set_text_local(HTML_DOM_NODE_T *node, CONST CHAR_T *text)
{
    if (!node || !text) return FALSE;
    strncpy(node->text, text, sizeof(node->text) - 1);
    node->text[sizeof(node->text) - 1] = '\0';
    if (node->obj) {
        if (strcmp(node->tag, "input") == 0 || strcmp(node->tag, "textarea") == 0) {
            lv_textarea_set_text(node->obj, text);
        } else if (strcmp(node->tag, "select") == 0) {
            /* not implemented: set by visible text later via setAttribute(selectedIndex) */
        } else if (strcmp(node->tag, "button") == 0) {
            lv_obj_t *child = lv_obj_get_child(node->obj, 0);
            if (child) {
                lv_label_set_text(child, text);
                lv_obj_center(child);
            }
        } else if (strcmp(node->tag, "p") == 0 || strcmp(node->tag, "a") == 0 ||
                   strcmp(node->tag, "li") == 0 || strcmp(node->tag, "pre") == 0 ||
                   strcmp(node->tag, "h") == 0 || strcmp(node->tag, "marquee") == 0) {
            lv_label_set_text(node->obj, text);
        }
    }
    __dom_store_attr(node, "value", text);
    return TRUE;
}

STATIC VOID_T __url_append_encoded(CHAR_T *dst, UINT32_T cap, UINT32_T *len,
                                    CONST CHAR_T *src)
{
    static CONST CHAR_T hex[] = "0123456789ABCDEF";
    while (src && *src && *len + 4 < cap) {
        UCHAR_T c = (UCHAR_T)*src++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            dst[(*len)++] = (CHAR_T)c;
        } else if (c == ' ') {
            dst[(*len)++] = '+';
        } else {
            dst[(*len)++] = '%';
            dst[(*len)++] = hex[(c >> 4) & 0x0F];
            dst[(*len)++] = hex[c & 0x0F];
        }
    }
    dst[*len] = '\0';
}

STATIC BOOL_T __submit_form_idx(INT16_T form_idx)
{
    if (form_idx < 0 || form_idx >= (INT16_T)s_form_count || !s_forms[form_idx].used) {
        return FALSE;
    }
    if (!s_callbacks.form_cb) {
        return FALSE;
    }
    CHAR_T body[1024];
    UINT32_T blen = 0;
    body[0] = '\0';

    for (UINT32_T i = 0; i < s_dom_count; i++) {
        HTML_DOM_NODE_T *node = &s_dom_nodes[i];
        if (!node->used || node->form_idx != form_idx) continue;
        if (!node->name[0]) continue;
        if (strcmp(node->type, "submit") == 0 || strcmp(node->type, "button") == 0 ||
            strcmp(node->type, "form") == 0) {
            continue;
        }
        CHAR_T val[WIN95_HTML_TEXT_MAX];
        __dom_get_text_local(node, val, sizeof(val));
        if (blen > 0 && blen + 1 < sizeof(body)) {
            body[blen++] = '&';
            body[blen] = '\0';
        }
        __url_append_encoded(body, sizeof(body), &blen, node->name);
        if (blen + 1 < sizeof(body)) {
            body[blen++] = '=';
            body[blen] = '\0';
        }
        __url_append_encoded(body, sizeof(body), &blen, val);
    }

    s_callbacks.form_cb(s_forms[form_idx].method[0] ? s_forms[form_idx].method : "GET",
                        s_forms[form_idx].action,
                        s_forms[form_idx].enctype[0] ? s_forms[form_idx].enctype : "application/x-www-form-urlencoded",
                        body, s_callbacks.user_data);
    return TRUE;
}

STATIC OPERATE_RET __render_html(lv_obj_t *container, CONST CHAR_T *html,
                                  CONST WIN95_HTML_CALLBACKS_T *callbacks,
                                  BOOL_T reset_state, BOOL_T preprocess_js)
{
    if (container == NULL || html == NULL) return OPRT_INVALID_PARM;

    if (reset_state) {
        lv_obj_clean(container);
        memset(&s_img_list, 0, sizeof(s_img_list));
        memset(&s_js_result, 0, sizeof(s_js_result));
        memset(s_dom_nodes, 0, sizeof(s_dom_nodes));
        memset(s_forms, 0, sizeof(s_forms));
        s_dom_count = 0;
        s_form_count = 0;
        s_href_pool_idx = 0;
        memset(&s_callbacks, 0, sizeof(s_callbacks));
        if (callbacks) {
            s_callbacks = *callbacks;
        }
    } else {
        __dom_prune_container(container);
        lv_obj_clean(container);
    }

    CHAR_T *processed = NULL;
    CONST CHAR_T *src = html;
    if (preprocess_js) {
        processed = __js_preprocess(html);
        src = processed ? processed : html;
    }

    HTML_CTX_T ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.container = container;
    ctx.callbacks = callbacks ? *callbacks : s_callbacks;
    ctx.text_color = (UINT32_T)COLOR_TEXT;
    ctx.css_line_space = -1;
    ctx.css_border_color = 0x808080;
    ctx.cur_form_idx = -1;
    ctx.select_dom_idx = -1;
    ctx.textarea_dom_idx = -1;
    ctx.button_dom_idx = -1;
    ctx.inline_buf = s_inline_buf;
    ctx.inline_buf[0] = '\0';

    CONST CHAR_T *p = src;
    while (*p) {
        if (*p == '<') {
            if (strncmp(p, "<!--", 4) == 0) {
                CONST CHAR_T *e = strstr(p + 4, "-->");
                if (!e) break;
                p = e + 3;
                continue;
            }
            if (p[1] == '!' || p[1] == '?') {
                CONST CHAR_T *e = strchr(p + 1, '>');
                if (!e) break;
                p = e + 1;
                continue;
            }

            BOOL_T is_close = FALSE;
            CONST CHAR_T *tag_start = p + 1;
            if (*tag_start == '/') { is_close = TRUE; tag_start++; }

            CONST CHAR_T *name_end = tag_start;
            while (*name_end && *name_end != ' ' && *name_end != '>' &&
                   *name_end != '/' && *name_end != '\t' && *name_end != '\n') {
                name_end++;
            }

            CONST CHAR_T *tag_end = strchr(name_end, '>');
            if (!tag_end) break;

            UINT32_T nlen = (UINT32_T)(name_end - tag_start);
            UINT32_T alen = (UINT32_T)(tag_end - name_end);

            if (ctx.in_script) {
                if (is_close && __tag_eq(tag_start, nlen, "script")) {
                    __close_tag(&ctx, tag_start, nlen);
                }
                p = tag_end + 1;
                continue;
            }

            if (ctx.in_style) {
                if (is_close && __tag_eq(tag_start, nlen, "style")) {
                    __close_tag(&ctx, tag_start, nlen);
                }
                p = tag_end + 1;
                continue;
            }

            if (is_close) {
                __close_tag(&ctx, tag_start, nlen);
            } else {
                __open_tag(&ctx, tag_start, nlen, name_end, alen);
                if (tag_end > name_end && *(tag_end - 1) == '/') {
                    __close_tag(&ctx, tag_start, nlen);
                }
            }
            p = tag_end + 1;
            continue;
        }

        if (ctx.in_style) {
            if (ctx.style_buf && ctx.style_len + 1 < 4096) {
                ctx.style_buf[ctx.style_len++] = *p;
                ctx.style_buf[ctx.style_len] = '\0';
            }
            p++;
            continue;
        }

        if (ctx.in_script || (ctx.in_head && !ctx.in_title)) {
            p++;
            continue;
        }

        if (*p == '&') {
            CHAR_T decoded = '?';
            UINT32_T consumed = __decode_entity(p, &decoded);
            __inline_append(&ctx, decoded);
            p += consumed;
            continue;
        }

        __inline_append(&ctx, *p);
        p++;
    }

    __flush_inline(&ctx);

    if (ctx.emit_count >= MAX_EMIT_WIDGETS) {
        (VOID_T)__make_label(container, "[...] (page truncated)", &lv_font_unscii_8, COLOR_ITALIC);
    }

    if (ctx.style_buf) tal_free(ctx.style_buf);
    if (processed) tal_free(processed);
    return OPRT_OK;
}

OPERATE_RET win95_html_render_ex(lv_obj_t *container, CONST CHAR_T *html,
                                  CONST WIN95_HTML_CALLBACKS_T *callbacks)
{
    return __render_html(container, html, callbacks, TRUE, FALSE);
}

OPERATE_RET win95_html_render(lv_obj_t *container, CONST CHAR_T *html,
                               WIN95_HTML_LINK_CB link_cb, VOID_T *user_data)
{
    WIN95_HTML_CALLBACKS_T cb;
    memset(&cb, 0, sizeof(cb));
    cb.link_cb = link_cb;
    cb.user_data = user_data;
    return __render_html(container, html, &cb, TRUE, FALSE);
}

BOOL_T win95_html_dom_get_info(CONST CHAR_T *id, WIN95_HTML_DOM_INFO_T *out)
{
    HTML_DOM_NODE_T *node = __dom_find_by_id(id);
    if (!node || !out) return FALSE;
    memset(out, 0, sizeof(*out));
    out->obj = node->obj;
    strncpy(out->id, node->id, sizeof(out->id) - 1);
    strncpy(out->tag, node->tag, sizeof(out->tag) - 1);
    strncpy(out->name, node->name, sizeof(out->name) - 1);
    strncpy(out->type, node->type, sizeof(out->type) - 1);
    return TRUE;
}

BOOL_T win95_html_dom_get_attr(CONST CHAR_T *id, CONST CHAR_T *name,
                                CHAR_T *out, UINT32_T cap)
{
    HTML_DOM_NODE_T *node = __dom_find_by_id(id);
    if (!node || !name || !out || cap == 0) return FALSE;
    out[0] = '\0';
    if (strcmp(name, "innerHTML") == 0 || strcmp(name, "innerText") == 0 ||
        strcmp(name, "textContent") == 0 || strcmp(name, "value") == 0) {
        return __dom_get_text_local(node, out, cap);
    }
    if (strcmp(name, "id") == 0) {
        strncpy(out, node->id, cap - 1);
        out[cap - 1] = '\0';
        return TRUE;
    }
    if (strcmp(name, "name") == 0) {
        strncpy(out, node->name, cap - 1);
        out[cap - 1] = '\0';
        return TRUE;
    }
    if (strcmp(name, "type") == 0) {
        strncpy(out, node->type, cap - 1);
        out[cap - 1] = '\0';
        return TRUE;
    }
    CONST CHAR_T *val = __dom_get_attr_local(node, name);
    if (!val) return FALSE;
    strncpy(out, val, cap - 1);
    out[cap - 1] = '\0';
    return TRUE;
}

BOOL_T win95_html_dom_set_attr(CONST CHAR_T *id, CONST CHAR_T *name,
                                CONST CHAR_T *value)
{
    HTML_DOM_NODE_T *node = __dom_find_by_id(id);
    if (!node || !name || !value) return FALSE;
    if (strcmp(name, "innerHTML") == 0) {
        return win95_html_dom_set_inner_html(id, value);
    }
    if (strcmp(name, "innerText") == 0 || strcmp(name, "textContent") == 0 ||
        strcmp(name, "value") == 0) {
        return __dom_set_text_local(node, value);
    }
    if (strcmp(name, "selectedIndex") == 0 && node->obj && strcmp(node->tag, "select") == 0) {
        lv_dropdown_set_selected(node->obj, (uint32_t)atoi(value));
        return TRUE;
    }
    __dom_store_attr(node, name, value);
    return TRUE;
}

BOOL_T win95_html_dom_get_text(CONST CHAR_T *id, CHAR_T *out, UINT32_T cap)
{
    HTML_DOM_NODE_T *node = __dom_find_by_id(id);
    if (!node) return FALSE;
    return __dom_get_text_local(node, out, cap);
}

BOOL_T win95_html_dom_set_text(CONST CHAR_T *id, CONST CHAR_T *text)
{
    HTML_DOM_NODE_T *node = __dom_find_by_id(id);
    if (!node) return FALSE;
    return __dom_set_text_local(node, text);
}

BOOL_T win95_html_dom_set_inner_html(CONST CHAR_T *id, CONST CHAR_T *html)
{
    HTML_DOM_NODE_T *node = __dom_find_by_id(id);
    if (!node || !html) return FALSE;
    if (node->is_container && node->obj) {
        return __render_html(node->obj, html, &s_callbacks, FALSE, FALSE) == OPRT_OK;
    }
    return __dom_set_text_local(node, html);
}

BOOL_T win95_html_dom_submit(CONST CHAR_T *id)
{
    HTML_DOM_NODE_T *node = __dom_find_by_id(id);
    if (node && node->form_idx >= 0) {
        return __submit_form_idx(node->form_idx);
    }
    for (UINT32_T i = 0; i < s_form_count; i++) {
        if (s_forms[i].used && strcmp(s_forms[i].id, id) == 0) {
            return __submit_form_idx((INT16_T)i);
        }
    }
    return FALSE;
}
