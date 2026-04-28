/**
 * @file win95_spider.h
 * @brief Win95 Spider Solitaire (1-suit, 10 columns, 104 cards) — public API.
 */
#pragma once

#include "tuya_cloud_types.h"
#include "lvgl.h"

/** @brief Open the Spider Solitaire game window. */
VOID_T win95_spider_open(lv_obj_t *parent);
