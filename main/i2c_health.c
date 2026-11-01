/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include "i2c_health.h"

/* 两个任务之间只传递“位”级别的状态，用 volatile bool 即可：
   万一极端时序下漏掉或重复一次事件，后果只是多/少下发一次配置。
   丢失事件的风险已由“多个设备同时上报同一条总线”覆盖
   （I2C1 上 SW3538 与 LM75A 都在周期读）。 */
static volatile bool s_down[I2C_BUS_ID_MAX];
static volatile bool s_recovered[I2C_BUS_ID_MAX];

void i2c_health_report(i2c_bus_id_t bus, bool ok)
{
    if (bus < 0 || bus >= I2C_BUS_ID_MAX) {
        return;
    }
    if (!ok) {
        s_down[bus] = true;
    } else if (s_down[bus]) {
        s_down[bus] = false;
        s_recovered[bus] = true;
    }
}

bool i2c_health_take_recovered(i2c_bus_id_t bus)
{
    if (bus < 0 || bus >= I2C_BUS_ID_MAX) {
        return false;
    }
    if (!s_recovered[bus]) {
        return false;
    }
    s_recovered[bus] = false;
    return true;
}
