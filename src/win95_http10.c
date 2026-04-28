/**
 * @file win95_http10.c
 * @brief Raw-socket HTTP/1.0 client. Sends 'GET /path HTTP/1.0', handles Host,
 *        simple Content-Length or connection-close body delivery, and one-hop
 *        3xx Location redirect. Intentionally minimalist, to match the era.
 *
 *        2026-04-27 (v1.2.0): reverted the non-blocking-connect path that was
 *        introduced in v1.1.0. lwIP 2.1.2 vs the toolchain's <errno.h> have
 *        different EINPROGRESS values on this build, which made the post-
 *        connect errno-classification break for *every* host (info.cern.ch
 *        too). The current implementation uses a plain blocking connect with
 *        deterministic SO_RCV/SND timeouts and STA-IP source binding, plus
 *        verbose PR_NOTICE logging at every stage so we can see exactly where
 *        gate.yy.md:8090 fails on the device.
 *
 * @version 1.2.0
 * @date    2026-04-27
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_http10.h"
#include "tal_api.h"
#include "tal_network.h"
#include "tal_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define HTTP_RECV_CHUNK     1024
#define HTTP_HEADER_MAX     4096

/* Sentinel value reported when DNS resolution fails (so callers can
 * distinguish DNS errors from connect failures without dragging in netdb.h). */
#ifndef WIN95_ERR_DNS
#define WIN95_ERR_DNS       (-9001)
#endif

/* ---------------------------------------------------------------------------
 * Thread-safe DNS resolver
 *
 * lwip_gethostbyname() (used by tal_net_gethostbyname) stores its result in
 * static-storage locals (LWIP_DNS_API_HOSTENT_STORAGE=0 in lwipopts.h), so
 * a concurrent call from a Tuya SDK background thread overwrites the buffer
 * and returns a wrong IP -> connect fails -> OPRT_SOCK_CONN_ERR (-13).
 *
 * lwip_gethostbyname_r() writes into CALLER-provided buffers, making it
 * fully thread-safe. The function is already compiled into the firmware via
 * lwip/src/api/netdb.c; we forward-declare it here using a minimal equivalent
 * of struct hostent (same binary layout - 5 pointer/int fields, POSIX standard)
 * to avoid pulling in the lwIP src/include path that is not exposed to app code.
 * --------------------------------------------------------------------------- */
typedef struct {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
} w95_hostent_t;

extern int lwip_gethostbyname_r(const char *name,
                                 w95_hostent_t *ret, char *buf, UINT32_T buflen,
                                 w95_hostent_t **result, int *h_errnop);

/**
 * @brief Resolve hostname to IPv4 host-order address using a per-call buffer.
 * @param[in]  host  DNS name (NUL-terminated)
 * @param[out] addr  IPv4 address in host byte order
 * @return OPRT_OK or OPRT_COM_ERROR
 */
OPERATE_RET win95_dns_resolve(CONST CHAR_T *host, TUYA_IP_ADDR_T *addr)
{
    if (host == NULL || host[0] == '\0' || addr == NULL) {
        return OPRT_INVALID_PARM;
    }

    w95_hostent_t  he_buf;
    CHAR_T         strbuf[384];
    w95_hostent_t *result = NULL;
    INT32_T        herr   = 0;

    INT32_T rc = lwip_gethostbyname_r(host, &he_buf, strbuf,
                                      (UINT32_T)sizeof(strbuf),
                                      &result, &herr);
    if (rc != 0 || result == NULL ||
        result->h_addr_list == NULL || result->h_addr_list[0] == NULL) {
        PR_ERR("[HTTP] DNS failed for %s (rc=%d, h_errno=%d)",
               host, (int)rc, (int)herr);
        return OPRT_COM_ERROR;
    }

    CONST UINT8_T *p = (CONST UINT8_T *)result->h_addr_list[0];
    *addr = ((TUYA_IP_ADDR_T)p[0] << 24) | ((TUYA_IP_ADDR_T)p[1] << 16) |
            ((TUYA_IP_ADDR_T)p[2] <<  8) |  (TUYA_IP_ADDR_T)p[3];
    PR_NOTICE("[HTTP] DNS %s -> %u.%u.%u.%u",
              host, p[0], p[1], p[2], p[3]);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * TCP open helper
 * --------------------------------------------------------------------------- */
/**
 * @brief Best-effort bind the local socket to the WiFi station IP. If the AP
 *        interface is up too, this prevents the kernel from accidentally
 *        routing outbound TCP through the AP NIC (which has no internet).
 *        Failures are logged but ignored.
 */
STATIC VOID_T __bind_station_ip(INT32_T fd)
{
    NW_IP_S ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    if (tal_wifi_get_ip(WF_STATION, &ip_info) != OPRT_OK ||
        ip_info.ip[0] == '\0') {
        PR_DEBUG("[HTTP] no STA ip, skip bind");
        return;
    }
    TUYA_IP_ADDR_T bind_addr = tal_net_str2addr(ip_info.ip);
    if (bind_addr == 0 || bind_addr == 0xFFFFFFFF) {
        PR_DEBUG("[HTTP] STA ip parse failed: %s", ip_info.ip);
        return;
    }
    if (tal_net_bind(fd, bind_addr, 0) != 0) {
        PR_DEBUG("[HTTP] bind to STA ip %s failed", ip_info.ip);
        return;
    }
    PR_DEBUG("[HTTP] bound to STA ip %s", ip_info.ip);
}

/**
 * @brief Open a TCP socket and connect to host:port. Uses a *blocking*
 *        connect (lwIP's only reliable mode on this SDK), with deterministic
 *        SO_RCV/SND timeouts applied so subsequent send/recv don't hang.
 *
 *        Logs PR_NOTICE at every transition (DNS, socket-create, bind,
 *        pre-connect, post-connect) and PR_ERR with the Tuya errno on any
 *        failure so we can pinpoint where a slow / refused / RST'd host fails.
 *
 * @param[in]  host        DNS name
 * @param[in]  port        TCP port
 * @param[in]  timeout_ms  per-IO timeout (recv/send). Connect itself is
 *                         bounded by lwIP's TCP_SYNMAXRTX retry budget.
 * @param[out] err_out     populated on failure: WIN95_ERR_DNS for DNS
 *                         errors, otherwise tal_net_get_errno() result.
 *                         May be NULL.
 * @return     fd >= 0 on success; -1 on failure (socket already closed).
 */
INT32_T win95_tcp_connect(CONST CHAR_T *host, UINT16_T port,
                           UINT32_T timeout_ms, INT32_T *err_out)
{
    if (err_out) {
        *err_out = 0;
    }
    if (host == NULL || host[0] == '\0' || timeout_ms == 0) {
        if (err_out) {
            *err_out = -1;
        }
        return -1;
    }

    PR_NOTICE("[HTTP] connect %s:%u (timeout=%ums)",
              host, (UINT32_T)port, (UINT32_T)timeout_ms);

    TUYA_IP_ADDR_T addr = 0;
    if (win95_dns_resolve(host, &addr) != OPRT_OK || addr == 0) {
        if (err_out) {
            *err_out = WIN95_ERR_DNS;
        }
        return -1;
    }

    INT32_T fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0) {
        TUYA_ERRNO en = tal_net_get_errno();
        PR_ERR("[HTTP] socket() failed: fd=%d (errno=%d)", (int)fd, (int)en);
        if (err_out) {
            *err_out = (INT32_T)en;
        }
        return -1;
    }
    PR_DEBUG("[HTTP] socket fd=%d", (int)fd);

    __bind_station_ip(fd);

    /* Apply send/recv timeouts BEFORE connect so any delayed I/O on the
     * established socket can't hang the worker thread. lwIP ignores these
     * for connect itself, which is fine - blocking connect on lwIP returns
     * on TCP_SYNMAXRTX retry exhaustion (~30s). The HTTP worker thread is
     * dedicated and won't impact the UI. */
    tal_net_set_timeout(fd, (INT32_T)timeout_ms, TRANS_SEND);
    tal_net_set_timeout(fd, (INT32_T)timeout_ms, TRANS_RECV);

    PR_DEBUG("[HTTP] calling tal_net_connect %s:%u ...",
             host, (UINT32_T)port);
    INT32_T rc = tal_net_connect(fd, addr, port);
    if (rc != 0) {
        TUYA_ERRNO en = tal_net_get_errno();
        PR_ERR("[HTTP] connect %s:%u failed: rc=%d errno=%d",
               host, (UINT32_T)port, (int)rc, (int)en);
        if (err_out) {
            *err_out = (INT32_T)en;
        }
        tal_net_close(fd);
        return -1;
    }
    PR_NOTICE("[HTTP] connect ok %s:%u (fd=%d)",
              host, (UINT32_T)port, (int)fd);
    return fd;
}

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Case-insensitive strncasecmp.
 */
STATIC INT32_T __strncasecmp(CONST CHAR_T *a, CONST CHAR_T *b, UINT32_T n)
{
    for (UINT32_T i = 0; i < n; i++) {
        CHAR_T ca = a[i];
        CHAR_T cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (CHAR_T)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (CHAR_T)(cb + 32);
        }
        if (ca != cb) {
            return (UCHAR_T)ca - (UCHAR_T)cb;
        }
        if (ca == '\0') {
            return 0;
        }
    }
    return 0;
}

OPERATE_RET win95_http10_parse_url(CONST CHAR_T *url,
                                    CHAR_T *host_buf, UINT32_T host_cap,
                                    UINT16_T *port_out,
                                    CHAR_T *path_buf, UINT32_T path_cap)
{
    if (url == NULL || host_buf == NULL || port_out == NULL || path_buf == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (host_cap == 0 || path_cap == 0) {
        return OPRT_INVALID_PARM;
    }

    host_buf[0] = '\0';
    path_buf[0] = '/';
    path_buf[1] = '\0';
    *port_out = 80;

    CONST CHAR_T *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *port_out = 443;
    }

    CONST CHAR_T *slash = strchr(p, '/');
    CONST CHAR_T *colon = strchr(p, ':');
    if (colon && slash && colon > slash) {
        colon = NULL;
    }

    UINT32_T hlen = 0;
    if (colon) {
        hlen = (UINT32_T)(colon - p);
    } else if (slash) {
        hlen = (UINT32_T)(slash - p);
    } else {
        hlen = (UINT32_T)strlen(p);
    }
    if (hlen >= host_cap) {
        hlen = host_cap - 1;
    }
    memcpy(host_buf, p, hlen);
    host_buf[hlen] = '\0';

    if (colon) {
        UINT32_T port_val = 0;
        CONST CHAR_T *pp = colon + 1;
        while (*pp >= '0' && *pp <= '9') {
            port_val = port_val * 10 + (UINT32_T)(*pp - '0');
            pp++;
        }
        if (port_val > 0 && port_val < 65536) {
            *port_out = (UINT16_T)port_val;
        }
    }

    if (slash) {
        UINT32_T plen = (UINT32_T)strlen(slash);
        if (plen >= path_cap) {
            plen = path_cap - 1;
        }
        memcpy(path_buf, slash, plen);
        path_buf[plen] = '\0';
    }

    return OPRT_OK;
}

/**
 * @brief Close socket if valid.
 */
STATIC VOID_T __sock_close(INT32_T fd)
{
    if (fd >= 0) {
        tal_net_close(fd);
    }
}

/**
 * @brief Parse the HTTP status line "HTTP/1.x NNN ...". Writes status code.
 * @return TRUE if parsed.
 */
STATIC BOOL_T __parse_status_line(CONST CHAR_T *line, UINT16_T *code)
{
    if (__strncasecmp(line, "HTTP/1.", 7) != 0) {
        return FALSE;
    }
    CONST CHAR_T *sp = strchr(line, ' ');
    if (sp == NULL) {
        return FALSE;
    }
    UINT32_T c = 0;
    CONST CHAR_T *p = sp + 1;
    while (*p >= '0' && *p <= '9') {
        c = c * 10 + (UINT32_T)(*p - '0');
        p++;
    }
    *code = (UINT16_T)c;
    return TRUE;
}

STATIC VOID_T __copy_trimmed(CHAR_T *dst, UINT32_T cap,
                              CONST CHAR_T *src, UINT32_T len)
{
    UINT32_T start = 0;
    while (start < len && (src[start] == ' ' || src[start] == '\t')) {
        start++;
    }
    while (len > start &&
           (src[len - 1] == ' ' || src[len - 1] == '\t' ||
            src[len - 1] == '\r')) {
        len--;
    }
    if (cap == 0) {
        return;
    }
    if (len <= start) {
        dst[0] = '\0';
        return;
    }
    UINT32_T n = len - start;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src + start, n);
    dst[n] = '\0';
}

STATIC VOID_T __parse_headers_block(CONST CHAR_T *hdr, UINT32_T hdr_len,
                                     WIN95_HTTP_RESP_T *out)
{
    UINT32_T i = 0;
    while (i < hdr_len) {
        UINT32_T line_start = i;
        while (i < hdr_len && hdr[i] != '\n') {
            i++;
        }
        UINT32_T line_end = i;
        if (i < hdr_len && hdr[i] == '\n') {
            i++;
        }
        if (line_end > line_start && hdr[line_end - 1] == '\r') {
            line_end--;
        }
        if (line_end <= line_start) {
            continue;
        }

        CONST CHAR_T *line = &hdr[line_start];
        UINT32_T line_len = line_end - line_start;
        CONST CHAR_T *colon = memchr(line, ':', line_len);
        if (!colon) {
            continue;
        }
        UINT32_T key_len = (UINT32_T)(colon - line);
        CONST CHAR_T *val = colon + 1;
        UINT32_T val_len = line_len - key_len - 1;

        if (key_len == 8 && __strncasecmp(line, "Location", 8) == 0) {
            __copy_trimmed(out->location, sizeof(out->location), val, val_len);
            out->has_location = (out->location[0] != '\0');
        } else if (key_len == 12 && __strncasecmp(line, "Content-Type", 12) == 0) {
            __copy_trimmed(out->content_type, sizeof(out->content_type),
                           val, val_len);
            CHAR_T *semi = strchr(out->content_type, ';');
            if (semi) {
                *semi = '\0';
            }
        } else if (key_len == 10 && __strncasecmp(line, "Set-Cookie", 10) == 0) {
            if (out->set_cookie_count < WIN95_HTTP_COOKIE_MAX) {
                __copy_trimmed(out->set_cookie[out->set_cookie_count],
                               WIN95_HTTP_COOKIE_LEN, val, val_len);
                if (out->set_cookie[out->set_cookie_count][0] != '\0') {
                    out->set_cookie_count++;
                }
            }
        }
    }
}

/**
 * @brief Find first occurrence of needle in hay (bounded, not null-terminated).
 */
STATIC INT32_T __memfind(CONST CHAR_T *hay, UINT32_T hay_len,
                          CONST CHAR_T *needle, UINT32_T needle_len)
{
    if (needle_len == 0 || hay_len < needle_len) {
        return -1;
    }
    for (UINT32_T i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(&hay[i], needle, needle_len) == 0) {
            return (INT32_T)i;
        }
    }
    return -1;
}

/**
 * @brief Send all bytes in [buf, buf+len). Returns 0 on success, <0 on error.
 */
STATIC INT32_T __send_all(INT32_T fd, CONST CHAR_T *buf, UINT32_T len)
{
    UINT32_T sent = 0;
    while (sent < len) {
        INT32_T n = tal_net_send(fd, buf + sent, len - sent);
        if (n <= 0) {
            PR_ERR("[HTTP10] send failed at %u/%u (errno=%d)",
                   sent, len, (int)tal_net_get_errno());
            return -1;
        }
        sent += (UINT32_T)n;
    }
    return 0;
}

/**
 * @brief Do one GET, no redirect handling. Used internally.
 */
STATIC OPERATE_RET __http10_get_once(CONST CHAR_T *host, UINT16_T port,
                                       CONST CHAR_T *path, UINT32_T timeout_ms,
                                       WIN95_HTTP_RESP_T *out)
{
    INT32_T fd = -1;
    CHAR_T *buf = NULL;
    UINT32_T buf_cap = HTTP_RECV_CHUNK * 4;
    UINT32_T buf_len = 0;

    /* Preserve caller-provided pre-allocated buffer across the struct reset. */
    CHAR_T *const ext_buf = out->body_buf;
    UINT32_T const ext_cap = out->body_buf_cap;
    memset(out, 0, sizeof(*out));
    out->body_buf     = ext_buf;
    out->body_buf_cap = ext_cap;

    INT32_T conn_err = 0;
    fd = win95_tcp_connect(host, port, timeout_ms, &conn_err);
    if (fd < 0) {
        PR_ERR("[HTTP10] tcp_connect %s:%u failed (err=%d)",
               host, (UINT32_T)port, (int)conn_err);
        return OPRT_SOCK_CONN_ERR;
    }

    /* Build + send the request.
     * Include port in Host for non-standard ports (RFC 7230 §5.4). */
    CHAR_T host_hdr[WIN95_HTTP_HOST_MAX + 8];
    if (port == 80) {
        strncpy(host_hdr, host, sizeof(host_hdr) - 1);
        host_hdr[sizeof(host_hdr) - 1] = '\0';
    } else {
        snprintf(host_hdr, sizeof(host_hdr), "%s:%u",
                 host, (UINT32_T)port);
    }
    CHAR_T req[512];
    INT32_T n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/2.0 (compatible; MSIE 2.0; TuyaOS95; 16bit)\r\n"
        "Accept: text/html, text/plain, */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host_hdr);
    if (n <= 0 || n >= (INT32_T)sizeof(req)) {
        __sock_close(fd);
        return OPRT_COM_ERROR;
    }
    if (__send_all(fd, req, (UINT32_T)n) != 0) {
        __sock_close(fd);
        return OPRT_SOCK_ERR;
    }
    PR_DEBUG("[HTTP10] sent %d bytes of request to %s:%u",
             (int)n, host, (UINT32_T)port);

    /* Use caller's pre-allocated buffer if provided; otherwise malloc. */
    if (ext_buf && ext_cap > 0) {
        buf     = ext_buf;
        buf_cap = ext_cap;
    } else {
        buf = (CHAR_T *)tal_malloc(buf_cap);
        if (buf == NULL) {
            __sock_close(fd);
            return OPRT_MALLOC_FAILED;
        }
    }

    /* Recv loop. HTTP/1.0 with Connection: close means read until EOF. */
    for (;;) {
        if (buf_len + HTTP_RECV_CHUNK + 1 > buf_cap) {
            if (ext_buf) {
                break;
            }
            if (buf_cap >= WIN95_HTTP_BODY_MAX + HTTP_HEADER_MAX) {
                break;
            }
            UINT32_T new_cap = buf_cap * 2;
            if (new_cap > WIN95_HTTP_BODY_MAX + HTTP_HEADER_MAX) {
                new_cap = WIN95_HTTP_BODY_MAX + HTTP_HEADER_MAX;
            }
            CHAR_T *nb = (CHAR_T *)tal_realloc(buf, new_cap);
            if (nb == NULL) {
                break;
            }
            buf = nb;
            buf_cap = new_cap;
        }

        INT32_T got = tal_net_recv(fd, &buf[buf_len],
                                    buf_cap - buf_len - 1);
        if (got <= 0) {
            break;
        }
        buf_len += (UINT32_T)got;
        if (buf_len >= buf_cap - 1) {
            break;
        }
    }
    buf[buf_len] = '\0';
    __sock_close(fd);

    PR_NOTICE("[HTTP10] %s:%u -> %u bytes received",
              host, (UINT32_T)port, buf_len);

    if (buf_len == 0) {
        if (!ext_buf) {
            tal_free(buf);
        }
        return OPRT_COM_ERROR;
    }

    /* Split header / body at first \r\n\r\n (fallback \n\n). */
    INT32_T sep = __memfind(buf, buf_len, "\r\n\r\n", 4);
    UINT32_T body_ofs = 0;
    if (sep >= 0) {
        body_ofs = (UINT32_T)sep + 4;
    } else {
        sep = __memfind(buf, buf_len, "\n\n", 2);
        if (sep >= 0) {
            body_ofs = (UINT32_T)sep + 2;
        } else {
            body_ofs = buf_len;
        }
    }

    UINT16_T status_code = 0;
    __parse_status_line(buf, &status_code);
    out->status_code = status_code;

    if (sep >= 0) {
        __parse_headers_block(buf, (UINT32_T)sep, out);
    }

    UINT32_T body_len = buf_len - body_ofs;
    if (body_len > WIN95_HTTP_BODY_MAX) {
        body_len = WIN95_HTTP_BODY_MAX;
    }

    if (body_ofs > 0 && body_len > 0) {
        memmove(buf, &buf[body_ofs], body_len);
    }
    buf[body_len] = '\0';

    out->body = buf;
    out->body_len = body_len;
    return OPRT_OK;
}

OPERATE_RET win95_http10_get(CONST CHAR_T *host, UINT16_T port,
                              CONST CHAR_T *path, UINT32_T timeout_ms,
                              WIN95_HTTP_RESP_T *out)
{
    if (host == NULL || path == NULL || out == NULL) {
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = __http10_get_once(host, port, path, timeout_ms, out);
    if (rt != OPRT_OK) {
        return rt;
    }

    /* One-hop redirect follow. */
    if (out->status_code >= 300 && out->status_code < 400 && out->has_location) {
        CHAR_T next_host[WIN95_HTTP_HOST_MAX];
        CHAR_T next_path[WIN95_HTTP_PATH_MAX];
        UINT16_T next_port = 80;

        if (out->location[0] == '/') {
            strncpy(next_host, host, sizeof(next_host) - 1);
            next_host[sizeof(next_host) - 1] = '\0';
            strncpy(next_path, out->location, sizeof(next_path) - 1);
            next_path[sizeof(next_path) - 1] = '\0';
            next_port = port;
        } else {
            if (win95_http10_parse_url(out->location,
                    next_host, sizeof(next_host),
                    &next_port,
                    next_path, sizeof(next_path)) != OPRT_OK) {
                return rt;
            }
            if (next_port == 443) {
                PR_WARN("[HTTP10] HTTPS redirect not supported: %s",
                        out->location);
                return rt;
            }
        }

        win95_http10_free(out);
        return __http10_get_once(next_host, next_port, next_path,
                                 timeout_ms, out);
    }

    return rt;
}

VOID_T win95_http10_free(WIN95_HTTP_RESP_T *resp)
{
    if (resp == NULL) {
        return;
    }
    if (resp->body && resp->body_buf == NULL) {
        tal_free(resp->body);
    }
    resp->body = NULL;
    resp->body_len = 0;
    resp->has_location = FALSE;
    resp->location[0] = '\0';
    resp->content_type[0] = '\0';
    resp->set_cookie_count = 0;
    /* body_buf and body_buf_cap are NOT cleared - owned by the caller. */
}
