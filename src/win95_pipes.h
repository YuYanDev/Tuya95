/**
 * @file win95_pipes.h
 * @brief 3D Pipes screensaver — public API.
 */
#pragma once

#include "tuya_cloud_types.h"
#include "lv_vendor.h"

/**
 * @brief Start the 3D pipes screensaver with a full-screen modal canvas.
 *        If already running, this is a no-op.
 */
VOID_T win95_pipes_start(VOID_T);

/**
 * @brief Stop the pipes screensaver and free resources.
 *        If not running, this is a no-op.
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
 * @brief Set the idle timeout before the screensaver activates.
 * @param minutes  0 = disabled; >0 = minutes of inactivity before start.
 */
VOID_T win95_pipes_set_timeout(UINT32_T minutes);

/**
 * @brief Call once per second from the desktop clock timer.
 *        Advances the idle counter and starts the screensaver when the
 *        timeout is reached.
 */
VOID_T win95_pipes_tick(VOID_T);

/**
 * @brief Reset the idle counter (call on any user activity).
 *        Also stops the screensaver if it is currently running.
 */
VOID_T win95_pipes_reset_idle(VOID_T);
