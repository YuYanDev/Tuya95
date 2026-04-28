/**
 * @file win95_http11.c
 * @brief HTTP/1.1 client: GET with chunked decode, redirect, HTTP+HTTPS.
 *        Reuses WIN95_HTTP_RESP_T from win95_http10.h.
 */
#include "win95_http11.h"
#include "win95_tls.h"
#include "win95_http10.h"
#include "tal_api.h"
#include "tal_network.h"
#include "tal_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HTTP11_HDR_MAX       2048
#define HTTP11_REDIRECT_MAX  2

/* ---------------------------------------------------------------------------
 * Case-insensitive substring search helper
 * --------------------------------------------------------------------------- */
STATIC CONST CHAR_T *__hdr_find(CONST CHAR_T *hdr, CONST CHAR_T *key)
{
    UINT32_T kl = (UINT32_T)strlen(key);
    UINT32_T hl = (UINT32_T)strlen(hdr);
    for (UINT32_T i = 0; i + kl <= hl; i++) {
        UINT32_T j = 0;
        while (j < kl) {
            CHAR_T a = hdr[i + j], b = key[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (j == kl) return hdr + i;
    }
    return NULL;
}

STATIC INT32_T __strncasecmp(CONST CHAR_T *a, CONST CHAR_T *b, UINT32_T n)
{
    for (UINT32_T i = 0; i < n; i++) {
        CHAR_T ca = a[i];
        CHAR_T cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (CHAR_T)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (CHAR_T)(cb + 32);
        if (ca != cb) return (UCHAR_T)ca - (UCHAR_T)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

STATIC INT32_T __strcasecmp(CONST CHAR_T *a, CONST CHAR_T *b)
{
    UINT32_T n = (UINT32_T)(strlen(a) > strlen(b) ? strlen(a) : strlen(b)) + 1;
    return __strncasecmp(a, b, n);
}

STATIC VOID_T __copy_trimmed(CHAR_T *dst, UINT32_T cap,
                              CONST CHAR_T *src, UINT32_T len)
{
    UINT32_T start = 0;
    while (start < len && (src[start] == ' ' || src[start] == '\t')) {
        start++;
    }
    while (len > start &&
           (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r')) {
        len--;
    }
    if (cap == 0) return;
    if (len <= start) {
        dst[0] = '\0';
        return;
    }
    UINT32_T n = len - start;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src + start, n);
    dst[n] = '\0';
}

STATIC VOID_T __parse_response_headers(CONST CHAR_T *hdr, WIN95_HTTP_RESP_T *out,
                                        INT32_T *content_length, BOOL_T *chunked)
{
    if (content_length) *content_length = -1;
    if (chunked) *chunked = FALSE;

    CONST CHAR_T *sp = hdr;
    UINT32_T code = 0;
    while (*sp && *sp != ' ') sp++;
    if (*sp) sp++;
    while (*sp >= '0' && *sp <= '9') {
        code = code * 10 + (UINT32_T)(*sp - '0');
        sp++;
    }
    out->status_code = (UINT16_T)code;

    CONST CHAR_T *line = hdr;
    while (*line) {
        CONST CHAR_T *end = strstr(line, "\r\n");
        UINT32_T line_len = end ? (UINT32_T)(end - line) : (UINT32_T)strlen(line);
        if (line_len == 0) break;

        CONST CHAR_T *colon = memchr(line, ':', line_len);
        if (colon) {
            UINT32_T key_len = (UINT32_T)(colon - line);
            CONST CHAR_T *val = colon + 1;
            UINT32_T val_len = line_len - key_len - 1;

            if (key_len == 8 && __strncasecmp(line, "Location", 8) == 0) {
                __copy_trimmed(out->location, sizeof(out->location), val, val_len);
                out->has_location = (out->location[0] != '\0');
            } else if (key_len == 12 && __strncasecmp(line, "Content-Type", 12) == 0) {
                __copy_trimmed(out->content_type, sizeof(out->content_type), val, val_len);
                CHAR_T *semi = strchr(out->content_type, ';');
                if (semi) *semi = '\0';
            } else if (key_len == 14 && __strncasecmp(line, "Content-Length", 14) == 0) {
                CHAR_T tmp[24];
                __copy_trimmed(tmp, sizeof(tmp), val, val_len);
                if (content_length) *content_length = atoi(tmp);
            } else if (key_len == 17 && __strncasecmp(line, "Transfer-Encoding", 17) == 0) {
                CHAR_T tmp[64];
                __copy_trimmed(tmp, sizeof(tmp), val, val_len);
                if (chunked && strstr(tmp, "chunked")) *chunked = TRUE;
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

        if (!end) break;
        line = end + 2;
    }
}

/* ---------------------------------------------------------------------------
 * IO abstraction: plain TCP or TLS
 * --------------------------------------------------------------------------- */
typedef struct {
    INT32_T     fd;       /* plain socket, -1 if TLS */
    WIN95_TLS_T *tls;     /* NULL if plain */
} IO_T;

STATIC INT32_T __io_write(IO_T *io, CONST CHAR_T *buf, UINT32_T len)
{
    if (io->tls) return win95_tls_write(io->tls, (CONST UINT8_T *)buf, len);
    return (INT32_T)tal_net_send(io->fd, buf, len);
}

STATIC INT32_T __io_read(IO_T *io, CHAR_T *buf, UINT32_T len)
{
    if (io->tls) return win95_tls_read(io->tls, (UINT8_T *)buf, len);
    return (INT32_T)tal_net_recv(io->fd, buf, len);
}

STATIC VOID_T __io_close(IO_T *io)
{
    if (io->tls) { win95_tls_close(io->tls); io->tls = NULL; }
    if (io->fd >= 0) { tal_net_close(io->fd); io->fd = -1; }
}

/* ---------------------------------------------------------------------------
 * Core GET (one attempt, no redirect)
 * --------------------------------------------------------------------------- */
STATIC OPERATE_RET __http11_request_once(CONST CHAR_T *method,
                                          CONST CHAR_T *host, UINT16_T port,
                                          CONST CHAR_T *path, BOOL_T is_https,
                                          CONST CHAR_T *extra_headers,
                                          CONST uint8_t *body, UINT32_T body_len,
                                          UINT32_T timeout_ms,
                                          WIN95_HTTP_RESP_T *out)
{
    /* Preserve caller-provided pre-allocated buffer across the struct reset. */
    CHAR_T *const ext_buf = out->body_buf;
    UINT32_T const ext_cap = out->body_buf_cap;
    memset(out, 0, sizeof(*out));
    out->body_buf     = ext_buf;
    out->body_buf_cap = ext_cap;

    IO_T io = { -1, NULL };

    if (is_https) {
        io.tls = win95_tls_connect(host, port, (INT32_T)timeout_ms);
        if (!io.tls) {
            PR_ERR("[HTTP11] TLS connect failed %s:%u",
                   host, (UINT32_T)port);
            return OPRT_COM_ERROR;
        }
    } else {
        INT32_T conn_err = 0;
        io.fd = win95_tcp_connect(host, port, timeout_ms, &conn_err);
        if (io.fd < 0) {
            PR_ERR("[HTTP11] tcp_connect %s:%u failed (err=%d)",
                   host, (UINT32_T)port, (int)conn_err);
            return OPRT_SOCK_CONN_ERR;
        }
    }

    CONST CHAR_T *req_method = (method && method[0]) ? method : "GET";
    UINT32_T extra_len = extra_headers ? (UINT32_T)strlen(extra_headers) : 0;
    UINT32_T req_cap = 512 + (UINT32_T)strlen(req_method) +
                       (UINT32_T)strlen(path) + (UINT32_T)strlen(host) +
                       extra_len + 128;
    CHAR_T *req = (CHAR_T *)tal_malloc(req_cap);
    if (!req) {
        __io_close(&io);
        return OPRT_MALLOC_FAILED;
    }
    UINT32_T wi = 0;
    wi += (UINT32_T)snprintf(req + wi, req_cap - wi,
        "%s %s HTTP/1.1\r\n"
        "Host: %s",
        req_method, path, host);
    if ((is_https && port != 443) || (!is_https && port != 80)) {
        wi += (UINT32_T)snprintf(req + wi, req_cap - wi, ":%u", (UINT32_T)port);
    }
    wi += (UINT32_T)snprintf(req + wi, req_cap - wi,
        "\r\n"
        "Connection: close\r\n"
        "User-Agent: Mozilla/4.0 (compatible; MSIE 4.01; Windows 95)\r\n"
        "Accept: text/html, text/plain, image/png, image/jpeg, image/gif, */*\r\n");
    if (body || body_len > 0 || __strcasecmp(req_method, "POST") == 0) {
        wi += (UINT32_T)snprintf(req + wi, req_cap - wi,
                                 "Content-Length: %lu\r\n",
                                 (unsigned long)body_len);
    }
    if (extra_len > 0) {
        UINT32_T copy = extra_len;
        if (wi + copy + 4 >= req_cap) {
            copy = req_cap - wi - 4;
        }
        memcpy(req + wi, extra_headers, copy);
        wi += copy;
        if (wi >= 1 && req[wi - 1] != '\n') {
            req[wi++] = '\r';
            req[wi++] = '\n';
        } else if (wi >= 1 && req[wi - 1] == '\n' && (wi < 2 || req[wi - 2] != '\r')) {
            if (wi + 1 < req_cap) {
                req[wi++] = '\r';
                req[wi++] = '\n';
            }
        }
    }
    req[wi++] = '\r';
    req[wi++] = '\n';

    if (__io_write(&io, req, wi) <= 0) {
        tal_free(req);
        __io_close(&io);
        return OPRT_COM_ERROR;
    }
    tal_free(req);
    if (body && body_len > 0) {
        if (__io_write(&io, (CONST CHAR_T *)body, body_len) <= 0) {
            __io_close(&io);
            return OPRT_COM_ERROR;
        }
    }

    /* Receive headers */
    CHAR_T *hdr_buf = (CHAR_T *)tal_malloc(HTTP11_HDR_MAX + 1);
    if (!hdr_buf) { __io_close(&io); return OPRT_MALLOC_FAILED; }
    UINT32_T hdr_len = 0;

    /* Read until "\r\n\r\n" or buffer full */
    while (hdr_len < HTTP11_HDR_MAX) {
        INT32_T n = __io_read(&io, hdr_buf + hdr_len, 1);
        if (n <= 0) break;
        hdr_len++;
        if (hdr_len >= 4 &&
            hdr_buf[hdr_len - 4] == '\r' && hdr_buf[hdr_len - 3] == '\n' &&
            hdr_buf[hdr_len - 2] == '\r' && hdr_buf[hdr_len - 1] == '\n') {
            break;
        }
    }
    hdr_buf[hdr_len] = '\0';

    INT32_T content_length = -1;
    BOOL_T chunked = FALSE;
    __parse_response_headers(hdr_buf, out, &content_length, &chunked);

    tal_free(hdr_buf);

    /* Read body — use pre-allocated buffer if caller provided one. */
    CHAR_T  *resp_body;
    BOOL_T   body_is_ext = (ext_buf != NULL && ext_cap > 0);
    UINT32_T body_cap;

    if (body_is_ext) {
        resp_body = ext_buf;
        body_cap  = ext_cap - 1; /* reserve one byte for null terminator */
    } else {
        body_cap = 4096;
        if (content_length > 0) {
            body_cap = (UINT32_T)content_length;
            if (body_cap > (UINT32_T)WIN95_HTTP_BODY_MAX) {
                body_cap = (UINT32_T)WIN95_HTTP_BODY_MAX;
            }
        }
        if (body_cap < 1024) {
            body_cap = 1024;
        }
        resp_body = (CHAR_T *)tal_malloc(body_cap + 1);
        if (!resp_body) { __io_close(&io); return OPRT_MALLOC_FAILED; }
    }
    UINT32_T resp_len = 0;

    if (chunked) {
        /* Chunked transfer decode */
        CHAR_T size_line[32];
        while (resp_len < body_cap) {
            /* Read hex chunk-size line */
            UINT32_T sl = 0;
            CHAR_T c;
            while (sl + 1 < sizeof(size_line)) {
                INT32_T n = __io_read(&io, &c, 1);
                if (n <= 0) goto done_reading;
                if (c == '\r') { __io_read(&io, &c, 1); break; }
                size_line[sl++] = c;
            }
            size_line[sl] = '\0';
            UINT32_T chunk_sz = (UINT32_T)strtol(size_line, NULL, 16);
            if (chunk_sz == 0) break; /* last chunk */
            if (resp_len + chunk_sz > (UINT32_T)WIN95_HTTP_BODY_MAX) {
                chunk_sz = (UINT32_T)WIN95_HTTP_BODY_MAX - resp_len;
            }
            if (chunk_sz == 0) {
                goto done_reading;
            }
            if (resp_len + chunk_sz > body_cap) {
                if (body_is_ext) {
                    /* Fixed-size external buffer — cap chunk and stop after */
                    chunk_sz = body_cap - resp_len;
                    if (chunk_sz == 0) goto done_reading;
                } else {
                    UINT32_T new_cap = body_cap;
                    while (resp_len + chunk_sz > new_cap && new_cap < (UINT32_T)WIN95_HTTP_BODY_MAX) {
                        UINT32_T grown = new_cap * 2;
                        new_cap = (grown > (UINT32_T)WIN95_HTTP_BODY_MAX) ?
                                  (UINT32_T)WIN95_HTTP_BODY_MAX : grown;
                    }
                    CHAR_T *nb = (CHAR_T *)tal_realloc(resp_body, new_cap + 1);
                    if (!nb) {
                        tal_free(resp_body);
                        __io_close(&io);
                        return OPRT_MALLOC_FAILED;
                    }
                    resp_body = nb;
                    body_cap = new_cap;
                }
            }
            /* Read chunk data */
            UINT32_T got = 0;
            while (got < chunk_sz) {
                INT32_T n = __io_read(&io, resp_body + resp_len + got, chunk_sz - got);
                if (n <= 0) goto done_reading;
                got += (UINT32_T)n;
            }
            resp_len += got;
            /* Consume trailing CRLF */
            __io_read(&io, &c, 1); /* \r */
            __io_read(&io, &c, 1); /* \n */
        }
    } else if (content_length > 0) {
        /* Read exactly content_length bytes */
        UINT32_T need = (UINT32_T)content_length;
        if (need > (UINT32_T)WIN95_HTTP_BODY_MAX) need = (UINT32_T)WIN95_HTTP_BODY_MAX;
        if (need > body_cap) need = body_cap;
        while (resp_len < need) {
            INT32_T n = __io_read(&io, resp_body + resp_len, need - resp_len);
            if (n <= 0) break;
            resp_len += (UINT32_T)n;
        }
    } else {
        /* Read until connection close */
        while (resp_len < body_cap) {
            if (!body_is_ext && resp_len == body_cap) {
                UINT32_T new_cap = body_cap * 2;
                if (new_cap > (UINT32_T)WIN95_HTTP_BODY_MAX) {
                    new_cap = (UINT32_T)WIN95_HTTP_BODY_MAX;
                }
                CHAR_T *nb = (CHAR_T *)tal_realloc(resp_body, new_cap + 1);
                if (!nb) {
                    tal_free(resp_body);
                    __io_close(&io);
                    return OPRT_MALLOC_FAILED;
                }
                resp_body = nb;
                body_cap = new_cap;
            }
            INT32_T n = __io_read(&io, resp_body + resp_len, body_cap - resp_len);
            if (n <= 0) break;
            resp_len += (UINT32_T)n;
        }
    }

done_reading:
    __io_close(&io);

    resp_body[resp_len] = '\0';
    out->body     = resp_body;
    out->body_len = resp_len;

    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Public: GET with redirect following
 * --------------------------------------------------------------------------- */
OPERATE_RET win95_http11_request(CONST CHAR_T *method,
                                  CONST CHAR_T *host, UINT16_T port,
                                  CONST CHAR_T *path, BOOL_T is_https,
                                  CONST CHAR_T *extra_headers,
                                  CONST uint8_t *body, UINT32_T body_len,
                                  UINT32_T timeout_ms,
                                  WIN95_HTTP_RESP_T *out)
{
    CHAR_T cur_method[8];
    CHAR_T cur_host[WIN95_HTTP_HOST_MAX];
    CHAR_T cur_path[WIN95_HTTP_PATH_MAX];
    UINT16_T cur_port = port;
    BOOL_T   cur_https = is_https;
    CONST uint8_t *cur_body = body;
    UINT32_T cur_body_len = body_len;

    strncpy(cur_method, (method && method[0]) ? method : "GET",
            sizeof(cur_method) - 1);
    cur_method[sizeof(cur_method) - 1] = '\0';
    strncpy(cur_host, host, sizeof(cur_host) - 1); cur_host[sizeof(cur_host)-1] = '\0';
    strncpy(cur_path, path, sizeof(cur_path) - 1); cur_path[sizeof(cur_path)-1] = '\0';

    for (INT32_T hop = 0; hop <= HTTP11_REDIRECT_MAX; hop++) {
        OPERATE_RET rt = __http11_request_once(cur_method, cur_host, cur_port, cur_path,
                                               cur_https, extra_headers,
                                               cur_body, cur_body_len,
                                               timeout_ms, out);
        if (rt != OPRT_OK) return rt;

        /* Redirect? */
        if ((out->status_code == 301 || out->status_code == 302 ||
             out->status_code == 303 || out->status_code == 307) &&
             out->has_location && hop < HTTP11_REDIRECT_MAX) {
            /* Only free body if dynamically allocated (not a pre-alloc buffer). */
            if (out->body && out->body_buf == NULL) { tal_free(out->body); }
            out->body = NULL; out->body_len = 0;
            if (out->location[0] == '/') {
                strncpy(cur_path, out->location, sizeof(cur_path) - 1);
                cur_path[sizeof(cur_path) - 1] = '\0';
            } else {
                if (win95_http10_parse_url(out->location,
                                            cur_host, sizeof(cur_host),
                                            &cur_port,
                                            cur_path, sizeof(cur_path)) != OPRT_OK) {
                    return OPRT_COM_ERROR;
                }
                cur_https = (strncmp(out->location, "https://", 8) == 0) ||
                            (cur_port == 443);
            }
            if (__strcasecmp(cur_method, "GET") != 0 &&
                (out->status_code == 301 || out->status_code == 302 || out->status_code == 303)) {
                strncpy(cur_method, "GET", sizeof(cur_method) - 1);
                cur_method[sizeof(cur_method) - 1] = '\0';
                cur_body = NULL;
                cur_body_len = 0;
            }
            continue;
        }
        return rt;
    }
    return OPRT_COM_ERROR;
}
OPERATE_RET win95_http11_get(CONST CHAR_T *host, UINT16_T port,
                              CONST CHAR_T *path, BOOL_T is_https,
                              UINT32_T timeout_ms,
                              WIN95_HTTP_RESP_T *out)
{
    return win95_http11_request("GET", host, port, path, is_https,
                                NULL, NULL, 0, timeout_ms, out);
}
