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
 * @brief 初始化 INA226（与触摸共用 I2C0: SDA=39/SCL=40 总线，A0/A1=0 → 0x40）
 * @param bus 触摸用的 i2c_master 总线句柄
 */
esp_err_t ina226_init(i2c_master_bus_handle_t bus);

/**
 * @brief 独立采集任务：周期读分流/总线电压，缓存结果
 */
void ina226_task(void *arg);

/**
 * @brief 取最近一次采集值（UI 线程用，无 I2C 操作）
 * @param v_mv 输出电压 mV
 * @param i_ma 输出电流 mA
 * @return true=数据有效
 */
bool ina226_get_last(int *v_mv, int *i_ma);

#ifdef __cplusplus
}
#endif
