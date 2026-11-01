/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

/**
 * @brief 创建配网界面（自动启动 WiFi 扫描）
 * 需在 LVGL 初始化之后调用
 */
void prov_ui_create(void);

/**
 * @brief 已连接时的网络信息页（SSID / IP），点“返回”回主界面
 */
void prov_ui_create_info(void);

/**
 * @brief 配网状态机刷新（在主循环 lvgl_port_lock 内周期调用）
 */
void prov_ui_update(void);
