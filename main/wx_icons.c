/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include "wx_icons.h"

const lv_image_dsc_t *wx_icon_get(int code, bool is_day)
{
    switch (code) {
    case 0:                 return is_day ? &wx_100 : &wx_150;   /* 晴 */
    case 1:                 return is_day ? &wx_103 : &wx_153;   /* 少云（白天：云+半个太阳） */
    case 2:                 return is_day ? &wx_104 : &wx_154;   /* 多云（云+半个太阳 / 夜：云+月亮） */
    case 3:                 return &wx_101;                       /* 阴（纯云，不分昼夜） */
    case 45:
    case 48:                return &wx_501;                       /* 雾 */
    case 51: case 52: case 53:
    case 54: case 55: case 56:
    case 57:                return &wx_309;                       /* 毛毛雨 */
    case 61:                return &wx_305;                       /* 小雨 */
    case 63:                return &wx_306;                       /* 中雨 */
    case 65:                return &wx_307;                       /* 大雨 */
    case 66:
    case 67:                return &wx_313;                       /* 冻雨 */
    case 71:                return &wx_400;                       /* 小雪 */
    case 73:                return &wx_401;                       /* 中雪 */
    case 75:                return &wx_402;                       /* 大雪 */
    case 77:                return &wx_499;                       /* 雪粒 */
    case 80:                return is_day ? &wx_300 : &wx_350;   /* 阵雨 */
    case 81:
    case 82:                return is_day ? &wx_301 : &wx_351;   /* 强阵雨 */
    case 85:
    case 86:                return &wx_407;                       /* 阵雪 */
    case 95:                return &wx_302;                       /* 雷阵雨 */
    case 96:
    case 99:                return &wx_304;                       /* 雷阵雨伴冰雹 */
    default:                return is_day ? &wx_100 : &wx_150;
    }
}
