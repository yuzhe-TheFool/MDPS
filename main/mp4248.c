/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "i2c_health.h"
#include "mp4248.h"

static const char *TAG = "mp4248";

/* MP4248（与 MPQ4263 兼容） */
#define MP4248_ADDR          0x67        /* 7bit 地址 */
#define MP4248_FREQ_HZ       100000
#define MP4248_REG_OPERATION 0x01        /* OPERATION: 输出开关(0x80=ON/0x00=OFF) */
#define MP4248_REG_VOUT_CMD  0x21        /* VOUT_COMMAND: 16bit Linear16，低字节在前 */
#define MP4248_REG_CURRENT   0xD1        /* CURRENT_LIMIT: D[6:0]，每+1=50mA */

static i2c_master_dev_handle_t s_dev = NULL;

/* UI 目标寄存器值 vs 已成功下发值（-1 = 尚未给定）。
   电压直接就是 VOUT_COMMAND(Y)，电流直接就是 CURRENT_LIMIT(cl) */
static int s_target_v = -1;
static int s_target_i = -1;
static int s_applied_v = -1;
static int s_applied_i = -1;

/* EN（OPERATION 寄存器）状态：默认关；UI 设置过才下发。
   注意：s_en_applied_valid 区分「从没下发过」与「下发成 OFF」——
   芯片可能残留上次的 ON，首次必须真正写一次，否则 UI 显示关但仍有输出 */
static bool s_en_target = false;
static bool s_en_applied = false;
static bool s_en_valid = false;
static bool s_en_applied_valid = false;

static esp_err_t reg_write8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    for (int r = 0; r < 2; r++) {
        if (i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100)) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_FAIL;
}

static esp_err_t reg_write16(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, val & 0xFF, (val >> 8) & 0xFF};   /* PMBus：低字节在前 */
    for (int r = 0; r < 2; r++) {
        if (i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100)) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_FAIL;
}

static esp_err_t reg_read16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = {0, 0};
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, buf, 2, pdMS_TO_TICKS(100));
    *out = (uint16_t)((buf[1] << 8) | buf[0]);   /* PMBus：低字节先回 */
    return ret;
}

/* 电源板掉电再上电后，芯片寄存器会回到默认值，而 UI 目标值没变、applied 标记也还在，
   导致“有变化才下发”的逻辑永远不会重下 → 输出错误的电压，必须手动点 O/I 才行。
   这里不用周期性回读（那是白占带宽）：
   靠 I2C1 上已有设备（SW3538/LM75A）的周期读取上报的通/断状态，
   只在“通信失败 → 恢复”这一个边沿把 mark 清掉，让主循环重下一遍。 */
static void mp4248_check_bus_recovered(void)
{
    if (!i2c_health_take_recovered(I2C_BUS_ID_1)) {
        return;
    }
    ESP_LOGW(TAG, "I2C1 back online → chip was reset, re-apply V/I/EN");
    s_applied_v = -1;
    s_applied_i = -1;
    s_en_applied_valid = false;
}

esp_err_t mp4248_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        ESP_LOGE(TAG, "invalid bus");
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MP4248_ADDR,
        .scl_speed_hz = MP4248_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", MP4248_ADDR, esp_err_to_name(ret));
        return ret;
    }
    /* 探测：读 OPERATION(0x01)/VOUT_COMMAND(0x21)/CURRENT_LIMIT(0xD1) 初始值 */
    uint8_t op = 0xFF, cl = 0xFF;
    uint16_t vcmd = 0;
    uint8_t regop = 0x01, regcl = 0xD1;
    if (i2c_master_transmit_receive(s_dev, &regop, 1, &op, 1, pdMS_TO_TICKS(100)) == ESP_OK) {
        reg_read16(MP4248_REG_VOUT_CMD, &vcmd);
        i2c_master_transmit_receive(s_dev, &regcl, 1, &cl, 1, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "MP4248 probe OK @0x%02X (OP=0x%02X VOUT_CMD=0x%04X I_LIMIT=0x%02X)",
                 MP4248_ADDR, op, vcmd, cl);
    } else {
        ESP_LOGE(TAG, "MP4248 probe FAIL @0x%02X", MP4248_ADDR);
    }
    return ESP_OK;
}

void mp4248_set_target(int v_y, int i_cl)
{
    /* 参数已是寄存器值：电压 VOUT_COMMAND(16bit)，电流 CURRENT_LIMIT(7bit) */
    if (v_y < 1) v_y = 1;
    if (v_y > 0xFFFF) v_y = 0xFFFF;
    if (i_cl < 1) i_cl = 1;
    if (i_cl > 0x7F) i_cl = 0x7F;
    s_target_v = v_y;
    s_target_i = i_cl;
}

void mp4248_set_en(bool on)
{
    s_en_target = on;
    s_en_valid = true;
}

void mp4248_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));   /* 等 UI 先给出目标 */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        /* 0) 总线刚从通信失败恢复（电源板掉电再上电）→ 芯片已被复位，全部重下 */
        mp4248_check_bus_recovered();

        /* 1) EN 输出开关放前面：芯片可能只在 ON 状态下采纳 VOUT_COMMAND。
           切 ON 后把电压/限流的 applied 标记清掉 → 强制下一步重写一次。
           首次（applied_valid=false）无论目标开/关都会真正写一次 */
        if (s_en_valid && (!s_en_applied_valid || s_en_target != s_en_applied)) {
            if (reg_write8(MP4248_REG_OPERATION, s_en_target ? 0x80 : 0x00) == ESP_OK) {
                s_en_applied = s_en_target;
                s_en_applied_valid = true;
                if (s_en_target) {
                    s_applied_v = -1;   /* 强制重下电压/限流（在 ON 之后） */
                    s_applied_i = -1;
                }
            }
        }

        /* 2) 电压/限流：s_target_v/s_target_i 已经是寄存器值，直接下发 */
        if (s_target_v >= 0 && s_target_i >= 0 &&
            (s_target_v != s_applied_v || s_target_i != s_applied_i)) {
            uint8_t cl = (uint8_t)s_target_i;
            uint16_t y = (uint16_t)s_target_v;

            bool ok = (reg_write8(MP4248_REG_CURRENT, cl) == ESP_OK);
            ok = (reg_write16(MP4248_REG_VOUT_CMD, y) == ESP_OK) && ok;

            /* 回读校验：读回一致才算成功；不一致不标记 → 下轮静默重试 */
            uint16_t rb = 0;
            reg_read16(MP4248_REG_VOUT_CMD, &rb);
            if (ok && rb == y) {
                s_applied_v = s_target_v;
                s_applied_i = s_target_i;
            }
        }
    }
}
