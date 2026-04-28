/**
 * @file win95_defrag.c
 * @brief Win95-style Disk Defragmenter — animated cluster grid simulator.
 *
 * Visual: 40×18 grid of coloured 10×10 cells (400 clusters shown).
 * State colours:
 *   White       = free
 *   Blue        = used / optimized
 *   Red         = used / fragmented
 *   Cyan/green  = being read (source)
 *   Yellow      = being written (destination)
 *
 * The animation runs a fake optimisation pass: red clusters get moved to the
 * front (blue) one at a time, compacting the disk visually.
 */
#include "win95_defrag.h"
#include "win95_desktop.h"
#include "bios_simulator.h"
#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Layout
 * --------------------------------------------------------------------------- */
#define GRID_COLS       40
#define GRID_ROWS       18
#define CELL_SZ         9      /* px per cell (with 1px gap) */
#define CELL_GAP        1
#define GRID_X          4      /* grid left offset within content area */
#define GRID_Y          4

#define N_CELLS         (GRID_COLS * GRID_ROWS)   /* 720 */

/* Cluster states */
#define CS_FREE         0
#define CS_USED         1    /* optimized / in-place */
#define CS_FRAG         2    /* fragmented */
#define CS_READ         3    /* being read */
#define CS_WRITE        4    /* being written */

STATIC CONST UINT32_T s_colors[] = {
    0xFFFFFF,   /* CS_FREE  */
    0x0000CC,   /* CS_USED  */
    0xFF4444,   /* CS_FRAG  */
    0x00FFCC,   /* CS_READ  */
    0xFFEE00,   /* CS_WRITE */
};

/* ---------------------------------------------------------------------------
 * Defrag context
 * --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t  *win;
    lv_obj_t  *parent;         /* saved to allow close */
    lv_obj_t  *status_lbl;
    lv_obj_t  *pct_lbl;
    lv_obj_t  *cells[N_CELLS];   /* one lv_obj per cell */
    uint8_t    state[N_CELLS];
    lv_timer_t *timer;
    INT32_T    src_idx;          /* next fragmented cluster to move */
    INT32_T    dst_idx;          /* next free slot at front */
    INT32_T    total_frag;
    INT32_T    moved;
    BOOL_T     running;
    INT32_T    prev_src;
    INT32_T    prev_dst;
} DEFRAG_CTX_T;

STATIC DEFRAG_CTX_T s_df;

/* ---------------------------------------------------------------------------
 * LCG random (local — avoid dependency on global seed)
 * --------------------------------------------------------------------------- */
STATIC UINT32_T s_df_seed = 0xDEFACEu;
STATIC INT32_T __dfrand(INT32_T n)
{
    s_df_seed = s_df_seed * 1664525u + 1013904223u;
    return (INT32_T)((s_df_seed >> 8) & 0x7FFFFFu) % n;
}

/* ---------------------------------------------------------------------------
 * Initialise cluster state — roughly 60% used, 30% fragmented, 10% free
 * --------------------------------------------------------------------------- */
STATIC VOID_T __init_clusters(VOID_T)
{
    INT32_T i;
    for (i = 0; i < N_CELLS; i++) {
        INT32_T r = __dfrand(10);
        if (r < 3) s_df.state[i] = CS_FREE;
        else if (r < 7) s_df.state[i] = CS_USED;
        else s_df.state[i] = CS_FRAG;
    }
    /* Count fragmented */
    s_df.total_frag = 0;
    for (i = 0; i < N_CELLS; i++) {
        if (s_df.state[i] == CS_FRAG) s_df.total_frag++;
    }
    s_df.moved = 0;
}

/* ---------------------------------------------------------------------------
 * Update a single cell's colour
 * --------------------------------------------------------------------------- */
STATIC VOID_T __cell_paint(INT32_T idx)
{
    if (!s_df.cells[idx]) return;
    lv_obj_set_style_bg_color(s_df.cells[idx],
                              lv_color_hex(s_colors[s_df.state[idx]]), 0);
}

/* ---------------------------------------------------------------------------
 * Animation tick (40 ms — fast visual)
 * --------------------------------------------------------------------------- */
STATIC VOID_T __defrag_tick(lv_timer_t *t)
{
    (VOID_T)t;
    if (!s_df.running) return;

    /* Restore previous read/write cells */
    if (s_df.prev_src >= 0) {
        s_df.state[s_df.prev_src] = CS_FREE;
        __cell_paint(s_df.prev_src);
        s_df.prev_src = -1;
    }
    if (s_df.prev_dst >= 0) {
        s_df.state[s_df.prev_dst] = CS_USED;
        __cell_paint(s_df.prev_dst);
        s_df.prev_dst = -1;
    }

    /* Find next fragmented cluster (src) */
    while (s_df.src_idx < N_CELLS && s_df.state[s_df.src_idx] != CS_FRAG) {
        s_df.src_idx++;
    }
    /* Find next free slot at front (dst) */
    while (s_df.dst_idx < N_CELLS && s_df.state[s_df.dst_idx] != CS_FREE) {
        s_df.dst_idx++;
    }

    if (s_df.src_idx >= N_CELLS || s_df.dst_idx >= s_df.src_idx) {
        /* Done */
        s_df.running = FALSE;
        if (s_df.status_lbl) {
            lv_label_set_text(s_df.status_lbl, "Defragmentation complete.");
        }
        if (s_df.pct_lbl) {
            lv_label_set_text(s_df.pct_lbl, "100%");
        }
        lv_timer_delete(s_df.timer);
        s_df.timer = NULL;
        return;
    }

    /* Flash read/write state */
    s_df.state[s_df.src_idx] = CS_READ;
    __cell_paint(s_df.src_idx);
    s_df.state[s_df.dst_idx] = CS_WRITE;
    __cell_paint(s_df.dst_idx);
    s_df.prev_src = s_df.src_idx;
    s_df.prev_dst = s_df.dst_idx;

    s_df.src_idx++;
    s_df.dst_idx++;
    s_df.moved++;

    /* Update labels */
    if (s_df.total_frag > 0 && s_df.pct_lbl) {
        INT32_T pct = (s_df.moved * 100) / s_df.total_frag;
        if (pct > 100) pct = 100;
        CHAR_T p[8];
        snprintf(p, sizeof(p), "%d%%", (int)pct);
        lv_label_set_text(s_df.pct_lbl, p);
    }
}

/* ---------------------------------------------------------------------------
 * Close button
 * --------------------------------------------------------------------------- */
STATIC VOID_T __close_defrag_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_df.timer) {
        lv_timer_delete(s_df.timer);
        s_df.timer = NULL;
    }
    if (s_df.win) {
        lv_obj_delete(s_df.win);
    }
    memset(&s_df, 0, sizeof(s_df));
}

/* ---------------------------------------------------------------------------
 * Start / Stop buttons
 * --------------------------------------------------------------------------- */
STATIC VOID_T __start_defrag_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_df.running) return;

    /* Re-initialise a fresh disk state */
    __init_clusters();
    INT32_T i;
    for (i = 0; i < N_CELLS; i++) __cell_paint(i);

    s_df.src_idx  = 0;
    s_df.dst_idx  = 0;
    s_df.prev_src = -1;
    s_df.prev_dst = -1;
    s_df.running  = TRUE;

    if (s_df.status_lbl) {
        lv_label_set_text(s_df.status_lbl, "Defragmenting drive C: ...");
    }
    if (s_df.pct_lbl) {
        lv_label_set_text(s_df.pct_lbl, "0%");
    }

    if (s_df.timer) {
        lv_timer_delete(s_df.timer);
    }
    s_df.timer = lv_timer_create(__defrag_tick, 40, NULL);
}

STATIC VOID_T __stop_defrag_cb(lv_event_t *e)
{
    (VOID_T)e;
    s_df.running = FALSE;
    if (s_df.timer) {
        lv_timer_delete(s_df.timer);
        s_df.timer = NULL;
    }
    if (s_df.status_lbl) {
        lv_label_set_text(s_df.status_lbl, "Defragmentation stopped.");
    }
}

/* ---------------------------------------------------------------------------
 * Build legend
 * --------------------------------------------------------------------------- */
STATIC VOID_T __legend_cell(lv_obj_t *parent, INT32_T x, INT32_T y,
                              UINT32_T color, CONST CHAR_T *label)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 10, 10);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(lbl, label);
    lv_obj_set_pos(lbl, x + 13, y);
}

/* ---------------------------------------------------------------------------
 * Public: open defragmenter window
 * --------------------------------------------------------------------------- */
VOID_T win95_defrag_open(lv_obj_t *parent)
{
    if (s_df.win) {
        lv_obj_delete(s_df.win);
        memset(&s_df, 0, sizeof(s_df));
    }

    /* Window */
    lv_obj_t *w = lv_obj_create(parent);
    lv_obj_remove_style_all(w);
    lv_obj_set_size(w, 472, 310);
    lv_obj_align(w, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(w, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(w, 0, 0);
    lv_obj_set_style_border_color(w, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(w, 2, 0);
    lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    s_df.win = w;

    /* Title bar */
    lv_obj_t *tb = lv_obj_create(w);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, 468, WIN95_WINDOW_TITLE_H);
    lv_obj_set_pos(tb, 2, 2);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(tb);
    lv_obj_set_style_text_color(tl, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_unscii_8, 0);
    lv_label_set_text(tl, "Disk Defragmenter - Drive C: (SD Card)");
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 4, 0);

    /* Close button */
    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 14, 14);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_add_event_cb(xb, __close_defrag_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "x");
    lv_obj_align(xl, LV_ALIGN_CENTER, 0, 0);

    /* Content area — sunken border */
    lv_obj_t *ca = lv_obj_create(w);
    lv_obj_remove_style_all(ca);
    lv_obj_set_size(ca, 464, 200);
    lv_obj_set_pos(ca, 4, WIN95_WINDOW_TITLE_H + 4);
    lv_obj_set_style_bg_color(ca, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_bg_opa(ca, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ca, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_border_width(ca, 1, 0);
    lv_obj_clear_flag(ca, LV_OBJ_FLAG_SCROLLABLE);

    /* Build cluster grid */
    __init_clusters();
    INT32_T i;
    for (i = 0; i < N_CELLS; i++) {
        INT32_T col = i % GRID_COLS;
        INT32_T row = i / GRID_COLS;
        INT32_T cx = GRID_X + col * (CELL_SZ + CELL_GAP);
        INT32_T cy = GRID_Y + row * (CELL_SZ + CELL_GAP);

        lv_obj_t *c = lv_obj_create(ca);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, CELL_SZ, CELL_SZ);
        lv_obj_set_pos(c, cx, cy);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        s_df.cells[i] = c;
        __cell_paint(i);
    }

    /* Legend row */
    INT32_T legend_y = WIN95_WINDOW_TITLE_H + 210;
    __legend_cell(w, 4,   legend_y, 0xFFFFFF, "Free");
    __legend_cell(w, 60,  legend_y, 0x0000CC, "Optimized");
    __legend_cell(w, 140, legend_y, 0xFF4444, "Fragmented");
    __legend_cell(w, 224, legend_y, 0x00FFCC, "Reading");
    __legend_cell(w, 296, legend_y, 0xFFEE00, "Writing");

    /* Status label */
    s_df.status_lbl = lv_label_create(w);
    lv_obj_set_style_text_color(s_df.status_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_df.status_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_df.status_lbl, "Press Start to begin defragmentation.");
    lv_obj_set_pos(s_df.status_lbl, 4, legend_y + 14);

    /* Percent label */
    s_df.pct_lbl = lv_label_create(w);
    lv_obj_set_style_text_color(s_df.pct_lbl, lv_color_hex(WIN95_COLOR_TITLEBAR), 0);
    lv_obj_set_style_text_font(s_df.pct_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_df.pct_lbl, "");
    lv_obj_set_pos(s_df.pct_lbl, 420, legend_y + 14);

    /* Buttons */
    INT32_T btn_y = legend_y + 28;
    lv_obj_t *sbtn = lv_btn_create(w);
    lv_obj_set_size(sbtn, 60, 18);
    lv_obj_set_pos(sbtn, 4, btn_y);
    lv_obj_set_style_bg_color(sbtn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(sbtn, 0, 0);
    lv_obj_set_style_pad_all(sbtn, 0, 0);
    lv_obj_set_style_border_color(sbtn, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(sbtn, 1, 0);
    lv_obj_set_style_shadow_color(sbtn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(sbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_x(sbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_y(sbtn, 1, 0);
    lv_obj_add_event_cb(sbtn, __start_defrag_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl2 = lv_label_create(sbtn);
    lv_obj_set_style_text_color(sl2, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(sl2, &lv_font_unscii_8, 0);
    lv_label_set_text(sl2, "Start");
    lv_obj_align(sl2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *stpbtn = lv_btn_create(w);
    lv_obj_set_size(stpbtn, 60, 18);
    lv_obj_set_pos(stpbtn, 70, btn_y);
    lv_obj_set_style_bg_color(stpbtn, lv_color_hex(WIN95_COLOR_WINDOW), 0);
    lv_obj_set_style_radius(stpbtn, 0, 0);
    lv_obj_set_style_pad_all(stpbtn, 0, 0);
    lv_obj_set_style_border_color(stpbtn, lv_color_hex(WIN95_COLOR_LIGHT), 0);
    lv_obj_set_style_border_width(stpbtn, 1, 0);
    lv_obj_set_style_shadow_color(stpbtn, lv_color_hex(WIN95_COLOR_SHADOW), 0);
    lv_obj_set_style_shadow_width(stpbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_x(stpbtn, 1, 0);
    lv_obj_set_style_shadow_ofs_y(stpbtn, 1, 0);
    lv_obj_add_event_cb(stpbtn, __stop_defrag_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stpl = lv_label_create(stpbtn);
    lv_obj_set_style_text_color(stpl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(stpl, &lv_font_unscii_8, 0);
    lv_label_set_text(stpl, "Stop");
    lv_obj_align(stpl, LV_ALIGN_CENTER, 0, 0);

    s_df.prev_src = -1;
    s_df.prev_dst = -1;
}
