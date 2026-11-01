/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_IDLE = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_DISCONNECTED,
} wifi_mgr_state_t;

/* 外网（Internet）连通性：连上 AP 不代表能上网 */
typedef enum {
    WIFI_INET_UNKNOWN = 0,  /*!< 刚拿到 IP，尚未确认能否上网 */
    WIFI_INET_NO,           /*!< 无法访问外网 */
    WIFI_INET_YES,          /*!< 可正常访问外网 */
} wifi_mgr_inet_t;

typedef struct {
    char ssid[33];          /*!< 网络名称 */
    int8_t rssi;            /*!< 信号强度 dBm */
    wifi_auth_mode_t authmode;  /*!< 加密方式 */
} wifi_mgr_ap_t;

/**
 * @brief 初始化 WiFi（NVS/netif/event loop/驱动）
 *
 * 配置会保存到 NVS（WIFI_STORAGE_FLASH），下次开机自动加载并连接。
 */
esp_err_t wifi_mgr_init(void);

/**
 * @brief 设置并连接指定 WiFi（保存到 NVS，开机自动重连）
 */
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);

/**
 * @brief 断开 WiFi
 */
esp_err_t wifi_mgr_disconnect(void);

/**
 * @brief 启动异步 WiFi 扫描（不阻塞）
 */
esp_err_t wifi_mgr_scan_start(void);

/**
 * @brief 扫描是否完成
 */
bool wifi_mgr_scan_is_done(void);

/**
 * @brief 获取扫描结果（需在扫描完成后调用）
 * @return 返回的网络数量
 */
int wifi_mgr_scan_get_results(wifi_mgr_ap_t *aps, int max_count);

wifi_mgr_state_t wifi_mgr_get_state(void);
bool wifi_mgr_is_connected(void);

/**
 * @brief 获取外网连通性
 */
wifi_mgr_inet_t wifi_mgr_get_inet(void);

/**
 * @brief 由网络模块（如 NTP 同步成功）上报外网是否可达
 */
void wifi_mgr_set_inet(bool ok);

/**
 * @brief 最近一次断开的原因码（wifi_err_reason_t），未发生过断开时为 -1
 */
int wifi_mgr_get_last_reason(void);

/**
 * @brief 本轮连接是否已彻底失败（内部重试次数用尽）
 */
bool wifi_mgr_connect_failed(void);

#ifdef __cplusplus
}
#endif
