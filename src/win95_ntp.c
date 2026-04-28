/**
 * @file win95_ntp.c
 * @brief Minimal UDP NTP client for Win95 desktop clock sync.
 *        Sends a 48-byte NTP client packet to hard-coded server IPs (no DNS),
 *        extracts the transmit timestamp, and calls tal_time_set_posix().
 *        DNS is intentionally avoided to prevent races with HTTP threads.
 * @version 1.0.0
 * @date 2026-04-22
 * @copyright Copyright (c) Tuya Inc.
 */
#include "win95_ntp.h"

#include "tal_api.h"
#include "tal_network.h"
#include "tal_time_service.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define NTP_PORT        123
#define NTP_TIMEOUT_MS  5000
/* Seconds between NTP epoch (Jan 1 1900) and POSIX epoch (Jan 1 1970) */
#define NTP_DELTA       2208988800UL

/* Hard-coded NTP server IPs — DNS is intentionally skipped in the thread to
 * avoid racing with concurrent HTTP gethostbyname calls (lwIP uses a static
 * result buffer that is NOT thread-safe).  Both addresses are stable anycast
 * pools maintained by Cloudflare and Google respectively. */
#define NTP_IP_CLOUDFLARE ((162UL << 24) | (159UL << 16) | (200UL << 8) | 1UL)
#define NTP_IP_GOOGLE     ((216UL << 24) | (239UL << 16) | ( 35UL <<  8) | 0UL)
/* Keep old name for the primary choice */
#define NTP_FALLBACK_IP   NTP_IP_CLOUDFLARE

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC volatile BOOL_T  s_synced     = FALSE;
STATIC THREAD_HANDLE    s_ntp_thread = NULL;

/* ---------------------------------------------------------------------------
 * NTP thread
 * --------------------------------------------------------------------------- */
/* Try one NTP server IP; returns TRUE on successful sync. */
STATIC BOOL_T __ntp_try(TUYA_IP_ADDR_T addr)
{
    uint8_t pkt[48];
    uint8_t resp[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;

    INT32_T fd = tal_net_socket_create(PROTOCOL_UDP);
    if (fd < 0) {
        PR_ERR("NTP: socket create failed");
        return FALSE;
    }
    tal_net_set_timeout(fd, NTP_TIMEOUT_MS, TRANS_RECV);

    BOOL_T ok = FALSE;
    if (tal_net_send_to(fd, pkt, 48, addr, NTP_PORT) == 48) {
        TUYA_IP_ADDR_T from_addr = 0;
        uint16_t from_port = 0;
        INT32_T got = tal_net_recvfrom(fd, resp, 48, &from_addr, &from_port);
        if (got >= 44) {
            uint32_t ntp_secs = ((uint32_t)resp[40] << 24)
                              | ((uint32_t)resp[41] << 16)
                              | ((uint32_t)resp[42] <<  8)
                              |  (uint32_t)resp[43];
            if (ntp_secs > NTP_DELTA) {
                TIME_T posix = (TIME_T)(ntp_secs - NTP_DELTA);
                if (tal_time_set_posix(posix, 0) == OPRT_OK) {
                    s_synced = TRUE;
                    PR_NOTICE("NTP sync OK: posix=%lu", (unsigned long)posix);
                    ok = TRUE;
                }
            } else {
                PR_ERR("NTP: implausible timestamp %lu", (unsigned long)ntp_secs);
            }
        } else {
            PR_ERR("NTP: short response (%d)", got);
        }
    } else {
        PR_ERR("NTP: send failed");
    }

    tal_net_close(fd);
    return ok;
}

STATIC VOID_T __ntp_thread(VOID_T *arg)
{
    (VOID_T)arg;

    /* Use hard-coded IPs — no DNS in this thread.
     * lwIP gethostbyname() writes to a static struct that is shared across
     * threads; calling it here would race with concurrent HTTP DNS lookups
     * and corrupt the address returned to the browser (causing -13 errors). */
    static const TUYA_IP_ADDR_T s_ntp_ips[] = {
        (TUYA_IP_ADDR_T)NTP_IP_CLOUDFLARE,
        (TUYA_IP_ADDR_T)NTP_IP_GOOGLE,
    };
    for (UINT32_T i = 0; i < sizeof(s_ntp_ips) / sizeof(s_ntp_ips[0]); i++) {
        if (__ntp_try(s_ntp_ips[i])) {
            break;
        }
    }

    s_ntp_thread = NULL;
    tal_thread_delete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
void win95_ntp_trigger(void)
{
    if (s_synced || s_ntp_thread != NULL) {
        return;
    }
    THREAD_CFG_T cfg = {2048, 3, "w95_ntp"};
    OPERATE_RET rt = tal_thread_create_and_start(&s_ntp_thread, NULL, NULL,
                                                  __ntp_thread, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("NTP: thread create failed: %d", rt);
        s_ntp_thread = NULL;
    }
}

bool win95_ntp_synced(void)
{
    return (bool)s_synced;
}
