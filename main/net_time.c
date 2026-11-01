/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "wifi_mgr.h"
#include "main_ui.h"
#include "net_time.h"

static const char *TAG = "net_time";

static volatile bool s_synced = false;      /* SNTP 是否已同步（决定是否重发请求） */
static volatile bool s_time_valid = false;  /* 系统时间是否有效（决定界面显示） */

#define NTP_SERVER    "cn.pool.ntp.org"   /* 国内 NTP 服务器 */
#define NTP_RETRY_MIN_MS 1000             /* 失败后第一次重试只等 1s */
#define NTP_RETRY_MAX_MS 20000            /* 一直失败就退避到 20s 一次 */

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    wifi_mgr_set_inet(true);   /* 能同步到外网 NTP 服务器 ⇒ 外网可达 */
    ESP_LOGW(TAG, "NTP synced");
}

/* 未同步就尽快重试（1s → 2s → 4s → 8s → 16s → 20s 封顶）：
   lwIP/SNTP 自身请求失败后的重试间隔可能很长，而刚连上 WiFi 时 DNS 往往还没就绪、
   第一次查询极易失败。这里一失败就尽快重发，同步成功后自动停下。 */
static void ntp_retry_task(void *arg)
{
    (void)arg;
    int retry = 0;
    uint32_t wait_ms = NTP_RETRY_MIN_MS;
    uint32_t waited = 0;

    while (1) {
        /* 500ms 轮询：重连后 resync 请求能在半秒内被响应 */
        vTaskDelay(pdMS_TO_TICKS(500));

        if (s_synced || !wifi_mgr_is_connected()) {
            waited = 0;
            wait_ms = NTP_RETRY_MIN_MS;
            continue;
        }

        waited += 500;
        if (waited < wait_ms) {
            continue;
        }
        waited = 0;

        if (esp_sntp_enabled()) {
            esp_sntp_stop();
        }
        esp_sntp_init();
        ESP_LOGW(TAG, "NTP not synced, re-request #%d", ++retry);

        wait_ms *= 2;
        if (wait_ms > NTP_RETRY_MAX_MS) {
            wait_ms = NTP_RETRY_MAX_MS;
        }
    }
}

void net_time_init(void)
{
    /* 时区 UTC+8（中国） */
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (%s)", NTP_SERVER);

    xTaskCreate(ntp_retry_task, "ntp_retry", 3072, NULL, 4, NULL);
}

void net_time_update(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    /* 年份 >= 2020 视为时间有效（否则 SNTP 还没同步，显示默认 --:--） */
    if (t.tm_year >= 120) {
        if (!s_time_valid) {
            s_time_valid = true;
            ESP_LOGI(TAG, "Time valid: %02d:%02d", t.tm_hour, t.tm_min);
        }
        main_ui_set_time(t.tm_hour, t.tm_min, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_wday);
    } else {
        main_ui_set_time(-1, -1, -1, -1, -1, -1);   /* 未同步：恢复占位 */
    }
}

void net_time_resync(void)
{
    /* 重新进入“未同步”状态，retry 任务会在 500ms 内重发请求，
       同步成功后回调里会把 WiFi 外网状态置为可达 */
    s_synced = false;
    ESP_LOGI(TAG, "NTP resync requested");
}
