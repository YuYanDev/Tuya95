/**
 * @file win95_tls.c
 * @brief TLS wrapper using Tuya SDK mbedTLS for win95 HTTPS support.
 *        Certificate verification is disabled (IE4-era behaviour).
 */
#include "win95_tls.h"
#include "win95_http10.h"   /* win95_tcp_connect: non-blocking connect helper */
#include "tuya_tls.h"
#include "tal_network.h"
#include "tal_api.h"

#include <string.h>

struct WIN95_TLS_S {
    INT32_T          fd;
    tuya_tls_hander *hdr;
};

/* ---------------------------------------------------------------------------
 * BIO callbacks — wrap tal_net_send / tal_net_recv
 * --------------------------------------------------------------------------- */
STATIC INT32_T __tls_send_cb(VOID_T *ctx, CONST UINT8_T *buf, size_t len)
{
    WIN95_TLS_T *t = (WIN95_TLS_T *)ctx;
    TUYA_ERRNO n = tal_net_send(t->fd, buf, (UINT32_T)len);
    return (INT32_T)n;
}

STATIC INT32_T __tls_recv_cb(VOID_T *ctx, UINT8_T *buf, size_t len)
{
    WIN95_TLS_T *t = (WIN95_TLS_T *)ctx;
    TUYA_ERRNO n = tal_net_recv(t->fd, buf, (UINT32_T)len);
    return (INT32_T)n;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
WIN95_TLS_T *win95_tls_connect(CONST CHAR_T *host, UINT16_T port, INT32_T timeout_ms)
{
    if (!host) {
        return NULL;
    }

    /* Use the unified non-blocking connect helper. It performs DNS via
     * thread-safe lwip_gethostbyname_r, then non-blocking connect + select()
     * with a real timeout, captures errno, and logs every transition. */
    INT32_T conn_err = 0;
    INT32_T fd = win95_tcp_connect(host, port,
                                    (UINT32_T)(timeout_ms > 0 ? timeout_ms : 10000),
                                    &conn_err);
    if (fd < 0) {
        PR_ERR("[TLS] tcp_connect %s:%u failed (err=%d)",
               host, (UINT32_T)port, (int)conn_err);
        return NULL;
    }

    /* Allocate our wrapper */
    WIN95_TLS_T *t = (WIN95_TLS_T *)tal_malloc(sizeof(WIN95_TLS_T));
    if (!t) {
        tal_net_close(fd);
        return NULL;
    }
    t->fd = fd;

    /* Create TLS handler */
    t->hdr = tuya_tls_connect_create();
    if (!t->hdr) {
        PR_ERR("TLS: tuya_tls_connect_create failed");
        tal_net_close(fd);
        tal_free(t);
        return NULL;
    }

    /* Configure: server-cert mode, no verification, custom BIO */
    tuya_tls_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode      = TUYA_TLS_SERVER_CERT_MODE;
    cfg.hostname  = (CHAR_T *)host;
    cfg.port      = port;
    cfg.timeout   = (UINT32_T)(timeout_ms / 1000) + 1;
    cfg.verify    = FALSE;
    cfg.f_send    = (tuya_tls_send_cb)__tls_send_cb;
    cfg.f_recv    = (tuya_tls_recv_cb)__tls_recv_cb;
    cfg.user_data = t;

    if (tuya_tls_config_set(t->hdr, &cfg) != OPRT_OK) {
        PR_ERR("TLS: config_set failed");
        tuya_tls_connect_destroy(t->hdr);
        tal_net_close(fd);
        tal_free(t);
        return NULL;
    }

    /* Perform TLS handshake */
    INT32_T timeout_s = timeout_ms / 1000 + 1;
    if (tuya_tls_connect(t->hdr, (CHAR_T *)host, (INT32_T)port, fd, timeout_s) != OPRT_OK) {
        PR_ERR("TLS: handshake failed for %s", host);
        tuya_tls_connect_destroy(t->hdr);
        tal_net_close(fd);
        tal_free(t);
        return NULL;
    }

    return t;
}

INT32_T win95_tls_write(WIN95_TLS_T *t, CONST UINT8_T *buf, UINT32_T len)
{
    if (!t || !buf) return -1;
    return tuya_tls_write(t->hdr, (UINT8_T *)buf, len);
}

INT32_T win95_tls_read(WIN95_TLS_T *t, UINT8_T *buf, UINT32_T len)
{
    if (!t || !buf) return -1;
    return tuya_tls_read(t->hdr, buf, len);
}

VOID_T win95_tls_close(WIN95_TLS_T *t)
{
    if (!t) return;
    if (t->hdr) {
        tuya_tls_disconnect(t->hdr);
        tuya_tls_connect_destroy(t->hdr);
        t->hdr = NULL;
    }
    if (t->fd >= 0) {
        tal_net_close(t->fd);
        t->fd = -1;
    }
    tal_free(t);
}
