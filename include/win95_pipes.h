/**
 * @file win95_pipes.h
 * @brief 3D Pipes screensaver for Win95 95Simulator
 */
#ifndef __WIN95_PIPES_H__
#define __WIN95_PIPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bios_simulator.h"
#include "lv_vendor.h"

/**
 * @brief Start the pipes screensaver animation.
 *        Creates a full-screen modal host on the top layer with a canvas
 *        covering the full display.
 */
VOID_T win95_pipes_start(VOID_T);

/**
 * @brief Stop and destroy the pipes screensaver canvas.
 */
VOID_T win95_pipes_stop(VOID_T);

/**
 * @brief Start or restart an embedded 3D pipes preview inside @p parent.
 *        The preview fills the parent's current size.
 */
VOID_T win95_pipes_preview_start(lv_obj_t *parent);

/**
 * @brief Stop and destroy the embedded preview canvas.
 */
VOID_T win95_pipes_preview_stop(VOID_T);

/**
 * @brief Set the idle timeout before screensaver activates.
 * @param[in] minutes  0 = disabled ("Never"), otherwise minutes of idle time.
 */
VOID_T win95_pipes_set_timeout(UINT32_T minutes);

/**
 * @brief Called from the desktop clock tick to advance idle counter.
 *        Must be called every second from the LVGL task.
 */
VOID_T win95_pipes_tick(VOID_T);

/**
 * @brief Reset the idle counter (call on any touch/input event).
 */
VOID_T win95_pipes_reset_idle(VOID_T);

#ifdef __cplusplus
}
#endif
#endif /* __WIN95_PIPES_H__ */
