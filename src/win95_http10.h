/**
 * @file win95_http10.h
 * @brief Retro HTTP/1.0 client over raw sockets (for IE 1.0/2.0 simulation)
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __WIN95_HTTP10_H__
#define __WIN95_HTTP10_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

#define WIN95_HTTP_BODY_MAX     (16 * 1024)
/* PAGE_BUF_MAX: pre-alloc size for HTTP/1.0 recv (body + headers headroom). */
#define WIN95_HTTP_PAGE_BUF_MAX (WIN95_HTTP_BODY_MAX + 4096)
/* IMG_BODY_MAX: pre-alloc size per image slot (retro images are small). */
#define WIN95_HTTP_IMG_BODY_MAX (8 * 1024)

#define WIN95_HTTP_HOST_MAX     128
#define WIN95_HTTP_PATH_MAX     256
#define WIN95_HTTP_CTYPE_MAX    96
#define WIN95_HTTP_COOKIE_MAX   8
#define WIN95_HTTP_COOKIE_LEN   160

typedef struct {
    UINT16_T status_code;
    CHAR_T  *body;          /* body data; points into body_buf if pre-allocated */
    UINT32_T body_len;      /* actual data length */
    CHAR_T  *body_buf;      /* pre-allocated buffer (NULL = dynamic tal_malloc) */
    UINT32_T body_buf_cap;  /* capacity of body_buf; 0 if dynamic */
    CHAR_T   location[256]; /* set on 30x redirects */
    BOOL_T   has_location;
    CHAR_T   content_type[WIN95_HTTP_CTYPE_MAX];
    CHAR_T   set_cookie[WIN95_HTTP_COOKIE_MAX][WIN95_HTTP_COOKIE_LEN];
    UINT8_T  set_cookie_count;
} WIN95_HTTP_RESP_T;

/**
 * @brief Synchronous HTTP/1.0 GET. Closes the connection after response.
 *        If out->body_buf is non-NULL and out->body_buf_cap > 0, that buffer
 *        is used for recv and body will NOT be freed by win95_http10_free().
 *        Otherwise allocates with tal_malloc; caller must call win95_http10_free().
 */
OPERATE_RET win95_http10_get(CONST CHAR_T *host, UINT16_T port,
                              CONST CHAR_T *path, UINT32_T timeout_ms,
                              WIN95_HTTP_RESP_T *out);

/**
 * @brief Free the dynamic body buffer inside a response.
 *        Does NOT free body if body_buf is set (externally managed).
 *        Always safe to call multiple times.
 */
VOID_T win95_http10_free(WIN95_HTTP_RESP_T *resp);

/**
 * @brief Parse an http URL into host, port, path.
 */
OPERATE_RET win95_http10_parse_url(CONST CHAR_T *url,
                                    CHAR_T *host_buf, UINT32_T host_cap,
                                    UINT16_T *port_out,
                                    CHAR_T *path_buf, UINT32_T path_cap);

/**
 * @brief Thread-safe DNS resolver (uses lwip_gethostbyname_r with stack buffers).
 *        Returns OPRT_OK and sets *addr (host byte order) on success.
 */
OPERATE_RET win95_dns_resolve(CONST CHAR_T *host, TUYA_IP_ADDR_T *addr);

/**
 * @brief Open a TCP socket and (blocking) connect to host:port. Resolves DNS
 *        with the thread-safe resolver, best-effort binds the source IP to
 *        the WiFi station NIC (so the AP NIC isn't accidentally chosen when
 *        running in dual mode), applies SO_RCV/SND timeouts, then returns
 *        the connected fd. Logs at every step.
 *
 * @param[in]  host        DNS name (resolved internally)
 * @param[in]  port        TCP port
 * @param[in]  timeout_ms  per-IO (recv/send) timeout in milliseconds
 * @param[out] err_out     If non-NULL, populated on failure: WIN95_ERR_DNS
 *                         (-9001) for DNS resolution failures, otherwise the
 *                         tal_net_get_errno() value at the point of failure.
 * @return     fd >= 0 on success, -1 on failure (socket already closed)
 *
 * @note A non-blocking connect + select() implementation was tried and reverted:
 *       lwIP's LWIP_PROVIDE_ERRNO produces values that don't always match the
 *       toolchain's <errno.h>, which made post-connect EINPROGRESS recognition
 *       unreliable. Using the SDK's blocking connect gives deterministic
 *       behaviour for every host, at the cost of up to TCP_SYNMAXRTX retries
 *       worth of latency for completely unreachable hosts.
 */
INT32_T win95_tcp_connect(CONST CHAR_T *host, UINT16_T port,
                           UINT32_T timeout_ms, INT32_T *err_out);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_HTTP10_H__ */
