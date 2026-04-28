/**
 * @file win95_winamp.c
 * @brief Winamp 2.x style audio player for TuyaOS 95.
 *        Plays 16 kHz / 16-bit / mono WAV files from /sdcard/music/.
 */
#include "win95_winamp.h"
#include "win95_disk.h"
#include "win95_desktop.h"
#include "tdl_audio_manage.h"
#include "tkl_fs.h"
#include "tal_api.h"
#include "lv_vendor.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Dimensions
 * --------------------------------------------------------------------------- */
#define WA_W         280
#define WA_TOTAL_H   212
#define WA_TITLE_H   14
#define SPEC_H       18
#define N_SPEC       20
#define N_SONGS_MAX  32
#define WAV_CHUNK    4096

/* ---------------------------------------------------------------------------
 * Colors  (classic Winamp dark skin)
 * --------------------------------------------------------------------------- */
#define WA_BG        0x1A1A1A
#define WA_TITLE     0x0B1D45
#define WA_LCD_BG    0x000C00
#define WA_LCD_FG    0x00FF00
#define WA_SPEC_CLR  0x00CC00
#define WA_TEXT      0x99FF99
#define WA_DIM       0x444444
#define WA_BTN       0x2E2E2E
#define WA_BTN_LT    0x505050
#define WA_BTN_DK    0x0A0A0A
#define WA_PL_BG     0x050505
#define WA_PL_TEXT   0x88EE88
#define WA_SEL       0x195990
#define WA_SEL_TXT   0xFFFFFF

/* ---------------------------------------------------------------------------
 * Types
 * --------------------------------------------------------------------------- */
typedef enum { WA_STOP = 0, WA_PLAY, WA_PAUSE } WA_STATE_T;

typedef struct {
    CHAR_T path[192];
    CHAR_T name[64];
} WA_SONG_T;

typedef struct {
    lv_obj_t   *win;

    /* Widgets */
    lv_obj_t   *title_lbl;
    lv_obj_t   *time_lbl;
    lv_obj_t   *status_lbl;
    lv_obj_t   *spec_bars[N_SPEC];
    lv_obj_t   *vol_slider;
    lv_obj_t   *pl_list;

    /* Spectrum state */
    INT32_T     spec_h[N_SPEC];
    INT32_T     spec_tgt[N_SPEC];

    /* Songs */
    WA_SONG_T   songs[N_SONGS_MAX];
    INT32_T     n_songs;
    INT32_T     cur_song;

    /* Playback */
    WA_STATE_T  state;
    UINT32_T    elapsed_sec;
    UINT32_T    byte_rate;
    volatile BOOL_T stop_req;
    volatile BOOL_T next_req;

    /* Audio */
    TDL_AUDIO_HANDLE_T audio_handle;
    BOOL_T      audio_open;
    THREAD_HANDLE play_thread;

    /* Timer / random */
    lv_timer_t *ui_timer;
    UINT32_T    fake_seed;
} WA_CTX_T;

STATIC WA_CTX_T s_wa;

/* ---------------------------------------------------------------------------
 * LCG random
 * --------------------------------------------------------------------------- */
STATIC UINT32_T __wa_rand(VOID_T)
{
    s_wa.fake_seed = s_wa.fake_seed * 1664525u + 1013904223u;
    return s_wa.fake_seed >> 16;
}

/* ---------------------------------------------------------------------------
 * WAV header parser — fills *byte_rate_out, *data_ofs, *data_sz.
 * --------------------------------------------------------------------------- */
STATIC BOOL_T __wa_parse_wav(TUYA_FILE f, UINT32_T *data_ofs,
                              UINT32_T *data_sz, UINT32_T *byte_rate_out)
{
    UINT8_T hdr[12];
    tkl_fseek(f, 0, SEEK_SET);
    if (tkl_fread(hdr, 12, f) < 12) return FALSE;
    if (hdr[0]!='R'||hdr[1]!='I'||hdr[2]!='F'||hdr[3]!='F') return FALSE;
    if (hdr[8]!='W'||hdr[9]!='A'||hdr[10]!='V'||hdr[11]!='E') return FALSE;

    /* Scan chunks */
    UINT32_T br = 32000; /* fallback: 16kHz 16-bit mono */
    for (INT32_T lim = 0; lim < 20; lim++) {
        UINT8_T ch[8];
        if (tkl_fread(ch, 8, f) < 8) return FALSE;
        UINT32_T csz = (UINT32_T)ch[4] | ((UINT32_T)ch[5]<<8)
                     | ((UINT32_T)ch[6]<<16) | ((UINT32_T)ch[7]<<24);
        if (ch[0]=='f' && ch[1]=='m' && ch[2]=='t') {
            /* read fmt body (up to 16 bytes) */
            UINT8_T fmt[16];
            UINT32_T rb = csz < 16 ? csz : 16;
            if (tkl_fread(fmt, (INT_T)rb, f) >= 28 - 8) {
                br = (UINT32_T)fmt[8]  | ((UINT32_T)fmt[9]<<8)
                   | ((UINT32_T)fmt[10]<<16) | ((UINT32_T)fmt[11]<<24);
            }
            if (csz > rb) tkl_fseek(f, (INT64_T)(csz - rb), SEEK_CUR);
        } else if (ch[0]=='d' && ch[1]=='a' && ch[2]=='t' && ch[3]=='a') {
            *byte_rate_out = (br > 0) ? br : 32000;
            *data_ofs = (UINT32_T)(INT64_T)tkl_ftell(f);
            *data_sz  = csz;
            return TRUE;
        } else {
            tkl_fseek(f, (INT64_T)csz, SEEK_CUR);
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * Audio playback thread
 * --------------------------------------------------------------------------- */
STATIC VOID_T __wa_play_thread(VOID_T *arg)
{
    (VOID_T)arg;

    if (s_wa.n_songs <= 0 || s_wa.cur_song >= s_wa.n_songs) {
        s_wa.state = WA_STOP;
        s_wa.play_thread = NULL;
        return;
    }

    CHAR_T path[192];
    strncpy(path, s_wa.songs[s_wa.cur_song].path, sizeof(path)-1);
    path[sizeof(path)-1] = '\0';

    TUYA_FILE f = tkl_fopen(path, "r");
    if (!f) {
        PR_WARN("Winamp: open failed: %s", path);
        s_wa.state = WA_STOP;
        s_wa.play_thread = NULL;
        return;
    }

    UINT32_T dofs, dsz, brate;
    if (!__wa_parse_wav(f, &dofs, &dsz, &brate)) {
        PR_WARN("Winamp: WAV parse failed: %s", path);
        tkl_fclose(f);
        s_wa.state = WA_STOP;
        s_wa.play_thread = NULL;
        return;
    }
    s_wa.byte_rate = brate;
    tkl_fseek(f, (INT64_T)dofs, SEEK_SET);

    UINT8_T buf[WAV_CHUNK];
    UINT32_T bytes_played = 0;
    s_wa.elapsed_sec = 0;

    while (!s_wa.stop_req && bytes_played < dsz) {
        if (s_wa.state == WA_PAUSE) {
            tal_system_sleep(50);
            continue;
        }
        INT_T nr = tkl_fread(buf, WAV_CHUNK, f);
        if (nr <= 0) break;
        if (s_wa.audio_open) {
            tdl_audio_play(s_wa.audio_handle, buf, (UINT32_T)nr);
        }
        bytes_played += (UINT32_T)nr;
        s_wa.elapsed_sec = (brate > 0) ? (bytes_played / brate) : 0;
    }

    tkl_fclose(f);
    if (!s_wa.stop_req) {
        s_wa.next_req = TRUE;  /* signal UI timer to auto-advance */
    }
    s_wa.state = WA_STOP;
    s_wa.play_thread = NULL;
}

/* ---------------------------------------------------------------------------
 * Start playing current song
 * --------------------------------------------------------------------------- */
STATIC VOID_T __wa_play_cur(VOID_T)
{
    if (s_wa.n_songs <= 0) return;

    /* Stop existing thread */
    if (s_wa.play_thread) {
        s_wa.stop_req = TRUE;
        if (s_wa.audio_open) tdl_audio_play_stop(s_wa.audio_handle);
        tal_system_sleep(150);
        s_wa.play_thread = NULL;
    }

    s_wa.stop_req    = FALSE;
    s_wa.next_req    = FALSE;
    s_wa.elapsed_sec = 0;
    s_wa.state       = WA_PLAY;

    THREAD_CFG_T tcfg = {8192, 5, "wa_play"};
    OPERATE_RET rt = tal_thread_create_and_start(&s_wa.play_thread, NULL, NULL,
                                                  __wa_play_thread, NULL, &tcfg);
    if (rt != OPRT_OK) {
        PR_ERR("Winamp: thread create failed: %d", rt);
        s_wa.state = WA_STOP;
    }
}

/* ---------------------------------------------------------------------------
 * Playlist selection highlight
 * --------------------------------------------------------------------------- */
STATIC VOID_T __wa_pl_sel(VOID_T)
{
    if (!s_wa.pl_list) return;
    UINT32_T n = (UINT32_T)lv_obj_get_child_count(s_wa.pl_list);
    for (UINT32_T i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_wa.pl_list, (INT32_T)i);
        BOOL_T sel = ((INT32_T)i == s_wa.cur_song);
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? WA_SEL : WA_PL_BG), 0);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            lv_color_hex(sel ? WA_SEL_TXT : WA_PL_TEXT), 0);
    }
}

/* ---------------------------------------------------------------------------
 * Update scrolling title label
 * --------------------------------------------------------------------------- */
STATIC VOID_T __wa_update_title(VOID_T)
{
    if (!s_wa.title_lbl) return;
    if (s_wa.n_songs == 0) {
        lv_label_set_text(s_wa.title_lbl, "  --- No files in /sdcard/music/ ---  ");
    } else {
        CHAR_T buf[80];
        snprintf(buf, sizeof(buf), "  %d. %s  ",
                 s_wa.cur_song + 1, s_wa.songs[s_wa.cur_song].name);
        lv_label_set_text(s_wa.title_lbl, buf);
    }
}

/* ---------------------------------------------------------------------------
 * UI update timer (300 ms)
 * --------------------------------------------------------------------------- */
STATIC VOID_T __wa_ui_tick(lv_timer_t *t)
{
    (VOID_T)t;

    /* Auto-advance when song finishes */
    if (s_wa.next_req) {
        s_wa.next_req = FALSE;
        if (s_wa.n_songs > 0) {
            s_wa.cur_song = (s_wa.cur_song + 1) % s_wa.n_songs;
            __wa_update_title();
            __wa_pl_sel();
            __wa_play_cur();
        }
    }

    /* Time display */
    if (s_wa.time_lbl) {
        CHAR_T tb[8];
        UINT32_T m = s_wa.elapsed_sec / 60;
        UINT32_T s = s_wa.elapsed_sec % 60;
        snprintf(tb, sizeof(tb), "%02u:%02u", (unsigned)m, (unsigned)s);
        lv_label_set_text(s_wa.time_lbl, tb);
    }

    /* Status */
    if (s_wa.status_lbl) {
        CONST CHAR_T *st = "STOP";
        if      (s_wa.state == WA_PLAY)  st = "PLAY";
        else if (s_wa.state == WA_PAUSE) st = "PAUS";
        lv_label_set_text(s_wa.status_lbl, st);
    }

    /* Spectrum animation */
    INT32_T bar_slot = (WA_W - 4) / N_SPEC;
    for (INT32_T i = 0; i < N_SPEC; i++) {
        if (s_wa.state == WA_PLAY) {
            INT32_T r = (INT32_T)(__wa_rand() & 0x7) - 2;
            s_wa.spec_tgt[i] += r;
            if (s_wa.spec_tgt[i] < 1)    s_wa.spec_tgt[i] = 1;
            if (s_wa.spec_tgt[i] > SPEC_H-2) s_wa.spec_tgt[i] = SPEC_H-2;
        } else {
            if (s_wa.spec_tgt[i] > 0) s_wa.spec_tgt[i]--;
        }
        if      (s_wa.spec_h[i] < s_wa.spec_tgt[i]) s_wa.spec_h[i]++;
        else if (s_wa.spec_h[i] > s_wa.spec_tgt[i]) s_wa.spec_h[i]--;
        INT32_T h = (s_wa.spec_h[i] < 1) ? 1 : s_wa.spec_h[i];
        if (s_wa.spec_bars[i]) {
            lv_obj_set_height(s_wa.spec_bars[i], h);
            lv_obj_set_pos(s_wa.spec_bars[i], i * bar_slot, SPEC_H - h);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Button callbacks
 * --------------------------------------------------------------------------- */
STATIC VOID_T __wa_prev_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_wa.n_songs == 0) return;
    s_wa.cur_song = (s_wa.cur_song > 0) ? s_wa.cur_song-1 : s_wa.n_songs-1;
    __wa_update_title();
    __wa_pl_sel();
    if (s_wa.state != WA_STOP) __wa_play_cur();
}

STATIC VOID_T __wa_play_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_wa.n_songs == 0) return;
    if      (s_wa.state == WA_PAUSE) s_wa.state = WA_PLAY;
    else if (s_wa.state == WA_STOP)  __wa_play_cur();
}

STATIC VOID_T __wa_pause_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_wa.state == WA_PLAY) {
        s_wa.state = WA_PAUSE;
        if (s_wa.audio_open) tdl_audio_play_stop(s_wa.audio_handle);
    }
}

STATIC VOID_T __wa_stop_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_wa.state != WA_STOP) {
        s_wa.stop_req    = TRUE;
        s_wa.state       = WA_STOP;
        s_wa.elapsed_sec = 0;
        if (s_wa.audio_open) tdl_audio_play_stop(s_wa.audio_handle);
    }
}

STATIC VOID_T __wa_next_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_wa.n_songs == 0) return;
    s_wa.cur_song = (s_wa.cur_song + 1) % s_wa.n_songs;
    __wa_update_title();
    __wa_pl_sel();
    if (s_wa.state != WA_STOP) __wa_play_cur();
}

STATIC VOID_T __wa_vol_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    UINT8_T v = (UINT8_T)lv_slider_get_value(sl);
    if (s_wa.audio_open) tdl_audio_volume_set(s_wa.audio_handle, v);
}

STATIC VOID_T __wa_row_cb(lv_event_t *e)
{
    INT32_T idx = (INT32_T)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_wa.n_songs) return;
    s_wa.cur_song = idx;
    __wa_update_title();
    __wa_pl_sel();
    __wa_play_cur();
}

STATIC VOID_T __wa_close_cb(lv_event_t *e)
{
    (VOID_T)e;
    if (s_wa.ui_timer) { lv_timer_delete(s_wa.ui_timer); s_wa.ui_timer = NULL; }
    s_wa.stop_req = TRUE;
    s_wa.state    = WA_STOP;
    if (s_wa.audio_open) {
        tdl_audio_play_stop(s_wa.audio_handle);
        tdl_audio_close(s_wa.audio_handle);
        s_wa.audio_open = FALSE;
    }
    if (s_wa.win) { lv_obj_delete(s_wa.win); }
    memset(&s_wa, 0, sizeof(s_wa));
}

/* ---------------------------------------------------------------------------
 * Helper: build a dark raised Winamp button
 * --------------------------------------------------------------------------- */
STATIC lv_obj_t *__wa_btn(lv_obj_t *parent, INT32_T x, INT32_T y,
                            INT32_T w, INT32_T h,
                            CONST CHAR_T *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(WA_BTN), 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(WA_BTN_LT), 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(WA_BTN_DK), 0);
    lv_obj_set_style_shadow_width(b, 1, 0);
    lv_obj_set_style_shadow_ofs_x(b, 1, 0);
    lv_obj_set_style_shadow_ofs_y(b, 1, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(WA_TEXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

/* ---------------------------------------------------------------------------
 * Scan /sdcard/music/ for .wav files
 * --------------------------------------------------------------------------- */
STATIC VOID_T __wa_scan(VOID_T)
{
    s_wa.n_songs = 0;
    if (!win95_disk_is_mounted()) return;

    /* Try /sdcard/music first, fall back to /sdcard */
    CONST CHAR_T *dirs[] = { "/sdcard/music", "/sdcard" };
    TUYA_DIR dir = NULL;
    CONST CHAR_T *used_dir = NULL;
    for (INT32_T d = 0; d < 2; d++) {
        if (tkl_dir_open(dirs[d], &dir) == 0) { used_dir = dirs[d]; break; }
    }
    if (!dir || !used_dir) return;

    TUYA_FILEINFO info;
    while (tkl_dir_read(dir, &info) == 0 && s_wa.n_songs < N_SONGS_MAX) {
        CONST CHAR_T *fname = NULL;
        tkl_dir_name(info, &fname);
        if (!fname || fname[0] == '.') continue;
        BOOL_T is_reg = FALSE;
        tkl_dir_is_regular(info, &is_reg);
        if (!is_reg) continue;

        /* Check .wav extension (case-insensitive) */
        INT32_T fl = (INT32_T)strlen(fname);
        if (fl < 5) continue;
        CHAR_T ext3 = fname[fl-3] | 0x20;
        CHAR_T ext2 = fname[fl-2] | 0x20;
        CHAR_T ext1 = fname[fl-1] | 0x20;
        if (fname[fl-4]!='.' || ext3!='w' || ext2!='a' || ext1!='v') continue;

        WA_SONG_T *s = &s_wa.songs[s_wa.n_songs];
        snprintf(s->path, sizeof(s->path), "%s/%s", used_dir, fname);
        strncpy(s->name, fname, sizeof(s->name)-1);
        s->name[sizeof(s->name)-1] = '\0';
        /* Strip .wav from display name */
        INT32_T nl = (INT32_T)strlen(s->name);
        if (nl > 4) s->name[nl-4] = '\0';
        s_wa.n_songs++;
    }
    tkl_dir_close(dir);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_winamp_open(lv_obj_t *parent)
{
    /* Destroy existing instance */
    if (s_wa.win) {
        if (s_wa.ui_timer) { lv_timer_delete(s_wa.ui_timer); s_wa.ui_timer = NULL; }
        s_wa.stop_req = TRUE;
        s_wa.state    = WA_STOP;
        if (s_wa.audio_open) {
            tdl_audio_play_stop(s_wa.audio_handle);
            tdl_audio_close(s_wa.audio_handle);
        }
        lv_obj_delete(s_wa.win);
        memset(&s_wa, 0, sizeof(s_wa));
    }

    s_wa.fake_seed = (UINT32_T)tal_time_get_posix();
    for (INT32_T i = 0; i < N_SPEC; i++) { s_wa.spec_h[i] = 1; s_wa.spec_tgt[i] = 1; }

    /* Initialize audio device */
    if (tdl_audio_find("audio", &s_wa.audio_handle) == OPRT_OK) {
        if (tdl_audio_open(s_wa.audio_handle, NULL) == OPRT_OK) {
            s_wa.audio_open = TRUE;
            tdl_audio_volume_set(s_wa.audio_handle, 80);
        }
    }
    if (!s_wa.audio_open) PR_WARN("Winamp: audio device unavailable");

    __wa_scan();

    /* -----------------------------------------------------------------------
     * Build window
     * ----------------------------------------------------------------------- */
    lv_obj_t *w = lv_obj_create(parent);
    lv_obj_remove_style_all(w);
    lv_obj_set_size(w, WA_W, WA_TOTAL_H);
    lv_obj_align(w, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(w, lv_color_hex(WA_BG), 0);
    lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(w, lv_color_hex(WA_BTN_LT), 0);
    lv_obj_set_style_border_width(w, 2, 0);
    lv_obj_set_style_border_side(w, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_color(w, lv_color_hex(WA_BTN_DK), 0);
    lv_obj_set_style_shadow_width(w, 2, 0);
    lv_obj_set_style_shadow_ofs_x(w, 2, 0);
    lv_obj_set_style_shadow_ofs_y(w, 2, 0);
    lv_obj_set_style_radius(w, 0, 0);
    lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(w);
    s_wa.win = w;

    /* ----- Title bar ----- */
    lv_obj_t *tb = lv_obj_create(w);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, WA_W - 4, WA_TITLE_H);
    lv_obj_set_pos(tb, 2, 2);
    lv_obj_set_style_bg_color(tb, lv_color_hex(WA_TITLE), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(tb);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_unscii_8, 0);
    lv_label_set_text(ttl, "Winamp 2.00 - TuyaOS Edition");
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *xb = lv_btn_create(tb);
    lv_obj_set_size(xb, 12, 12);
    lv_obj_align(xb, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(xb, lv_color_hex(WA_BTN), 0);
    lv_obj_set_style_radius(xb, 0, 0);
    lv_obj_set_style_pad_all(xb, 0, 0);
    lv_obj_set_style_border_width(xb, 1, 0);
    lv_obj_set_style_border_color(xb, lv_color_hex(WA_BTN_LT), 0);
    lv_obj_set_style_border_side(xb, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_add_event_cb(xb, __wa_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xb);
    lv_obj_set_style_text_color(xl, lv_color_hex(WA_TEXT), 0);
    lv_obj_set_style_text_font(xl, &lv_font_unscii_8, 0);
    lv_label_set_text(xl, "x");
    lv_obj_center(xl);

    /* ----- Layout y-tracker ----- */
    INT32_T cy = WA_TITLE_H + 2;

    /* ----- Scrolling song title ----- */
    s_wa.title_lbl = lv_label_create(w);
    lv_label_set_long_mode(s_wa.title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_wa.title_lbl, WA_W - 8);
    lv_obj_set_style_text_color(s_wa.title_lbl, lv_color_hex(WA_LCD_FG), 0);
    lv_obj_set_style_text_font(s_wa.title_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_bg_color(s_wa.title_lbl, lv_color_hex(WA_LCD_BG), 0);
    lv_obj_set_style_bg_opa(s_wa.title_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(s_wa.title_lbl, 1, 0);
    lv_obj_set_pos(s_wa.title_lbl, 4, cy);
    cy += 12;

    /* ----- LCD time box + status indicator ----- */
    cy += 2;
    lv_obj_t *lcd = lv_obj_create(w);
    lv_obj_remove_style_all(lcd);
    lv_obj_set_size(lcd, 72, 22);
    lv_obj_set_pos(lcd, 4, cy);
    lv_obj_set_style_bg_color(lcd, lv_color_hex(WA_LCD_BG), 0);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(lcd, lv_color_hex(0x003300), 0);
    lv_obj_set_style_border_width(lcd, 1, 0);
    lv_obj_clear_flag(lcd, LV_OBJ_FLAG_SCROLLABLE);

    s_wa.time_lbl = lv_label_create(lcd);
    lv_obj_set_style_text_color(s_wa.time_lbl, lv_color_hex(WA_LCD_FG), 0);
    lv_obj_set_style_text_font(s_wa.time_lbl, &lv_font_unscii_16, 0);
    lv_label_set_text(s_wa.time_lbl, "00:00");
    lv_obj_center(s_wa.time_lbl);

    /* Status (PLAY/STOP/PAUS) */
    lv_obj_t *stbox = lv_obj_create(w);
    lv_obj_remove_style_all(stbox);
    lv_obj_set_size(stbox, 38, 22);
    lv_obj_set_pos(stbox, 78, cy);
    lv_obj_set_style_bg_color(stbox, lv_color_hex(WA_LCD_BG), 0);
    lv_obj_set_style_bg_opa(stbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(stbox, lv_color_hex(0x003300), 0);
    lv_obj_set_style_border_width(stbox, 1, 0);
    lv_obj_clear_flag(stbox, LV_OBJ_FLAG_SCROLLABLE);

    s_wa.status_lbl = lv_label_create(stbox);
    lv_obj_set_style_text_color(s_wa.status_lbl, lv_color_hex(WA_LCD_FG), 0);
    lv_obj_set_style_text_font(s_wa.status_lbl, &lv_font_unscii_8, 0);
    lv_label_set_text(s_wa.status_lbl, "STOP");
    lv_obj_center(s_wa.status_lbl);

    /* Volume label + slider */
    lv_obj_t *vl = lv_label_create(w);
    lv_obj_set_style_text_color(vl, lv_color_hex(WA_TEXT), 0);
    lv_obj_set_style_text_font(vl, &lv_font_unscii_8, 0);
    lv_label_set_text(vl, "VOL");
    lv_obj_set_pos(vl, 120, cy + 7);

    s_wa.vol_slider = lv_slider_create(w);
    lv_obj_set_size(s_wa.vol_slider, WA_W - 162, 10);
    lv_obj_set_pos(s_wa.vol_slider, 148, cy + 6);
    lv_slider_set_range(s_wa.vol_slider, 0, 100);
    lv_slider_set_value(s_wa.vol_slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_wa.vol_slider, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_color(s_wa.vol_slider, lv_color_hex(WA_SPEC_CLR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_wa.vol_slider, lv_color_hex(WA_TEXT), LV_PART_KNOB);
    lv_obj_set_style_radius(s_wa.vol_slider, 0, 0);
    lv_obj_set_style_radius(s_wa.vol_slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_wa.vol_slider, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(s_wa.vol_slider, __wa_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    cy += 26;

    /* ----- Spectrum visualizer ----- */
    lv_obj_t *spec = lv_obj_create(w);
    lv_obj_remove_style_all(spec);
    lv_obj_set_size(spec, WA_W - 4, SPEC_H);
    lv_obj_set_pos(spec, 2, cy);
    lv_obj_set_style_bg_color(spec, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(spec, LV_OPA_COVER, 0);
    lv_obj_clear_flag(spec, LV_OBJ_FLAG_SCROLLABLE);

    INT32_T bar_slot = (WA_W - 4) / N_SPEC;
    for (INT32_T i = 0; i < N_SPEC; i++) {
        lv_obj_t *bar = lv_obj_create(spec);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, bar_slot - 1, 1);
        lv_obj_set_pos(bar, i * bar_slot, SPEC_H - 1);
        lv_obj_set_style_bg_color(bar, lv_color_hex(WA_SPEC_CLR), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        s_wa.spec_bars[i] = bar;
    }
    cy += SPEC_H + 2;

    /* ----- Transport controls ----- */
    INT32_T bw = 48, bh = 22, bx = 2, bg = 4;
    __wa_btn(w, bx,             cy, bw, bh, "|<<", __wa_prev_cb);
    __wa_btn(w, bx+(bw+bg)*1,  cy, bw, bh, " > ", __wa_play_cb);
    __wa_btn(w, bx+(bw+bg)*2,  cy, bw, bh, "| |", __wa_pause_cb);
    __wa_btn(w, bx+(bw+bg)*3,  cy, bw, bh, "[ ]", __wa_stop_cb);
    __wa_btn(w, bx+(bw+bg)*4,  cy, bw, bh, ">>|", __wa_next_cb);
    cy += bh + 4;

    /* ----- Playlist divider ----- */
    lv_obj_t *div = lv_obj_create(w);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, WA_W - 4, 1);
    lv_obj_set_pos(div, 2, cy);
    lv_obj_set_style_bg_color(div, lv_color_hex(WA_DIM), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    cy += 3;

    /* Playlist header */
    lv_obj_t *plhdr = lv_label_create(w);
    lv_obj_set_style_text_color(plhdr, lv_color_hex(WA_TEXT), 0);
    lv_obj_set_style_text_font(plhdr, &lv_font_unscii_8, 0);
    lv_label_set_text(plhdr, "Playlist");
    lv_obj_set_pos(plhdr, 4, cy);
    cy += 12;

    /* ----- Playlist scrollable list ----- */
    INT32_T pl_h = WA_TOTAL_H - cy - 2;
    lv_obj_t *pl = lv_obj_create(w);
    lv_obj_remove_style_all(pl);
    lv_obj_set_size(pl, WA_W - 4, pl_h);
    lv_obj_set_pos(pl, 2, cy);
    lv_obj_set_style_bg_color(pl, lv_color_hex(WA_PL_BG), 0);
    lv_obj_set_style_bg_opa(pl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pl, lv_color_hex(WA_DIM), 0);
    lv_obj_set_style_border_width(pl, 1, 0);
    lv_obj_set_style_pad_all(pl, 1, 0);
    lv_obj_set_flex_flow(pl, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(pl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pl, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_radius(pl, 0, 0);
    s_wa.pl_list = pl;

    if (s_wa.n_songs == 0) {
        lv_obj_t *em = lv_label_create(pl);
        lv_obj_set_style_text_color(em, lv_color_hex(WA_DIM), 0);
        lv_obj_set_style_text_font(em, &lv_font_unscii_8, 0);
        lv_label_set_text(em, "No .wav files found");
        lv_obj_set_pos(em, 4, 4);
    } else {
        for (INT32_T i = 0; i < s_wa.n_songs; i++) {
            CHAR_T rtxt[80];
            snprintf(rtxt, sizeof(rtxt), " %2d. %s", i+1, s_wa.songs[i].name);
            lv_obj_t *row = lv_obj_create(pl);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, WA_W - 22, 14);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(WA_PL_BG), 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *rl = lv_label_create(row);
            lv_obj_set_style_text_font(rl, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(rl, lv_color_hex(WA_PL_TEXT), 0);
            lv_label_set_text(rl, rtxt);
            lv_obj_set_pos(rl, 2, 2);
            lv_obj_add_event_cb(row, __wa_row_cb, LV_EVENT_CLICKED,
                                 (VOID_T *)(intptr_t)i);
        }
    }

    __wa_update_title();
    __wa_pl_sel();

    s_wa.ui_timer = lv_timer_create(__wa_ui_tick, 300, NULL);
}
