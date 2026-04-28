/**
 * @file win95_net.h
 * @brief Network Neighborhood window — WiFi AP scan + LAN device discovery.
 */
#ifndef __WIN95_NET_H__
#define __WIN95_NET_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"

/**
 * @brief Open the Network Neighborhood window.
 * @param[in] parent  desktop or parent object
 */
VOID_T win95_net_open(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_NET_H__ */
