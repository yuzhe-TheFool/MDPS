/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LM75A（挂在 I2C1: SDA41/SCL42，与 SW3538/MP4248 同总线）
 * @param bus 已创建的 i2c_master 总线句柄
 */
esp_err_t lm75a_init(i2c_master_bus_handle_t bus);

/**
 * @brief 独立采样任务：周期读温度并串口打印（后续改为 PWM 风扇控制）
 */
void lm75a_task(void *arg);

/**
 * @brief 取最近一次温度（0.1°C 单位），true=数据有效
 */
bool lm75a_get_temp(int *tenth_c);

#ifdef __cplusplus
}
#endif
