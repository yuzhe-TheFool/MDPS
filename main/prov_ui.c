/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lvgl.h"
#include "wifi_mgr.h"
#include "main_ui.h"
#include "prov_ui.h"

static const char *TAG = "prov_ui";

#define MAX_APS 20

typedef enum {
    PROV_IDLE,
    PROV_SCANNING,
    PROV_LIST,
    PROV_PASSWORD,
    PROV_CONNECTING,
    PROV_FAILED,
} prov_state_t;

static prov_state_t s_state = PROV_IDLE;

static lv_obj_t *s_scan_scr = NULL;    // 扫描/列表屏
static lv_obj_t *s_pass_scr = NULL;    // 密码屏

static lv_obj_t *s_pass_ta = NULL;
static lv_obj_t *s_pass_kb = NULL;
static lv_obj_t *s_pass_ssid = NULL;     /* 当前 SSID 标签 */
static lv_obj_t *s_pass_status = NULL;   /* 连接状态 / 失败原因 */
static lv_obj_t *s_pass_retry = NULL;    /* 失败态的 RETRY 按钮 */
static lv_obj_t *s_pass_cancel = NULL;   /* 失败态的 CANCEL 按钮 */
static uint32_t s_connect_tick = 0;

static wifi_mgr_ap_t s_aps[MAX_APS];
static int s_ap_count = 0;
static char s_selected_ssid[33] = {0};
static bool s_started_scan = false;

/* 前置声明（供界面构建函数引用） */
static void prov_ap_click_cb(lv_event_t *e);
static void prov_rescan_cb(lv_event_t *e);
static void prov_pass_ta_event_cb(lv_event_t *e);
static void prov_kb_ready_cb(lv_event_t *e);
static void prov_do_connect(void);
static void prov_cancel_cb(lv_event_t *e);
static void prov_list_cancel_cb(lv_event_t *e);
static void prov_password_input_mode(void);
static void prov_password_fail_mode(const char *msg);
static void prov_retry_cb(lv_event_t *e);
static void prov_fail_cancel_cb(lv_event_t *e);

/* ---------- 通用控件 ---------- */

/* 底部文字按钮：蓝底白字，旋钮聚焦时青色高亮 */
static lv_obj_t *info_btn(lv_obj_t *parent, const char *txt,
                          lv_align_t align, int x, int y, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 78, 34);
    lv_obj_align(b, align, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2196F3), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(b, lv_color_hex(0x00E5FF), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(b, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2196F3), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(b, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(b, 3, LV_STATE_FOCUSED);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);

    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* 信号强度：4 根由矮到高的竖条，按 RSSI 点亮数量 */
static void sig_bars(lv_obj_t *parent, int rssi)
{
    int level;
    if (rssi >= -55) {
        level = 4;
    } else if (rssi >= -67) {
        level = 3;
    } else if (rssi >= -78) {
        level = 2;
    } else {
        level = 1;
    }

    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(b, 4, 4 + i * 4);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -8 - (3 - i) * 6, -11);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, i < level ? lv_color_hex(0x4FC3F7)
                                               : lv_color_hex(0x37474F), 0);
    }
}

/* ---------- 界面构建 ---------- */

static void prov_scanning_ui(void)
{
    if (!s_scan_scr) {
        s_scan_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_scan_scr, lv_color_black(), 0);
    }
    lv_obj_clean(s_scan_scr);

    lv_obj_t *label = lv_label_create(s_scan_scr);
    lv_label_set_text(label, "SCANNING...");
    lv_obj_set_style_text_color(label, lv_color_hex(0x81D4FA), 0);
    lv_obj_center(label);

    lv_scr_load(s_scan_scr);
}

static void prov_list_ui(void)
{
    if (!s_scan_scr) {
        s_scan_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_scan_scr, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_scan_scr, 0, 0);
        lv_obj_set_style_pad_all(s_scan_scr, 0, 0);
        lv_obj_set_scrollbar_mode(s_scan_scr, LV_SCROLLBAR_MODE_OFF);
    }
    if (ui_group) {
        lv_group_remove_all_objs(ui_group);
        lv_group_set_editing(ui_group, false);
    }
    lv_obj_clean(s_scan_scr);

    lv_obj_t *title = lv_label_create(s_scan_scr);
    lv_label_set_text(title, "SELECT WIFI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x81D4FA), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* 屏幕 320x172：列表底部必须留出 CANCEL/RESCAN 按钮区（34px + 6px 底距 + 4px 间隙） */
    lv_display_t *disp = lv_display_get_default();
    int32_t vres = disp ? (int32_t)lv_display_get_vertical_resolution(disp) : 172;
    int32_t list_h = vres - 26 - 40 - 4;

    lv_obj_t *list = lv_obj_create(s_scan_scr);
    lv_obj_set_size(list, 300, list_h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (s_ap_count == 0) {
        lv_obj_t *lbl = lv_label_create(list);
        lv_label_set_text(lbl, "NO NETWORK FOUND");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x9E9E9E), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    } else {
        for (int i = 0; i < s_ap_count; i++) {
            lv_obj_t *item = lv_button_create(list);
            lv_obj_set_size(item, LV_PCT(100), 32);
            lv_obj_set_style_bg_color(item, lv_color_hex(0x1A2230), 0);
            lv_obj_set_style_border_width(item, 0, 0);
            lv_obj_set_style_pad_all(item, 0, 0);
            lv_obj_set_style_radius(item, 4, 0);
            lv_obj_set_style_bg_color(item, lv_color_hex(0x0E4A5A), LV_STATE_FOCUS_KEY);
            lv_obj_set_style_border_color(item, lv_color_hex(0x00E5FF), LV_STATE_FOCUS_KEY);
            lv_obj_set_style_border_width(item, 2, LV_STATE_FOCUS_KEY);
            lv_obj_set_style_bg_color(item, lv_color_hex(0x0E4A5A), LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(item, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(item, 2, LV_STATE_FOCUSED);

            /* SSID 名称 */
            lv_obj_t *t = lv_label_create(item);
            lv_label_set_text(t, s_aps[i].ssid);
            lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(t, lv_color_white(), 0);
            lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);

            /* 信号强度：格数图标 */
            sig_bars(item, s_aps[i].rssi);

            lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(item, prov_ap_click_cb, LV_EVENT_CLICKED,
                                (void *)s_aps[i].ssid);
            if (ui_group) {
                lv_group_add_obj(ui_group, item);
            }
        }
    }

    /* 底部：CANCEL 回主界面 / RESCAN 重新扫描 */
    lv_obj_t *b_cancel = info_btn(s_scan_scr, "CANCEL", LV_ALIGN_BOTTOM_LEFT, 8, -6,
                                  prov_list_cancel_cb);
    lv_obj_t *b_rescan = info_btn(s_scan_scr, "RESCAN", LV_ALIGN_BOTTOM_RIGHT, -8, -6,
                                  prov_rescan_cb);

    if (ui_group) {
        lv_group_add_obj(ui_group, b_cancel);
        lv_group_add_obj(ui_group, b_rescan);
        lv_group_focus_obj(s_ap_count > 0 ? lv_group_get_obj_by_index(ui_group, 0) : b_cancel);
    }

    lv_scr_load(s_scan_scr);
}

/* 编码器旋转产生 NEXT/PREV，键盘底层只认 LEFT/RIGHT，这里做一层映射 */
static void kb_enc_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    uint32_t mapped;
    if (key == LV_KEY_NEXT) {
        mapped = LV_KEY_RIGHT;
    } else if (key == LV_KEY_PREV) {
        mapped = LV_KEY_LEFT;
    } else {
        return;
    }
    lv_obj_send_event(lv_event_get_target(e), LV_EVENT_KEY, &mapped);
}

/* 键盘上的关闭键 → 放弃，返回列表 */
static void kb_close_cb(lv_event_t *e)
{
    (void)e;
    prov_cancel_cb(NULL);
}

/* 自定义小写键盘：左下角用 × 表示取消（LVGL 默认用“键盘”图标，功能相同但易误解） */
#define PRV_KB_BTN(w) (LV_BUTTONMATRIX_CTRL_POPOVER | (w))

static const char * const kb_map_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_CLOSE, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t kb_ctrl_lc[] = {
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 5, PRV_KB_BTN(4), PRV_KB_BTN(4), PRV_KB_BTN(4),
    PRV_KB_BTN(4), PRV_KB_BTN(4), PRV_KB_BTN(4), PRV_KB_BTN(4), PRV_KB_BTN(4),
    PRV_KB_BTN(4), PRV_KB_BTN(4), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6, PRV_KB_BTN(3), PRV_KB_BTN(3), PRV_KB_BTN(3),
    PRV_KB_BTN(3), PRV_KB_BTN(3), PRV_KB_BTN(3), PRV_KB_BTN(3), PRV_KB_BTN(3),
    PRV_KB_BTN(3), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_BUTTONMATRIX_CTRL_CHECKED | PRV_KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | PRV_KB_BTN(1),
    PRV_KB_BTN(1), PRV_KB_BTN(1), PRV_KB_BTN(1), PRV_KB_BTN(1), PRV_KB_BTN(1),
    PRV_KB_BTN(1), PRV_KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | PRV_KB_BTN(1),
    LV_BUTTONMATRIX_CTRL_CHECKED | PRV_KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | PRV_KB_BTN(1),
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2, LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};

static void prov_password_ui(void)
{
    if (!s_pass_scr) {
        s_pass_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_pass_scr, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_pass_scr, 0, 0);
        lv_obj_set_style_pad_all(s_pass_scr, 0, 0);
        lv_obj_set_scrollbar_mode(s_pass_scr, LV_SCROLLBAR_MODE_OFF);
    }
    if (ui_group) {
        lv_group_remove_all_objs(ui_group);
        lv_group_set_editing(ui_group, false);
    }
    lv_obj_clean(s_pass_scr);

    /* 顶部：当前 SSID */
    s_pass_ssid = lv_label_create(s_pass_scr);
    lv_label_set_text(s_pass_ssid, s_selected_ssid);
    lv_obj_set_style_text_font(s_pass_ssid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pass_ssid, lv_color_hex(0x81D4FA), 0);
    lv_obj_align(s_pass_ssid, LV_ALIGN_TOP_MID, 0, 4);

    /* 密码框 */
    s_pass_ta = lv_textarea_create(s_pass_scr);
    lv_obj_set_size(s_pass_ta, 300, 28);
    lv_obj_align(s_pass_ta, LV_ALIGN_TOP_MID, 0, 32);
    lv_textarea_set_placeholder_text(s_pass_ta, "Password");
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_cursor_click_pos(s_pass_ta, true);
    lv_obj_add_event_cb(s_pass_ta, prov_pass_ta_event_cb, LV_EVENT_CLICKED, NULL);

    /* 状态 / 失败原因：屏幕中央偏上，可多行居中 */
    s_pass_status = lv_label_create(s_pass_scr);
    lv_label_set_text(s_pass_status, "");
    lv_obj_set_style_text_font(s_pass_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pass_status, lv_color_hex(0x81D4FA), 0);
    lv_obj_set_style_text_align(s_pass_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_pass_status, 300);
    lv_obj_align(s_pass_status, LV_ALIGN_CENTER, 0, -20);

    /* 键盘 */
    s_pass_kb = lv_keyboard_create(s_pass_scr);
    lv_obj_set_size(s_pass_kb, 320, 106);
    lv_obj_align(s_pass_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(s_pass_kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
    lv_keyboard_set_textarea(s_pass_kb, s_pass_ta);
    lv_obj_add_event_cb(s_pass_kb, prov_kb_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_pass_kb, kb_close_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(s_pass_kb, kb_enc_key_cb, LV_EVENT_KEY, NULL);

    /* 失败态的 RETRY / CANCEL（默认隐藏） */
    s_pass_retry = info_btn(s_pass_scr, "RETRY", LV_ALIGN_BOTTOM_LEFT, 8, -6, prov_retry_cb);
    s_pass_cancel = info_btn(s_pass_scr, "CANCEL", LV_ALIGN_BOTTOM_RIGHT, -8, -6,
                             prov_fail_cancel_cb);
    lv_obj_add_flag(s_pass_retry, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pass_cancel, LV_OBJ_FLAG_HIDDEN);

    lv_scr_load(s_pass_scr);
    prov_password_input_mode();
}

/* ---------- 密码界面三态 ---------- */

static void prov_retry_cb(lv_event_t *e)
{
    (void)e;
    prov_password_input_mode();   /* 回到输入态，用户可重新输密码 */
}

static void prov_fail_cancel_cb(lv_event_t *e)
{
    (void)e;
    s_state = PROV_LIST;
    prov_list_ui();               /* 放弃本次配网，回列表 */
}

/* 输入态：SSID + 密码框 + 键盘 */
static void prov_password_input_mode(void)
{
    if (s_pass_ssid) {
        lv_obj_clear_flag(s_pass_ssid, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_ta) {
        lv_obj_clear_flag(s_pass_ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(s_pass_ta, LV_STATE_DISABLED);
        lv_textarea_set_text(s_pass_ta, "");
    }
    if (s_pass_kb) {
        lv_obj_clear_flag(s_pass_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_status) {
        lv_label_set_text(s_pass_status, "");
    }
    if (s_pass_retry) {
        lv_obj_add_flag(s_pass_retry, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_cancel) {
        lv_obj_add_flag(s_pass_cancel, LV_OBJ_FLAG_HIDDEN);
    }

    s_state = PROV_PASSWORD;

    if (ui_group) {
        lv_group_remove_all_objs(ui_group);
        lv_group_add_obj(ui_group, s_pass_kb);
        lv_group_focus_obj(s_pass_kb);
        lv_group_set_editing(ui_group, true);
    }
}

/* 失败态：只留原因文字 + RETRY/CANCEL，等用户自己选 */
static void prov_password_fail_mode(const char *msg)
{
    if (s_pass_ssid) {
        lv_obj_add_flag(s_pass_ssid, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_ta) {
        lv_obj_add_flag(s_pass_ta, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_kb) {
        lv_obj_add_flag(s_pass_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_status) {
        lv_label_set_text(s_pass_status, msg);
        lv_obj_set_style_text_color(s_pass_status, lv_color_hex(0xFF5252), 0);
    }
    if (s_pass_retry) {
        lv_obj_clear_flag(s_pass_retry, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_cancel) {
        lv_obj_clear_flag(s_pass_cancel, LV_OBJ_FLAG_HIDDEN);
    }

    s_state = PROV_FAILED;

    if (ui_group) {
        lv_group_remove_all_objs(ui_group);
        lv_group_set_editing(ui_group, false);
        lv_group_add_obj(ui_group, s_pass_retry);
        lv_group_add_obj(ui_group, s_pass_cancel);
        lv_group_focus_obj(s_pass_retry);
    }
}

/* ---------- 事件回调 ---------- */

static void prov_ap_click_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid) {
        return;
    }
    strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
    s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';
    s_state = PROV_PASSWORD;
    prov_password_ui();
}

static void prov_rescan_cb(lv_event_t *e)
{
    s_started_scan = false;
    s_state = PROV_SCANNING;
    prov_scanning_ui();
    wifi_mgr_scan_start();
}

static void prov_pass_ta_event_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_pass_kb, ta);
}

static void prov_do_connect(void)
{
    const char *pass = lv_textarea_get_text(s_pass_ta);
    if (pass == NULL || strlen(pass) == 0) {
        if (s_pass_status) {
            lv_label_set_text(s_pass_status, "Password required");
            lv_obj_set_style_text_color(s_pass_status, lv_color_hex(0xFFA726), 0);
        }
        return;
    }
    ESP_LOGI(TAG, "Connecting to '%s'", s_selected_ssid);

    esp_err_t err = wifi_mgr_connect(s_selected_ssid, pass);
    if (err != ESP_OK) {
        /* 发起失败（如 WIFI 忙、SSID 非法），立即报错，不必等超时 */
        char msg[64];
        snprintf(msg, sizeof(msg), "Connect failed\n%s", esp_err_to_name(err));
        prov_password_fail_mode(msg);
        return;
    }

    s_state = PROV_CONNECTING;
    s_connect_tick = lv_tick_get();

    /* 连接中：隐藏 SSID / 密码框 / 键盘，只保留进度提示 */
    if (s_pass_ssid) {
        lv_obj_add_flag(s_pass_ssid, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_ta) {
        lv_obj_add_flag(s_pass_ta, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_kb) {
        lv_obj_add_flag(s_pass_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pass_status) {
        lv_label_set_text(s_pass_status, "Connecting...");
        lv_obj_set_style_text_color(s_pass_status, lv_color_hex(0x81D4FA), 0);
    }
    if (ui_group) {
        lv_group_remove_all_objs(ui_group);
        lv_group_set_editing(ui_group, false);
    }
}

/* 断连原因码 → 简短英文说明 */
static const char *prov_reason_text(int code)
{
    switch (code) {
    case 4:
    case 15:
        return "Wrong password";
    case 201:
        return "Network not found";
    case 202:
    case 203:
        return "Auth failed";
    case 204:
    case 205:
        return "Connect failed";
    default:
        return NULL;
    }
}

/* 连接失败：切到失败态，显示原因并等用户选择 RETRY / CANCEL */
static void prov_show_connect_failure(void)
{
    int code = wifi_mgr_get_last_reason();
    const char *txt = prov_reason_text(code);
    char msg[64];

    if (txt) {
        snprintf(msg, sizeof(msg), "Connect failed\n%s", txt);
    } else {
        snprintf(msg, sizeof(msg), "Connect failed\n(code %d)", code);
    }
    ESP_LOGW(TAG, "connect failed: %s", msg);

    prov_password_fail_mode(msg);
}

static void prov_kb_ready_cb(lv_event_t *e)
{
    prov_do_connect();
}

static void prov_cancel_cb(lv_event_t *e)
{
    (void)e;
    s_state = PROV_LIST;
    prov_list_ui();   /* 重建列表，顺带恢复输入组与 editing 状态 */
}

/* 配网列表：CANCEL 放弃配网，返回主界面 */
static void prov_list_cancel_cb(lv_event_t *e)
{
    (void)e;
    main_ui_show();
}

/* ---------- 对外接口 ---------- */

void prov_ui_create(void)
{
    s_state = PROV_SCANNING;
    s_started_scan = false;
    /* 从信息页（BACK/RESCAN）进入时，组里残留旧按钮，需先清空 */
    if (ui_group) {
        lv_group_remove_all_objs(ui_group);
        lv_group_set_editing(ui_group, false);
    }
    prov_scanning_ui();
}

/* 已连接时的网络信息页：标题 + SSID/IP + 底部 BACK/RESCAN 两个按钮 */
static lv_obj_t *s_info_scr = NULL;

static void prov_info_back_cb(lv_event_t *e)
{
    (void)e;
    main_ui_show();
}

static void prov_info_rescan_cb(lv_event_t *e)
{
    (void)e;
    prov_ui_create();   /* 重新配网入口：回到扫描列表 */
}

void prov_ui_create_info(void)
{
    char ssid[33] = "--";
    char ip[16] = "--";
    char line[64];

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(ssid, sizeof(ssid), "%s", (const char *)ap.ssid);
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info = {0};
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    ESP_LOGI(TAG, "WiFi info: ssid=%s ip=%s", ssid, ip);

    if (!s_info_scr) {
        s_info_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_info_scr, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_info_scr, 0, 0);
        lv_obj_set_style_pad_all(s_info_scr, 0, 0);
        lv_obj_set_scrollbar_mode(s_info_scr, LV_SCROLLBAR_MODE_OFF);
    }
    if (ui_group) {
        lv_group_remove_all_objs(ui_group);
        lv_group_set_editing(ui_group, false);
    }
    lv_obj_clean(s_info_scr);

    /* 标题（顶部居中） */
    lv_obj_t *title = lv_label_create(s_info_scr);
    lv_label_set_text(title, "WIFI INFORMATION");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x81D4FA), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* SSID / IP：屏幕正中，两行 */
    lv_obj_t *l1 = lv_label_create(s_info_scr);
    snprintf(line, sizeof(line), "SSID: %s", ssid);
    lv_label_set_text(l1, line);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(l1, LV_ALIGN_CENTER, 0, -22);

    lv_obj_t *l2 = lv_label_create(s_info_scr);
    snprintf(line, sizeof(line), "IP: %s", ip);
    lv_label_set_text(l2, line);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l2, lv_color_hex(0xFFCC80), 0);
    lv_obj_align(l2, LV_ALIGN_CENTER, 0, 8);

    /* 底部两个按钮：BACK 返回主界面 / RESCAN 重新配网 */
    lv_obj_t *b_back = info_btn(s_info_scr, "BACK", LV_ALIGN_BOTTOM_LEFT, 8, -6,
                                prov_info_back_cb);
    lv_obj_t *b_scan = info_btn(s_info_scr, "RESCAN", LV_ALIGN_BOTTOM_RIGHT, -8, -6,
                                prov_info_rescan_cb);

    if (ui_group) {
        lv_group_add_obj(ui_group, b_back);
        lv_group_add_obj(ui_group, b_scan);
        lv_group_focus_obj(b_back);
    }

    lv_scr_load(s_info_scr);
}

void prov_ui_update(void)
{
    switch (s_state) {
    case PROV_SCANNING:
        if (!s_started_scan) {
            s_started_scan = true;
            wifi_mgr_scan_start();
        }
        if (wifi_mgr_scan_is_done()) {
            s_ap_count = wifi_mgr_scan_get_results(s_aps, MAX_APS);
            s_state = PROV_LIST;
            prov_list_ui();
        }
        break;
    case PROV_CONNECTING:
        if (wifi_mgr_is_connected()) {
            ESP_LOGI(TAG, "Connected to '%s'", s_selected_ssid);
            s_state = PROV_IDLE;
            main_ui_show();   /* 连上立刻回主界面 */
        } else if (wifi_mgr_connect_failed() ||
                   (uint32_t)(lv_tick_get() - s_connect_tick) > 15000) {
            prov_show_connect_failure();
        }
        break;
    case PROV_FAILED:
        /* 停在失败界面，等用户点 RETRY 或 CANCEL，不做自动跳转 */
        break;
    default:
        break;
    }
}
