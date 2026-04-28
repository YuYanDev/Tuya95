/**
 * @file win95_js.h
 * @brief Minimal JavaScript interpreter for Tuya Navigator (IE4-level subset)
 *        Supports: var/let/const, if/else, for/while, functions, closures,
 *        document.write, alert, Math, String methods, typeof, etc.
 */
#ifndef __WIN95_JS_H__
#define __WIN95_JS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

#define JS_WBUF_MAX    8192
#define JS_ALERT_MAX   256
#define JS_TITLE_MAX   128
#define JS_URL_MAX     256

/** Results accumulated by a win95_js_run() call. */
typedef struct {
    CHAR_T  *write_buf;                  /* document.write() output (heap, may be NULL) */
    UINT32_T write_len;
    BOOL_T   has_alert;
    CHAR_T   alert_msg[JS_ALERT_MAX];
    BOOL_T   has_title;
    CHAR_T   title[JS_TITLE_MAX];
    BOOL_T   has_navigate;
    CHAR_T   navigate_url[JS_URL_MAX];
    INT32_T  tz_offset_minutes;          /* timezone offset for new Date() */
} WIN95_JS_RESULT_T;

/**
 * @brief Execute a JS snippet.
 *        result->write_buf is heap-allocated lazily; free with win95_js_result_free().
 *        Caller must memset(result, 0, sizeof(*result)) before calling.
 */
VOID_T win95_js_run(CONST CHAR_T *script, WIN95_JS_RESULT_T *result);

/** @brief Free heap resources inside result (does not free result itself). */
VOID_T win95_js_result_free(WIN95_JS_RESULT_T *result);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_JS_H__ */
