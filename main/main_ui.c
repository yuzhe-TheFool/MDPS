/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "lvgl.h"
#include "wifi_mgr.h"
#include "main_ui.h"
#include "prov_ui.h"
#include "power_ui.h"
#include "wx_icons.h"
#include "fan.h"

static const char *TAG = "main_ui";

/* 共享输入组（定义于此，main.c / prov_ui.c 通过 extern 使用） */
lv_group_t *ui_group = NULL;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_weather_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_wifi_btn = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_wifi_badge = NULL;   // 叠加在 WiFi 图标上的角标：无 / ! / X
static lv_obj_t *s_charge_btn = NULL;  // 底部：充电器入口
static lv_obj_t *s_power_btn = NULL;   // 底部：电源入口

static int s_wifi_state = -1;   /* -1=未初始化 0=未连接 1=已连但无外网 2=可上网 */

/* 左上角风扇状态图标（lv_canvas 自绘三叶，26×26） */
#define FAN_ICON_SZ 26
static lv_obj_t *s_fan_canvas = NULL;
static uint8_t s_fan_buf[FAN_ICON_SZ * FAN_ICON_SZ * 2] __attribute__((aligned(8)));
static int  s_fan_state = -1;          /* 上次绘制状态：0停 1转 2堵转 */
static int  s_fan_rot = 0;             /* 叶片旋转角(0~119，三叶120°对称) */
static bool s_fan_vis = true;
static uint32_t s_fan_anim_tick = 0;
static uint32_t s_fan_blink_tick = 0;

/* 重画一帧三叶风扇。中心留孔，叶宽~36°，三叶每 120° 一片；半径随画布缩放 */
static void fan_icon_draw(int rot_deg, uint32_t color)
{
    if (!s_fan_canvas) return;
    lv_canvas_fill_bg(s_fan_canvas, lv_color_black(), LV_OPA_COVER);
    const float cx = (FAN_ICON_SZ - 1) * 0.5f;
    const float cy = cx;
    const float r_in = FAN_ICON_SZ * 0.09f;   /* 中心孔 */
    const float r_out = FAN_ICON_SZ * 0.40f;  /* 扇叶外径 */
    lv_color_t col = lv_color_hex(color);
    for (int y = 0; y < FAN_ICON_SZ; y++) {
        for (int x = 0; x < FAN_ICON_SZ; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx * dx + dy * dy);
            if (r <= r_in || r > r_out) continue;          /* 中心孔 / 超出扇面 */
            float ang = atan2f(dy, dx) * 180.0f / 3.14159265f;
            int deg = ((int)ang + 360) % 360;
            int rel = (deg - rot_deg % 120 + 1200) % 120;
            if (rel < 36) lv_canvas_set_px(s_fan_canvas, x, y, col, LV_OPA_COVER);
        }
    }
}

/* 时间 / 日期 / 天气数据（由 NTP / 天气模块填充） */
static int s_hour = -1, s_min = -1;
static int s_year = -1, s_mon = -1, s_mday = -1, s_wday = -1;
static int s_temp = -100;
static int s_wx_code = -1;        /* WMO weather code */
static bool s_weather_valid = false;

static char s_time_str[16] = "--:--";
static char s_weather_str[32] = "Weather --";
static char s_date_str[32] = "";

/* 联网数据门：时间+天气都获取完成才一起显示（避免随机先后） */
static bool s_net_pending = true;
static uint32_t s_net_start_tick = 0;
static bool s_weather_shown = false;

/* 天气图标（QWeather 图片） */
static lv_obj_t *s_wx_img = NULL;

/* 点击 WiFi 图标：已连接 → 显示当前网络/IP 信息页；未连接 → 进配网扫描 */
static void main_ui_wifi_click_cb(lv_event_t *e)
{
    if (wifi_mgr_is_connected()) {
        ESP_LOGI(TAG, "Enter wifi info (connected)");
        prov_ui_create_info();
    } else {
        ESP_LOGI(TAG, "Enter provisioning (not connected)");
        prov_ui_create();
    }
}

/* 点击底部按钮 → 进入充电器 / 电源界面 */
static void main_ui_charge_cb(lv_event_t *e)
{
    (void)e;
    power_ui_create_charge();
}
static void main_ui_power_cb(lv_event_t *e)
{
    (void)e;
    power_ui_create_power();
}

void main_ui_init(lv_group_t *group)
{
    ui_group = group;

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    /* 关闭滚动条 + 无边框/padding，避免屏底出现贯穿横线 */
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);

    /* 左上角：26×26 风扇贴顶（避开天气图标），略往右与内容错开 */
    s_fan_canvas = lv_canvas_create(s_scr);
    lv_obj_set_size(s_fan_canvas, FAN_ICON_SZ, FAN_ICON_SZ);
    lv_canvas_set_buffer(s_fan_canvas, s_fan_buf, FAN_ICON_SZ, FAN_ICON_SZ,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(s_fan_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_fan_canvas, 0, 0);
    lv_obj_set_style_radius(s_fan_canvas, 0, 0);
    lv_obj_align(s_fan_canvas, LV_ALIGN_TOP_LEFT, 14, 4);
    fan_icon_draw(0, 0x81D4FA);

    /* 天气图标（QWeather，56x56，屏幕左侧上部；温度随之与日期同行） */
    s_wx_img = lv_image_create(s_scr);
    lv_image_set_src(s_wx_img, &wx_100);
    lv_obj_set_size(s_wx_img, 56, 56);
    lv_obj_align(s_wx_img, LV_ALIGN_LEFT_MID, 8, -29);
    /* 联网前不显示（避免晚上开机出现太阳占位图） */
    lv_obj_add_flag(s_wx_img, LV_OBJ_FLAG_HIDDEN);

    /* 温度（天气图标正下方，相对图标居中，字号与日期一致） */
    s_weather_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_weather_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_weather_label, lv_color_hex(0xB0BEC5), 0);
    lv_label_set_text(s_weather_label, "--°C");
    lv_obj_align_to(s_weather_label, s_wx_img, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    /* 时间（居中大字，略下移给上方天气组合让位） */
    s_time_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -20);

    /* 日期（时间下方居中） */
    s_date_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xB0BEC5), 0);
    lv_label_set_text(s_date_label, "");
    lv_obj_align(s_date_label, LV_ALIGN_CENTER, 0, 10);

    /* WiFi 标识（右上角，透明按钮，仅图标；点击进入配网） */
    s_wifi_btn = lv_button_create(s_scr);
    lv_obj_set_size(s_wifi_btn, 44, 44);
    lv_obj_align(s_wifi_btn, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_set_style_bg_opa(s_wifi_btn, LV_OPA_TRANSP, 0);   // 透明背景
    lv_obj_set_style_border_width(s_wifi_btn, 0, 0);         // 无边框
    lv_obj_set_style_shadow_width(s_wifi_btn, 0, 0);         // 无阴影
    lv_obj_set_style_pad_all(s_wifi_btn, 0, 0);
    /* 编码器聚焦时的醒目高亮（亮青背景 + 粗边框，不用 outline 避免内存压力） */
    lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(0x0E4A5A), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(s_wifi_btn, lv_color_hex(0x00E5FF), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(s_wifi_btn, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_radius(s_wifi_btn, 6, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(0x0E4A5A), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(s_wifi_btn, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_wifi_btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(s_wifi_btn, 6, LV_STATE_FOCUSED);
    /* 未连接时的白色小斜线标识（角标） */
    s_wifi_label = lv_label_create(s_wifi_btn);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_wifi_label);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x9E9E9E), 0);  // 默认灰色（未连接）

    /* WiFi 角标：可上网=隐藏；无外网=琥珀色 !；未连接=红色 X */
    s_wifi_badge = lv_label_create(s_wifi_btn);
    lv_obj_set_style_text_font(s_wifi_badge, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_wifi_badge, lv_color_hex(0xFF5252), 0);
    lv_label_set_text(s_wifi_badge, LV_SYMBOL_CLOSE);
    lv_obj_align(s_wifi_badge, LV_ALIGN_CENTER, 5, -7);   // 偏右上：角标叠在 WiFi 信号上
    lv_obj_add_event_cb(s_wifi_btn, main_ui_wifi_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_remove_flag(s_wifi_btn, LV_OBJ_FLAG_SCROLLABLE);
    /* 图标 44×44 偏小且贴角：命中区四周各外扩 14px（实际 72×72） */
    lv_obj_set_ext_click_area(s_wifi_btn, 14);

    /* 底部：充电器 + 电源 入口（透明图标按钮，同 WiFi 风格） */
    const char *nav_sym[2] = {LV_SYMBOL_CHARGE, LV_SYMBOL_POWER};
    lv_event_cb_t nav_cb[2] = {main_ui_charge_cb, main_ui_power_cb};
    lv_align_t nav_align[2] = {LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT};
    /* 右侧偏移用 -6，与右上角 WiFi 按钮同一 x 线 */
    const int nav_xoff[2] = {8, -6};
    lv_obj_t **nav_ptrs[2] = {&s_charge_btn, &s_power_btn};
    for (int i = 0; i < 2; i++) {
        *nav_ptrs[i] = lv_button_create(s_scr);
        lv_obj_set_size(*nav_ptrs[i], 44, 44);
        lv_obj_align(*nav_ptrs[i], nav_align[i], nav_xoff[i], -6);
        lv_obj_set_style_bg_opa(*nav_ptrs[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(*nav_ptrs[i], 0, 0);
        lv_obj_set_style_shadow_width(*nav_ptrs[i], 0, 0);
        lv_obj_set_style_pad_all(*nav_ptrs[i], 0, 0);
        lv_obj_set_style_bg_color(*nav_ptrs[i], lv_color_hex(0x0E4A5A), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(*nav_ptrs[i], lv_color_hex(0x00E5FF), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(*nav_ptrs[i], 3, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_radius(*nav_ptrs[i], 6, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_color(*nav_ptrs[i], lv_color_hex(0x0E4A5A), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(*nav_ptrs[i], lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(*nav_ptrs[i], 3, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(*nav_ptrs[i], 6, LV_STATE_FOCUSED);
        lv_obj_t *lbl = lv_label_create(*nav_ptrs[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_center(lbl);
        lv_label_set_text(lbl, nav_sym[i]);
        /* 主屏(天气)主题淡蓝；充电屏绿、电源屏琥珀黄，三界面入口一看可辨 */
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x81D4FA), 0);
        lv_obj_add_event_cb(*nav_ptrs[i], nav_cb[i], LV_EVENT_CLICKED, NULL);
        lv_obj_remove_flag(*nav_ptrs[i], LV_OBJ_FLAG_SCROLLABLE);
        /* 底部两个入口同样贴角小图标：命中区外扩 14px，中间大片空白无冲突 */
        lv_obj_set_ext_click_area(*nav_ptrs[i], 14);
    }

    /* 不加入输入组：主界面保持空组 → 开机无焦点框；首次旋转/按键时由
       main_ui_focus_main 统一加入（WiFi/Charge/Power） */
    main_ui_update();
    lv_scr_load(s_scr);
}

void main_ui_show(void)
{
    if (!s_scr) {
        return;
    }
    lv_group_remove_all_objs(ui_group);
    lv_group_set_editing(ui_group, false);
    /* 不加入输入组：返回主界面无焦点框，首次旋转/按键时再 add+focus */
    lv_scr_load(s_scr);
}

/* 主界面导航状态：0=无选中，1=WiFi，2=Charge，3=Power */
void main_ui_set_nav(int state)
{
    if (!ui_group) {
        return;
    }
    lv_group_remove_all_objs(ui_group);
    lv_group_set_editing(ui_group, false);
    lv_obj_t *obj = NULL;
    if (state == 1) {
        obj = s_wifi_btn;
    } else if (state == 2) {
        obj = s_charge_btn;
    } else if (state == 3) {
        obj = s_power_btn;
    }
    if (obj) {
        lv_group_add_obj(ui_group, obj);
        lv_group_focus_obj(obj);
    }
    /* state == 0：组为空 → 无焦点框（任意都不选中） */
}

/* 主界面是否当前活动屏 */
bool main_ui_is_active(void)
{
    return s_scr && (lv_screen_active() == s_scr);
}

void main_ui_update(void)
{
    if (!s_scr) {
        return;
    }

    /* 左上风扇三态：0停=淡蓝静止，1转=绿+旋转，2堵转=红+闪烁 */
    if (s_fan_canvas) {
        int st = fan_get_state();
        uint32_t now = lv_tick_get();
        if (st == 0) {
            if (s_fan_state != 0) {
                s_fan_state = 0;
                lv_obj_clear_flag(s_fan_canvas, LV_OBJ_FLAG_HIDDEN);
                fan_icon_draw(0, 0x81D4FA);
            }
        } else if (st == 1) {
            if (s_fan_state != 1) {
                s_fan_state = 1;
                s_fan_vis = true;
                s_fan_anim_tick = now;
                lv_obj_clear_flag(s_fan_canvas, LV_OBJ_FLAG_HIDDEN);
                fan_icon_draw(s_fan_rot, 0x00E676);
            } else if ((int32_t)(now - s_fan_anim_tick) >= 120) {
                s_fan_anim_tick = now;
                s_fan_rot = (s_fan_rot + 15) % 120;   /* 每 120ms 转 15° */
                fan_icon_draw(s_fan_rot, 0x00E676);
            }
        } else {   /* 堵转/失效：红 + 0.5s 闪烁 */
            if (s_fan_state != 2) {
                s_fan_state = 2;
                s_fan_vis = true;
                s_fan_blink_tick = now;
                lv_obj_clear_flag(s_fan_canvas, LV_OBJ_FLAG_HIDDEN);
                fan_icon_draw(0, 0xFF5252);
            }
            if ((int32_t)(now - s_fan_blink_tick) >= 500) {
                s_fan_blink_tick = now;
                s_fan_vis = !s_fan_vis;
                if (s_fan_vis) {
                    lv_obj_clear_flag(s_fan_canvas, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_fan_canvas, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    /* WiFi 三档状态：
       可上网       → 绿色 WiFi，无角标
       无外网       → 琥珀色 WiFi + 琥珀色 ! 角标
       未连接       → 灰色 WiFi + 红色 X 角标 */
    int inet = 0;
    if (wifi_mgr_is_connected()) {
        inet = (wifi_mgr_get_inet() == WIFI_INET_YES) ? 2 : 1;
    }
    if (inet != s_wifi_state) {
        s_wifi_state = inet;
        if (inet == 2) {
            lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x00E676), 0);
            lv_obj_add_flag(s_wifi_badge, LV_OBJ_FLAG_HIDDEN);
        } else if (inet == 1) {
            lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0xFFA726), 0);
            lv_label_set_text(s_wifi_badge, "!");
            lv_obj_set_style_text_color(s_wifi_badge, lv_color_hex(0xFFA726), 0);
            lv_obj_clear_flag(s_wifi_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x9E9E9E), 0);
            lv_label_set_text(s_wifi_badge, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(s_wifi_badge, lv_color_hex(0xFF5252), 0);
            lv_obj_clear_flag(s_wifi_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 联网数据门：时间+天气都获取完成（或 20s 超时兜底）才一起显示，避免随机先后 */
    if (s_net_pending) {
        if ((s_hour >= 0 && s_weather_valid) ||
            (lv_tick_get() - s_net_start_tick > 20000)) {
            s_net_pending = false;
        } else {
            /* 未就绪：显示缺省 */
            if (strcmp(s_time_str, "--:--") != 0) {
                strncpy(s_time_str, "--:--", sizeof(s_time_str));
                s_time_str[sizeof(s_time_str) - 1] = '\0';
                lv_label_set_text(s_time_label, s_time_str);
            }
            if (strcmp(s_weather_str, "Weather --") != 0) {
                strncpy(s_weather_str, "Weather --", sizeof(s_weather_str));
                s_weather_str[sizeof(s_weather_str) - 1] = '\0';
                lv_label_set_text(s_weather_label, s_weather_str);
            }
            return;
        }
    }

    /* 时间（就绪后持续更新） */
    if (s_hour >= 0 && s_min >= 0) {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "%02d:%02d", s_hour % 24, s_min % 60);
        if (strcmp(tmp, s_time_str) != 0) {
            strncpy(s_time_str, tmp, sizeof(s_time_str) - 1);
            s_time_str[sizeof(s_time_str) - 1] = '\0';
            lv_label_set_text(s_time_label, s_time_str);
        }
    } else if (strcmp(s_time_str, "--:--") != 0) {
        strncpy(s_time_str, "--:--", sizeof(s_time_str));
        s_time_str[sizeof(s_time_str) - 1] = '\0';
        lv_label_set_text(s_time_label, s_time_str);
    }

    /* 天气：就绪后一次性应用（图标 + 温度） */
    if (s_weather_valid && !s_weather_shown) {
        s_weather_shown = true;
        bool is_day = (s_hour >= 6 && s_hour < 18);
        lv_image_set_src(s_wx_img, wx_icon_get(s_wx_code, is_day));        lv_obj_clear_flag(s_wx_img, LV_OBJ_FLAG_HIDDEN);        snprintf(s_weather_str, sizeof(s_weather_str), "%d°C", s_temp);
        lv_label_set_text(s_weather_label, s_weather_str);
    } else if (!s_weather_valid && strcmp(s_weather_str, "Weather --") != 0) {
        strncpy(s_weather_str, "Weather --", sizeof(s_weather_str));
        s_weather_str[sizeof(s_weather_str) - 1] = '\0';
        lv_label_set_text(s_weather_label, s_weather_str);
    }

    /* 日期（时间下方，联网后显示） */
    if (s_year >= 2020 && s_mday >= 1) {
        static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int w = s_wday;
        if (w < 0 || w > 6) w = 0;
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d-%02d-%02d %s", s_year, s_mon, s_mday, wd[w]);
        if (strcmp(tmp, s_date_str) != 0) {
            strncpy(s_date_str, tmp, sizeof(s_date_str) - 1);
            s_date_str[sizeof(s_date_str) - 1] = '\0';
            lv_label_set_text(s_date_label, s_date_str);
        }
    }
}

void main_ui_set_time(int hour, int minute, int year, int mon, int mday, int wday)
{
    s_hour = hour;
    s_min = minute;
    s_year = year;
    s_mon = mon;
    s_mday = mday;
    s_wday = wday;
}

/* 联网成功：启动联网数据门（期间显示缺省，等时间+天气都就绪再一起显示） */
void main_ui_net_connected(void)
{
    s_net_pending = true;
    s_net_start_tick = lv_tick_get();
    s_weather_shown = false;
}

/* 天气任务调用：只存数据（不操作 LVGL，避免跨任务并发），由 main_ui_update 统一应用 */
void main_ui_set_weather(int temp_c, int weather_code)
{
    s_temp = temp_c;
    s_wx_code = weather_code;
    s_weather_valid = true;
}
