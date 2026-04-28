/**
 * @file win95_defrag.h
 * @brief Win95-style Disk Defragmenter — public API.
 */
#pragma once

#include "tuya_cloud_types.h"
#include "lvgl.h"

/**
 * @brief Open the Disk Defragmenter window inside the given parent.
 * @param parent  Parent LVGL object (e.g. desktop).
 */
VOID_T win95_defrag_open(lv_obj_t *parent);
