/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

/**
 * @brief 初始化 SNTP 网络时间同步（联网后调用一次）
 */
void net_time_init(void);

/**
 * @brief 周期刷新（主循环调用）：时间同步后更新主界面显示
 */
void net_time_update(void);

/**
 * @brief WiFi 重新连上后调用：立即重新对时，并重新确认外网可达
 *        （同步成功后会自动把 WiFi 图标从“叹号”刷新为正常状态）
 */
void net_time_resync(void);
