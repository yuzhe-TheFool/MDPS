/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "encoder.h"

static const char *TAG = "encoder";

/* EC11 引脚 */
#define ENC_A     GPIO_NUM_13
#define ENC_B     GPIO_NUM_14
#define ENC_KEY   GPIO_NUM_12

#define KEY_DEBOUNCE_MS 20

/* 旋转状态机 */
static int8_t s_prev_state = -1;   /* 上次 AB 组合（-1 表示未知） */
static int8_t s_quad = 0;          /* 累计象限数（4 个象限 = 1 档） */
static int8_t s_last_dir = 0;      /* 上次方向 */
static volatile int32_t s_enc_accum = 0;  /* 累计旋转格数（read 时清零） */

/* 按键稳定状态（按下=低电平） */
static volatile bool s_key_state = false;
static TickType_t s_last_edge_tick = 0;

/*
 * EC11 旋转格雷码方向表：
 * 索引 = prev<<2 | cur，值 = +1(顺时针一步) / -1(逆时针一步) / 0(无效或未变)
 * 一个完整档位（detent）对应 AB 状态变化 4 步。
 */
static const int8_t s_dir_table[16] = {
    0, 1, -1, 0,
   -1, 0, 0, 1,
    1, 0, 0, -1,
    0, -1, 1, 0
};

static void IRAM_ATTR enc_isr(void *arg)
{
    uint32_t pin = (uint32_t)arg;

    if (pin == ENC_A || pin == ENC_B) {
        int8_t cur = (gpio_get_level(ENC_A) << 1) | gpio_get_level(ENC_B);
        if (s_prev_state < 0) {
            s_prev_state = cur;
            return;
        }
        int8_t dir = s_dir_table[(s_prev_state << 2) | cur];
        s_prev_state = cur;
        if (dir == 0) {
            return;
        }
        /* 方向反转则重新累计，避免抖动/回摆误计 */
        if (dir != s_last_dir) {
            s_quad = 0;
            s_last_dir = dir;
        }
        s_quad += dir;
        if (s_quad >= 4) {
            s_quad = 0;
            s_enc_accum++;
        } else if (s_quad <= -4) {
            s_quad = 0;
            s_enc_accum--;
        }
    } else if (pin == ENC_KEY) {
        /* 按键：内部上拉，按下=低。按下需消抖，松开立即生效
           （避免松开边沿被消抖窗口吞掉，导致状态一直卡在按下、旋转被 LVGL 丢弃） */
        TickType_t now = xTaskGetTickCountFromISR();
        bool lvl = (gpio_get_level(ENC_KEY) == 0);
        if (lvl) {
            if ((now - s_last_edge_tick) >= pdMS_TO_TICKS(KEY_DEBOUNCE_MS)) {
                s_last_edge_tick = now;
                s_key_state = true;
            }
        } else {
            s_last_edge_tick = now;
            s_key_state = false;
        }
    }
}

esp_err_t encoder_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << ENC_A) | (1ULL << ENC_B) | (1ULL << ENC_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&cfg);

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Install ISR service failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_A, enc_isr, (void *)ENC_A));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_B, enc_isr, (void *)ENC_B));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_KEY, enc_isr, (void *)ENC_KEY));

    ESP_LOGI(TAG, "EC11 encoder ready (A=%d B=%d KEY=%d)", ENC_A, ENC_B, ENC_KEY);
    return ESP_OK;
}

int32_t encoder_get_rotation(void)
{
    int32_t v = s_enc_accum;
    s_enc_accum = 0;
    return v;
}

bool encoder_key_pressed(void)
{
    return s_key_state;
}
