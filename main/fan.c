/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "lm75a.h"
#include "fan.h"

static const char *TAG = "fan";

/* ===== 风扇引脚与参数 ===== */
#define FAN_PWM_IO      GPIO_NUM_9     /* PWM 信号（反相：1=停，0=转） */
#define FAN_FG_IO       GPIO_NUM_10    /* FG 速度反馈（开漏，需上拉） */

#define FAN_PWM_FREQ    25000          /* 4 线 PWM 风扇标准频率 */
#define FAN_PWM_RES     LEDC_TIMER_10_BIT
#define FAN_PWM_MAX     ((1 << 10) - 1) /* 1023 */

#define FAN_PULSE_PER_REV 2            /* FG 每转脉冲数（先按 2，对比后校正） */

/* 温度→转速（壳内温度，0.1°C 单位）：
   低于 FAN_ON_C1 停；FAN_ON_C1~FAN_FULL_C1 从 20%→100% 线性；高于全速 */
#define FAN_ON_C1    400               /* 40.0°C 起转 */
#define FAN_FULL_C1  550               /* 55.0°C 全速 */
#define FAN_MIN_DUTY 20                /* 起转最小转速 % */

static int s_duty_pct = 0;             /* 最近目标转速 0~100 */
static int s_rpm = 0;                  /* 最近一次测得的转速（粗略） */
static bool s_manual = false;          /* true=手动调速；false=按温度自动 */
static pcnt_unit_handle_t s_fan_unit = NULL;   /* FG 计数单元 */

/* 状态：0=停，1=转，2=堵转/失效(红闪) */
static bool s_pwm_on = false;          /* 是否正输出 PWM */
static bool s_fault = false;
static int  s_fault_cycles = 0;
#define FAN_FAULT_CYCLES 3             /* PWM 开但 rpm=0 连续 3s → 判定堵转 */

static void fan_set_speed(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_duty_pct = pct;
    s_pwm_on = (pct > 0);

    /* 反相：高电平占空比 = (100 - pct)%。pct=0 → 恒高(停)；pct=100 → 恒低(全转) */
    uint32_t duty = (uint32_t)(FAN_PWM_MAX * (100 - pct) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void fan_init(void)
{
    /* LEDC 定时器：25kHz */
    ledc_timer_config_t tm = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = FAN_PWM_RES,
        .freq_hz = FAN_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tm));

    /* LEDC 通道：IO9 */
    ledc_channel_config_t ch = {
        .gpio_num = FAN_PWM_IO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = FAN_PWM_MAX,          /* 初始全高 = 停 */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    /* FG 反馈（IO10）：开漏，内部上拉；做脉冲计数 */
    gpio_set_direction(FAN_FG_IO, GPIO_MODE_INPUT);
    gpio_pullup_en(FAN_FG_IO);

    pcnt_unit_config_t ucfg = {
        .low_limit = -10000,
        .high_limit = 10000,
    };
    pcnt_unit_handle_t unit = NULL;
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &unit));
    pcnt_chan_config_t ccfg = {
        .edge_gpio_num = FAN_FG_IO,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &ccfg, &chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(
        chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));
    s_fan_unit = unit;

    ESP_LOGI(TAG, "fan init: PWM=IO%u (25kHz, inverted), FG=IO%u",
             (unsigned)FAN_PWM_IO, (unsigned)FAN_FG_IO);
}

void fan_task(void *arg)
{
    (void)arg;
    int prev = 0;
    bool has_prev = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int t = 0;
        bool t_ok = lm75a_get_temp(&t);

        /* 自动模式：温度 → 转速（温度无效则停，安全）。手动模式：不动占空比 */
        if (!s_manual) {
            if (!t_ok) {
                fan_set_speed(0);
            } else {
                int pct;
                if (t <= FAN_ON_C1) {
                    pct = 0;
                } else if (t >= FAN_FULL_C1) {
                    pct = 100;
                } else {
                    pct = FAN_MIN_DUTY + (t - FAN_ON_C1) * (100 - FAN_MIN_DUTY)
                                             / (FAN_FULL_C1 - FAN_ON_C1);
                }
                fan_set_speed(pct);
            }
        }

        /* FG 脉冲（rpm 只用于判断转/停，不需精确） */
        int rpm = 0;
        if (s_fan_unit) {
            int cur = 0;
            pcnt_unit_get_count(s_fan_unit, &cur);
            if (has_prev) {
                int16_t delta = (int16_t)(cur - prev);
                rpm = (int)((int32_t)delta * 60 / FAN_PULSE_PER_REV);
                if (rpm < 0) rpm = 0;
            }
            prev = cur;
            has_prev = true;
        }
        s_rpm = rpm;

        /* 堵转/失效判定：PWM 在转但 FG 连续几秒无脉冲 */
        if (s_pwm_on && rpm == 0) {
            if (++s_fault_cycles >= FAN_FAULT_CYCLES) s_fault = true;
        } else {
            s_fault_cycles = 0;
            s_fault = false;
        }
    }
}

/* 供 UI 读取：0=停 1=转 2=堵转/失效 */
int fan_get_state(void)
{
    if (s_fault) return 2;
    if (s_pwm_on) return 1;
    return 0;
}

int fan_get_rpm(void)
{
    return s_rpm;
}

int fan_get_duty(void)
{
    return s_duty_pct;
}

bool fan_is_manual(void)
{
    return s_manual;
}

void fan_set_manual(int pct)
{
    if (pct < 0) {
        s_manual = false;      /* 交回温度自动控制 */
        return;
    }
    s_manual = true;
    fan_set_speed(pct);
}
