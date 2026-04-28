/**
 * @file win95_spider.c
 * @brief Win95 Spider Solitaire (1 suit, 10 columns, 104 cards)
 *
 * Rules:
 *  - 104 cards (2 decks, spades only in 1-suit mode)
 *  - 10 tableau columns: cols 0-3 get 6 cards each, cols 4-9 get 5 each (54 total)
 *  - 50 remaining cards in the stock (5 deals of 10)
 *  - Top card of each column is face-up; the rest are face-down
 *  - Move a sequence of face-up cards (descending ranks, same suit) onto a
 *    card one rank higher. Moving to empty column is also allowed.
 *  - When a full K->A sequence (13 cards) forms, it auto-removes.
 *  - "Deal" places one card face-up on each column from stock.
 *
 * Display:
 *  - Full 480x320 window, dark Win95 teal felt
 *  - Cards have a 1px Win95 raised bevel, no rounded corners,
 *    pixelated rank/suit text in the unscii_8 bitmap font.
 *  - Face-down cards use a chunky cross-hatch pattern.
 *
 * Interaction:
 *  - Press + drag a face-up card (or a movable run starting at it) to
 *    drop on another column. Drop validity follows Spider rules.
 *  - Tap (no drag) keeps the legacy click-to-select / click-to-target flow.
 *  - Tap the stock to deal one row.
 */
#include "win95_spider.h"
#include "win95_desktop.h"
#include "win95_chrome.h"
#include "bios_simulator.h"
#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define N_COLS              10
#define N_CARDS             104
#define RANK_MAX            13       /* A=1 ... K=13 */

#define CARD_W              40
#define CARD_H              54
#define CARD_OVERLAP        14       /* vertical overlap for face-up cards */
#define CARD_BACK_OVERLAP   8        /* tighter overlap for face-down cards */
#define COL_X0              4        /* left edge of first column */
#define COL_Y0              38       /* top of tableau area (below toolbar) */
#define STOCK_X             (BIOS_SCREEN_WIDTH - 4 - CARD_W)
#define STOCK_Y             4

#define SP_W                BIOS_SCREEN_WIDTH
#define SP_H                BIOS_SCREEN_HEIGHT

#define SP_DRAG_THRESHOLD_SQ 16      /* 4 px squared */

#define SP_FELT_COLOR        0x007050   /* Win95-style billiard green */
#define SP_FACE_DOWN_COLOR   0x000088   /* Win95 navy */
#define SP_FACE_DOWN_STIPPLE 0x4080FF   /* highlight stipple */
#define SP_HOVER_HIGHLIGHT   0xFFFFAA

/* ---------------------------------------------------------------------------
 * Helpers to pack (col, pos) into the LVGL user_data void* without alloc.
 *  - col: signed but stored in 0..N_COLS so positive
 *  - pos: -1 means "empty column placeholder" -> stored as +1
 * --------------------------------------------------------------------------- */
#define SP_LOC_PACK(col, pos) \
    ((VOID_T *)(uintptr_t)((((UINT32_T)(col)) << 16) | (((UINT32_T)((pos) + 1)) & 0xFFFFu)))
#define SP_LOC_COL(p)         ((INT32_T)(((uintptr_t)(p)) >> 16) & 0xFFFF)
#define SP_LOC_POS(p)         (((INT32_T)(((uintptr_t)(p)) & 0xFFFFu)) - 1)

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    UINT8_T rank;       /* 1=A, 2-10, 11=J, 12=Q, 13=K */
    UINT8_T face_up;
    UINT8_T removed;    /* part of completed sequence */
} CARD_T;

typedef struct {
    UINT8_T cards[N_CARDS];
    INT32_T n;
} COL_T;

typedef struct {
    UINT8_T cards[N_CARDS];
    INT32_T n;
} STOCK_T;

typedef struct {
    BOOL_T   active;       /* press tracking is in progress */
    BOOL_T   moved;        /* cursor crossed drag threshold */
    INT32_T  col;          /* source column */
    INT32_T  seq_start;    /* index of top dragged card in column */
    INT32_T  n_drag;       /* how many cards being dragged */
    lv_point_t press_pt;   /* press anchor */
    lv_obj_t *ghost;       /* floating composite during drag */
} SP_DRAG_T;

typedef struct {
    lv_obj_t  *screen;
    lv_obj_t  *score_lbl;
    lv_obj_t  *status_lbl;
    lv_obj_t  *card_area;

    CARD_T     deck[N_CARDS];
    COL_T      cols[N_COLS];
    STOCK_T    stock;
    INT32_T    score;
    INT32_T    completed;
    UINT32_T   rand_seed;

    INT32_T    sel_col;     /* -1 = no selection (tap fallback) */

    SP_DRAG_T  drag;
} SPIDER_CTX_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC SPIDER_CTX_T s_sp;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __sp_render(VOID_T);
STATIC VOID_T __sp_update_score_label(VOID_T);
STATIC INT32_T __sp_seq_start(INT32_T col);

/* ---------------------------------------------------------------------------
 * Random
 * --------------------------------------------------------------------------- */
/**
 * @brief LCG to drive Fisher-Yates shuffle
 * @return next pseudo-random word
 */
STATIC UINT32_T __sp_rand(VOID_T)
{
    s_sp.rand_seed = s_sp.rand_seed * 1664525u + 1013904223u;
    return s_sp.rand_seed >> 1;
}

/**
 * @brief Map rank index to display string
 * @param[in] r 1..13
 * @return short string
 */
STATIC CONST CHAR_T *__rank_str(INT32_T r)
{
    STATIC CONST CHAR_T *s[] = {"?","A","2","3","4","5","6","7","8","9","10","J","Q","K"};
    if (r < 1 || r > 13) {
        return "?";
    }
    return s[r];
}

/**
 * @brief Shuffle the 104-card deck (8 copies of A..K) in place
 * @return none
 */
STATIC VOID_T __sp_shuffle(VOID_T)
{
    for (INT32_T i = 0; i < N_CARDS; i++) {
        s_sp.deck[i].rank    = (UINT8_T)((i % RANK_MAX) + 1);
        s_sp.deck[i].face_up = 0;
        s_sp.deck[i].removed = 0;
    }
    for (INT32_T i = N_CARDS - 1; i > 0; i--) {
        INT32_T j = (INT32_T)(__sp_rand() % (UINT32_T)(i + 1));
        CARD_T tmp = s_sp.deck[i];
        s_sp.deck[i] = s_sp.deck[j];
        s_sp.deck[j] = tmp;
    }
}

/**
 * @brief Deal initial tableau and stock pile
 * @return none
 */
STATIC VOID_T __sp_deal_initial(VOID_T)
{
    __sp_shuffle();
    INT32_T card_idx = 0;
    for (INT32_T c = 0; c < N_COLS; c++) {
        s_sp.cols[c].n = 0;
        INT32_T n = (c < 4) ? 6 : 5;
        for (INT32_T k = 0; k < n; k++) {
            s_sp.cols[c].cards[s_sp.cols[c].n++] = (UINT8_T)card_idx;
            card_idx++;
        }
        INT32_T top = s_sp.cols[c].n - 1;
        s_sp.deck[s_sp.cols[c].cards[top]].face_up = 1;
    }
    s_sp.stock.n = 0;
    for (; card_idx < N_CARDS; card_idx++) {
        s_sp.stock.cards[s_sp.stock.n++] = (UINT8_T)card_idx;
    }
}

/**
 * @brief Auto-remove a completed K->A run from the top of \a col
 * @param[in] col column index
 * @return none
 */
STATIC VOID_T __sp_check_complete(INT32_T col)
{
    COL_T *c = &s_sp.cols[col];
    if (c->n < RANK_MAX) {
        return;
    }
    INT32_T k_pos = -1;
    for (INT32_T i = c->n - RANK_MAX; i <= c->n - RANK_MAX; i++) {
        if (i < 0) {
            continue;
        }
        if (!s_sp.deck[c->cards[i]].face_up) {
            continue;
        }
        if (s_sp.deck[c->cards[i]].rank != RANK_MAX) {
            continue;
        }
        BOOL_T ok = TRUE;
        for (INT32_T j = 0; j < RANK_MAX; j++) {
            INT32_T ci = i + j;
            if (ci >= c->n) {
                ok = FALSE; break;
            }
            if (!s_sp.deck[c->cards[ci]].face_up) {
                ok = FALSE; break;
            }
            if (s_sp.deck[c->cards[ci]].rank != (UINT8_T)(RANK_MAX - j)) {
                ok = FALSE; break;
            }
        }
        if (ok) {
            k_pos = i;
            break;
        }
    }
    if (k_pos < 0) {
        return;
    }
    for (INT32_T j = 0; j < RANK_MAX; j++) {
        s_sp.deck[c->cards[k_pos + j]].removed = 1;
    }
    c->n = k_pos;
    if (c->n > 0) {
        s_sp.deck[c->cards[c->n - 1]].face_up = 1;
    }
    s_sp.completed++;
    s_sp.score += 100;
}

/**
 * @brief Test if the run starting at \a seq_start in \a from_col can drop on \a to_col
 * @param[in] from_col source column
 * @param[in] seq_start index of the lowest-rank card in the sequence (top of move)
 * @param[in] to_col destination column
 * @return TRUE if move is legal
 */
STATIC BOOL_T __sp_can_move(INT32_T from_col, INT32_T seq_start, INT32_T to_col)
{
    COL_T *from = &s_sp.cols[from_col];
    INT32_T rank_bottom = (INT32_T)s_sp.deck[from->cards[seq_start]].rank;
    COL_T *to = &s_sp.cols[to_col];
    if (to->n == 0) {
        return TRUE;
    }
    INT32_T rank_top_dest = (INT32_T)s_sp.deck[to->cards[to->n - 1]].rank;
    return (rank_top_dest == rank_bottom + 1) ? TRUE : FALSE;
}

/**
 * @brief Find the deepest card index in \a col that begins a movable sequence
 * @param[in] col column index
 * @return index in col->cards[]; -1 if column is empty
 */
STATIC INT32_T __sp_seq_start(INT32_T col)
{
    COL_T *c = &s_sp.cols[col];
    if (c->n == 0) {
        return -1;
    }
    INT32_T start = c->n - 1;
    while (start > 0) {
        if (!s_sp.deck[c->cards[start - 1]].face_up) {
            break;
        }
        if ((INT32_T)s_sp.deck[c->cards[start - 1]].rank !=
            (INT32_T)s_sp.deck[c->cards[start]].rank + 1) {
            break;
        }
        start--;
    }
    return start;
}

/**
 * @brief Execute a tableau move (assumes legality already validated)
 * @param[in] from_col source column
 * @param[in] seq_start top of move within from_col
 * @param[in] to_col destination column
 * @return none
 */
STATIC VOID_T __sp_move(INT32_T from_col, INT32_T seq_start, INT32_T to_col)
{
    COL_T *from = &s_sp.cols[from_col];
    COL_T *to   = &s_sp.cols[to_col];
    INT32_T n_move = from->n - seq_start;
    for (INT32_T i = 0; i < n_move; i++) {
        to->cards[to->n++] = from->cards[seq_start + i];
    }
    from->n = seq_start;
    if (from->n > 0 && !s_sp.deck[from->cards[from->n - 1]].face_up) {
        s_sp.deck[from->cards[from->n - 1]].face_up = 1;
        s_sp.score += 5;
    }
    s_sp.score -= 1;
    __sp_check_complete(to_col);
}

/**
 * @brief Deal one card face-up onto every non-empty column from the stock
 * @return none
 */
STATIC VOID_T __sp_deal_stock(VOID_T)
{
    if (s_sp.stock.n < N_COLS) {
        return;
    }
    for (INT32_T c = 0; c < N_COLS; c++) {
        UINT8_T ci = s_sp.stock.cards[--s_sp.stock.n];
        s_sp.deck[ci].face_up = 1;
        s_sp.cols[c].cards[s_sp.cols[c].n++] = ci;
        __sp_check_complete(c);
    }
}

/* ---------------------------------------------------------------------------
 * Card widget rendering
 * --------------------------------------------------------------------------- */
/**
 * @brief Paint card content (rank/suit text or face-down hatching)
 * @param[in] obj card container
 * @param[in] rank 1..13
 * @param[in] face_up TRUE for face-up
 * @param[in] selected TRUE to tint yellow (legacy click select)
 * @return none
 */
STATIC VOID_T __paint_card_obj(lv_obj_t *obj, INT32_T rank, BOOL_T face_up, BOOL_T selected)
{
    if (obj == NULL) {
        return;
    }

    UINT32_T bg;
    if (!face_up) {
        bg = SP_FACE_DOWN_COLOR;
    } else if (selected) {
        bg = SP_HOVER_HIGHLIGHT;
    } else {
        bg = 0xFFFFFF;
    }
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    if (!face_up) {
        for (INT32_T y = 4; y < CARD_H - 4; y += 4) {
            for (INT32_T x = 4; x < CARD_W - 4; x += 4) {
                lv_obj_t *p = lv_obj_create(obj);
                lv_obj_remove_style_all(p);
                lv_obj_set_size(p, 1, 1);
                lv_obj_set_pos(p, x, y);
                lv_obj_set_style_bg_color(p, lv_color_hex(SP_FACE_DOWN_STIPPLE), 0);
                lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
                lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
            }
        }
        return;
    }

    CHAR_T buf[8];
    snprintf(buf, sizeof(buf), "%s\n%s", __rank_str(rank), "S");
    lv_obj_t *lbl = lv_label_create(obj);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(lbl, buf);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 3, 2);

    lv_obj_t *lbl2 = lv_label_create(obj);
    lv_obj_set_style_text_color(lbl2, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl2, &lv_font_unscii_8, 0);
    lv_label_set_text(lbl2, buf);
    lv_obj_align(lbl2, LV_ALIGN_BOTTOM_RIGHT, -3, -2);
    lv_obj_set_style_transform_pivot_x(lbl2, 0, 0);
}

/**
 * @brief Build a single card widget into \a parent at the given offset
 * @param[in] parent container
 * @param[in] x x position relative to parent
 * @param[in] y y position relative to parent
 * @param[in] rank 1..13
 * @param[in] face_up TRUE for face-up rendering
 * @param[in] selected TRUE to tint yellow
 * @return new card object
 */
STATIC lv_obj_t *__make_card(lv_obj_t *parent, INT32_T x, INT32_T y,
                              INT32_T rank, BOOL_T face_up, BOOL_T selected)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    win95_chrome_raised(card);
    __paint_card_obj(card, rank, face_up, selected);
    return card;
}

/* ---------------------------------------------------------------------------
 * Drag-and-drop ghost
 * --------------------------------------------------------------------------- */
/**
 * @brief Build a floating composite of the dragged stack (semi-translucent ghost)
 * @return none
 */
STATIC VOID_T __drag_create_ghost(VOID_T)
{
    if (s_sp.drag.ghost != NULL || s_sp.screen == NULL) {
        return;
    }
    INT32_T n = s_sp.drag.n_drag;
    if (n <= 0) {
        return;
    }
    INT32_T h = CARD_H + (n - 1) * CARD_OVERLAP;
    lv_obj_t *g = lv_obj_create(s_sp.screen);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, CARD_W, h);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_move_foreground(g);

    COL_T *col = &s_sp.cols[s_sp.drag.col];
    for (INT32_T k = 0; k < n; k++) {
        UINT8_T ci = col->cards[s_sp.drag.seq_start + k];
        __make_card(g, 0, k * CARD_OVERLAP,
                    (INT32_T)s_sp.deck[ci].rank, TRUE, FALSE);
    }
    s_sp.drag.ghost = g;
}

/**
 * @brief Reset transient drag state and tear down the ghost
 * @return none
 */
STATIC VOID_T __drag_reset(VOID_T)
{
    if (s_sp.drag.ghost) {
        lv_obj_delete(s_sp.drag.ghost);
    }
    memset(&s_sp.drag, 0, sizeof(s_sp.drag));
}

/* ---------------------------------------------------------------------------
 * Event callbacks
 * --------------------------------------------------------------------------- */
/**
 * @brief Card pressed (LV_EVENT_PRESSED) - capture drag candidate
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __card_press_cb(lv_event_t *e)
{
    VOID_T *u = lv_event_get_user_data(e);
    INT32_T col = SP_LOC_COL(u);
    INT32_T pos = SP_LOC_POS(u);
    if (col < 0 || col >= N_COLS) {
        return;
    }
    if (pos < 0 || pos >= s_sp.cols[col].n) {
        return;
    }
    if (!s_sp.deck[s_sp.cols[col].cards[pos]].face_up) {
        return;
    }
    INT32_T seq = __sp_seq_start(col);
    if (seq < 0 || pos < seq) {
        return;
    }
    s_sp.drag.active    = TRUE;
    s_sp.drag.moved     = FALSE;
    s_sp.drag.col       = col;
    s_sp.drag.seq_start = pos;
    s_sp.drag.n_drag    = s_sp.cols[col].n - pos;
    s_sp.drag.ghost     = NULL;

    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &s_sp.drag.press_pt);
    } else {
        s_sp.drag.press_pt.x = 0;
        s_sp.drag.press_pt.y = 0;
    }
}

/**
 * @brief Card pressing (LV_EVENT_PRESSING) - track movement, lazily build ghost
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __card_pressing_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!s_sp.drag.active) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (!s_sp.drag.moved) {
        INT32_T dx = pt.x - s_sp.drag.press_pt.x;
        INT32_T dy = pt.y - s_sp.drag.press_pt.y;
        if (dx * dx + dy * dy < SP_DRAG_THRESHOLD_SQ) {
            return;
        }
        s_sp.drag.moved = TRUE;
        __drag_create_ghost();
    }
    if (s_sp.drag.ghost) {
        INT32_T x = pt.x - CARD_W / 2;
        INT32_T y = pt.y - 8;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > SP_W - CARD_W) x = SP_W - CARD_W;
        lv_obj_set_pos(s_sp.drag.ghost, x, y);
    }
}

/**
 * @brief Card released (LV_EVENT_RELEASED) - commit drop or revert
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __card_released_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!s_sp.drag.active) {
        return;
    }
    BOOL_T did_drag = s_sp.drag.moved;
    INT32_T from_col = s_sp.drag.col;
    INT32_T seq      = s_sp.drag.seq_start;

    INT32_T target_col = -1;
    if (did_drag) {
        lv_indev_t *indev = lv_indev_active();
        lv_point_t pt;
        if (indev) {
            lv_indev_get_point(indev, &pt);
        } else {
            pt.x = 0; pt.y = 0;
        }
        INT32_T col_stride = (SP_W - COL_X0 - 4) / N_COLS;
        INT32_T tcol = (pt.x - COL_X0) / col_stride;
        if (tcol < 0) tcol = 0;
        if (tcol >= N_COLS) tcol = N_COLS - 1;
        target_col = tcol;
    }

    __drag_reset();

    if (did_drag && target_col >= 0 && target_col != from_col) {
        if (__sp_can_move(from_col, seq, target_col)) {
            __sp_move(from_col, seq, target_col);
            s_sp.sel_col = -1;
        }
    }
    if (did_drag) {
        __sp_render();
        __sp_update_score_label();
        if (s_sp.completed >= 8 && s_sp.status_lbl) {
            lv_label_set_text(s_sp.status_lbl, "You Win!");
        }
    }
}

/**
 * @brief Card press lost (LV_EVENT_PRESS_LOST) - cancel drag without commit
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __card_press_lost_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!s_sp.drag.active) {
        return;
    }
    BOOL_T was_dragging = s_sp.drag.moved;
    __drag_reset();
    if (was_dragging) {
        __sp_render();
    }
}

/**
 * @brief Card clicked (LV_EVENT_CLICKED) - tap fallback (no drag occurred)
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __card_click_cb(lv_event_t *e)
{
    VOID_T *u = lv_event_get_user_data(e);
    INT32_T col = SP_LOC_COL(u);
    INT32_T pos = SP_LOC_POS(u);
    if (col < 0 || col >= N_COLS) {
        return;
    }

    if (s_sp.sel_col >= 0) {
        INT32_T from = s_sp.sel_col;
        INT32_T seq_start = __sp_seq_start(from);
        if (seq_start < 0) {
            s_sp.sel_col = -1;
            __sp_render();
            return;
        }
        if (col != from && __sp_can_move(from, seq_start, col)) {
            __sp_move(from, seq_start, col);
            s_sp.sel_col = -1;
        } else {
            if (pos >= 0 && s_sp.cols[col].n > 0 &&
                s_sp.deck[s_sp.cols[col].cards[pos]].face_up) {
                s_sp.sel_col = col;
            } else {
                s_sp.sel_col = -1;
            }
        }
    } else {
        if (pos >= 0 && s_sp.cols[col].n > 0 &&
            s_sp.deck[s_sp.cols[col].cards[pos]].face_up) {
            s_sp.sel_col = col;
        }
    }

    __sp_update_score_label();
    if (s_sp.completed >= 8 && s_sp.status_lbl) {
        lv_label_set_text(s_sp.status_lbl, "You Win!");
        s_sp.sel_col = -1;
    }
    __sp_render();
}

/**
 * @brief Stock pile clicked - deal one row
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __stock_click_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_sp.stock.n < N_COLS) {
        if (s_sp.status_lbl) {
            lv_label_set_text(s_sp.status_lbl, "Stock empty!");
        }
        return;
    }
    s_sp.sel_col = -1;
    __sp_deal_stock();
    __sp_update_score_label();
    __sp_render();
}

/**
 * @brief Close button clicked
 * @param[in] e LVGL event
 * @return none
 */
STATIC VOID_T __sp_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    __drag_reset();
    if (s_sp.screen) {
        lv_obj_delete(s_sp.screen);
    }
    memset(&s_sp, 0, sizeof(s_sp));
}

/* ---------------------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------------------- */
/**
 * @brief Refresh the score label from current state
 * @return none
 */
STATIC VOID_T __sp_update_score_label(VOID_T)
{
    if (!s_sp.score_lbl) {
        return;
    }
    CHAR_T buf[32];
    snprintf(buf, sizeof(buf), "Score: %d  Sets: %d",
             (INT32_T)s_sp.score, (INT32_T)s_sp.completed);
    lv_label_set_text(s_sp.score_lbl, buf);
}

/**
 * @brief Re-create all card widgets from game state
 * @return none
 */
STATIC VOID_T __sp_render(VOID_T)
{
    if (!s_sp.card_area) {
        return;
    }
    lv_obj_clean(s_sp.card_area);

    INT32_T col_stride = (SP_W - COL_X0 - 4) / N_COLS;

    for (INT32_T c = 0; c < N_COLS; c++) {
        INT32_T cx = COL_X0 + c * col_stride;
        COL_T *col = &s_sp.cols[c];

        lv_obj_t *placeholder = lv_obj_create(s_sp.card_area);
        lv_obj_remove_style_all(placeholder);
        lv_obj_set_size(placeholder, CARD_W, CARD_H);
        lv_obj_set_pos(placeholder, cx, COL_Y0);
        lv_obj_set_style_bg_opa(placeholder, LV_OPA_TRANSP, 0);
        win95_chrome_sunken(placeholder);
        lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_SCROLLABLE);

        if (col->n == 0) {
            lv_obj_add_flag(placeholder, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(placeholder, __card_click_cb,
                                LV_EVENT_CLICKED, SP_LOC_PACK(c, -1));
            lv_obj_add_event_cb(placeholder, __card_press_cb,
                                LV_EVENT_PRESSED, SP_LOC_PACK(c, -1));
            lv_obj_add_event_cb(placeholder, __card_pressing_cb,
                                LV_EVENT_PRESSING, SP_LOC_PACK(c, -1));
            lv_obj_add_event_cb(placeholder, __card_released_cb,
                                LV_EVENT_RELEASED, SP_LOC_PACK(c, -1));
            lv_obj_add_event_cb(placeholder, __card_press_lost_cb,
                                LV_EVENT_PRESS_LOST, SP_LOC_PACK(c, -1));
        }

        INT32_T y = COL_Y0;
        for (INT32_T k = 0; k < col->n; k++) {
            UINT8_T ci = col->cards[k];
            BOOL_T fu = (BOOL_T)s_sp.deck[ci].face_up;
            BOOL_T sel = (s_sp.sel_col == c) && (k >= __sp_seq_start(c));
            lv_obj_t *card = __make_card(s_sp.card_area, cx, y,
                                          (INT32_T)s_sp.deck[ci].rank, fu, sel);

            if (fu) {
                lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(card, __card_click_cb,
                                    LV_EVENT_CLICKED, SP_LOC_PACK(c, k));
                lv_obj_add_event_cb(card, __card_press_cb,
                                    LV_EVENT_PRESSED, SP_LOC_PACK(c, k));
                lv_obj_add_event_cb(card, __card_pressing_cb,
                                    LV_EVENT_PRESSING, SP_LOC_PACK(c, k));
                lv_obj_add_event_cb(card, __card_released_cb,
                                    LV_EVENT_RELEASED, SP_LOC_PACK(c, k));
                lv_obj_add_event_cb(card, __card_press_lost_cb,
                                    LV_EVENT_PRESS_LOST, SP_LOC_PACK(c, k));
            }
            if (!fu) {
                y += CARD_BACK_OVERLAP;
            } else {
                y += CARD_OVERLAP;
            }
        }
    }

    INT32_T deals = s_sp.stock.n / N_COLS;
    CHAR_T sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "[%d]", (INT32_T)deals);

    lv_obj_t *stockobj = lv_obj_create(s_sp.card_area);
    lv_obj_remove_style_all(stockobj);
    lv_obj_set_size(stockobj, CARD_W, CARD_H);
    lv_obj_set_pos(stockobj, STOCK_X, STOCK_Y);
    lv_obj_set_style_bg_color(stockobj, lv_color_hex(
        s_sp.stock.n >= N_COLS ? SP_FACE_DOWN_COLOR : 0x444444), 0);
    lv_obj_set_style_bg_opa(stockobj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(stockobj, LV_OBJ_FLAG_SCROLLABLE);
    win95_chrome_raised(stockobj);
    lv_obj_add_flag(stockobj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stockobj, __stock_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *slbl = lv_label_create(stockobj);
    lv_obj_set_style_text_color(slbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(slbl, &lv_font_unscii_8, 0);
    lv_label_set_text(slbl, sbuf);
    lv_obj_center(slbl);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Open Spider Solitaire window
 * @param[in] parent parent screen / layer; pass NULL for top layer
 * @return none
 */
VOID_T win95_spider_open(lv_obj_t *parent)
{
    if (s_sp.screen) {
        __drag_reset();
        lv_obj_delete(s_sp.screen);
        memset(&s_sp, 0, sizeof(s_sp));
    }

    s_sp.rand_seed = (UINT32_T)tal_time_get_posix();
    s_sp.score     = 500;
    s_sp.sel_col   = -1;
    __sp_deal_initial();

    lv_obj_t *par = parent ? parent : lv_layer_top();
    lv_obj_t *scr = lv_obj_create(par);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, SP_W, SP_H);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(SP_FELT_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(scr);
    s_sp.screen = scr;

    lv_obj_t *tb = lv_obj_create(scr);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, SP_W, 18);
    lv_obj_set_pos(tb, 0, 0);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Spider Solitaire");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    s_sp.score_lbl = lv_label_create(tb);
    lv_obj_set_style_text_color(s_sp.score_lbl, lv_color_hex(0xFFFF88), 0);
    lv_obj_set_style_text_font(s_sp.score_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_sp.score_lbl, "Score: 500  Sets: 0");
    lv_obj_align(s_sp.score_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    win95_chrome_button(xb);
    lv_obj_add_event_cb(xb, __sp_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "x");
    lv_obj_center(xl);

    s_sp.status_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_sp.status_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_sp.status_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_sp.status_lbl,
                      "Drag a card to move; tap stock to deal a row.");
    lv_obj_set_pos(s_sp.status_lbl, 4, SP_H - 14);
    lv_obj_set_width(s_sp.status_lbl, SP_W - CARD_W - 10);

    lv_obj_t *ca = lv_obj_create(scr);
    lv_obj_remove_style_all(ca);
    lv_obj_set_size(ca, SP_W, SP_H - 20);
    lv_obj_set_pos(ca, 0, 20);
    lv_obj_set_style_bg_opa(ca, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ca, LV_OBJ_FLAG_SCROLLABLE);
    s_sp.card_area = ca;

    __sp_render();
}
