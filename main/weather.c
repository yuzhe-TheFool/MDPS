/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "main_ui.h"
#include "weather.h"

static const char *TAG = "weather";

#define WX_RESP_MAX 2048
#define WX_REFRESH_MS (60 * 60 * 1000)   /* 成功后每 1 小时刷新一次 */
#define WX_RETRY_MIN_MS (1000)           /* 失败后第一次重试只等 1s */
#define WX_RETRY_MAX_MS (20000)          /* 一直失败就退避到 20s 一次 */

/* 定位失败时的默认坐标：北京 */
#define DEF_LAT 39.9
#define DEF_LON 116.4

static char s_resp[WX_RESP_MAX];
static double s_lat = DEF_LAT, s_lon = DEF_LON;
static bool s_loc_ok = false;

static esp_err_t wx_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t room = WX_RESP_MAX - 1 - strlen(s_resp);
        if (evt->data_len < room) {
            memcpy(s_resp + strlen(s_resp), evt->data, evt->data_len);
            s_resp[strlen(s_resp) + evt->data_len] = '\0';
        }
    }
    return ESP_OK;
}

static double json_double(const char *json, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    return atof(p + strlen(pat));
}

/* IP 定位：通过外网 IP 获取所在城市经纬度（城市级），无需 GPS */
static bool loc_get_coords(void)
{
    memset(s_resp, 0, sizeof(s_resp));
    esp_http_client_config_t cfg = {
        .url = "http://ip-api.com/json/",
        .event_handler = wx_http_event,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IP locate HTTP failed: %s", esp_err_to_name(err));
        return false;
    }
    if (strstr(s_resp, "\"status\":\"success\"") == NULL) {
        ESP_LOGW(TAG, "IP locate not success: %.80s", s_resp);
        return false;
    }
    double lat = json_double(s_resp, "lat");
    double lon = json_double(s_resp, "lon");
    if (lat == 0 && lon == 0) {
        return false;
    }
    s_lat = lat;
    s_lon = lon;
    ESP_LOGI(TAG, "IP located: lat=%.4f lon=%.4f", s_lat, s_lon);
    return true;
}

static bool wx_parse_response(void)
{
    /* Open-Meteo 响应结构固定：在 "current_weather":{...} 段内提取字段。
       在段内搜索可避免误匹配 current_weather_units 里的同名键。
       （ESP-IDF v6 移除了内置 cJSON，用字符串提取避免额外依赖） */
    const char *cw = strstr(s_resp, "\"current_weather\":");
    if (cw == NULL) {
        ESP_LOGE(TAG, "No current_weather in response");
        return false;
    }
    const char *t = strstr(cw, "\"temperature\":");
    const char *w = strstr(cw, "\"weathercode\":");
    if (t == NULL || w == NULL) {
        ESP_LOGE(TAG, "temperature/weathercode not found");
        return false;
    }
    int temp = (int)atof(t + strlen("\"temperature\":"));
    int code = atoi(w + strlen("\"weathercode\":"));
    ESP_LOGI(TAG, "Weather: %dC code=%d", temp, code);
    main_ui_set_weather(temp, code);
    return true;
}

static void wx_task(void *arg)
{
    (void)arg;
    uint32_t wait_ms = WX_RETRY_MIN_MS;

    while (1) {
        /* 定位：失败就下次循环接着试（成功后不再定位） */
        if (!s_loc_ok) {
            s_loc_ok = loc_get_coords();
            if (!s_loc_ok) {
                ESP_LOGW(TAG, "Locate failed, fallback to default (%.1f,%.1f)", DEF_LAT, DEF_LON);
            }
        }

        char url[192];
        snprintf(url, sizeof(url),
                 "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true",
                 s_lat, s_lon);

        bool ok = false;
        memset(s_resp, 0, sizeof(s_resp));
        esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = wx_http_event,
            .timeout_ms = 10000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == NULL) {
            ESP_LOGE(TAG, "HTTP client init failed");
        } else {
            esp_err_t err = esp_http_client_perform(client);
            esp_http_client_cleanup(client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
            } else {
                /* 解析失败也算失败 → 快速重试，而不是白等 1 小时 */
                ok = wx_parse_response();
            }
        }

        if (ok) {
            ESP_LOGI(TAG, "Weather refreshed, next in 1h");
            wait_ms = WX_RETRY_MIN_MS;      /* 下次失败重新从 1s 起 */
            vTaskDelay(pdMS_TO_TICKS(WX_REFRESH_MS));
        } else {
            ESP_LOGW(TAG, "Weather fetch failed, retry in %ums", (unsigned)wait_ms);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            wait_ms *= 2;
            if (wait_ms > WX_RETRY_MAX_MS) {
                wait_ms = WX_RETRY_MAX_MS;
            }
        }
    }
}

void weather_start(void)
{
    ESP_LOGI(TAG, "Starting weather (auto-locate by IP)...");
    xTaskCreate(wx_task, "weather", 4096, NULL, 5, NULL);
}
