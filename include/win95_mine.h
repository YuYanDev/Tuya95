/**
 * @file win95_mine.h
 * @brief Win95-style Minesweeper game for 95Simulator
 */
#ifndef __WIN95_MINE_H__
#define __WIN95_MINE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"
#include "lv_vendor.h"

/**
 * @brief Open the Minesweeper window.
 * @param[in] parent  Parent screen object (desktop or NULL for lv_layer_top)
 */
VOID_T win95_mine_open(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_MINE_H__ */
