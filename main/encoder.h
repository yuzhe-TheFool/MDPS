/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 EC11 编码器（GPIO ISR，A/B 相位 + 按键状态）
 * 引脚固定：A=IO13, B=IO14, 按键=IO12（内部上拉，按下为低）
 */
esp_err_t encoder_init(void);

/**
 * @brief 读取并清零累计旋转格数（正=顺时针，负=逆时针）
 * @return 自上次读取以来旋转的格数
 */
int32_t encoder_get_rotation(void);

/**
 * @brief 读取按键当前状态（持续电平，按下为 true）
 */
bool encoder_key_pressed(void);

#ifdef __cplusplus
}
#endif
