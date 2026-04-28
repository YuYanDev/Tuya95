/**
 * @file win95_tls.h
 * @brief Thin TLS wrapper (uses Tuya SDK's mbedTLS layer) for win95 HTTPS.
 */
#ifndef __WIN95_TLS_H__
#define __WIN95_TLS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

typedef struct WIN95_TLS_S WIN95_TLS_T;

/**
 * @brief Open a TCP connection and perform TLS handshake (no cert verify).
 * @return opaque handle, or NULL on failure.
 */
WIN95_TLS_T *win95_tls_connect(CONST CHAR_T *host, UINT16_T port, INT32_T timeout_ms);

/**
 * @brief Write bytes over TLS. Returns bytes written or <0 on error.
 */
INT32_T win95_tls_write(WIN95_TLS_T *t, CONST UINT8_T *buf, UINT32_T len);

/**
 * @brief Read bytes from TLS. Returns bytes read, 0 on closed, <0 on error.
 */
INT32_T win95_tls_read(WIN95_TLS_T *t, UINT8_T *buf, UINT32_T len);

/**
 * @brief Close and free a TLS connection.
 */
VOID_T win95_tls_close(WIN95_TLS_T *t);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_TLS_H__ */
