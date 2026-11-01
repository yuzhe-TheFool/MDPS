/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 信号格图标（20x16，RGB565A8）：sig_0=无信号 ... sig_4=满格 */
LV_IMAGE_DECLARE(sig_0);
LV_IMAGE_DECLARE(sig_1);
LV_IMAGE_DECLARE(sig_2);
LV_IMAGE_DECLARE(sig_3);
LV_IMAGE_DECLARE(sig_4);

/**
 * @brief 根据 RSSI(dBm) 返回对应格数的信号图标
 */
const lv_image_dsc_t *sig_icon_for_rssi(int8_t rssi);

#ifdef __cplusplus
}
#endif
