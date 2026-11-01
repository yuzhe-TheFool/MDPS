/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include <stdint.h>
#include "esp_lcd_panel_io.h"
#include "driver/i2c_master.h"

/**
 * @brief 自建触摸专用 panel IO（替代 IDF 的 esp_lcd_new_panel_io_i2c）
 *
 * 为什么要自己写：
 *   IDF v6.0.2 的 esp_lcd_panel_io_i2c.c 里，I2C 事务超时被硬编码成 -1（无限等待）：
 *       i2c_master_transmit_receive(..., -1)
 *   于是当 I2C 总线被外部器件拉死（例如电源板断电后 SDA/SCL 被拖到低电平）时，
 *   在 LVGL 回调里读触摸会永久阻塞 → 整个界面（含不依赖 I2C 的旋钮）全部卡死。
 *
 * 本实现功能与 IDF 版本一致，只是把超时做成可配置的有限值：
 *   超时返回 ESP_ERR_TIMEOUT，驱动内部还会自动做一次硬件总线恢复（clr_bus）。
 *
 * 只实现触摸实际用到的接口（CST816S 驱动只调用 esp_lcd_panel_io_rx_param）。
 *
 * @param bus           已创建的 I2C master 总线
 * @param dev_addr      从机地址（CST816S 为 0x15）
 * @param scl_speed_hz  时钟频率（100000）
 * @param timeout_ms    单次事务超时（毫秒），建议 >= 100，见 main.c 里的说明
 * @return panel IO 句柄，失败返回 NULL
 */
esp_lcd_panel_io_handle_t touch_io_i2c_create(i2c_master_bus_handle_t bus, uint8_t dev_addr,
                                              uint32_t scl_speed_hz, int timeout_ms);
