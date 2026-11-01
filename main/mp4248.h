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
 * @brief 初始化 MP4248（与 SW3538 共用 I2C1: SDA=41/SCL=42 总线）
 * @param bus 已创建的 i2c_master 总线句柄
 */
esp_err_t mp4248_init(i2c_master_bus_handle_t bus);

/**
 * @brief 设置输出电压/限流目标（UI 线程调用，仅记录，由 mp4248_task 异步下发）
 * @param v_y  VOUT_COMMAND 寄存器值（1~0xFFFF，Vout[V]=Y×2⁻¹⁰×1.5）
 * @param i_cl CURRENT_LIMIT 寄存器值（1~127，每+1=50mA）
 */
void mp4248_set_target(int v_y, int i_cl);

/**
 * @brief 输出使能（写 OPERATION_REG 0x01）：true=输出开，false=输出关
 * @note IO38(EN) 保持常高，输出开关用软件寄存器控制
 */
void mp4248_set_en(bool on);

/**
 * @brief 独立下发任务：目标值变化时写 VOUT_COMMAND / CURRENT_LIMIT
 */
void mp4248_task(void *arg);

#ifdef __cplusplus
}
#endif
