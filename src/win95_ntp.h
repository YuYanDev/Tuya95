/**
 * @file win95_ntp.h
 * @brief Win95 NTP time synchronization declarations
 */
#ifndef __WIN95_NTP_H__
#define __WIN95_NTP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/**
 * @brief Spawn NTP sync thread (no-op if already synced or running).
 */
void win95_ntp_trigger(void);

/**
 * @brief Returns true once NTP sync has succeeded.
 */
bool win95_ntp_synced(void);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_NTP_H__ */
