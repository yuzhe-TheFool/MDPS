/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "i2c_health.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SW3538 一次读取的结果（借鉴 SW3538_Library 的换算） */
typedef struct {
    uint16_t inputVoltagemV;   /* 输入电压 mV */
    uint16_t outputVoltagemV;  /* 输出电压 mV */
    int16_t  currentPath1mA;   /* Path1 电流 mA */
    int16_t  currentPath2mA;   /* Path2 电流 mA */
    bool     path1Online;      /* Path1 设备在线 */
    bool     path2Online;      /* Path2 设备在线 */
} sw3538_data_t;

/* 实例数：0 = 路1 充电器(I2C1, SDA41/SCL42)；1 = 路2 充电器(I2C0, SDA39/SCL40) */
#define SW3538_INST_NUM 2

/**
 * @brief 初始化某一路 SW3538（挂到外部传入的 i2c 总线，地址 0x3C）
 * @param inst   0=路1(I2C1) / 1=路2(I2C0)
 * @param bus    该路所在 i2c_master 总线句柄
 * @param bus_id 该总线编号（用于把“通信失败/恢复”上报给 i2c_health）
 */
esp_err_t sw3538_init(int inst, i2c_master_bus_handle_t bus, i2c_bus_id_t bus_id);

/**
 * @brief 读取某一路 SW3538 电压/电流/在线状态
 * @param inst 0/1
 * @param out 输出结果
 * @return true=成功（数据有效）；false=通信失败
 */
bool sw3538_read(int inst, sw3538_data_t *out);

/**
 * @brief 获取某一路最近一次成功读取的结果（锁内刷新 UI 用，避免锁内做 I2C）
 * @param inst 0/1
 * @param out 输出结果
 * @return true=已有有效数据
 */
bool sw3538_get_last(int inst, sw3538_data_t *out);

#ifdef __cplusplus
}
#endif
