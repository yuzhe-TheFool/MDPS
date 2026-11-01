/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "ina226.h"

static const char *TAG = "ina226";

/* INA226：A0=A1=0 → 地址 0x40 */
#define INA226_ADDR       0x40
#define INA226_FREQ_HZ    100000

#define INA226_REG_CONFIG    0x00
#define INA226_REG_SHUNT     0x01    /* 有符号补码，LSB=2.5µV */
#define INA226_REG_BUS       0x02    /* 无符号，LSB=1.25mV */
#define INA226_REG_CALIB     0x05
#define INA226_REG_DEV_ID    0xFF    /* 应 = 0x2260 */

#define INA226_RS_OHM    0.010f       /* 分流电阻 10mΩ */
/* 配置字（bit: AVG[11:9], VBUS_CT[8:6], VSH_CT[5:3], MODE[2:0]）
   0x44F = AVG=16(010) | VBUS_CT=204µs(001) | VSH_CT=204µs(001) | MODE=连续shunt+bus(111)
   → 16 次平均降噪，更新周期 ≈ 6.5ms（对 200ms 轮询无影响） */
#define INA226_CONFIG    0x44F

/* 共享 I2C0 总线上偶发读失败很常见：连续失败多少次才判定“无数据”(显示 --) */
#define INA226_FAIL_MAX  3

/* bus 电压补偿：如果显示不准，可以乘系数微调 */
#define INA226_BUS_CAL  1.00f

static i2c_master_dev_handle_t s_dev = NULL;

static int s_v_mv = 0;
static int s_i_ma = 0;
static bool s_valid = false;          /* 最近一次读取成功 */

static esp_err_t reg_write16(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (val >> 8) & 0xFF, val & 0xFF};   /* INA226: MSB 在前 */
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t reg_read16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = {0, 0};
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, buf, 2, pdMS_TO_TICKS(100));
    *out = (uint16_t)((buf[0] << 8) | buf[1]);   /* INA226: 高字节先回 */
    return ret;
}

esp_err_t ina226_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        ESP_LOGE(TAG, "invalid bus (touch I2C0 not ready)");
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDR,
        .scl_speed_hz = INA226_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", INA226_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* 探测：device id 应为 0x2260 */
    uint16_t id = 0;
    if (reg_read16(INA226_REG_DEV_ID, &id) == ESP_OK) {
        ESP_LOGI(TAG, "INA226 probe OK @0x%02X (id=0x%04X)", INA226_ADDR, id);
    } else {
        ESP_LOGE(TAG, "INA226 probe FAIL @0x%02X", INA226_ADDR);
    }

    /* 写默认配置：连续测量 */
    if (reg_write16(INA226_REG_CONFIG, INA226_CONFIG) != ESP_OK) {
        ESP_LOGE(TAG, "config write failed");
    }
    return ESP_OK;
}

void ina226_task(void *arg)
{
    (void)arg;
    int fail_cnt = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        uint16_t shunt = 0, bus = 0;
        esp_err_t e1 = reg_read16(INA226_REG_SHUNT, &shunt);
        esp_err_t e2 = (e1 == ESP_OK) ? reg_read16(INA226_REG_BUS, &bus) : ESP_ERR_INVALID_STATE;

        if (e1 != ESP_OK || e2 != ESP_OK) {
            /* 偶发读失败不立即失效（否则 UI 会在 "0.00A" 和 "--" 之间闪）；
               连续 INA226_FAIL_MAX 次（约 600ms）都失败才清有效标志 */
            if (++fail_cnt >= INA226_FAIL_MAX) {
                s_valid = false;
            }
            continue;
        }
        fail_cnt = 0;

        int16_t s = (int16_t)shunt;             /* 分流电压（补码，LSB 2.5µV） */

        s_v_mv = (int)(bus * 1.25f / INA226_BUS_CAL + 0.5f);   /* bus×1.25mV，÷补偿 */

        /* 输出电流不会为负：空载时分流电压噪声会让结果出现 -1/-2mA 之类的负值，
           而 UI 用 <0 表示“无数据(--)”，会导致空载时在 0.00A 与 -- 之间跳。
           这里把负值归零，"--" 只留给真正的读取失败。 */
        int i_ma = (int)((float)s * 2.5e-6f / INA226_RS_OHM * 1000.0f);  /* ×0.25mA */
        if (i_ma < 0) {
            i_ma = 0;
        }
        s_i_ma = i_ma;
        s_valid = true;
    }
}

bool ina226_get_last(int *v_mv, int *i_ma)
{
    if (!s_valid) return false;
    *v_mv = s_v_mv;
    *i_ma = s_i_ma;
    return true;
}
