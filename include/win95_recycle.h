/**
 * @file win95_recycle.h
 * @brief Win95 Recycle Bin - KV-backed file store with restore/empty operations
 */
#ifndef __WIN95_RECYCLE_H__
#define __WIN95_RECYCLE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

#define KV_KEY_RB_NAME "rb_name"
#define KV_KEY_RB_DATA "rb_data"
#define RB_DATA_MAX    2048

/**
 * @brief Move a named file into the Recycle Bin (overwrites any previous entry).
 * @param[in] name  display filename (e.g. "Untitled.txt")
 * @param[in] data  file content (NUL-terminated string)
 * @return OPRT_OK on success
 */
OPERATE_RET win95_recycle_add(CONST CHAR_T *name, CONST CHAR_T *data);

/**
 * @brief Open the Recycle Bin window.
 */
VOID_T win95_recycle_open(VOID_T);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_RECYCLE_H__ */
