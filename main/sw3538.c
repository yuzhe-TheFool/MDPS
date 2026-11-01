/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "sw3538.h"

static const char *TAG = "sw3538";

/* ===== 地址/频率（总线由 main.c 分别创建：路1=I2C1/41-42，路2=I2C0/39-40） ===== */
#define SW3538_ADDR      0x3C
#define SW3538_FREQ_HZ   100000
#define SW3538_INST_MAX  2     /* 0=路1，1=路2 */

/* ===== SW3538 寄存器（借鉴 SW3538_Library） ===== */
#define SW3538_REG_VERSION            0x00
#define SW3538_REG_MAX_POWER          0x02
#define SW3538_REG_FAST_CHARGE_IND    0x09
#define SW3538_REG_SYS_STATUS0        0x0A
#define SW3538_REG_SYS_STATUS1        0x0D
#define SW3538_REG_I2C_ENABLE         0x10
#define SW3538_REG_FORCE_OP_ENABLE    0x15
#define SW3538_REG_FORCE_OP2          0x18
#define SW3538_REG_ADC_CONFIG         0x40
#define SW3538_REG_ADC_DATA_LOW       0x41
#define SW3538_REG_ADC_DATA_HIGH      0x42

/* ADC 通道：1=Path1 电流，2=Path2 电流，6=输入电压，11=输出电压(14bit) */
#define SW3538_ADC_PATH1_I  1
#define SW3538_ADC_PATH2_I  2
#define SW3538_ADC_IN_V     6
#define SW3538_ADC_OUT_V    11

static i2c_master_dev_handle_t s_dev[SW3538_INST_MAX];

/* 最近一次成功读数缓存（供 UI 锁内读取） */
static sw3538_data_t s_last[SW3538_INST_MAX];
static bool s_last_valid[SW3538_INST_MAX];

/* 芯片被掉电复位（电源板断电再上电）后需要重新初始化：
   0x18 的 VIN ADC 使能位会丢，不重开会一直读不到输入电压 */
static bool s_need_reinit[SW3538_INST_MAX];

/* 本实例所在总线编号（上报 i2c_health 用） */
static i2c_bus_id_t s_bus_id[SW3538_INST_MAX];

/* 读一个寄存器（带重试） */
static uint8_t sw_reg_read(int inst, uint8_t reg)
{
    i2c_master_dev_handle_t dev = s_dev[inst];
    if (!dev) return 0xFF;
    uint8_t data = 0xFF;
    for (int retry = 0; retry < 3; retry++) {
        esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, &data, 1,
                                                    pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            return data;
        }
        vTaskDelay(pdMS_TO_TICKS(5 << retry));
    }
    return 0xFF;
}

/* 写一个寄存器（带重试） */
static bool sw_reg_write(int inst, uint8_t reg, uint8_t value)
{
    i2c_master_dev_handle_t dev = s_dev[inst];
    if (!dev) return false;
    uint8_t buf[2] = {reg, value};
    for (int retry = 0; retry < 3; retry++) {
        if (i2c_master_transmit(dev, buf, 2, pdMS_TO_TICKS(100)) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/* 解锁强制操作写（寄存器 0x15 依次写 0x20/0x40/0x80）
   注意：不要额外写 0x10(I2C_ENABLE) —— 那会误关输出（实测 Vout=0） */
static bool sw_force_op_enable(int inst)
{
    return sw_reg_write(inst, SW3538_REG_FORCE_OP_ENABLE, 0x20) &&
           sw_reg_write(inst, SW3538_REG_FORCE_OP_ENABLE, 0x40) &&
           sw_reg_write(inst, SW3538_REG_FORCE_OP_ENABLE, 0x80);
}

/* 使能一个 ADC 通道（置 FORCE_OP2 对应位），初始化时调用一次即可 */
static bool sw_adc_enable(int inst, uint8_t ch)
{
    if (!sw_force_op_enable(inst)) return false;
    uint8_t v = sw_reg_read(inst, SW3538_REG_FORCE_OP2);
    v |= (1 << ch);
    return sw_reg_write(inst, SW3538_REG_FORCE_OP2, v);
}

/* 读取指定通道 ADC 原始值（12bit；通道 11 为 14bit） */
static uint16_t sw_adc_read(int inst, uint8_t ch)
{
    if (!sw_reg_write(inst, SW3538_REG_ADC_CONFIG, ch)) {
        return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(5));   /* ADC 转换时间 */

    uint8_t low  = sw_reg_read(inst, SW3538_REG_ADC_DATA_LOW);
    uint8_t high = sw_reg_read(inst, SW3538_REG_ADC_DATA_HIGH);

    if (ch == 11) {
        return ((uint16_t)(high & 0x7F) << 8) | low;
    }
    return ((uint16_t)(high & 0x0F) << 8) | low;
}

esp_err_t sw3538_init(int inst, i2c_master_bus_handle_t bus, i2c_bus_id_t bus_id)
{
    if (inst < 0 || inst >= SW3538_INST_MAX || !bus) {
        ESP_LOGE(TAG, "invalid inst/bus");
        return ESP_ERR_INVALID_ARG;
    }
    s_bus_id[inst] = bus_id;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SW3538_ADDR,
        .scl_speed_hz = SW3538_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev[inst]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%d] add device failed: %s", inst, esp_err_to_name(ret));
        return ret;
    }

    /* 探测：读多个关键寄存器，确认通信与状态（此阶段 USB 串口还没断，能打印出来） */
    uint8_t ver = sw_reg_read(inst, SW3538_REG_VERSION);        /* 0x00 版本 */
    uint8_t pwr = sw_reg_read(inst, SW3538_REG_MAX_POWER);      /* 0x02 */
    uint8_t i2c = sw_reg_read(inst, SW3538_REG_I2C_ENABLE);     /* 0x10 */
    uint8_t st0 = sw_reg_read(inst, SW3538_REG_SYS_STATUS0);    /* 0x0A */
    uint8_t st1 = sw_reg_read(inst, SW3538_REG_SYS_STATUS1);    /* 0x0D */
    ESP_LOGI(TAG, "SW3538[%d] regs: ver=0x%02X pwr=0x%02X i2c_en=0x%02X st0=0x%02X st1=0x%02X",
             inst, ver, pwr, i2c, st0, st1);

    /* 使能 VIN ADC（0x18 bit6，通道6）：VIN 采样默认关闭，需置位打开一次，之后自动采样 */
    if (sw_adc_enable(inst, SW3538_ADC_IN_V)) {
        ESP_LOGI(TAG, "SW3538[%d] VIN ADC enabled (0x18 bit6 set)", inst);
    }
    return ESP_OK;
}

bool sw3538_read(int inst, sw3538_data_t *out)
{
    if (inst < 0 || inst >= SW3538_INST_MAX || !s_dev[inst] || !out) {
        return false;
    }

    uint8_t pwr = sw_reg_read(inst, SW3538_REG_MAX_POWER);
    i2c_health_report(s_bus_id[inst], pwr != 0xFF);   /* 上报总线通/断，供 MP4248 等自愈 */
    if (pwr == 0xFF) {
        s_last_valid[inst] = false;   /* 通信失败：清有效标志，UI 相应显示 -- */
        s_need_reinit[inst] = true;   /* 芯片可能是被掉电了，恢复后要重新初始化 */
        return false;
    }

    /* 从“通信不上”恢复过来 → 芯片肯定被复位过，重做一次初始化（幂等） */
    if (s_need_reinit[inst]) {
        ESP_LOGW(TAG, "[%d] chip back online, re-init (VIN ADC enable)", inst);
        sw_adc_enable(inst, SW3538_ADC_IN_V);
        s_need_reinit[inst] = false;
    }

    uint8_t st1 = sw_reg_read(inst, SW3538_REG_SYS_STATUS1);
    out->path1Online = (st1 >> 1) & 0x01;
    out->path2Online = (st1 >> 0) & 0x01;

    /* 不经过 force-op（0x15/0x18/0x19），直接写 0x40 选通道 + 读 0x41/0x42 */
    uint16_t adc;
    adc = sw_adc_read(inst, SW3538_ADC_PATH1_I);
    out->currentPath1mA = (int16_t)(adc * 2.5f);

    adc = sw_adc_read(inst, SW3538_ADC_PATH2_I);
    out->currentPath2mA = (int16_t)(adc * 2.5f);

    adc = sw_adc_read(inst, SW3538_ADC_IN_V);
    out->inputVoltagemV = (uint16_t)(adc * 10.0f);

    adc = sw_adc_read(inst, SW3538_ADC_OUT_V);
    out->outputVoltagemV = (uint16_t)(adc * 1.0f);

    s_last[inst] = *out;
    s_last_valid[inst] = true;
    return true;
}

bool sw3538_get_last(int inst, sw3538_data_t *out)
{
    if (inst < 0 || inst >= SW3538_INST_MAX || !out) {
        return false;
    }
    if (!s_last_valid[inst]) {
        return false;
    }
    *out = s_last[inst];
    return true;
}
