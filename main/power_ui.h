/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 进入充电器界面（两路 SW3538，4 列电压电流，值先占位）
 */
void power_ui_create_charge(void);

/**
 * @brief 进入可调电源界面（MP4248 设定值 + INA226 实际值，先占位）
 */
void power_ui_create_power(void);

/**
 * @brief 周期刷新（主循环 LVGL 锁内调用）：后续轮询真实采集并刷新显示
 */
void power_ui_update(void);

/**
 * @brief 是否处于旋钮调值模式（已选中电压或电流槽）
 * @return true=旋转应调值而非移动焦点
 */
bool power_ui_is_adjusting(void);

/**
 * @brief 旋钮调值（由编码器 read_cb 在调节模式下调用）
 * @param diff 本次旋转格数（正=增大）
 */
void power_ui_encoder_adjust(int diff);

/**
 * @brief 更新 ACT 实际电压/电流（INA226 采集到后调用）
 * @param v_mv 实际电压 mV；<0 表示无数据显示 "--"
 * @param i_ma 实际电流 mA；<0 表示无数据显示 "--"
 */
void power_ui_set_act(int v_mv, int i_ma);

/**
 * @brief 读取可调电源设定值（物理单位）
 * @param v_mv 输出电压设定 mV（可为 NULL）
 * @param i_ma 限流设定 mA（可为 NULL）
 * @param en   输出使能（可为 NULL）
 */
void power_ui_get_output(int *v_mv, int *i_ma, bool *en);

/**
 * @brief 设置可调电源设定值（物理单位）。
 *        会自动限幅、同步 power 屏显示、下发 MP4248 并延时写入 NVS。
 * @param v_mv 电压 mV（<=0 表示不改）
 * @param i_ma 限流 mA（<=0 表示不改）
 * @param en   输出使能
 */
void power_ui_set_output(int v_mv, int i_ma, bool en);

#ifdef __cplusplus
}
#endif
