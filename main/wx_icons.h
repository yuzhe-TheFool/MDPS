/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QWeather 天气图标（S2 风格，RGB565A8，56x56） */
LV_IMAGE_DECLARE(wx_100);
LV_IMAGE_DECLARE(wx_101);
LV_IMAGE_DECLARE(wx_102);
LV_IMAGE_DECLARE(wx_103);
LV_IMAGE_DECLARE(wx_104);
LV_IMAGE_DECLARE(wx_150);
LV_IMAGE_DECLARE(wx_153);
LV_IMAGE_DECLARE(wx_154);
LV_IMAGE_DECLARE(wx_300);
LV_IMAGE_DECLARE(wx_301);
LV_IMAGE_DECLARE(wx_302);
LV_IMAGE_DECLARE(wx_304);
LV_IMAGE_DECLARE(wx_305);
LV_IMAGE_DECLARE(wx_306);
LV_IMAGE_DECLARE(wx_307);
LV_IMAGE_DECLARE(wx_309);
LV_IMAGE_DECLARE(wx_313);
LV_IMAGE_DECLARE(wx_350);
LV_IMAGE_DECLARE(wx_351);
LV_IMAGE_DECLARE(wx_400);
LV_IMAGE_DECLARE(wx_401);
LV_IMAGE_DECLARE(wx_402);
LV_IMAGE_DECLARE(wx_407);
LV_IMAGE_DECLARE(wx_499);
LV_IMAGE_DECLARE(wx_500);
LV_IMAGE_DECLARE(wx_501);

/**
 * @brief open-meteo WMO 天气代码 -> QWeather 图标
 * @param is_day 白天/夜晚（影响晴图标选择）
 */
const lv_image_dsc_t *wx_icon_get(int weather_code, bool is_day);

#ifdef __cplusplus
}
#endif
