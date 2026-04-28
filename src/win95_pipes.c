/**
 * @file win95_pipes.c
 * @brief Win95-style 3D Pipes screensaver and embedded preview renderer.
 */
#include "win95_pipes.h"
#include "bios_simulator.h"
#include "win95_cursor.h"
#include "tal_api.h"
#include "lv_vendor.h"

#include <stdint.h>
#include <string.h>

#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
#define PIPES_MEM_ALLOC  tal_psram_malloc
#define PIPES_MEM_FREE   tal_psram_free
#else
#define PIPES_MEM_ALLOC  tal_malloc
#define PIPES_MEM_FREE   tal_free
#endif

/* ---------------------------------------------------------------------------
 * Scene constants
 * --------------------------------------------------------------------------- */
#define GRID_X           8
#define GRID_Y           6
#define GRID_Z           5

#define N_PIPES          3
#define RESET_TICKS      960
#define MAX_PIPE_LEN     56
#define EDGE_RESET_LIMIT 156
#define ANIM_PERIOD_MS   96
#define DEPTH_SCALE      2048
#define ELBOW_RADIUS     0.36f
#define FP_BITS          12
#define FP_ONE           (1 << FP_BITS)
#define FP_HALF          (FP_ONE >> 1)

#define ROT_Y_COS_Q      3355
#define ROT_Y_SIN_Q      2349
#define ROT_X_COS_Q      3712
#define ROT_X_SIN_Q      1731

/* ---------------------------------------------------------------------------
 * Pipe palette
 * --------------------------------------------------------------------------- */
STATIC CONST UINT32_T s_palette[] = {
    0xFF3030, 0x30FF5A, 0x3094FF, 0xFFE040,
    0xFF58FF, 0x40F4FF, 0xFF9A30, 0xF4F4F4,
};
#define PALETTE_N ((INT32_T)(sizeof(s_palette) / sizeof(s_palette[0])))

STATIC CONST UINT16_T s_arc_sin_q8[] = {
      0,  40,  79, 116, 150, 181, 207, 228, 243, 253, 256
};
STATIC CONST UINT16_T s_arc_cos_q8[] = {
    256, 253, 243, 228, 207, 181, 150, 116,  79,  40,   0
};
#define ARC_SAMPLE_N ((INT32_T)(sizeof(s_arc_sin_q8) / sizeof(s_arc_sin_q8[0])))

/* ---------------------------------------------------------------------------
 * Pipe state
 * --------------------------------------------------------------------------- */
typedef struct {
    INT8_T   x;
    INT8_T   y;
    INT8_T   z;
} PIPE_NODE_T;

typedef struct {
    INT8_T   x;
    INT8_T   y;
    INT8_T   z;
    UINT8_T  dir;
    UINT8_T  color_idx;
    UINT16_T len;
    UINT8_T  run_goal;
    UINT8_T  warmup;
    UINT8_T  retreat_steps;
    UINT8_T  path_len;
    UINT8_T  run_start_idx;
    PIPE_NODE_T path[MAX_PIPE_LEN + 1];
} PIPE_T;

typedef struct {
    lv_obj_t   *parent;
    lv_obj_t   *host;
    lv_obj_t   *return_screen;
    lv_obj_t   *canvas;
    VOID_T     *buf;
    uint16_t   *zbuf;
    lv_timer_t *anim_timer;
    PIPE_T      pipes[N_PIPES];
    UINT8_T     used_node[GRID_X][GRID_Y][GRID_Z];
    UINT8_T     used_x[GRID_X - 1][GRID_Y][GRID_Z];
    UINT8_T     used_y[GRID_X][GRID_Y - 1][GRID_Z];
    UINT8_T     used_z[GRID_X][GRID_Y][GRID_Z - 1];
    INT32_T     width;
    INT32_T     height;
    INT32_T     tick_count;
    INT32_T     edge_count;
    INT32_T     dirty_x1;
    INT32_T     dirty_y1;
    INT32_T     dirty_x2;
    INT32_T     dirty_y2;
    INT32_T     depth_span;
    float       focal;
    float       cam_dist;
    float       world_radius;
    float       joint_radius;
    float       scene_cx;
    float       scene_cy;
    float       scene_cz;
    float       screen_cx;
    float       screen_cy;
    INT32_T     focal_q;
    INT32_T     cam_dist_q;
    INT32_T     world_radius_q;
    INT32_T     joint_radius_q;
    INT32_T     rot_y_cos_q;
    INT32_T     rot_y_sin_q;
    INT32_T     rot_x_cos_q;
    INT32_T     rot_x_sin_q;
    INT32_T     scene_cx_q;
    INT32_T     scene_cy_q;
    INT32_T     scene_cz_q;
    INT32_T     screen_cx_q;
    INT32_T     screen_cy_q;
    INT32_T     anim_pipe_idx;
    uint32_t    start_tick_ms;
    BOOL_T      active;
    BOOL_T      stop_on_click;
    BOOL_T      host_is_screen;
    BOOL_T      dirty_full;
} PIPES_CTX_T;

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------- */
STATIC PIPES_CTX_T s_saver;
STATIC PIPES_CTX_T s_preview;
STATIC UINT32_T    s_seed        = 0x1234ABCDu;
STATIC UINT32_T    s_idle_sec    = 0;
STATIC UINT32_T    s_timeout_sec = 0;

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
STATIC INT32_T __imin(INT32_T a, INT32_T b) { return (a < b) ? a : b; }
STATIC INT32_T __imax(INT32_T a, INT32_T b) { return (a > b) ? a : b; }
STATIC INT32_T __iabs(INT32_T a) { return (a < 0) ? -a : a; }

STATIC INT32_T __rand_n(INT32_T n)
{
    if (n <= 1) return 0;
    s_seed = s_seed * 1664525u + 1013904223u;
    return (INT32_T)((s_seed >> 8) & 0x7FFFFFu) % n;
}

STATIC uint16_t __rgb565(UINT32_T hex)
{
    UINT8_T r = (UINT8_T)((hex >> 16) & 0xFF);
    UINT8_T g = (UINT8_T)((hex >>  8) & 0xFF);
    UINT8_T b = (UINT8_T)( hex        & 0xFF);
    return (uint16_t)(((UINT16_T)(r >> 3) << 11) |
                      ((UINT16_T)(g >> 2) <<  5) |
                       (UINT16_T)(b >> 3));
}

STATIC uint16_t __scale_rgb565(UINT32_T hex, INT32_T intensity)
{
    UINT32_T r;
    UINT32_T g;
    UINT32_T b;

    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    r = (((hex >> 16) & 0xFFu) * (UINT32_T)intensity) / 255u;
    g = (((hex >>  8) & 0xFFu) * (UINT32_T)intensity) / 255u;
    b = (( hex        & 0xFFu) * (UINT32_T)intensity) / 255u;
    return __rgb565((r << 16) | (g << 8) | b);
}

STATIC uint16_t __shade_rgb565(UINT32_T hex,
                               INT32_T nx256, INT32_T ny256, INT32_T nz256,
                               INT32_T fog)
{
    INT32_T base_r = (INT32_T)((hex >> 16) & 0xFFu);
    INT32_T base_g = (INT32_T)((hex >>  8) & 0xFFu);
    INT32_T base_b = (INT32_T)( hex        & 0xFFu);
    INT32_T diff;
    INT32_T half_dot;
    INT32_T spec;
    INT32_T shadow_side;
    INT32_T shade;
    INT32_T highlight;
    INT32_T cool_fill;
    INT32_T r;
    INT32_T g;
    INT32_T b;

    diff = ((nx256 * -72) + (ny256 * -116) + (nz256 * 224)) >> 8;
    if (diff < 0) diff = 0;
    if (diff > 255) diff = 255;

    half_dot = ((nx256 * -36) + (ny256 * -52) + (nz256 * 248)) >> 8;
    if (half_dot < 0) half_dot = 0;
    if (half_dot > 255) half_dot = 255;

    spec = (half_dot * half_dot) >> 8;
    spec = (spec * spec) >> 8;

    shadow_side = ((nx256 * 88) + (ny256 * 144) - (nz256 * 36)) >> 8;
    if (shadow_side < 0) shadow_side = 0;
    if (shadow_side > 255) shadow_side = 255;

    shade = 52 + ((diff * 176) / 255);
    shade -= (shadow_side * 92) / 255;
    shade -= fog;
    if (shade < 8) shade = 8;
    if (shade > 255) shade = 255;

    highlight = 12 + ((spec * 228) / 255);
    cool_fill = __imax(0, 22 - fog / 2);

    r = ((base_r * shade) + (255 * highlight)) / 255;
    g = ((base_g * shade) + (244 * highlight) + (cool_fill * 8)) / 255;
    b = ((base_b * shade) + (224 * highlight) + (cool_fill * 20)) / 255;

    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    return __rgb565(((UINT32_T)r << 16) | ((UINT32_T)g << 8) | (UINT32_T)b);
}

STATIC UINT32_T __isqrt_u32(UINT32_T n)
{
    UINT32_T res = 0;
    UINT32_T bit = 1uL << 30;

    while (bit > n) bit >>= 2;

    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

STATIC INT32_T __round_f(float v)
{
    return (INT32_T)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

STATIC INT32_T __fp_from_float(float v)
{
    return __round_f(v * (float)FP_ONE);
}

STATIC INT32_T __fp_round(INT32_T v)
{
    if (v >= 0) return (v + FP_HALF) >> FP_BITS;
    return -(((-v) + FP_HALF) >> FP_BITS);
}

STATIC INT32_T __fp_mul(INT32_T a, INT32_T b)
{
    return (INT32_T)(((int64_t)a * (int64_t)b) >> FP_BITS);
}

STATIC VOID_T __dirty_reset(PIPES_CTX_T *ctx);
STATIC VOID_T __dirty_mark(PIPES_CTX_T *ctx, INT32_T x, INT32_T y);
STATIC VOID_T __dirty_mark_full(PIPES_CTX_T *ctx);
STATIC VOID_T __dirty_invalidate(PIPES_CTX_T *ctx);

STATIC INT32_T __grid_radial_score(INT8_T x, INT8_T y)
{
    return __iabs(((INT32_T)x << 1) - (GRID_X - 1)) +
           __iabs(((INT32_T)y << 1) - (GRID_Y - 1));
}

STATIC BOOL_T __node_used(PIPES_CTX_T *ctx, INT8_T x, INT8_T y, INT8_T z)
{
    return ctx->used_node[x][y][z] != 0;
}

STATIC INT32_T __depth_focus_bonus(PIPES_CTX_T *ctx,
                                   INT32_T depth_q,
                                   INT32_T target_offset_q,
                                   INT32_T spread_q,
                                   INT32_T max_bonus)
{
    INT32_T diff;

    if (!ctx || spread_q <= 0 || max_bonus <= 0) return 0;

    diff = __iabs(depth_q - (ctx->cam_dist_q + target_offset_q));
    if (diff >= spread_q) return 0;

    return (max_bonus * (spread_q - diff)) / spread_q;
}

STATIC VOID_T __mark_node(PIPES_CTX_T *ctx, INT8_T x, INT8_T y, INT8_T z)
{
    ctx->used_node[x][y][z] = 1;
}

STATIC INT32_T __node_crowd(PIPES_CTX_T *ctx, INT8_T x, INT8_T y, INT8_T z)
{
    INT32_T crowd = 0;

    for (INT32_T zz = __imax(0, z - 1); zz <= __imin(GRID_Z - 1, z + 1); zz++) {
        for (INT32_T yy = __imax(0, y - 1); yy <= __imin(GRID_Y - 1, y + 1); yy++) {
            for (INT32_T xx = __imax(0, x - 1); xx <= __imin(GRID_X - 1, x + 1); xx++) {
                if (xx == x && yy == y && zz == z) continue;
                crowd += ctx->used_node[xx][yy][zz] ? 1 : 0;
            }
        }
    }

    return crowd;
}

STATIC VOID_T __dir_vec(UINT8_T dir, float *vx, float *vy, float *vz)
{
    *vx = 0.0f;
    *vy = 0.0f;
    *vz = 0.0f;

    switch (dir) {
        case 0: *vx =  1.0f; break;
        case 1: *vx = -1.0f; break;
        case 2: *vy =  1.0f; break;
        case 3: *vy = -1.0f; break;
        case 4: *vz =  1.0f; break;
        case 5: *vz = -1.0f; break;
        default: break;
    }
}

STATIC UINT8_T __dir_between_nodes(CONST PIPE_NODE_T *a, CONST PIPE_NODE_T *b)
{
    if (b->x > a->x) return 0;
    if (b->x < a->x) return 1;
    if (b->y > a->y) return 2;
    if (b->y < a->y) return 3;
    if (b->z > a->z) return 4;
    return 5;
}

STATIC VOID_T __pipe_store_node(PIPE_T *p, INT8_T x, INT8_T y, INT8_T z)
{
    if (!p || p->path_len >= (MAX_PIPE_LEN + 1)) return;

    p->path[p->path_len].x = x;
    p->path[p->path_len].y = y;
    p->path[p->path_len].z = z;
    p->path_len++;
}

STATIC VOID_T __select_camera_preset(PIPES_CTX_T *ctx)
{
    if (!ctx) return;

    ctx->rot_y_cos_q = ROT_Y_COS_Q;
    ctx->rot_y_sin_q = ROT_Y_SIN_Q;
    ctx->rot_x_cos_q = ROT_X_COS_Q;
    ctx->rot_x_sin_q = ROT_X_SIN_Q;
}

STATIC INT32_T __camera_depth_grid_q(PIPES_CTX_T *ctx, INT32_T gx, INT32_T gy, INT32_T gz)
{
    INT32_T x0_q = (gx << FP_BITS) - ctx->scene_cx_q;
    INT32_T y0_q = (gy << FP_BITS) - ctx->scene_cy_q;
    INT32_T z0_q = (gz << FP_BITS) - ctx->scene_cz_q;
    INT32_T z1_q = __fp_mul(x0_q, ctx->rot_y_sin_q) + __fp_mul(z0_q, ctx->rot_y_cos_q);

    return __fp_mul(y0_q, ctx->rot_x_sin_q) + __fp_mul(z1_q, ctx->rot_x_cos_q) + ctx->cam_dist_q;
}

STATIC VOID_T __scene_layout(PIPES_CTX_T *ctx)
{
    ctx->scene_cx     = ((float)(GRID_X - 1)) * 0.5f;
    ctx->scene_cy     = ((float)(GRID_Y - 1)) * 0.5f;
    ctx->scene_cz     = ((float)(GRID_Z - 1)) * 0.5f;
    ctx->screen_cx    = ((float)ctx->width) * 0.5f;
    ctx->screen_cy    = ((float)ctx->height) * 0.55f;
    ctx->focal        = ((float)ctx->width) * 1.89f;
    ctx->cam_dist     = 17.4f;
    ctx->world_radius = 0.17f;
    ctx->joint_radius = 0.20f;
    ctx->depth_span   = (INT32_T)(ctx->world_radius * (float)DEPTH_SCALE);
    ctx->focal_q      = __fp_from_float(ctx->focal);
    ctx->cam_dist_q   = __fp_from_float(ctx->cam_dist);
    ctx->world_radius_q = __fp_from_float(ctx->world_radius);
    ctx->joint_radius_q = __fp_from_float(ctx->joint_radius);
    ctx->scene_cx_q   = __fp_from_float(ctx->scene_cx);
    ctx->scene_cy_q   = __fp_from_float(ctx->scene_cy);
    ctx->scene_cz_q   = __fp_from_float(ctx->scene_cz);
    ctx->screen_cx_q  = __fp_from_float(ctx->screen_cx);
    ctx->screen_cy_q  = __fp_from_float(ctx->screen_cy);
}

STATIC VOID_T __clear_scene(PIPES_CTX_T *ctx)
{
    UINT32_T px_count;

    if (!ctx || !ctx->buf || !ctx->zbuf) return;

    px_count = (UINT32_T)(ctx->width * ctx->height);
    memset(ctx->buf, 0, (size_t)(px_count * 2u));
    memset(ctx->zbuf, 0xFF, (size_t)(px_count * sizeof(uint16_t)));
    __dirty_mark_full(ctx);
}

STATIC VOID_T __reset_scene_state(PIPES_CTX_T *ctx)
{
    if (!ctx) return;

    memset(ctx->used_node, 0, sizeof(ctx->used_node));
    memset(ctx->used_x, 0, sizeof(ctx->used_x));
    memset(ctx->used_y, 0, sizeof(ctx->used_y));
    memset(ctx->used_z, 0, sizeof(ctx->used_z));
    ctx->tick_count = 0;
    ctx->edge_count = 0;
    ctx->anim_pipe_idx = 0;
}

STATIC VOID_T __camera_transform(PIPES_CTX_T *ctx,
                                 float wx, float wy, float wz,
                                 float *rx, float *ry, float *rz)
{
    INT32_T wx_q = __fp_from_float(wx);
    INT32_T wy_q = __fp_from_float(wy);
    INT32_T wz_q = __fp_from_float(wz);
    INT32_T x0_q = wx_q - ctx->scene_cx_q;
    INT32_T y0_q = wy_q - ctx->scene_cy_q;
    INT32_T z0_q = wz_q - ctx->scene_cz_q;
    INT32_T x1_q = __fp_mul(x0_q, ctx->rot_y_cos_q) - __fp_mul(z0_q, ctx->rot_y_sin_q);
    INT32_T z1_q = __fp_mul(x0_q, ctx->rot_y_sin_q) + __fp_mul(z0_q, ctx->rot_y_cos_q);
    INT32_T ry_q = __fp_mul(y0_q, ctx->rot_x_cos_q) - __fp_mul(z1_q, ctx->rot_x_sin_q);
    INT32_T rz_q = __fp_mul(y0_q, ctx->rot_x_sin_q) + __fp_mul(z1_q, ctx->rot_x_cos_q) + ctx->cam_dist_q;

    *rx = (float)x1_q / (float)FP_ONE;
    *ry = (float)ry_q / (float)FP_ONE;
    *rz = (float)rz_q / (float)FP_ONE;
}

STATIC BOOL_T __project_point(PIPES_CTX_T *ctx,
                              float wx, float wy, float wz,
                              float *sx, float *sy, float *zcam)
{
    INT32_T wx_q = __fp_from_float(wx);
    INT32_T wy_q = __fp_from_float(wy);
    INT32_T wz_q = __fp_from_float(wz);
    INT32_T x0_q = wx_q - ctx->scene_cx_q;
    INT32_T y0_q = wy_q - ctx->scene_cy_q;
    INT32_T z0_q = wz_q - ctx->scene_cz_q;
    INT32_T x1_q = __fp_mul(x0_q, ctx->rot_y_cos_q) - __fp_mul(z0_q, ctx->rot_y_sin_q);
    INT32_T z1_q = __fp_mul(x0_q, ctx->rot_y_sin_q) + __fp_mul(z0_q, ctx->rot_y_cos_q);
    INT32_T ry_q = __fp_mul(y0_q, ctx->rot_x_cos_q) - __fp_mul(z1_q, ctx->rot_x_sin_q);
    INT32_T rz_q = __fp_mul(y0_q, ctx->rot_x_sin_q) + __fp_mul(z1_q, ctx->rot_x_cos_q) + ctx->cam_dist_q;
    INT32_T sx_q;
    INT32_T sy_q;

    if (rz_q <= (FP_ONE / 5)) return FALSE;

    sx_q = ctx->screen_cx_q + (INT32_T)(((int64_t)x1_q * (int64_t)ctx->focal_q) / (int64_t)rz_q);
    sy_q = ctx->screen_cy_q + (INT32_T)(((int64_t)ry_q * (int64_t)ctx->focal_q) / (int64_t)rz_q);

    *sx   = (float)sx_q / (float)FP_ONE;
    *sy   = (float)sy_q / (float)FP_ONE;
    *zcam = (float)rz_q / (float)FP_ONE;
    return TRUE;
}

STATIC VOID_T __put_pixel_depth(PIPES_CTX_T *ctx,
                                INT32_T x, INT32_T y,
                                uint16_t depth, uint16_t color)
{
    UINT32_T idx;

    if ((UINT32_T)x >= (UINT32_T)ctx->width ||
        (UINT32_T)y >= (UINT32_T)ctx->height) {
        return;
    }

    idx = (UINT32_T)y * (UINT32_T)ctx->width + (UINT32_T)x;
    if (depth <= ctx->zbuf[idx]) {
        ctx->zbuf[idx] = depth;
        ((uint16_t *)ctx->buf)[idx] = color;
        __dirty_mark(ctx, x, y);
    }
}

STATIC INT32_T __projected_radius_px(PIPES_CTX_T *ctx,
                                     float world_radius,
                                     float zcam)
{
    INT32_T zcam_q;
    INT32_T radius_px;

    zcam_q = __fp_from_float(zcam);
    if (zcam_q <= 0) return 1;

    radius_px = __fp_round((INT32_T)(((int64_t)__fp_from_float(world_radius) *
                                      (int64_t)ctx->focal_q) /
                                     (int64_t)zcam_q));
    return __imax(1, radius_px);
}

STATIC VOID_T __dirty_reset(PIPES_CTX_T *ctx)
{
    if (!ctx) return;

    ctx->dirty_x1 = ctx->width;
    ctx->dirty_y1 = ctx->height;
    ctx->dirty_x2 = -1;
    ctx->dirty_y2 = -1;
    ctx->dirty_full = FALSE;
}

STATIC VOID_T __dirty_mark(PIPES_CTX_T *ctx, INT32_T x, INT32_T y)
{
    if (!ctx) return;

    if (x < ctx->dirty_x1) ctx->dirty_x1 = x;
    if (y < ctx->dirty_y1) ctx->dirty_y1 = y;
    if (x > ctx->dirty_x2) ctx->dirty_x2 = x;
    if (y > ctx->dirty_y2) ctx->dirty_y2 = y;
}

STATIC VOID_T __dirty_mark_full(PIPES_CTX_T *ctx)
{
    if (!ctx) return;

    ctx->dirty_full = TRUE;
    ctx->dirty_x1 = 0;
    ctx->dirty_y1 = 0;
    ctx->dirty_x2 = ctx->width - 1;
    ctx->dirty_y2 = ctx->height - 1;
}

STATIC VOID_T __dirty_invalidate(PIPES_CTX_T *ctx)
{
    lv_area_t area;

    if (!ctx || !ctx->canvas || !lv_obj_is_valid(ctx->canvas)) return;

    if (ctx->dirty_full) {
        lv_obj_invalidate(ctx->canvas);
        return;
    }

    if (ctx->dirty_x2 < ctx->dirty_x1 || ctx->dirty_y2 < ctx->dirty_y1) {
        return;
    }

    area.x1 = ctx->canvas->coords.x1 + __imax(0, ctx->dirty_x1 - 2);
    area.y1 = ctx->canvas->coords.y1 + __imax(0, ctx->dirty_y1 - 2);
    area.x2 = ctx->canvas->coords.x1 + __imin(ctx->width - 1, ctx->dirty_x2 + 2);
    area.y2 = ctx->canvas->coords.y1 + __imin(ctx->height - 1, ctx->dirty_y2 + 2);
    lv_obj_invalidate_area(ctx->canvas, &area);
}

STATIC VOID_T __erase_ball(PIPES_CTX_T *ctx,
                           float wx, float wy, float wz,
                           float world_radius)
{
    float sx;
    float sy;
    float zcam;
    INT32_T radius_px;
    INT32_T cx;
    INT32_T cy;

    if (!ctx || !ctx->buf || !ctx->zbuf) return;
    if (!__project_point(ctx, wx, wy, wz, &sx, &sy, &zcam)) return;

    radius_px = __projected_radius_px(ctx, world_radius, zcam) + 3;
    cx = __round_f(sx);
    cy = __round_f(sy);

    for (INT32_T y = cy - radius_px; y <= cy + radius_px; y++) {
        for (INT32_T x = cx - radius_px; x <= cx + radius_px; x++) {
            INT32_T dx = x - cx;
            INT32_T dy = y - cy;
            UINT32_T idx;

            if ((dx * dx) + (dy * dy) > (radius_px * radius_px)) continue;
            if ((UINT32_T)x >= (UINT32_T)ctx->width || (UINT32_T)y >= (UINT32_T)ctx->height) continue;

            idx = (UINT32_T)y * (UINT32_T)ctx->width + (UINT32_T)x;
            ctx->zbuf[idx] = 0xFFFFu;
            ((uint16_t *)ctx->buf)[idx] = 0u;
            __dirty_mark(ctx, x, y);
        }
    }
}

STATIC VOID_T __render_ball(PIPES_CTX_T *ctx,
                            float sx, float sy, float zcam,
                            float world_radius, UINT32_T color_hex,
                            BOOL_T draw_shadow)
{
    INT32_T radius_px;
    INT32_T cx;
    INT32_T cy;
    INT32_T depth_base;
    INT32_T depth_span;
    INT32_T r2;
    INT32_T shadow_radius;
    INT32_T shadow_r2;
    INT32_T shadow_dx;
    INT32_T shadow_dy;
    INT32_T shadow_intensity;

    radius_px = __projected_radius_px(ctx, world_radius, zcam);

    cx = __round_f(sx);
    cy = __round_f(sy);
    depth_base = (INT32_T)(((int64_t)__fp_from_float(zcam) * (int64_t)DEPTH_SCALE) >> FP_BITS);
    depth_span = (INT32_T)(((int64_t)__fp_from_float(world_radius) * (int64_t)DEPTH_SCALE) >> FP_BITS);
    r2 = radius_px * radius_px;
    if (draw_shadow) {
        shadow_radius = radius_px + __imax(1, radius_px / 2);
        shadow_r2 = shadow_radius * shadow_radius;
        shadow_dx = __imax(1, radius_px / 2);
        shadow_dy = __imax(1, radius_px / 3);
        shadow_intensity = 14 + __imax(0, 28 - __round_f((zcam - ctx->cam_dist) * 8.0f));
        if (shadow_intensity > 52) shadow_intensity = 52;

        for (INT32_T dy = -shadow_radius; dy <= shadow_radius; dy++) {
            for (INT32_T dx = -shadow_radius; dx <= shadow_radius; dx++) {
                INT32_T dist2 = dx * dx + dy * dy;
                INT32_T edge;
                uint16_t depth;

                if (dist2 > shadow_r2) continue;

                edge = 255 - ((dist2 * 255) / shadow_r2);
                if (edge < 24) continue;

                depth = (uint16_t)__imax(0, depth_base + depth_span + 24 + ((255 - edge) >> 3));
                __put_pixel_depth(ctx, cx + dx + shadow_dx, cy + dy + shadow_dy, depth,
                                  __scale_rgb565(0x10203A, (shadow_intensity * edge) / 255));
            }
        }
    }

    for (INT32_T dy = -radius_px; dy <= radius_px; dy++) {
        for (INT32_T dx = -radius_px; dx <= radius_px; dx++) {
            INT32_T dist2 = dx * dx + dy * dy;
            UINT32_T nz;
            INT32_T nx256;
            INT32_T ny256;
            INT32_T fog;
            uint16_t depth;
            uint16_t shaded;

            if (dist2 > r2) continue;

            nz = __isqrt_u32((UINT32_T)((r2 - dist2) * 65536u / (UINT32_T)r2));
            nx256 = (dx * 256) / radius_px;
            ny256 = (dy * 256) / radius_px;
            fog = __imax(0, __round_f((zcam - ctx->cam_dist) * 11.0f));

            depth = (uint16_t)__imax(0, depth_base - (INT32_T)((nz * (UINT32_T)depth_span) >> 8));
            shaded = __shade_rgb565(color_hex, nx256, ny256, (INT32_T)nz, fog);
            __put_pixel_depth(ctx, cx + dx, cy + dy, depth, shaded);
        }
    }
}

STATIC VOID_T __render_world_ball(PIPES_CTX_T *ctx,
                                  float wx, float wy, float wz,
                                  float world_radius, UINT32_T color_hex,
                                  BOOL_T draw_shadow)
{
    float sx;
    float sy;
    float zcam;

    if (!__project_point(ctx, wx, wy, wz, &sx, &sy, &zcam)) return;
    __render_ball(ctx, sx, sy, zcam, world_radius, color_hex, draw_shadow);
}

STATIC VOID_T __render_joint(PIPES_CTX_T *ctx,
                             INT32_T gx, INT32_T gy, INT32_T gz,
                             UINT32_T color_hex)
{
    __render_world_ball(ctx, (float)gx, (float)gy, (float)gz,
                        ctx->joint_radius, color_hex, TRUE);
}

STATIC VOID_T __render_segment(PIPES_CTX_T *ctx,
                               float x1, float y1, float z1,
                               float x2, float y2, float z2,
                               UINT32_T color_hex)
{
    float sx1;
    float sy1;
    float zc1;
    float sx2;
    float sy2;
    float zc2;
    float dx;
    float dy;
    float len2;
    INT32_T r1;
    INT32_T r2;
    float min_fx;
    float max_fx;
    float min_fy;
    float max_fy;
    INT32_T min_x;
    INT32_T max_x;
    INT32_T min_y;
    INT32_T max_y;

    if (!__project_point(ctx, x1, y1, z1, &sx1, &sy1, &zc1)) return;
    if (!__project_point(ctx, x2, y2, z2, &sx2, &sy2, &zc2)) return;

    r1 = __projected_radius_px(ctx, ctx->world_radius, zc1);
    r2 = __projected_radius_px(ctx, ctx->world_radius, zc2);
    dx = sx2 - sx1;
    dy = sy2 - sy1;
    len2 = dx * dx + dy * dy;

    if (len2 < 0.25f) {
        __render_ball(ctx, (sx1 + sx2) * 0.5f, (sy1 + sy2) * 0.5f,
                      (zc1 + zc2) * 0.5f, ctx->world_radius, color_hex, FALSE);
        return;
    }

    min_fx = ((sx1 - (float)r1) < (sx2 - (float)r2)) ? (sx1 - (float)r1) : (sx2 - (float)r2);
    max_fx = ((sx1 + (float)r1) > (sx2 + (float)r2)) ? (sx1 + (float)r1) : (sx2 + (float)r2);
    min_fy = ((sy1 - (float)r1) < (sy2 - (float)r2)) ? (sy1 - (float)r1) : (sy2 - (float)r2);
    max_fy = ((sy1 + (float)r1) > (sy2 + (float)r2)) ? (sy1 + (float)r1) : (sy2 + (float)r2);

    min_x = __imax(0, __round_f(min_fx) - 1);
    max_x = __imin(ctx->width - 1, __round_f(max_fx) + 1);
    min_y = __imax(0, __round_f(min_fy) - 1);
    max_y = __imin(ctx->height - 1, __round_f(max_fy) + 1);

    for (INT32_T y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;

        for (INT32_T x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;
            float t = (((px - sx1) * dx) + ((py - sy1) * dy)) / len2;
            float cx;
            float cy;
            float radius;
            INT32_T radius_q;
            INT32_T offx_q;
            INT32_T offy_q;
            UINT32_T dist2_q;
            UINT32_T radius2_q;
            INT32_T nx256;
            INT32_T ny256;
            UINT32_T nz256;
            float zcam;
            INT32_T fog;
            INT32_T depth_base;
            uint16_t depth;
            uint16_t shaded;

            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;

            cx = sx1 + (dx * t);
            cy = sy1 + (dy * t);
            radius = (float)r1 + ((float)(r2 - r1) * t);
            if (radius < 1.0f) radius = 1.0f;

            radius_q = __imax(256, __round_f(radius * 256.0f));
            offx_q = __round_f((px - cx) * 256.0f);
            offy_q = __round_f((py - cy) * 256.0f);
            dist2_q = (UINT32_T)((offx_q * offx_q) + (offy_q * offy_q));
            radius2_q = (UINT32_T)(radius_q * radius_q);
            if (dist2_q > radius2_q) continue;

            nx256 = (offx_q * 256) / radius_q;
            ny256 = (offy_q * 256) / radius_q;
            nz256 = __isqrt_u32(((radius2_q - dist2_q) * 65536u) / radius2_q);

            zcam = zc1 + ((zc2 - zc1) * t);
            fog = __imax(0, __round_f((zcam - ctx->cam_dist) * 11.0f));
            depth_base = (INT32_T)(((int64_t)__fp_from_float(zcam) * (int64_t)DEPTH_SCALE) >> FP_BITS);
            depth = (uint16_t)__imax(0, depth_base - (INT32_T)((nz256 * (UINT32_T)ctx->depth_span) >> 8));
            shaded = __shade_rgb565(color_hex, nx256, ny256, (INT32_T)nz256, fog);
            __put_pixel_depth(ctx, x, y, depth, shaded);
        }
    }
}

STATIC VOID_T __render_elbow(PIPES_CTX_T *ctx,
                             INT32_T gx, INT32_T gy, INT32_T gz,
                             UINT8_T in_dir, UINT8_T out_dir,
                             UINT32_T color_hex)
{
    float sx;
    float sy;
    float zcam;
    float inx;
    float iny;
    float inz;
    float outx;
    float outy;
    float outz;
    INT32_T radius_px;
    float center_scale;
    float side_scale;
    float side_offset;

    if (((in_dir >> 1) == (out_dir >> 1)) || ((in_dir ^ out_dir) == 1u)) {
        return;
    }
    if (!__project_point(ctx, (float)gx, (float)gy, (float)gz, &sx, &sy, &zcam)) {
        return;
    }

    __dir_vec(in_dir, &inx, &iny, &inz);
    __dir_vec(out_dir, &outx, &outy, &outz);
    radius_px = __projected_radius_px(ctx, ctx->world_radius, zcam);

    if (radius_px <= 3) {
        __render_ball(ctx, sx, sy, zcam, ctx->world_radius * 1.34f, color_hex, FALSE);
        return;
    }

    center_scale = (radius_px >= 8) ? 1.46f : 1.54f;
    side_scale = (radius_px >= 8) ? 0.98f : 1.06f;
    side_offset = (radius_px >= 8) ? 0.12f : 0.14f;

    /* Use a compact bulb blend at the corner instead of many tiny arc
     * segments. This removes the remaining fish-scale ridges on bends. */
    __render_world_ball(ctx, (float)gx, (float)gy, (float)gz,
                        ctx->world_radius * center_scale, color_hex, FALSE);
    __render_world_ball(ctx,
                        (float)gx - (inx * side_offset),
                        (float)gy - (iny * side_offset),
                        (float)gz - (inz * side_offset),
                        ctx->world_radius * side_scale, color_hex, FALSE);
    __render_world_ball(ctx,
                        (float)gx + (outx * side_offset),
                        (float)gy + (outy * side_offset),
                        (float)gz + (outz * side_offset),
                        ctx->world_radius * side_scale, color_hex, FALSE);
}

STATIC VOID_T __render_pipe_path(PIPES_CTX_T *ctx, PIPE_T *p)
{
    INT32_T last_idx;
    INT32_T run_start;
    UINT8_T prev_dir;
    UINT32_T color_hex;

    if (!ctx || !p || p->path_len == 0) return;

    color_hex = s_palette[p->color_idx];
    __render_joint(ctx, p->path[0].x, p->path[0].y, p->path[0].z, color_hex);

    if (p->path_len == 1) {
        return;
    }

    last_idx = (INT32_T)p->path_len - 1;
    run_start = 0;
    prev_dir = __dir_between_nodes(&p->path[0], &p->path[1]);

    for (INT32_T i = 1; i < last_idx; i++) {
        UINT8_T dir = __dir_between_nodes(&p->path[i], &p->path[i + 1]);

        if (dir != prev_dir) {
            __render_segment(ctx,
                             (float)p->path[run_start].x,
                             (float)p->path[run_start].y,
                             (float)p->path[run_start].z,
                             (float)p->path[i].x,
                             (float)p->path[i].y,
                             (float)p->path[i].z,
                             color_hex);
            __render_elbow(ctx, p->path[i].x, p->path[i].y, p->path[i].z,
                           prev_dir, dir, color_hex);
            run_start = i;
            prev_dir = dir;
        }
    }

    __render_segment(ctx,
                     (float)p->path[run_start].x,
                     (float)p->path[run_start].y,
                     (float)p->path[run_start].z,
                     (float)p->path[last_idx].x,
                     (float)p->path[last_idx].y,
                     (float)p->path[last_idx].z,
                     color_hex);
    __render_joint(ctx, p->path[last_idx].x, p->path[last_idx].y, p->path[last_idx].z,
                   color_hex);
}

STATIC BOOL_T __dir_target(INT8_T x, INT8_T y, INT8_T z,
                           UINT8_T dir, INT8_T *nx, INT8_T *ny, INT8_T *nz)
{
    *nx = x;
    *ny = y;
    *nz = z;

    switch (dir) {
        case 0: (*nx)++; break;
        case 1: (*nx)--; break;
        case 2: (*ny)++; break;
        case 3: (*ny)--; break;
        case 4: (*nz)++; break;
        case 5: (*nz)--; break;
        default: return FALSE;
    }

    return (*nx >= 0 && *nx < GRID_X &&
            *ny >= 0 && *ny < GRID_Y &&
            *nz >= 0 && *nz < GRID_Z);
}

STATIC BOOL_T __edge_used(PIPES_CTX_T *ctx, INT8_T x, INT8_T y, INT8_T z, UINT8_T dir)
{
    INT8_T nx;
    INT8_T ny;
    INT8_T nz;

    if (!__dir_target(x, y, z, dir, &nx, &ny, &nz)) return TRUE;

    switch (dir) {
        case 0: return ctx->used_x[x][y][z] != 0;
        case 1: return ctx->used_x[x - 1][y][z] != 0;
        case 2: return ctx->used_y[x][y][z] != 0;
        case 3: return ctx->used_y[x][y - 1][z] != 0;
        case 4: return ctx->used_z[x][y][z] != 0;
        case 5: return ctx->used_z[x][y][z - 1] != 0;
        default: return TRUE;
    }
}

STATIC VOID_T __edge_mark(PIPES_CTX_T *ctx, INT8_T x, INT8_T y, INT8_T z, UINT8_T dir)
{
    switch (dir) {
        case 0:
            if (ctx->used_x[x][y][z] == 0) ctx->edge_count++;
            ctx->used_x[x][y][z] = 1;
            break;
        case 1:
            if (ctx->used_x[x - 1][y][z] == 0) ctx->edge_count++;
            ctx->used_x[x - 1][y][z] = 1;
            break;
        case 2:
            if (ctx->used_y[x][y][z] == 0) ctx->edge_count++;
            ctx->used_y[x][y][z] = 1;
            break;
        case 3:
            if (ctx->used_y[x][y - 1][z] == 0) ctx->edge_count++;
            ctx->used_y[x][y - 1][z] = 1;
            break;
        case 4:
            if (ctx->used_z[x][y][z] == 0) ctx->edge_count++;
            ctx->used_z[x][y][z] = 1;
            break;
        case 5:
            if (ctx->used_z[x][y][z - 1] == 0) ctx->edge_count++;
            ctx->used_z[x][y][z - 1] = 1;
            break;
        default: break;
    }
}

STATIC INT32_T __collect_dirs(PIPES_CTX_T *ctx,
                              INT8_T x, INT8_T y, INT8_T z,
                              INT32_T forbid_dir, UINT8_T out[6])
{
    INT32_T n = 0;

    for (INT32_T d = 0; d < 6; d++) {
        INT8_T nx;
        INT8_T ny;
        INT8_T nz;

        if (d == forbid_dir) continue;
        if (!__dir_target(x, y, z, (UINT8_T)d, &nx, &ny, &nz)) continue;
        if (__node_used(ctx, nx, ny, nz)) continue;
        if (!__edge_used(ctx, x, y, z, (UINT8_T)d)) {
            out[n++] = (UINT8_T)d;
        }
    }
    return n;
}

STATIC INT32_T __score_dir(PIPES_CTX_T *ctx,
                           PIPE_T *p,
                           UINT8_T dir,
                           INT8_T nx, INT8_T ny, INT8_T nz)
{
    UINT8_T next_dirs[6];
    INT32_T exits;
    INT32_T score = 28;
    INT32_T depth_now = __camera_depth_grid_q(ctx, p->x, p->y, p->z);
    INT32_T depth_next = __camera_depth_grid_q(ctx, nx, ny, nz);
    INT32_T depth_delta = depth_next - depth_now;
    INT32_T radial_now = __grid_radial_score(p->x, p->y);
    INT32_T radial_next = __grid_radial_score(nx, ny);
    INT32_T radial_delta = radial_next - radial_now;
    INT32_T crowd = __node_crowd(ctx, nx, ny, nz);

    exits = __collect_dirs(ctx, nx, ny, nz, dir ^ 1u, next_dirs);
    score += exits * 5;
    score -= crowd * 12;

    if (dir == p->dir) {
        score += (p->run_goal > 0) ? 38 : 16;
    } else if (p->run_goal > 0) {
        score -= 20;
    } else {
        score += 2;
    }

    if (p->len > 0 && dir != p->dir) {
        if (radial_delta > 0) {
            score += 16 + __imin(18, radial_delta * 4);
        } else if (radial_delta < 0) {
            score -= 18 + __imin(20, (-radial_delta) * 4);
        } else {
            score -= 4;
        }
    }

    if (p->retreat_steps > 0) {
        if (depth_delta <= 0) {
            return 0;
        }
        score += 68 + __imin(112, depth_delta >> 5);
        if (radial_delta > 0) {
            score += 18 + __imin(24, radial_delta * 4);
        } else if (radial_delta < 0) {
            score -= 28 + __imin(24, (-radial_delta) * 4);
        }
    } else if (p->len < 9) {
        if (depth_delta < 0) {
            return 0;
        }
        score += 24 + __imin(72, depth_delta >> 6);
        if (radial_delta > 0) {
            score += 10 + __imin(12, radial_delta * 3);
        }
    }

    if (p->warmup > 0) {
        if (depth_delta > 0) {
            score += __imin(54, depth_delta >> 6);
        } else {
            score -= __imin(42, (-depth_delta) >> 6);
        }
        if (depth_next < ctx->cam_dist_q - (FP_ONE >> 1)) {
            score -= 160;
        }
    } else {
        if (depth_delta > 0) {
            score += __imin(14, depth_delta >> 7);
        } else if (depth_delta < 0) {
            score -= __imin(20, (-depth_delta) >> 7);
        }
        if (depth_next < ctx->cam_dist_q + (FP_ONE << 1)) {
            score -= __imin(26, ((ctx->cam_dist_q + (FP_ONE << 1)) - depth_next) >> 8);
        }
    }

    if (score < 1) score = 1;
    return score;
}

STATIC BOOL_T __pipe_reset(PIPES_CTX_T *ctx, PIPE_T *p, INT32_T color_idx)
{
    UINT8_T dirs[6];
    INT32_T n;
    INT32_T best_score = -32767;
    INT8_T best_x = 0;
    INT8_T best_y = 0;
    INT8_T best_z = 0;
    UINT8_T best_dir = 0;
    BOOL_T found = FALSE;

    for (INT32_T tries = 0; tries < 128; tries++) {
        INT8_T x = (INT8_T)__rand_n(GRID_X);
        INT8_T y = (INT8_T)__rand_n(GRID_Y);
        INT8_T z = (INT8_T)__rand_n(GRID_Z);
        INT32_T depth_now;
        INT32_T radial;
        INT32_T depth_bonus;
        INT32_T cell_best = -32767;
        UINT8_T cell_dir = 0;

        if (__node_used(ctx, x, y, z)) continue;
        n = __collect_dirs(ctx, x, y, z, -1, dirs);
        if (n <= 0) continue;

        depth_now = __camera_depth_grid_q(ctx, x, y, z);
        radial = __grid_radial_score(x, y);
        depth_bonus = __depth_focus_bonus(ctx, depth_now, FP_ONE * 2, FP_ONE * 5, 108);

        for (INT32_T i = 0; i < n; i++) {
            INT8_T tx;
            INT8_T ty;
            INT8_T tz;
            INT32_T score;
            INT32_T depth_delta;
            INT32_T exits;
            UINT8_T next_dirs[6];

            if (!__dir_target(x, y, z, dirs[i], &tx, &ty, &tz)) continue;
            depth_delta = __camera_depth_grid_q(ctx, tx, ty, tz) - depth_now;
            exits = __collect_dirs(ctx, tx, ty, tz, dirs[i] ^ 1u, next_dirs);
            score = 18 + __rand_n(10) + radial * 8 + depth_bonus +
                    exits * 8 - (__node_crowd(ctx, tx, ty, tz) * 10);
            if (depth_delta >= 0) {
                score += 80 + __imin(148, depth_delta >> 5);
            } else {
                score -= 220;
            }
            if (depth_now < ctx->cam_dist_q - (FP_ONE >> 1)) {
                score -= 260;
            }
            if (score > cell_best) {
                cell_best = score;
                cell_dir = dirs[i];
            }
        }

        if (cell_best > best_score) {
            best_score = cell_best;
            best_x = x;
            best_y = y;
            best_z = z;
            best_dir = cell_dir;
            found = TRUE;
        }
    }

    if (found) {
        p->x         = best_x;
        p->y         = best_y;
        p->z         = best_z;
        p->dir       = best_dir;
        p->color_idx = (UINT8_T)__rand_n(PALETTE_N);
        p->len       = 0;
        p->run_goal  = (UINT8_T)(4 + __rand_n(4));
        p->warmup    = (UINT8_T)(10 + __rand_n(8));
        p->retreat_steps = (UINT8_T)(12 + __rand_n(6));
        p->path_len = 0;
        p->run_start_idx = 0;
        __pipe_store_node(p, best_x, best_y, best_z);
        __mark_node(ctx, best_x, best_y, best_z);
        return TRUE;
    }

    for (INT32_T x = 0; x < GRID_X; x++) {
        for (INT32_T y = 0; y < GRID_Y; y++) {
            for (INT32_T z = 0; z < GRID_Z; z++) {
                INT32_T depth_now;
                INT32_T radial;
                INT32_T depth_bonus;
                INT32_T cell_best = -32767;
                UINT8_T cell_dir = 0;

                if (__node_used(ctx, (INT8_T)x, (INT8_T)y, (INT8_T)z)) continue;
                n = __collect_dirs(ctx, (INT8_T)x, (INT8_T)y, (INT8_T)z, -1, dirs);
                if (n <= 0) continue;

                depth_now = __camera_depth_grid_q(ctx, x, y, z);
                radial = __grid_radial_score((INT8_T)x, (INT8_T)y);
                depth_bonus = __depth_focus_bonus(ctx, depth_now, FP_ONE * 2, FP_ONE * 5, 108);

                for (INT32_T i = 0; i < n; i++) {
                    INT8_T tx;
                    INT8_T ty;
                    INT8_T tz;
                    INT32_T score;
                    INT32_T depth_delta;
                    INT32_T exits;
                    UINT8_T next_dirs[6];

                    if (!__dir_target((INT8_T)x, (INT8_T)y, (INT8_T)z, dirs[i], &tx, &ty, &tz)) continue;
                    depth_delta = __camera_depth_grid_q(ctx, tx, ty, tz) - depth_now;
                    exits = __collect_dirs(ctx, tx, ty, tz, dirs[i] ^ 1u, next_dirs);
                    score = 18 + radial * 8 + depth_bonus + exits * 8 -
                            (__node_crowd(ctx, tx, ty, tz) * 10);
                    if (depth_delta >= 0) {
                        score += 80 + __imin(148, depth_delta >> 5);
                    } else {
                        score -= 220;
                    }
                    if (depth_now < ctx->cam_dist_q - (FP_ONE >> 1)) {
                        score -= 260;
                    }
                    if (score > cell_best) {
                        cell_best = score;
                        cell_dir = dirs[i];
                    }
                }

                if (cell_best <= -32767) {
                    continue;
                }

                p->x         = (INT8_T)x;
                p->y         = (INT8_T)y;
                p->z         = (INT8_T)z;
                p->dir       = cell_dir;
                p->color_idx = (UINT8_T)__rand_n(PALETTE_N);
                p->len       = 0;
                p->run_goal  = (UINT8_T)(4 + __rand_n(4));
                p->warmup    = (UINT8_T)(10 + __rand_n(8));
                p->retreat_steps = (UINT8_T)(12 + __rand_n(6));
                p->path_len = 0;
                p->run_start_idx = 0;
                __pipe_store_node(p, (INT8_T)x, (INT8_T)y, (INT8_T)z);
                __mark_node(ctx, (INT8_T)x, (INT8_T)y, (INT8_T)z);
                return TRUE;
            }
        }
    }

    return FALSE;
}

STATIC VOID_T __pipes_reset_scene(PIPES_CTX_T *ctx)
{
    __select_camera_preset(ctx);
    __scene_layout(ctx);
    __clear_scene(ctx);
    __reset_scene_state(ctx);
    for (INT32_T i = 0; i < N_PIPES; i++) {
        if (!__pipe_reset(ctx, &ctx->pipes[i], i % PALETTE_N)) {
            memset(&ctx->pipes[i], 0, sizeof(ctx->pipes[i]));
        }
    }
}

STATIC VOID_T __pipe_step(PIPES_CTX_T *ctx, PIPE_T *p)
{
    UINT8_T dirs[6];
    INT32_T n;
    INT32_T old_dir = p->dir;
    INT32_T choose_dir = old_dir;
    INT32_T total_weight = 0;
    INT8_T nx = p->x;
    INT8_T ny = p->y;
    INT8_T nz = p->z;
    UINT32_T color_hex = s_palette[p->color_idx];

    n = __collect_dirs(ctx, p->x, p->y, p->z, (p->len > 0) ? (old_dir ^ 1u) : -1, dirs);
    if (n <= 0) {
        if (!__pipe_reset(ctx, p, (INT32_T)(p->color_idx + 1 + __rand_n(PALETTE_N - 1)))) {
            __pipes_reset_scene(ctx);
        }
        return;
    }

    for (INT32_T i = 0; i < n; i++) {
        INT8_T tx;
        INT8_T ty;
        INT8_T tz;

        if (!__dir_target(p->x, p->y, p->z, dirs[i], &tx, &ty, &tz)) continue;
        total_weight += __score_dir(ctx, p, dirs[i], tx, ty, tz);
    }

    if (total_weight > 0) {
        INT32_T pick = __rand_n(total_weight);

        for (INT32_T i = 0; i < n; i++) {
            INT8_T tx;
            INT8_T ty;
            INT8_T tz;
            INT32_T weight;

            if (!__dir_target(p->x, p->y, p->z, dirs[i], &tx, &ty, &tz)) continue;
            weight = __score_dir(ctx, p, dirs[i], tx, ty, tz);
            if (pick < weight) {
                choose_dir = dirs[i];
                break;
            }
            pick -= weight;
        }
    } else {
        choose_dir = dirs[__rand_n(n)];
    }

    if (!__dir_target(p->x, p->y, p->z, (UINT8_T)choose_dir, &nx, &ny, &nz)) {
        if (!__pipe_reset(ctx, p, (INT32_T)(p->color_idx + 1 + __rand_n(PALETTE_N - 1)))) {
            __pipes_reset_scene(ctx);
        }
        return;
    }

    if (p->len == 0) {
        __render_joint(ctx, p->x, p->y, p->z, color_hex);
    } else if (choose_dir != old_dir) {
        __render_elbow(ctx, p->x, p->y, p->z, (UINT8_T)old_dir, (UINT8_T)choose_dir, color_hex);
    } else {
        __erase_ball(ctx, (float)p->x, (float)p->y, (float)p->z, ctx->world_radius * 1.18f);
    }

    __edge_mark(ctx, p->x, p->y, p->z, (UINT8_T)choose_dir);

    if (ctx->edge_count >= EDGE_RESET_LIMIT) {
        __pipes_reset_scene(ctx);
        return;
    }

    __mark_node(ctx, nx, ny, nz);
    __pipe_store_node(p, nx, ny, nz);

    if (p->len > 0 && choose_dir != old_dir && p->path_len >= 2) {
        p->run_start_idx = p->path_len - 2;
    }

    if (p->path_len >= 2) {
        PIPE_NODE_T *run_start = &p->path[p->run_start_idx];
        PIPE_NODE_T *tail = &p->path[p->path_len - 1];

        __render_segment(ctx,
                         (float)run_start->x, (float)run_start->y, (float)run_start->z,
                         (float)tail->x, (float)tail->y, (float)tail->z,
                         color_hex);
    }

    p->x = nx;
    p->y = ny;
    p->z = nz;
    p->dir = (UINT8_T)choose_dir;
    p->len++;
    if (p->warmup > 0) p->warmup--;
    if (p->retreat_steps > 0) p->retreat_steps--;
    if (choose_dir == old_dir) {
        if (p->run_goal > 0) p->run_goal--;
    } else {
        p->run_goal = (UINT8_T)(2 + __rand_n(5));
    }

    if (p->len >= MAX_PIPE_LEN) {
        if (!__pipe_reset(ctx, p, (INT32_T)(p->color_idx + 1 + __rand_n(PALETTE_N - 1)))) {
            __pipes_reset_scene(ctx);
        }
    }
}

STATIC VOID_T __pipes_stop_ctx(PIPES_CTX_T *ctx)
{
    lv_obj_t *host;
    lv_obj_t *return_screen;
    if (!ctx) return;

    ctx->active = FALSE;
    if (ctx == &s_saver) {
        win95_cursor_set_visible(TRUE);
    }

    if (ctx->anim_timer) {
        lv_timer_delete(ctx->anim_timer);
        ctx->anim_timer = NULL;
    }

    host = ctx->host;
    return_screen = ctx->return_screen;
    if (ctx->host_is_screen && host && lv_obj_is_valid(host)) {
        if (return_screen && lv_obj_is_valid(return_screen) &&
            lv_screen_active() == host) {
            lv_screen_load(return_screen);
        }
        lv_obj_delete(host);
    } else if (host && lv_obj_is_valid(host)) {
        lv_obj_delete(host);
    } else if (ctx->canvas && lv_obj_is_valid(ctx->canvas)) {
        lv_obj_delete(ctx->canvas);
    }
    ctx->host = NULL;
    ctx->return_screen = NULL;
    ctx->canvas = NULL;

    if (ctx->zbuf) {
        PIPES_MEM_FREE(ctx->zbuf);
        ctx->zbuf = NULL;
    }
    if (ctx->buf) {
        PIPES_MEM_FREE(ctx->buf);
        ctx->buf = NULL;
    }

    ctx->parent = NULL;
    ctx->width = 0;
    ctx->height = 0;
    ctx->tick_count = 0;
    ctx->edge_count = 0;
    ctx->dirty_x1 = 0;
    ctx->dirty_y1 = 0;
    ctx->dirty_x2 = -1;
    ctx->dirty_y2 = -1;
    ctx->depth_span = 0;
    ctx->focal = 0.0f;
    ctx->cam_dist = 0.0f;
    ctx->world_radius = 0.0f;
    ctx->joint_radius = 0.0f;
    ctx->scene_cx = 0.0f;
    ctx->scene_cy = 0.0f;
    ctx->scene_cz = 0.0f;
    ctx->screen_cx = 0.0f;
    ctx->screen_cy = 0.0f;
    ctx->anim_pipe_idx = 0;
    ctx->start_tick_ms = 0;
    ctx->stop_on_click = FALSE;
    ctx->host_is_screen = FALSE;
    ctx->dirty_full = FALSE;
    memset(ctx->pipes, 0, sizeof(ctx->pipes));
    memset(ctx->used_node, 0, sizeof(ctx->used_node));
    memset(ctx->used_x, 0, sizeof(ctx->used_x));
    memset(ctx->used_y, 0, sizeof(ctx->used_y));
    memset(ctx->used_z, 0, sizeof(ctx->used_z));
}

STATIC VOID_T __canvas_click_cb(lv_event_t *e)
{
    PIPES_CTX_T *ctx = (PIPES_CTX_T *)lv_event_get_user_data(e);
    if (ctx && ctx->stop_on_click) {
        if (lv_tick_elaps(ctx->start_tick_ms) < 300u) {
            return;
        }
        __pipes_stop_ctx(ctx);
    }
}

STATIC VOID_T __pipes_anim_cb(lv_timer_t *t)
{
    PIPES_CTX_T *ctx = (PIPES_CTX_T *)lv_timer_get_user_data(t);

    if (!ctx || !ctx->active || !ctx->canvas || !ctx->buf || !ctx->zbuf) return;
    if (!lv_obj_is_valid(ctx->canvas)) {
        __pipes_stop_ctx(ctx);
        return;
    }

    __dirty_reset(ctx);

    for (INT32_T i = 0; i < N_PIPES; i++) {
        __pipe_step(ctx, &ctx->pipes[i]);
    }

    __dirty_invalidate(ctx);

    ctx->tick_count++;
    if (ctx->tick_count >= RESET_TICKS) {
        __pipes_reset_scene(ctx);
        __dirty_invalidate(ctx);
    }
}

STATIC VOID_T __pipes_start_ctx(PIPES_CTX_T *ctx,
                                lv_obj_t *parent,
                                INT32_T width,
                                INT32_T height,
                                BOOL_T stop_on_click)
{
    UINT32_T px_count;
    UINT32_T buf_sz;
    UINT32_T zbuf_sz;

    if (!ctx || !parent || width < 24 || height < 24) return;

    __pipes_stop_ctx(ctx);

    ctx->parent        = parent;
    ctx->width         = width;
    ctx->height        = height;
    ctx->stop_on_click = stop_on_click;
    ctx->return_screen = NULL;
    ctx->host_is_screen = FALSE;
    ctx->start_tick_ms = lv_tick_get();
    __dirty_reset(ctx);
    __scene_layout(ctx);

    px_count = (UINT32_T)(width * height);
    buf_sz   = px_count * 2u;
    zbuf_sz  = px_count * (UINT32_T)sizeof(uint16_t);

    if (stop_on_click) {
        win95_cursor_set_visible(FALSE);
        ctx->return_screen = lv_screen_active();
        ctx->host_is_screen = TRUE;
        ctx->host = lv_obj_create(NULL);
        if (!ctx->host) {
            PR_ERR("Pipes: host screen create failed");
            __pipes_stop_ctx(ctx);
            return;
        }
        lv_obj_remove_style_all(ctx->host);
        lv_obj_set_size(ctx->host, width, height);
        lv_obj_set_pos(ctx->host, 0, 0);
        lv_obj_set_style_bg_color(ctx->host, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(ctx->host, LV_OPA_COVER, 0);
        lv_obj_clear_flag(ctx->host, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ctx->host, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ctx->host, __canvas_click_cb, LV_EVENT_CLICKED, ctx);
        lv_screen_load(ctx->host);
    }

    ctx->buf = PIPES_MEM_ALLOC(buf_sz);
    if (!ctx->buf) {
        PR_ERR("Pipes: framebuffer alloc failed (%lu bytes)", (UINT32_T)buf_sz);
        __pipes_stop_ctx(ctx);
        return;
    }

    ctx->zbuf = (uint16_t *)PIPES_MEM_ALLOC(zbuf_sz);
    if (!ctx->zbuf) {
        PR_ERR("Pipes: zbuffer alloc failed (%lu bytes)", (UINT32_T)zbuf_sz);
        __pipes_stop_ctx(ctx);
        return;
    }

    if (stop_on_click) {
        ctx->canvas = lv_canvas_create(ctx->host);
    } else {
        ctx->canvas = lv_canvas_create(parent);
    }

    if (!ctx->canvas) {
        PR_ERR("Pipes: canvas create failed");
        __pipes_stop_ctx(ctx);
        return;
    }

    lv_obj_remove_style_all(ctx->canvas);
    lv_obj_set_size(ctx->canvas, width, height);
    lv_obj_set_pos(ctx->canvas, 0, 0);
    lv_obj_clear_flag(ctx->canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(ctx->canvas, ctx->buf, width, height,
                         LV_COLOR_FORMAT_RGB565);

    if (stop_on_click) {
        lv_obj_add_flag(ctx->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ctx->canvas, __canvas_click_cb,
                            LV_EVENT_CLICKED, ctx);
    }

    __pipes_reset_scene(ctx);
    for (INT32_T warm = 0; warm < 10; warm++) {
        for (INT32_T i = 0; i < N_PIPES; i++) {
            __pipe_step(ctx, &ctx->pipes[i]);
        }
    }
    __dirty_invalidate(ctx);
    ctx->active = TRUE;
    ctx->anim_timer = lv_timer_create(__pipes_anim_cb, ANIM_PERIOD_MS, ctx);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_pipes_start(VOID_T)
{
    s_seed = (UINT32_T)tal_time_get_posix();
    s_idle_sec = 0;
    win95_pipes_preview_stop();
    __pipes_start_ctx(&s_saver, lv_screen_active(),
                      (INT32_T)BIOS_SCREEN_WIDTH,
                      (INT32_T)BIOS_SCREEN_HEIGHT,
                      TRUE);
}

VOID_T win95_pipes_stop(VOID_T)
{
    __pipes_stop_ctx(&s_saver);
    s_idle_sec = 0;
}

VOID_T win95_pipes_preview_start(lv_obj_t *parent)
{
    INT32_T w;
    INT32_T h;

    if (!parent) return;

    s_seed = (UINT32_T)tal_time_get_posix();
    w = lv_obj_get_width(parent);
    h = lv_obj_get_height(parent);
    __pipes_start_ctx(&s_preview, parent, w, h, FALSE);
}

VOID_T win95_pipes_preview_stop(VOID_T)
{
    __pipes_stop_ctx(&s_preview);
}

VOID_T win95_pipes_set_timeout(UINT32_T minutes)
{
    s_timeout_sec = minutes * 60u;
    s_idle_sec    = 0;
}

VOID_T win95_pipes_tick(VOID_T)
{
    lv_display_t *disp;

    if (s_saver.active) return;

    disp = lv_display_get_default();
    if (disp && lv_display_get_inactive_time(disp) < 1500u) {
        s_idle_sec = 0;
        return;
    }

    s_idle_sec++;

    if (s_timeout_sec > 0 && s_idle_sec >= s_timeout_sec) {
        win95_pipes_start();
    }
}

VOID_T win95_pipes_reset_idle(VOID_T)
{
    s_idle_sec = 0;
    if (s_saver.active) {
        win95_pipes_stop();
    }
}
