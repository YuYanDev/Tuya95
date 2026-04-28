/**
 * @file win95_disk.c
 * @brief TF card (SDIO) filesystem for 95Simulator.
 *
 * Hardware: P2=CLK P3=CMD P4=D0 P5=D1 P10=D2 P11=D3 P8=CD
 * P10/P11 also serve as UART boot pins, so init is deferred until the
 * desktop has loaded (called from win95_desktop_init at the very end).
 *
 * Uses the TuyaOS tkl_fs layer over the BK7258 FATFS/SDIO driver.
 */
#include "win95_disk.h"

#include "tkl_fs.h"
#include "tal_api.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * File scope state
 * --------------------------------------------------------------------------- */
STATIC volatile BOOL_T  s_mounted     = FALSE;
STATIC THREAD_HANDLE    s_init_thread = NULL;

/* ---------------------------------------------------------------------------
 * Init thread: mounts the SD card asynchronously
 * --------------------------------------------------------------------------- */
STATIC VOID_T __disk_init_thread(VOID_T *arg)
{
    (VOID_T)arg;
    INT_T rt = tkl_fs_mount(WIN95_DISK_MOUNT, DEV_SDCARD);
    if (rt == 0) {
        s_mounted = TRUE;
        PR_NOTICE("Disk: SD card mounted at %s", WIN95_DISK_MOUNT);
    } else {
        PR_WARN("Disk: SD card mount failed: %d", rt);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
VOID_T win95_disk_init(VOID_T)
{
    if (s_init_thread != NULL) {
        return;
    }
    THREAD_CFG_T tcfg = {2048, 4, "w95_disk"};
    OPERATE_RET rt = tal_thread_create_and_start(&s_init_thread, NULL, NULL,
                                                  __disk_init_thread, NULL, &tcfg);
    if (rt != OPRT_OK) {
        PR_ERR("Disk: init thread create failed: %d", rt);
    }
}

BOOL_T win95_disk_is_mounted(VOID_T)
{
    return s_mounted;
}

VOID_T win95_disk_get_info(UINT32_T *total_mb, UINT32_T *free_mb)
{
    if (total_mb) *total_mb = 0;
    if (free_mb)  *free_mb  = 0;
    if (!s_mounted) return;

    /* Total capacity from the SD card driver (sectors of 512B each) */
    BOOL_T is_exist = FALSE;
    tkl_fs_is_exist(WIN95_DISK_MOUNT, &is_exist);
    if (!is_exist) return;

    /* Approximate total via FATFS stat.  tkl_fgetsize returns -1 on dirs,
     * so we rely on the fact that bk_sd_card_get_card_size() gives sectors. */
    /* Walk a dummy file to force FATFS to report stats — fallback: use
     * a known-large constant until a better kernel API is wired up.       */

    /* We use the TUYA_DIR approach to get cluster info via a known pattern. */
    /* For now: report total from a quick read of the volume label entry.    */
    /* Best available portable approach: open root, check if it works.       */

    /* tkl_fgetsize on directories returns -1 on this platform.  Use a
     * conservative estimate: if mounted, report at least 1 MB total.       */
    if (total_mb) *total_mb = 1;   /* will be overwritten if we get real data */

    /* Try to open root dir and count entries to infer card is readable.
     * Real capacity is obtained from bk_sd_card_get_card_size() but that
     * header is platform-private.  We surface 0/0 and let the UI show
     * "Mounted" vs a precise figure — a known limitation.                  */
    TUYA_DIR dir = NULL;
    if (tkl_dir_open(WIN95_DISK_MOUNT, &dir) == 0) {
        tkl_dir_close(dir);
        if (total_mb) *total_mb = 0;   /* unknown exact size */
        if (free_mb)  *free_mb  = 0;
    }
}

UINT32_T win95_disk_list_dir(CONST CHAR_T *path,
                              CHAR_T names[][WIN95_DISK_NAME_MAX],
                              UINT8_T is_dir[],
                              UINT32_T max_cnt)
{
    if (!s_mounted || !path || !names || !is_dir || max_cnt == 0) return 0;

    TUYA_DIR dir = NULL;
    if (tkl_dir_open(path, &dir) != 0) {
        PR_WARN("Disk: opendir failed: %s", path);
        return 0;
    }

    UINT32_T cnt = 0;
    TUYA_FILEINFO info = NULL;
    while (cnt < max_cnt && tkl_dir_read(dir, &info) == 0 && info != NULL) {
        CONST CHAR_T *name = NULL;
        if (tkl_dir_name(info, &name) != 0 || name == NULL) continue;
        /* Skip . and .. */
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        strncpy(names[cnt], name, WIN95_DISK_NAME_MAX - 1);
        names[cnt][WIN95_DISK_NAME_MAX - 1] = '\0';

        BOOL_T d = FALSE;
        tkl_dir_is_directory(info, &d);
        is_dir[cnt] = d ? 1 : 0;
        cnt++;
    }
    tkl_dir_close(dir);
    return cnt;
}

OPERATE_RET win95_disk_create_file(CONST CHAR_T *path)
{
    if (!s_mounted || !path) return OPRT_INVALID_PARM;

    BOOL_T exists = FALSE;
    tkl_fs_is_exist(path, &exists);
    if (exists) return OPRT_COM_ERROR;   /* already exists */

    TUYA_FILE f = tkl_fopen(path, "w");
    if (f == NULL) {
        PR_WARN("Disk: create file failed: %s", path);
        return OPRT_COM_ERROR;
    }
    tkl_fclose(f);
    return OPRT_OK;
}

OPERATE_RET win95_disk_delete(CONST CHAR_T *path)
{
    if (!s_mounted || !path) return OPRT_INVALID_PARM;
    INT_T rt = tkl_fs_remove(path);
    return (rt == 0) ? OPRT_OK : OPRT_COM_ERROR;
}

INT32_T win95_disk_get_size(CONST CHAR_T *path)
{
    if (!s_mounted || !path) return -1;
    return (INT32_T)tkl_fgetsize(path);
}
