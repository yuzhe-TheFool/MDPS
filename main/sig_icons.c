/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include "sig_icons.h"

const lv_image_dsc_t *sig_icon_for_rssi(int8_t rssi)
{
    if (rssi >= -50) return &sig_4;    /* 强：4 格 */
    if (rssi >= -65) return &sig_3;    /* 3 格 */
    if (rssi >= -80) return &sig_2;    /* 2 格 */
    if (rssi >= -90) return &sig_1;    /* 1 格 */
    return &sig_0;                     /* 极弱/无 */
}
