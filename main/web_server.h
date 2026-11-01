/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动内置网页服务器（端口 80）
 *
 * 需在 WiFi 连接成功后调用；重复调用无副作用。
 * 浏览器访问 http://<设备IP>/ 即可查看参数并做简单控制。
 */
void web_server_start(void);

#ifdef __cplusplus
}
#endif
