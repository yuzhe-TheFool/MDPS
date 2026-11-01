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
 * @brief 初始化 PWM 风扇（LEDC 25kHz）与 FG 反馈计数（pcnt）
 * @note IO9 反相：高=停，低=转；IO10 = FG 速度反馈（开漏，需上拉）
 */
void fan_init(void);

/**
 * @brief 独立任务：每 1s 读 LM75A 温度 → 映射转速 → 更新 PWM；
 *        读 FG 脉冲并做堵转/失效判定
 */
void fan_task(void *arg);

/**
 * @brief 供 UI 读取风扇状态
 * @return 0=停，1=转，2=堵转/失效（PWM 在转但 FG 无脉冲）
 */
int fan_get_state(void);

/**
 * @brief 最近一次测得的转速（RPM，粗略值）
 */
int fan_get_rpm(void);

/**
 * @brief 当前 PWM 占空比百分比（0~100）
 */
int fan_get_duty(void);

/**
 * @brief 是否处于手动调速模式
 */
bool fan_is_manual(void);

/**
 * @brief 手动调速
 * @param pct 0~100 = 固定转速；<0 = 恢复“按温度自动”
 */
void fan_set_manual(int pct);

#ifdef __cplusplus
}
#endif
