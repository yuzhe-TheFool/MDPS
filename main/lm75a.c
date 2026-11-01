/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "i2c_health.h"
#include "lm75a.h"

static const char *TAG = "lm75a";

/* LM75A：A0=A1=A2=0 → 0x48 */
#define LM75A_ADDR       0x48
#define LM75A_FREQ_HZ    100000
#define LM75A_REG_TEMP   0x00    /* 温度：2 字节，高字节在前，LSB=0.125°C */

static i2c_master_dev_handle_t s_dev = NULL;

static int s_temp_c1 = 0;    /* 最近温度（0.1°C） */
static bool s_valid = false;

static esp_err_t reg_read2(uint8_t reg, uint8_t buf[2])
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, 2, pdMS_TO_TICKS(100));
}

/* LM75A 温度：16bit 中 11 位有效，左对齐在 bit15..bit5（LSB=0.125°C）。
   raw=0x1C80 → (raw>>5)=0xE4=228 → 28.5°C */
static int raw_to_c1(int16_t raw)
{
    int code = ((int)raw) >> 5;              /* 算术右移，取 11 位有符号编码 */
    return (code * 5 + (code >= 0 ? 2 : -2)) / 4;   /* code×1.25 → 0.1°C 四舍五入 */
}

static void fmt_c1(char *s, size_t n, int c1)
{
    if (c1 < 0) {
        snprintf(s, n, "-%d.%d", (-c1) / 10, (-c1) % 10);
    } else {
        snprintf(s, n, "%d.%d", c1 / 10, c1 % 10);
    }
}

esp_err_t lm75a_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        ESP_LOGE(TAG, "invalid bus");
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LM75A_ADDR,
        .scl_speed_hz = LM75A_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", LM75A_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* 探测：读一次温度寄存器确认通信 */
    uint8_t buf[2] = {0, 0};
    if (reg_read2(LM75A_REG_TEMP, buf) == ESP_OK) {
        int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
        char s[16];
        fmt_c1(s, sizeof(s), raw_to_c1(raw));
        ESP_LOGI(TAG, "LM75A probe OK @0x%02X raw=0x%04X %s °C", LM75A_ADDR,
                 (unsigned)raw & 0xFFFF, s);
    } else {
        ESP_LOGE(TAG, "LM75A probe FAIL @0x%02X", LM75A_ADDR);
    }
    return ESP_OK;
}

void lm75a_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint8_t buf[2] = {0, 0};
        esp_err_t err = reg_read2(LM75A_REG_TEMP, buf);
        /* 上报总线通/断：LM75A 只在 I2C1（电源板）上，是判断电源板在不在的好信号 */
        i2c_health_report(I2C_BUS_ID_1, err == ESP_OK);
        if (err != ESP_OK) {
            s_valid = false;
            continue;
        }
        int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
        s_temp_c1 = raw_to_c1(raw);
        s_valid = true;
    }
}

bool lm75a_get_temp(int *tenth_c)
{
    if (!s_valid) return false;
    *tenth_c = s_temp_c1;
    return true;
}
