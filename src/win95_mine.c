/**
 * @file win95_mine.c
 * @brief Win95-style Minesweeper — 9×9 grid, 10 mines.
 *        Short tap = reveal, long press = flag, BFS flood-fill for empty cells.
 */
#include "win95_mine.h"
#include "win95_chrome.h"
#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Layout constants (fits in 480×320)
 * --------------------------------------------------------------------------- */
#define MINE_W          BIOS_SCREEN_WIDTH   /* 480 */
#define MINE_H          BIOS_SCREEN_HEIGHT  /* 320 */
#define MINE_TITLE_H    18
#define MINE_STATUS_H   28

#define MINE_ROWS       9
#define MINE_COLS       9
#define MINE_COUNT      10
#define MINE_CELL_SZ    28   /* 9*28 = 252px — fits in 320-(18+28) = 274px */
#define MINE_GRID_W     (MINE_COLS * MINE_CELL_SZ)   /* 252 */
#define MINE_GRID_H     (MINE_ROWS * MINE_CELL_SZ)   /* 252 */
#define MINE_GRID_X     ((MINE_W - MINE_GRID_W) / 2) /* 114 */
#define MINE_GRID_Y     (MINE_TITLE_H + MINE_STATUS_H + 2)

/* Number colours (classic Minesweeper) */
STATIC CONST UINT32_T s_num_clr[9] = {
    0xC0C0C0, /* 0 — not shown */
    0x0000FF, /* 1 — blue */
    0x008000, /* 2 — green */
    0xFF0000, /* 3 — red */
    0x000080, /* 4 — dark blue */
    0x800000, /* 5 — dark red */
    0x008080, /* 6 — teal */
    0x000000, /* 7 — black */
    0x808080, /* 8 — gray */
};

/* ---------------------------------------------------------------------------
 * Cell data
 * --------------------------------------------------------------------------- */
typedef struct {
    UINT8_T mine;
    UINT8_T revealed;
    UINT8_T flagged;
    UINT8_T adj;      /* adjacent mine count */
} CELL_T;

/* ---------------------------------------------------------------------------
 * Module context (single instance — only one Minesweeper window at a time)
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t  *screen;
    lv_obj_t  *mine_lbl;
    lv_obj_t  *face_btn;
    lv_obj_t  *time_lbl;
    lv_obj_t  *cells[MINE_ROWS][MINE_COLS];
    CELL_T     board[MINE_ROWS][MINE_COLS];
    INT32_T    flags_placed;
    INT32_T    revealed_count;
    INT32_T    elapsed;
    BOOL_T     game_over;
    BOOL_T     first_reveal; /* TRUE = mines not placed yet */
    lv_timer_t *timer;
} MINE_CTX_T;

STATIC MINE_CTX_T *s_mine = NULL;

/* ---------------------------------------------------------------------------
 * Simple LCG for mine placement
 * --------------------------------------------------------------------------- */
STATIC UINT32_T s_seed = 0;
STATIC INT32_T __rand_n(INT32_T n)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return (INT32_T)((s_seed >> 16) & 0x7fffu) % n;
}

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mine_reset(MINE_CTX_T *ctx);
STATIC VOID_T __mine_reveal(MINE_CTX_T *ctx, INT32_T r, INT32_T c);
STATIC VOID_T __mine_update_cell_ui(MINE_CTX_T *ctx, INT32_T r, INT32_T c);

/* ---------------------------------------------------------------------------
 * Board initialisation (mines placed AFTER first reveal to avoid instant-death)
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mine_place_mines(MINE_CTX_T *ctx, INT32_T safe_r, INT32_T safe_c)
{
    INT32_T pos[MINE_ROWS * MINE_COLS];
    INT32_T n = MINE_ROWS * MINE_COLS;
    for (INT32_T i = 0; i < n; i++) pos[i] = i;

    /* Fisher-Yates shuffle */
    for (INT32_T i = n - 1; i > 0; i--) {
        INT32_T j = __rand_n(i + 1);
        INT32_T tmp = pos[i]; pos[i] = pos[j]; pos[j] = tmp;
    }

    INT32_T placed = 0;
    for (INT32_T i = 0; i < n && placed < MINE_COUNT; i++) {
        INT32_T r = pos[i] / MINE_COLS;
        INT32_T c = pos[i] % MINE_COLS;
        /* Skip the safe first-click cell and its immediate neighbours */
        if (r >= safe_r - 1 && r <= safe_r + 1 &&
            c >= safe_c - 1 && c <= safe_c + 1) continue;
        ctx->board[r][c].mine = 1;
        placed++;
    }

    /* Compute adjacency counts */
    for (INT32_T r = 0; r < MINE_ROWS; r++) {
        for (INT32_T c = 0; c < MINE_COLS; c++) {
            if (ctx->board[r][c].mine) continue;
            UINT8_T cnt = 0;
            for (INT32_T dr = -1; dr <= 1; dr++) {
                for (INT32_T dc = -1; dc <= 1; dc++) {
                    INT32_T nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < MINE_ROWS && nc >= 0 && nc < MINE_COLS)
                        cnt += ctx->board[nr][nc].mine;
                }
            }
            ctx->board[r][c].adj = cnt;
        }
    }
}

/* ---------------------------------------------------------------------------
 * UI helpers
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mine_update_counters(MINE_CTX_T *ctx)
{
    CHAR_T buf[16];
    INT32_T remaining = MINE_COUNT - ctx->flags_placed;
    if (remaining < 0) remaining = 0;
    snprintf(buf, sizeof(buf), "%03d", remaining);
    lv_label_set_text(ctx->mine_lbl, buf);
    snprintf(buf, sizeof(buf), "%03d", ctx->elapsed);
    lv_label_set_text(ctx->time_lbl, buf);
}

STATIC VOID_T __mine_update_cell_ui(MINE_CTX_T *ctx, INT32_T r, INT32_T c)
{
    lv_obj_t *btn = ctx->cells[r][c];
    CELL_T   *cell = &ctx->board[r][c];
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);

    if (!cell->revealed) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        win95_chrome_raised(btn);
        if (cell->flagged) {
            lv_label_set_text(lbl, "P");
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF0000), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        } else {
            lv_label_set_text(lbl, "");
        }
        return;
    }

    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(WIN95_COLOR_SHADOW), 0);

    if (cell->mine) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF4444), 0);
        lv_label_set_text(lbl, "*");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_16, 0);
    } else if (cell->adj > 0) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
        CHAR_T s[4]; snprintf(s, sizeof(s), "%d", (INT32_T)cell->adj);
        lv_label_set_text(lbl, s);
        lv_obj_set_style_text_color(lbl, lv_color_hex(s_num_clr[cell->adj]), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_16, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
        lv_label_set_text(lbl, "");
    }
}

STATIC VOID_T __mine_reveal_all_mines(MINE_CTX_T *ctx)
{
    for (INT32_T r = 0; r < MINE_ROWS; r++) {
        for (INT32_T c = 0; c < MINE_COLS; c++) {
            if (ctx->board[r][c].mine && !ctx->board[r][c].revealed) {
                ctx->board[r][c].revealed = 1;
                __mine_update_cell_ui(ctx, r, c);
            }
        }
    }
}

STATIC VOID_T __mine_check_win(MINE_CTX_T *ctx)
{
    INT32_T safe = MINE_ROWS * MINE_COLS - MINE_COUNT;
    if (ctx->revealed_count >= safe) {
        ctx->game_over = TRUE;
        lv_label_set_text(lv_obj_get_child(ctx->face_btn, 0), ":D");
        if (ctx->timer) { lv_timer_pause(ctx->timer); }
    }
}

/* ---------------------------------------------------------------------------
 * BFS flood-fill reveal
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mine_reveal(MINE_CTX_T *ctx, INT32_T r, INT32_T c)
{
    if (r < 0 || r >= MINE_ROWS || c < 0 || c >= MINE_COLS) return;
    CELL_T *cell = &ctx->board[r][c];
    if (cell->revealed || cell->flagged) return;

    cell->revealed = 1;
    ctx->revealed_count++;
    __mine_update_cell_ui(ctx, r, c);

    if (cell->mine) {
        ctx->game_over = TRUE;
        lv_label_set_text(lv_obj_get_child(ctx->face_btn, 0), ":(");
        __mine_reveal_all_mines(ctx);
        if (ctx->timer) { lv_timer_pause(ctx->timer); }
        return;
    }

    if (cell->adj == 0) {
        /* BFS queue */
        INT32_T queue[MINE_ROWS * MINE_COLS][2];
        INT32_T head = 0, tail = 0;
        queue[tail][0] = r; queue[tail][1] = c; tail++;

        while (head < tail) {
            INT32_T qr = queue[head][0], qc = queue[head][1]; head++;
            for (INT32_T dr = -1; dr <= 1; dr++) {
                for (INT32_T dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    INT32_T nr = qr + dr, nc = qc + dc;
                    if (nr < 0 || nr >= MINE_ROWS || nc < 0 || nc >= MINE_COLS) continue;
                    CELL_T *nb = &ctx->board[nr][nc];
                    if (nb->revealed || nb->flagged || nb->mine) continue;
                    nb->revealed = 1;
                    ctx->revealed_count++;
                    __mine_update_cell_ui(ctx, nr, nc);
                    if (nb->adj == 0 && tail < MINE_ROWS * MINE_COLS) {
                        queue[tail][0] = nr; queue[tail][1] = nc; tail++;
                    }
                }
            }
        }
    }

    __mine_check_win(ctx);
}

/* ---------------------------------------------------------------------------
 * Event callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __cell_click_cb(lv_event_t *e)
{
    if (!s_mine || s_mine->game_over) return;
    INT32_T idx = (INT32_T)(uintptr_t)lv_event_get_user_data(e);
    INT32_T r = idx / MINE_COLS, c = idx % MINE_COLS;
    CELL_T *cell = &s_mine->board[r][c];
    if (cell->revealed || cell->flagged) return;

    if (s_mine->first_reveal) {
        s_mine->first_reveal = FALSE;
        __mine_place_mines(s_mine, r, c);
        lv_timer_resume(s_mine->timer);
    }
    __mine_reveal(s_mine, r, c);
}

STATIC VOID_T __cell_long_press_cb(lv_event_t *e)
{
    if (!s_mine || s_mine->game_over) return;
    INT32_T idx = (INT32_T)(uintptr_t)lv_event_get_user_data(e);
    INT32_T r = idx / MINE_COLS, c = idx % MINE_COLS;
    CELL_T *cell = &s_mine->board[r][c];
    if (cell->revealed) return;

    cell->flagged ^= 1;
    s_mine->flags_placed += cell->flagged ? 1 : -1;
    __mine_update_cell_ui(s_mine, r, c);
    __mine_update_counters(s_mine);
}

STATIC VOID_T __face_click_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!s_mine) return;
    __mine_reset(s_mine);
}

STATIC VOID_T __close_click_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (!s_mine) return;
    if (s_mine->timer) { lv_timer_delete(s_mine->timer); s_mine->timer = NULL; }
    lv_obj_delete(s_mine->screen);
    tal_free(s_mine);
    s_mine = NULL;
}

STATIC VOID_T __mine_tick_cb(lv_timer_t *t)
{
    (VOID_T)t;
    if (!s_mine || s_mine->game_over) return;
    if (s_mine->elapsed < 999) s_mine->elapsed++;
    __mine_update_counters(s_mine);
}

/* ---------------------------------------------------------------------------
 * Board reset
 * --------------------------------------------------------------------------- */
STATIC VOID_T __mine_reset(MINE_CTX_T *ctx)
{
    memset(ctx->board, 0, sizeof(ctx->board));
    ctx->flags_placed  = 0;
    ctx->revealed_count = 0;
    ctx->elapsed       = 0;
    ctx->game_over     = FALSE;
    ctx->first_reveal  = TRUE;

    for (INT32_T r = 0; r < MINE_ROWS; r++) {
        for (INT32_T c = 0; c < MINE_COLS; c++) {
            __mine_update_cell_ui(ctx, r, c);
        }
    }
    lv_label_set_text(lv_obj_get_child(ctx->face_btn, 0), ":)");
    lv_timer_pause(ctx->timer);
    __mine_update_counters(ctx);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_mine_open(lv_obj_t *parent)
{
    /* Only one instance at a time */
    if (s_mine) {
        lv_obj_move_foreground(s_mine->screen);
        return;
    }

    /* Seed RNG from system time */
    s_seed = (UINT32_T)tal_time_get_posix();

    MINE_CTX_T *ctx = (MINE_CTX_T *)tal_malloc(sizeof(MINE_CTX_T));
    if (!ctx) return;
    memset(ctx, 0, sizeof(MINE_CTX_T));
    ctx->first_reveal = TRUE;
    s_mine = ctx;

    /* ---- Full-screen window with raised outer bevel ---- */
    lv_obj_t *par = parent ? parent : lv_layer_top();
    lv_obj_t *win = lv_obj_create(par);
    lv_obj_remove_style_all(win);
    lv_obj_set_size(win, MINE_W, MINE_H);
    lv_obj_set_pos(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    win95_chrome_raised(win);
    ctx->screen = win;

    /* ---- Title bar ---- */
    lv_obj_t *tb = lv_obj_create(win);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, MINE_W, MINE_TITLE_H);
    lv_obj_set_pos(tb, 0, 0);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Minesweeper");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 16, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    win95_chrome_button(xb);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "X");
    lv_obj_center(xl);
    lv_obj_add_event_cb(xb, __close_click_cb, LV_EVENT_CLICKED, NULL);

    /* ---- Status row (sunken bevel, classic Win95 minesweeper top bar) ---- */
    lv_obj_t *sr = lv_obj_create(win);
    lv_obj_remove_style_all(sr);
    lv_obj_set_size(sr, MINE_W - 8, MINE_STATUS_H);
    lv_obj_set_pos(sr, 4, MINE_TITLE_H + 2);
    lv_obj_set_style_bg_color(sr, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(sr, LV_OPA_COVER, 0);
    win95_chrome_inset(sr);
    lv_obj_clear_flag(sr, LV_OBJ_FLAG_SCROLLABLE);

    /* Mine count label */
    ctx->mine_lbl = lv_label_create(sr);
    lv_obj_set_style_text_color(ctx->mine_lbl, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(ctx->mine_lbl, &lv_font_unscii_16, 0);
    lv_obj_set_style_bg_color(ctx->mine_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ctx->mine_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ctx->mine_lbl, 2, 0);
    lv_label_set_text(ctx->mine_lbl, "010");
    lv_obj_align(ctx->mine_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    /* Smiley/face button (raised Win95 button) */
    ctx->face_btn = lv_btn_create(sr);
    lv_obj_set_size(ctx->face_btn, 24, 22);
    lv_obj_align(ctx->face_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ctx->face_btn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    win95_chrome_button(ctx->face_btn);
    lv_obj_t *fl = lv_label_create(ctx->face_btn);
    lv_obj_set_style_text_font(fl, &lv_font_unscii_8, 0);
    lv_label_set_text(fl, ":)");
    lv_obj_center(fl);
    lv_obj_add_event_cb(ctx->face_btn, __face_click_cb, LV_EVENT_CLICKED, NULL);

    /* Timer label */
    ctx->time_lbl = lv_label_create(sr);
    lv_obj_set_style_text_color(ctx->time_lbl, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(ctx->time_lbl, &lv_font_unscii_16, 0);
    lv_obj_set_style_bg_color(ctx->time_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ctx->time_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ctx->time_lbl, 2, 0);
    lv_label_set_text(ctx->time_lbl, "000");
    lv_obj_align(ctx->time_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ---- Grid container: sunken inset frames the playing field ---- */
    lv_obj_t *grid_cont = lv_obj_create(win);
    lv_obj_remove_style_all(grid_cont);
    lv_obj_set_size(grid_cont, MINE_GRID_W + 4, MINE_GRID_H + 4);
    lv_obj_set_pos(grid_cont, MINE_GRID_X - 2, MINE_GRID_Y - 2);
    lv_obj_set_style_bg_color(grid_cont, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
    lv_obj_set_style_bg_opa(grid_cont, LV_OPA_COVER, 0);
    win95_chrome_inset(grid_cont);
    lv_obj_clear_flag(grid_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Cells ---- */
    for (INT32_T r = 0; r < MINE_ROWS; r++) {
        for (INT32_T c = 0; c < MINE_COLS; c++) {
            lv_obj_t *btn = lv_obj_create(grid_cont);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, MINE_CELL_SZ, MINE_CELL_SZ);
            lv_obj_set_pos(btn, c * MINE_CELL_SZ + 2, r * MINE_CELL_SZ + 2);
            lv_obj_set_style_bg_color(btn, lv_color_hex(WIN95_COLOR_TASKBAR), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            win95_chrome_raised(btn);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
            lv_label_set_text(lbl, "");
            lv_obj_center(lbl);

            INT32_T idx = r * MINE_COLS + c;
            lv_obj_add_event_cb(btn, __cell_click_cb,      LV_EVENT_CLICKED,
                                 (VOID_T *)(uintptr_t)idx);
            lv_obj_add_event_cb(btn, __cell_long_press_cb, LV_EVENT_LONG_PRESSED,
                                 (VOID_T *)(uintptr_t)idx);
            ctx->cells[r][c] = btn;
        }
    }

    /* ---- Timer (starts paused; unpaused on first reveal) ---- */
    ctx->timer = lv_timer_create(__mine_tick_cb, 1000, NULL);
    lv_timer_pause(ctx->timer);
    __mine_update_counters(ctx);
}
