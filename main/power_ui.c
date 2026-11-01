/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "main_ui.h"
#include "power_ui.h"
#include "sw3538.h"
#include "mp4248.h"
#include "ina226.h"

/* ==================================================================
 * MP4248 可调电源：输出开关用 OPERATION 寄存器（IO38 常高供电）；
 * 设定电压/电流（NVS 存储，mp4248_task 异步下发）
 * ================================================================== */

/* 设定值直接存 MP4248 寄存器单位（仅 UI 显示时换算）：
   电压 Y = VOUT_COMMAND：分辨率 ~1.5mV/位(0x400=1024→1.5V)，默认 5.0V≈3413
   电流 cl = CURRENT_LIMIT：每 +1 = 50mA，默认 1.0A = 20
   操作：电压每次 ±7 寄存器(≈10.5mV≈0.01V 两位小数显示)；电流每次 ±1 */
static int s_set_v_y = 3413;     /* ≈5.00V */
static int s_set_i_cl = 20;      /* 1.00A */
#define V_STEP     7             /* 电压每步寄存器 +7 */
#define SET_VY_MIN 683            /* ≈1.00V */
#define SET_VY_MAX 19115          /* ≈28.00V */
#define SET_ICL_MIN 1             /* 0.05A */
#define SET_ICL_MAX 100           /* 5.00A */

/* 充电器 4 口 C1 A1 C2 A2：电压 x100、电流 x100、功率 x10。C2/A2=路2 未接入 */
static int s_cg_v[4] = {0, 0, 0, 0};
static int s_cg_i[4] = {0, 0, 0, 0};
static int s_cg_p[4] = {0, 0, 0, 0};

typedef enum { ADJ_NONE, ADJ_V, ADJ_I } adj_mode_t;
static adj_mode_t s_adj = ADJ_NONE;
static adj_mode_t s_last_adj = ADJ_V;   /* 上次选中项：无选中时加减的作用对象 */

static bool s_en_on = false;            /* MP4248 EN 状态（默认关，由底部电源图标打开） */

static lv_obj_t *s_scr = NULL;

/* power 屏对象 */
static lv_obj_t *s_slot_v = NULL;
static lv_obj_t *s_slot_i = NULL;
static lv_obj_t *s_val_v = NULL;
static lv_obj_t *s_val_i = NULL;
static lv_obj_t *s_act_v = NULL;        /* ACT 实际电压 label（INA226 预留） */
static lv_obj_t *s_act_i = NULL;        /* ACT 实际电流 label（INA226 预留） */
static lv_obj_t *s_en_btn = NULL;

/* ACT 实际值（mV/mA），-1 = 尚无数据 → 显示 "--" */
static int s_act_v_mv = -1;
static int s_act_i_ma = -1;

/* charge 屏对象 */
static lv_obj_t *s_cg_pw[4] = {NULL, NULL, NULL, NULL};   /* 功率（大字） */
static lv_obj_t *s_cg_vi[4] = {NULL, NULL, NULL, NULL};   /* 电压/电流并排（小字） */

/* 当前屏类型：0=charge，1=power，-1=其它 */
static int s_ui_kind = -1;

/* ---------- 通用 ---------- */

static void new_scr(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    /* 关闭滚动条 + 无边框/padding，避免屏底出现贯穿横线 */
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);
}

static void grp_add(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x0E4A5A), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00E5FF), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x0E4A5A), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUSED);
    lv_group_add_obj(ui_group, obj);
}

/* 三界面底部导航主题色（可整体调整色值，区分各界面）
   主屏(天气)=淡蓝 / 充电=绿 / 可调电源=琥珀黄 */
#define NAV_COLOR_MAIN   0x81D4FA
#define NAV_COLOR_CHARGE 0x81C784
#define NAV_COLOR_POWER  0xFFD54F

/* 底部导航按钮（图标式）。fg=图标颜色=本屏主题色，各界面底部一看即能区分；
   EN 使能开关不在此列（单独用标准 I/O 字母：开=I红字 / 关=O白字） */
static lv_obj_t *bottom_icon_btn(lv_obj_t *parent, const char *sym, uint32_t fg,
                                 lv_event_cb_t cb, lv_align_t align, int x, int y)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 40, 40);
    lv_obj_align(b, align, x, y);
    /* 视觉仍是 40×40，但把触摸命中区四周各外扩 10px（实际 60×60），
       手指略偏也能点上；相邻按钮间距 65px，外扩后仍留 5px 互不重叠 */
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1A2230), 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, sym);
    lv_obj_center(lbl);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    grp_add(b);
    return b;
}

/* ---------- 调节 ---------- */

static void mark_dirty(void);
static void refresh_values(void);

static void clamp_y(void) { if (s_set_v_y < SET_VY_MIN) s_set_v_y = SET_VY_MIN; if (s_set_v_y > SET_VY_MAX) s_set_v_y = SET_VY_MAX; }
static void clamp_cl(void) { if (s_set_i_cl < SET_ICL_MIN) s_set_i_cl = SET_ICL_MIN; if (s_set_i_cl > SET_ICL_MAX) s_set_i_cl = SET_ICL_MAX; }

static void refresh_values(void)
{
    if (s_val_v) {
        /* 寄存器→显示两位小数：V[0.01V] = round(Y×3000/20480) */
        int cv = (s_set_v_y * 3000 + 10240) / 20480;
        lv_label_set_text_fmt(s_val_v, "%d.%02dV", cv / 100, cv % 100);
        lv_obj_set_style_text_color(s_val_v,
            (s_adj == ADJ_V) ? lv_color_hex(0x00E676) : lv_color_white(), 0);
    }
    if (s_val_i) {
        /* 寄存器→显示：I = cl×50mA，用 0.01A 单位 */
        int c100 = s_set_i_cl * 5;
        lv_label_set_text_fmt(s_val_i, "%d.%02dA", c100 / 100, c100 % 100);
        lv_obj_set_style_text_color(s_val_i,
            (s_adj == ADJ_I) ? lv_color_hex(0x00E676) : lv_color_white(), 0);
    }
}

/* 点击电压/电流槽：切换选中（选中=值可调 + 字体变绿；再次点击取消） */
static void slot_v_cb(lv_event_t *e)
{
    (void)e;
    s_adj = (s_adj == ADJ_V) ? ADJ_NONE : ADJ_V;
    if (s_adj != ADJ_NONE) s_last_adj = s_adj;
    refresh_values();
}
static void slot_i_cb(lv_event_t *e)
{
    (void)e;
    s_adj = (s_adj == ADJ_I) ? ADJ_NONE : ADJ_I;
    if (s_adj != ADJ_NONE) s_last_adj = s_adj;
    refresh_values();
}

/* 触摸接管处理：旋钮正在选中调节时，若触摸点到「其它控件」(非当前选中槽)，
   立即退出选中态。修复：触摸把组焦点移走后，旋钮仍卡在选中态、按键取消不了的问题。
   若点的是正在选中的槽本身 → 保留，交给 CLICKED 决定是否取消(再次点击=取消)。 */
static void press_exit_adj_cb(lv_event_t *e)
{
    lv_obj_t *me = lv_event_get_current_target(e);
    if (s_adj == ADJ_NONE) return;
    if ((s_adj == ADJ_V && me == s_slot_v) ||
        (s_adj == ADJ_I && me == s_slot_i)) {
        return;
    }
    s_adj = ADJ_NONE;
    refresh_values();
}

/* 加减一步（共用按钮）：调当前选中项；无选中时调上次选中的项。
   电压 ±V_STEP(寄存器)，电流 ±1(cl) */
static void power_step(int dir)
{
    adj_mode_t m = (s_adj != ADJ_NONE) ? s_adj : s_last_adj;
    if (m == ADJ_V) {
        s_set_v_y += dir * V_STEP;
        clamp_y();
    } else {
        s_set_i_cl += dir;
        clamp_cl();
    }
    refresh_values();
    mark_dirty();
}
static void dec_cb(lv_event_t *e) { (void)e; power_step(-1); }
static void inc_cb(lv_event_t *e) { (void)e; power_step(1); }
/* 长按连续步进：触摸按住 / 编码器按键按住 均触发 LV_EVENT_LONG_PRESSED_REPEAT
   （默认按住 0.4s 后触发一次，之后每 ~100ms 一次） */
static void dec_rep_cb(lv_event_t *e) { (void)e; power_step(-1); }
static void inc_rep_cb(lv_event_t *e) { (void)e; power_step(1); }

/* ---------- NVS 存取设定值 ---------- */
static bool s_dirty = false;
static uint32_t s_dirty_tick = 0;

static void mark_dirty(void)
{
    s_dirty = true;
    s_dirty_tick = lv_tick_get();
    /* 设定值变化 → 直传寄存器值给 mp4248_task 异步下发（不在锁内做 I2C） */
    mp4248_set_target(s_set_v_y, s_set_i_cl);
}

static void power_load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("power", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = s_set_v_y, i = s_set_i_cl, en = s_en_on ? 1 : 0;
    nvs_get_i32(h, "set_v", &v);
    nvs_get_i32(h, "set_i", &i);
    nvs_get_i32(h, "en", &en);
    s_set_v_y = (int)v;
    s_set_i_cl = (int)i;
    s_en_on = (en != 0);
    clamp_y();
    clamp_cl();
    nvs_close(h);
}

static void power_save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("power", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "set_v", s_set_v_y);
        nvs_set_i32(h, "set_i", s_set_i_cl);
        nvs_set_i32(h, "en", s_en_on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 值稳定 2s 后落盘（避免频繁调值狂写 flash） */
static void power_maybe_save(void)
{
    if (s_dirty && (lv_tick_get() - s_dirty_tick > 2000)) {
        s_dirty = false;
        power_save_settings();
    }
}

/* ---------- MP4248 EN 输出使能（写 OPERATION 寄存器；标准 I/O 开关：开=I红/关=O白） ---------- */
static void en_apply(bool on)
{
    s_en_on = on;
    mp4248_set_en(on);   /* 记录目标，mp4248_task 异步写 OPERATION_REG */
    if (s_en_btn) {
        lv_obj_t *lbl = lv_obj_get_child(s_en_btn, 0);
        lv_label_set_text(lbl, on ? "I" : "O");
        /* 底色保持与导航一致的深底，仅用前景字母+颜色区分状态 */
        lv_obj_set_style_text_color(lbl,
            on ? lv_color_hex(0xFF5252) : lv_color_white(), 0);
    }
}
static void en_cb(lv_event_t *e) { (void)e; en_apply(!s_en_on); mark_dirty(); }

/* ---------- ACT 实际值显示（INA226，与 SET 一致两位小数） ---------- */
static void act_apply(void)
{
    if (s_act_v) {
        if (s_act_v_mv < 0) {
            lv_label_set_text(s_act_v, "--");
        } else {
            int cv = (s_act_v_mv + 5) / 10;   /* 舍入到 0.01V */
            lv_label_set_text_fmt(s_act_v, "%d.%02dV", cv / 100, cv % 100);
        }
    }
    if (s_act_i) {
        if (s_act_i_ma < 0) {
            lv_label_set_text(s_act_i, "--");
        } else {
            lv_label_set_text_fmt(s_act_i, "%d.%02dA", s_act_i_ma / 1000, (s_act_i_ma / 10) % 100);
        }
    }
}

static void back_main_cb(lv_event_t *e)
{
    (void)e;
    if (s_dirty) { s_dirty = false; power_save_settings(); }
    main_ui_show();
}
static void go_charge_cb(lv_event_t *e) { (void)e; power_ui_create_charge(); }
static void go_power_cb(lv_event_t *e) { (void)e; power_ui_create_power(); }

/* ---------- Charge 屏：C1 A1 C2 A2 四口（功率大字 + 电压/电流小字） ---------- */

static void charge_build(void)
{
    new_scr();
    s_adj = ADJ_NONE;
    s_ui_kind = 0;   /* charge 屏 */

    static const char *names[4] = {"C1", "A1", "C2", "A2"};
    static const int cx[4] = {50, 120, 200, 270};   /* 两端各内收 10px，避免贴边被裁 */

    for (int i = 0; i < 4; i++) {
        int xo = cx[i] - 160;

        /* 口标识（10 号，统一蓝色） */
        lv_obj_t *nm = lv_label_create(s_scr);
        lv_label_set_text(nm, names[i]);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0x4FC3F7), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_MID, xo, 36);

        /* 功率（大字；用 20 号：99.9W≈61px，小于列距 70，避免相邻打架） */
        s_cg_pw[i] = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_cg_pw[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_cg_pw[i], lv_color_white(), 0);
        lv_label_set_text(s_cg_pw[i], "--");
        lv_obj_align(s_cg_pw[i], LV_ALIGN_TOP_MID, xo, 50);

        /* 电压/电流并排（同一行小字，紧贴功率下方） */
        s_cg_vi[i] = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_cg_vi[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_cg_vi[i], lv_color_hex(0xFFCC80), 0);
        lv_label_set_text(s_cg_vi[i], "--");
        lv_obj_align(s_cg_vi[i], LV_ALIGN_TOP_MID, xo, 78);
    }

    lv_group_remove_all_objs(ui_group);
    lv_group_set_editing(ui_group, false);
    /* charge 屏只有左右两个角落按钮，间距充裕，命中区再放宽到 14px */
    lv_obj_t *cb_home = bottom_icon_btn(s_scr, LV_SYMBOL_HOME, NAV_COLOR_CHARGE, back_main_cb, LV_ALIGN_BOTTOM_LEFT, 8, -6);
    lv_obj_t *cb_pw = bottom_icon_btn(s_scr, LV_SYMBOL_POWER, NAV_COLOR_CHARGE, go_power_cb, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_ext_click_area(cb_home, 14);
    lv_obj_set_ext_click_area(cb_pw, 14);
    if (lv_group_get_obj_count(ui_group) > 0) {
        lv_group_focus_obj(lv_group_get_obj_by_index(ui_group, 0));
    }
    lv_scr_load(s_scr);
}

/* ---------- Power 屏：SET(左,可调) / ACT(右,INA226实际值) 两栏 + 底部加减/EN/导航 ---------- */

static void power_build(void)
{
    new_scr();
    s_adj = ADJ_NONE;
    s_ui_kind = 1;   /* power 屏 */

    lv_group_remove_all_objs(ui_group);
    lv_group_set_editing(ui_group, false);

    /* 栏标：SET(蓝) / ACT(橙) */
    lv_obj_t *set_t = lv_label_create(s_scr);
    lv_label_set_text(set_t, "SET");
    lv_obj_set_style_text_font(set_t, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(set_t, lv_color_hex(0x4FC3F7), 0);
    lv_obj_align(set_t, LV_ALIGN_TOP_MID, -77, 4);

    lv_obj_t *act_t = lv_label_create(s_scr);
    lv_label_set_text(act_t, "ACT");
    lv_obj_set_style_text_font(act_t, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(act_t, lv_color_hex(0xFFCC80), 0);
    lv_obj_align(act_t, LV_ALIGN_TOP_MID, 77, 4);

    /* ---------- SET 栏（电压/电流槽：可点击选中，选中调值+字变绿） ---------- */
    s_slot_v = lv_button_create(s_scr);
    lv_obj_set_size(s_slot_v, 146, 44);
    lv_obj_align(s_slot_v, LV_ALIGN_TOP_LEFT, 10, 18);
    lv_obj_set_style_bg_color(s_slot_v, lv_color_hex(0x0E1A26), 0);
    lv_obj_set_style_border_width(s_slot_v, 1, 0);
    lv_obj_set_style_border_color(s_slot_v, lv_color_hex(0x3A4A5A), 0);
    lv_obj_set_style_radius(s_slot_v, 6, 0);
    lv_obj_set_style_pad_all(s_slot_v, 0, 0);
    s_val_v = lv_label_create(s_slot_v);
    lv_obj_center(s_val_v);
    lv_obj_set_style_text_font(s_val_v, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_val_v, lv_color_white(), 0);
    lv_obj_add_event_cb(s_slot_v, slot_v_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_slot_v, press_exit_adj_cb, LV_EVENT_PRESSED, NULL);
    grp_add(s_slot_v);

    s_slot_i = lv_button_create(s_scr);
    lv_obj_set_size(s_slot_i, 146, 44);
    lv_obj_align(s_slot_i, LV_ALIGN_TOP_LEFT, 10, 66);
    lv_obj_set_style_bg_color(s_slot_i, lv_color_hex(0x0E1A26), 0);
    lv_obj_set_style_border_width(s_slot_i, 1, 0);
    lv_obj_set_style_border_color(s_slot_i, lv_color_hex(0x3A4A5A), 0);
    lv_obj_set_style_radius(s_slot_i, 6, 0);
    lv_obj_set_style_pad_all(s_slot_i, 0, 0);
    s_val_i = lv_label_create(s_slot_i);
    lv_obj_center(s_val_i);
    lv_obj_set_style_text_font(s_val_i, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_val_i, lv_color_white(), 0);
    lv_obj_add_event_cb(s_slot_i, slot_i_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_slot_i, press_exit_adj_cb, LV_EVENT_PRESSED, NULL);
    grp_add(s_slot_i);

    /* ---------- ACT 栏（实际值显示框，不可点；INA226 数据接入后由 power_ui_set_act 刷新） ---------- */
    lv_obj_t *avb = lv_obj_create(s_scr);
    lv_obj_set_size(avb, 146, 44);
    lv_obj_align(avb, LV_ALIGN_TOP_LEFT, 164, 18);
    lv_obj_remove_flag(avb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(avb, lv_color_hex(0x0E1A26), 0);
    lv_obj_set_style_border_width(avb, 1, 0);
    lv_obj_set_style_border_color(avb, lv_color_hex(0x3A4A5A), 0);
    lv_obj_set_style_radius(avb, 6, 0);
    lv_obj_set_style_pad_all(avb, 0, 0);
    s_act_v = lv_label_create(avb);
    lv_obj_center(s_act_v);
    lv_obj_set_style_text_font(s_act_v, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_act_v, lv_color_hex(0xFFCC80), 0);

    lv_obj_t *aib = lv_obj_create(s_scr);
    lv_obj_set_size(aib, 146, 44);
    lv_obj_align(aib, LV_ALIGN_TOP_LEFT, 164, 66);
    lv_obj_remove_flag(aib, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(aib, lv_color_hex(0x0E1A26), 0);
    lv_obj_set_style_border_width(aib, 1, 0);
    lv_obj_set_style_border_color(aib, lv_color_hex(0x3A4A5A), 0);
    lv_obj_set_style_radius(aib, 6, 0);
    lv_obj_set_style_pad_all(aib, 0, 0);
    s_act_i = lv_label_create(aib);
    lv_obj_center(s_act_i);
    lv_obj_set_style_text_font(s_act_i, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_act_i, lv_color_hex(0xFFCC80), 0);

    /* ---------- 底部菜单：HOME | CHARGE | [-] | [+] | EN ---------- */
    lv_obj_t *homeb = bottom_icon_btn(s_scr, LV_SYMBOL_HOME, NAV_COLOR_POWER, back_main_cb, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    lv_obj_add_event_cb(homeb, press_exit_adj_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *chargeb = bottom_icon_btn(s_scr, LV_SYMBOL_CHARGE, NAV_COLOR_POWER, go_charge_cb, LV_ALIGN_BOTTOM_LEFT, 75, -6);
    lv_obj_add_event_cb(chargeb, press_exit_adj_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *decb = bottom_icon_btn(s_scr, "-", NAV_COLOR_POWER, dec_cb, LV_ALIGN_BOTTOM_LEFT, 140, -6);
    lv_obj_add_event_cb(decb, dec_rep_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(decb, press_exit_adj_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *incb = bottom_icon_btn(s_scr, "+", NAV_COLOR_POWER, inc_cb, LV_ALIGN_BOTTOM_LEFT, 205, -6);
    lv_obj_add_event_cb(incb, inc_rep_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(incb, press_exit_adj_cb, LV_EVENT_PRESSED, NULL);
    /* EN 输出使能：标准 I/O 开关，先以 O(关/白) 创建，build 末尾 en_apply 统一刷状态 */
    s_en_btn = bottom_icon_btn(s_scr, "O", 0xFFFFFF, en_cb, LV_ALIGN_BOTTOM_LEFT, 270, -6);
    lv_obj_add_event_cb(s_en_btn, press_exit_adj_cb, LV_EVENT_PRESSED, NULL);

    refresh_values();
    act_apply();          /* 无 INA226 数据 → ACT 栏显示 -- */
    en_apply(s_en_on);    /* EN 默认关：同步图标底色 */

    if (lv_group_get_obj_count(ui_group) > 0) {
        lv_group_focus_obj(lv_group_get_obj_by_index(ui_group, 0));
    }
    lv_scr_load(s_scr);
}

/* ---------- 对外接口 ---------- */

void power_ui_create_charge(void)
{
    charge_build();
}

/* 设定值是否已从 NVS 载入（首次进入 power 屏或首次被 Web 读写时触发） */
static bool s_settings_loaded = false;

static void power_ensure_loaded(void)
{
    if (s_settings_loaded) {
        return;
    }
    s_settings_loaded = true;
    power_load_settings();
    /* 首次：把（NVS 恢复的）设定值也作为 MP4248 目标下发一次 */
    mp4248_set_target(s_set_v_y, s_set_i_cl);
}

void power_ui_create_power(void)
{
    power_ensure_loaded();
    power_build();
}

/* 更新 charge 某一口（idx：0=C1 1=A1 2=C2 3=A2）。online=false → 该口全显示 --。
   v_mv 输出电压(mV)、i_ma 该口电流(mA)；功率=v*i，都一位小数显示 */
static void cg_update_col(int idx, bool online, int v_mv, int i_ma)
{
    if (!online) {
        if (s_cg_v[idx] != -1) {
            s_cg_v[idx] = -1;
            s_cg_i[idx] = -1;
            s_cg_p[idx] = -1;
            if (s_cg_pw[idx]) lv_label_set_text(s_cg_pw[idx], "--");
            if (s_cg_vi[idx]) lv_label_set_text(s_cg_vi[idx], "--");
        }
        return;
    }
    int v100 = v_mv / 10;                              /* 0.01V */
    int i100 = i_ma / 10;                              /* 0.01A */
    int p10  = (int)((int32_t)v_mv * i_ma / 100000);   /* 0.1W */

    if (s_cg_p[idx] != p10) {
        s_cg_p[idx] = p10;
        if (s_cg_pw[idx]) lv_label_set_text_fmt(s_cg_pw[idx], "%d.%dW", p10 / 10, p10 % 10);
    }
    if (s_cg_v[idx] != v100 || s_cg_i[idx] != i100) {
        s_cg_v[idx] = v100;
        s_cg_i[idx] = i100;
        if (s_cg_vi[idx]) {
            lv_label_set_text_fmt(s_cg_vi[idx], "%d.%dV %d.%dA",
                                  v100 / 100, (v100 / 10) % 10,
                                  i100 / 100, (i100 / 10) % 10);
        }
    }
}

void power_ui_update(void)
{
    /* 设定值落盘：与当前显示哪个屏无关（Web 改了值也要能存下来） */
    power_maybe_save();

    if (!s_scr || lv_screen_active() != s_scr) {
        return;
    }
    if (s_ui_kind == 0) {
        /* charge 屏：C1/A1=路1(I2C1)，C2/A2=路2(I2C0)，都是 SW3538 */
        sw3538_data_t d0, d1;
        if (sw3538_get_last(0, &d0)) {
            /* C1 = Path1(C口)，A1 = Path2(A口) */
            cg_update_col(0, d0.path1Online, d0.outputVoltagemV, d0.currentPath1mA);
            cg_update_col(1, d0.path2Online, d0.outputVoltagemV, d0.currentPath2mA);
        } else {
            cg_update_col(0, false, 0, 0);
            cg_update_col(1, false, 0, 0);
        }
        if (sw3538_get_last(1, &d1)) {
            cg_update_col(2, d1.path1Online, d1.outputVoltagemV, d1.currentPath1mA);
            cg_update_col(3, d1.path2Online, d1.outputVoltagemV, d1.currentPath2mA);
        } else {
            cg_update_col(2, false, 0, 0);
            cg_update_col(3, false, 0, 0);
        }
    } else if (s_ui_kind == 1) {
        /* power 屏：ACT 栏刷新 INA226 实测值 */
        int av = 0, ai = 0;
        if (ina226_get_last(&av, &ai)) {
            power_ui_set_act(av, ai);
        } else {
            power_ui_set_act(-1, -1);   /* INA226 无数据 → ACT 显示 -- */
        }
    }
}

bool power_ui_is_adjusting(void)
{
    /* 仅在 power 屏前台时，选中电压/电流才拦截旋钮调值 */
    return s_ui_kind == 1 && s_scr && lv_screen_active() == s_scr && s_adj != ADJ_NONE;
}

void power_ui_encoder_adjust(int diff)
{
    if (diff == 0) return;
    if (s_adj == ADJ_V) {
        s_set_v_y += diff * V_STEP;
        clamp_y();
    } else if (s_adj == ADJ_I) {
        s_set_i_cl += diff;
        clamp_cl();
    } else {
        return;
    }
    refresh_values();
    mark_dirty();
}

/* 供外部(INA226)更新 ACT 实际电压/电流：v_mv<0 或 i_ma<0 → 该栏显示 "--" */
void power_ui_set_act(int v_mv, int i_ma)
{
    if (s_act_v_mv == v_mv && s_act_i_ma == i_ma) return;
    s_act_v_mv = v_mv;
    s_act_i_ma = i_ma;
    if (s_scr && lv_screen_active() == s_scr && s_ui_kind == 1) act_apply();
}

/* ---------- 供 Web/外部读写设定值（物理单位） ---------- */

void power_ui_get_output(int *v_mv, int *i_ma, bool *en)
{
    power_ensure_loaded();
    if (v_mv) {
        /* 内部存 VOUT_COMMAND 寄存器 Y：Vout[V] = Y×1.5/1024 → mV = Y×1500/1024 */
        *v_mv = (int)((int64_t)s_set_v_y * 1500 / 1024);
    }
    if (i_ma) {
        *i_ma = s_set_i_cl * 50;    /* cl 每 +1 = 50mA */
    }
    if (en) {
        *en = s_en_on;
    }
}

void power_ui_set_output(int v_mv, int i_ma, bool en)
{
    power_ensure_loaded();

    if (v_mv > 0) {
        s_set_v_y = (int)(((int64_t)v_mv * 1024 + 750) / 1500);   /* mV → Y（四舍五入） */
        clamp_y();
    }
    if (i_ma > 0) {
        s_set_i_cl = (i_ma + 25) / 50;                            /* mA → cl（四舍五入） */
        clamp_cl();
    }
    if (s_ui_kind == 1 && s_scr) {
        refresh_values();   /* 正在 power 屏 → 同步显示 */
    }
    en_apply(en);           /* 同步 EN 按钮显示 + 记录目标（mp4248_task 异步下发） */
    mark_dirty();           /* 下发 MP4248 + 2s 后写入 NVS */
}
