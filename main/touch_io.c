/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io_interface.h"
#include "touch_io.h"

static const char *TAG = "touch_io";

/* CST816S 的寄存器地址是 1 字节（lcd_cmd_bits = 8） */
#define TOUCH_CMD_BYTES 1

typedef struct {
    esp_lcd_panel_io_t base;    /* 必须是第一个成员（__containerof 依赖它） */
    i2c_master_dev_handle_t dev;
    int timeout_ms;
} touch_io_t;

/* 写寄存器地址后再读数据：与 IDF 实现一致，区别只在于有限超时 */
static esp_err_t touch_io_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size)
{
    touch_io_t *h = __containerof(io, touch_io_t, base);

    if (lcd_cmd < 0) {
        return i2c_master_receive(h->dev, param, param_size, h->timeout_ms);
    }
    uint8_t cmd = (uint8_t)lcd_cmd;
    return i2c_master_transmit_receive(h->dev, &cmd, TOUCH_CMD_BYTES, param, param_size, h->timeout_ms);
}

/* 写寄存器（触摸驱动目前用不到，留着以防驱动升级） */
static esp_err_t touch_io_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size)
{
    touch_io_t *h = __containerof(io, touch_io_t, base);

    i2c_master_transmit_multi_buffer_info_t buf[2];
    size_t n = 0;
    uint8_t cmd = (uint8_t)lcd_cmd;

    if (lcd_cmd >= 0) {
        buf[n].write_buffer = &cmd;
        buf[n].buffer_size = TOUCH_CMD_BYTES;
        n++;
    }
    if (param != NULL && param_size > 0) {
        buf[n].write_buffer = (const uint8_t *)param;
        buf[n].buffer_size = param_size;
        n++;
    }
    if (n == 0) {
        return ESP_OK;
    }
    return i2c_master_multi_buffer_transmit(h->dev, buf, n, h->timeout_ms);
}

static esp_err_t touch_io_del(esp_lcd_panel_io_t *io)
{
    touch_io_t *h = __containerof(io, touch_io_t, base);

    esp_err_t err = i2c_master_bus_rm_device(h->dev);
    free(h);
    return err;
}

esp_lcd_panel_io_handle_t touch_io_i2c_create(i2c_master_bus_handle_t bus, uint8_t dev_addr,
                                             uint32_t scl_speed_hz, int timeout_ms)
{
    if (bus == NULL) {
        ESP_LOGE(TAG, "i2c bus is NULL");
        return NULL;
    }

    touch_io_t *h = calloc(1, sizeof(touch_io_t));
    if (h == NULL) {
        ESP_LOGE(TAG, "no mem for touch io");
        return NULL;
    }

    i2c_device_config_t dev_cfg = {
        .device_address = dev_addr,
        .scl_speed_hz = scl_speed_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device 0x%02X failed: %s", dev_addr, esp_err_to_name(err));
        free(h);
        return NULL;
    }

    h->timeout_ms = timeout_ms;
    h->base.rx_param = touch_io_rx_param;
    h->base.tx_param = touch_io_tx_param;
    h->base.del = touch_io_del;

    ESP_LOGI(TAG, "touch io ready: addr=0x%02X @%uHz, timeout=%dms (IDF default is infinite)",
             dev_addr, (unsigned)scl_speed_hz, timeout_ms);
    return &h->base;
}
