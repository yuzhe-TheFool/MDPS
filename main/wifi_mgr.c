/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "wifi_mgr.h"

static const char *TAG = "wifi_mgr";

#define WIFI_MAX_RETRY 3
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;
static wifi_mgr_state_t s_state = WIFI_MGR_IDLE;
static volatile bool s_scan_done = false;
static volatile wifi_mgr_inet_t s_inet = WIFI_INET_UNKNOWN;
static volatile int s_last_reason = -1;
static volatile bool s_connect_failed = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // 开机后自动连接 NVS 里保存的网络
        ESP_LOGI(TAG, "WiFi started, connecting to saved AP...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        int reason = disc ? disc->reason : -1;

        /* 主动断开（切换网络 / 用户取消）产生的事件：
           既不是失败也不需要自动重连，直接忽略，避免和新的连接抢状态 */
        if (reason == WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGI(TAG, "ASSOC_LEAVE (manual disconnect), skip retry");
            return;
        }

        ESP_LOGW(TAG, "Disconnected, reason=%d (15=handshake timeout/密码错, 201=找不到AP, 203=认证失败)",
                 reason);
        s_last_reason = reason;
        s_state = WIFI_MGR_DISCONNECTED;
        s_inet = WIFI_INET_NO;

        /* 密码错 / 认证失败：重试没有意义，立刻判失败，让用户马上看到原因 */
        bool auth_fail = (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                          reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                          reason == WIFI_REASON_AUTH_FAIL);
        if (auth_fail) {
            ESP_LOGW(TAG, "Auth error (reason %d), stop retrying", reason);
            s_retry_count = 0;
            s_connect_failed = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "Disconnected, retrying %d/%d", s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Connect failed after %d retries", WIFI_MAX_RETRY);
            s_retry_count = 0;
            s_connect_failed = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        s_scan_done = true;
        ESP_LOGI(TAG, "WiFi scan done");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_state = WIFI_MGR_CONNECTED;
        s_connect_failed = false;
        s_last_reason = -1;
        /* 已确认过外网可达就保持，避免重连瞬间掉回“叹号”状态 */
        if (s_inet != WIFI_INET_YES) {
            s_inet = WIFI_INET_UNKNOWN;
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_mgr_init(void)
{
    // 初始化 NVS（WiFi 凭证存储）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, NULL));

    // 配置保存到 FLASH（NVS），断电重启后不丢失
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    /* 先断开旧连接：开机可能已自动连上 NVS 里的网络，
       此时直接 connect 会返回 ESP_ERR_WIFI_CONN 且不产生任何事件 */
    esp_wifi_disconnect();

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = WIFI_MGR_CONNECTING;
    s_retry_count = 0;
    s_connect_failed = false;
    s_last_reason = -1;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wifi_mgr_disconnect(void)
{
    s_state = WIFI_MGR_IDLE;
    return esp_wifi_disconnect();
}

esp_err_t wifi_mgr_scan_start(void)
{
    s_scan_done = false;
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);  // 异步扫描
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool wifi_mgr_scan_is_done(void)
{
    return s_scan_done;
}

int wifi_mgr_scan_get_results(wifi_mgr_ap_t *aps, int max_count)
{
    if (!aps || max_count <= 0) {
        return 0;
    }

    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK) {
        return 0;
    }
    if (ap_num > max_count) {
        ap_num = max_count;
    }
    if (ap_num == 0) {
        return 0;
    }

    wifi_ap_record_t *records = malloc(ap_num * sizeof(wifi_ap_record_t));
    if (!records) {
        return 0;
    }

    uint16_t got = ap_num;
    if (esp_wifi_scan_get_ap_records(&got, records) != ESP_OK) {
        free(records);
        return 0;
    }

    for (int i = 0; i < got; i++) {
        strncpy(aps[i].ssid, (char *)records[i].ssid, sizeof(aps[i].ssid) - 1);
        aps[i].ssid[sizeof(aps[i].ssid) - 1] = '\0';
        aps[i].rssi = records[i].rssi;
        aps[i].authmode = records[i].authmode;
    }
    free(records);
    s_scan_done = false;
    return got;
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    return s_state;
}

bool wifi_mgr_is_connected(void)
{
    return s_state == WIFI_MGR_CONNECTED;
}

wifi_mgr_inet_t wifi_mgr_get_inet(void)
{
    return s_inet;
}

void wifi_mgr_set_inet(bool ok)
{
    s_inet = ok ? WIFI_INET_YES : WIFI_INET_NO;
}

int wifi_mgr_get_last_reason(void)
{
    return s_last_reason;
}

bool wifi_mgr_connect_failed(void)
{
    return s_connect_failed;
}
