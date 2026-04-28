/**
 * @file win95_illegal.h
 * @brief Win95-style "This program has performed an illegal operation" dialog.
 */
#pragma once

#include "tuya_cloud_types.h"

/**
 * @brief Show the illegal operation dialog.
 * @param app_name  Program name string (e.g. "MINESWEEPER"). May be NULL.
 */
VOID_T win95_illegal_op_show(CONST CHAR_T *app_name);

/**
 * @brief Dismiss the dialog if currently shown.
 */
VOID_T win95_illegal_op_dismiss(VOID_T);
