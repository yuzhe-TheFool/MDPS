/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 全局共享输入组：编码器 indev 的焦点组，所有界面共用 */
extern lv_group_t *ui_group;

/**
 * @brief 创建主界面（时间/天气占位 + WiFi 标识），并加载为当前屏
 * @param group 编码器输入组（由 main.c 创建后传入）
 */
void main_ui_init(lv_group_t *group);

/**
 * @brief 周期刷新主界面（WiFi 状态、时间、天气），需在 LVGL 锁内调用
 */
void main_ui_update(void);

/**
 * @brief 从配网界面返回主界面（重新装载当前屏并重建输入组焦点）
 */
void main_ui_show(void);

/**
 * @brief 设置主界面导航状态
 * @param state 0=无选中 1=WiFi 2=Charge 3=Power
 */
void main_ui_set_nav(int state);

/**
 * @brief 主界面是否当前活动屏
 */
bool main_ui_is_active(void);

/**
 * @brief 设置显示时间与日期（后续由 NTP 调用）
 * @param hour 小时；minute 分钟
 * @param year 年；mon 月(1-12)；mday 日(1-31)；wday 星期(0=周日)
 */
void main_ui_set_time(int hour, int minute, int year, int mon, int mday, int wday);

/**
 * @brief 联网成功：启动联网数据门（时间+天气都就绪后一起显示）
 */
void main_ui_net_connected(void);

/**
 * @brief 设置显示天气（联网后由天气模块调用）
 * @param temp_c       温度（摄氏度）
 * @param weather_code WMO 天气代码（用于选择图标/描述）
 */
void main_ui_set_weather(int temp_c, int weather_code);

#ifdef __cplusplus
}
#endif
