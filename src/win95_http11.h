/**
 * @file win95_http11.h
 * @brief HTTP/1.1 GET client (plain + TLS) for win95 browser.
 */
#ifndef __WIN95_HTTP11_H__
#define __WIN95_HTTP11_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "win95_http10.h"   /* reuses WIN95_HTTP_RESP_T */

OPERATE_RET win95_http11_request(CONST CHAR_T *method,
                                  CONST CHAR_T *host, UINT16_T port,
                                  CONST CHAR_T *path, BOOL_T is_https,
                                  CONST CHAR_T *extra_headers,
                                  CONST uint8_t *body, UINT32_T body_len,
                                  UINT32_T timeout_ms,
                                  WIN95_HTTP_RESP_T *out);

/**
 * @brief Synchronous HTTP/1.1 GET with chunked-encoding and redirect support.
 *        Set is_https=TRUE for HTTPS (port 443 default).
 *        Allocates out->body on success — caller frees with win95_http10_free().
 */
OPERATE_RET win95_http11_get(CONST CHAR_T *host, UINT16_T port,
                              CONST CHAR_T *path, BOOL_T is_https,
                              UINT32_T timeout_ms,
                              WIN95_HTTP_RESP_T *out);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_HTTP11_H__ */
