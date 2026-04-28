/**
 * @file win95_disk.h
 * @brief TF card (SDIO) filesystem support for 95Simulator.
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_DISK_H__
#define __WIN95_DISK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

#define WIN95_DISK_MOUNT    "/sdcard"
#define WIN95_DISK_NAME_MAX 64
#define WIN95_DISK_LIST_MAX 32

/**
 * @brief Async-init: spawns a thread to mount the SD card.
 *        Safe to call only after the desktop has loaded (P10/P11 UART boot phase).
 */
VOID_T win95_disk_init(VOID_T);

/** @return TRUE if the SD card is mounted and accessible. */
BOOL_T win95_disk_is_mounted(VOID_T);

/**
 * @brief Get disk capacity.
 * @param[out] total_mb total capacity in MiB (0 if not mounted)
 * @param[out] free_mb  free space in MiB   (0 if not mounted or unavailable)
 */
VOID_T win95_disk_get_info(UINT32_T *total_mb, UINT32_T *free_mb);

/**
 * @brief List directory entries.
 * @param[in]  path     absolute path (e.g. "/sdcard" or "/sdcard/subdir")
 * @param[out] names    array of entry name strings
 * @param[out] is_dir   1 if entry is a directory, 0 if file
 * @param[in]  max_cnt  capacity of names/is_dir arrays
 * @return number of entries populated (0 on error or empty dir)
 */
UINT32_T win95_disk_list_dir(CONST CHAR_T *path,
                              CHAR_T names[][WIN95_DISK_NAME_MAX],
                              UINT8_T is_dir[],
                              UINT32_T max_cnt);

/**
 * @brief Create a new empty file (fails if file exists).
 * @return OPRT_OK on success
 */
OPERATE_RET win95_disk_create_file(CONST CHAR_T *path);

/**
 * @brief Delete a file or empty directory.
 * @return OPRT_OK on success
 */
OPERATE_RET win95_disk_delete(CONST CHAR_T *path);

/**
 * @brief Get file size in bytes.
 * @return size >= 0, or -1 on error
 */
INT32_T win95_disk_get_size(CONST CHAR_T *path);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_DISK_H__ */
