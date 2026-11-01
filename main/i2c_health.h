/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I2C 总线“通信失败→恢复”事件
 *
 * 用途：电源板掉电时，那些**只在初始化时配置一次**的芯片会丢配置
 *   - MP4248：电压 / 限流 / 输出开关
 *   - SW3538：VIN ADC 使能位
 * 掉电再上电后需要重新下发一次，但**不需要周期性回读**去发现——
 * 这块板上的 I2C1 设备本来就在被周期读取（给 UI 显示电压/温度），
 * 把那次读取的结果上报到这里，就能拿到“恢复”这个边沿，
 * 零额外 I2C 流量。
 *
 * 用法：
 *   读取设备的地方 → i2c_health_report(bus, 读成功了吗)
 *   需要自愈的驱动   → if (i2c_health_take_recovered(bus)) { 重新下发配置; }
 */
typedef enum {
    I2C_BUS_ID_0 = 0,   /* SDA39/SCL40：触摸 + INA226 + SW3538[1] */
    I2C_BUS_ID_1 = 1,   /* SDA41/SCL42：SW3538[0] + MP4248 + LM75A */
    I2C_BUS_ID_MAX
} i2c_bus_id_t;

/**
 * @brief 上报一次总线通信结果（由已经在读该总线的代码调用）
 * @param bus 总线编号
 * @param ok  true=本次读取成功；false=失败
 * @note 只有“先失败过、之后又成功”才会产生恢复事件，开机首次成功不算。
 */
void i2c_health_report(i2c_bus_id_t bus, bool ok);

/**
 * @brief 取走“刚刚从通信失败中恢复”事件（取走即清除，只返回一次 true）
 * @return true=发生过失败到恢复的跳变
 */
bool i2c_health_take_recovered(i2c_bus_id_t bus);

#ifdef __cplusplus
}
#endif
