/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The Fool
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_touch_cst816s.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "touch_io.h"
#include "i2c_health.h"
#include "lvgl.h"
#include "wifi_mgr.h"
#include "encoder.h"
#include "net_time.h"
#include "weather.h"
#include "main_ui.h"
#include "prov_ui.h"
#include "power_ui.h"
#include "web_server.h"
#include "sw3538.h"
#include "mp4248.h"
#include "ina226.h"
#include "lm75a.h"
#include "fan.h"

static const char *TAG = "lvgl_app";

/* ============ 硬件引脚配置（按你的实际接线） ============ */
// SPI - 屏幕
#define LCD_SPI_HOST     SPI2_HOST
#define PIN_LCD_SCLK     GPIO_NUM_6
#define PIN_LCD_MOSI     GPIO_NUM_15
#define PIN_LCD_CS       GPIO_NUM_7
#define PIN_LCD_DC       GPIO_NUM_16
#define PIN_LCD_RST      (-1)          // RST 复用 EN 引脚，不额外控制
#define PIN_LCD_BL       GPIO_NUM_17

// I2C - 触摸 CST816D
#define PIN_TOUCH_SDA    GPIO_NUM_39
#define PIN_TOUCH_SCL    GPIO_NUM_40
#define PIN_TOUCH_RST    GPIO_NUM_4
#define PIN_TOUCH_INT    GPIO_NUM_5

/* 触摸 I2C 单次事务超时（毫秒）。
   必须大于“总线被其他任务占用”的最坏时间（I2C0 上还有 INA226 / SW3538，
   读一次可能几毫秒），否则会误报失败；同时又要足够小，保证总线被拉死时
   界面只是卡一下而不是卡死。100ms 是两者的平衡点。 */
#define TOUCH_I2C_TIMEOUT_MS  100

// MP4248 可调电源 EN（高电平使能）
#define PIN_MP4248_EN    GPIO_NUM_38

/* ============ 屏幕参数 ============ */
// ST7789 面板物理分辨率（驱动 IC 原生）
#define LCD_PANEL_H_RES  240
#define LCD_PANEL_V_RES  320
// 实际显示区域：172x320 的可见窗口（x 方向偏移居中）
#define LCD_X_OFFSET     34     // (240 - 172) / 2
#define LCD_Y_OFFSET     0
#define LCD_WIDTH        172
#define LCD_HEIGHT       320
// LVGL 横屏分辨率（旋转 90°）
#define LVGL_H_RES       320
#define LVGL_V_RES       172
// 帧缓冲行数（值越小越省内存，但刷新次数增多）
#define LVGL_DRAW_BUF_ROWS 40

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t panel_io_handle = NULL;
static esp_lcd_touch_handle_t tp_handle = NULL;
static i2c_master_bus_handle_t s_i2c0_bus = NULL;   /* I2C0(39/40)：触摸 CST816S + INA226 共用 */

/* 背光控制（此模组背光低电平有效） */
static void lcd_set_backlight(bool on)
{
    gpio_set_level(PIN_LCD_BL, on ? 0 : 1);
}

/* ============ 显示初始化 ============ */
static void display_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus for LCD");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LVGL_H_RES * LVGL_DRAW_BUF_ROWS * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,   // SPI 时钟降到 20MHz，避免花屏/马赛克
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &panel_io_handle));

    ESP_LOGI(TAG, "Create ST7789 panel");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,   // 屏幕 RGB 元素顺序（配合下方 INVON 反色补偿）
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    /* 此屏玻璃/驱动默认反相，用 INVON 反色补偿使颜色正常（已验证） */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_LOGI(TAG, "Panel INVON sent (invert color on)");

    // 设置可见窗口偏移，使 172x320 区域正确显示
    // 注意：面板做了 swap_xy 旋转，gap 的 X/Y 轴与驱动相反，需对调
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, LCD_Y_OFFSET, LCD_X_OFFSET));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

/* ============ 触摸初始化 ============ */
static esp_lcd_touch_handle_t touch_init(void)
{
    ESP_LOGI(TAG, "Initialize I2C bus for touch");
    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_config, &bus_handle));
    s_i2c0_bus = bus_handle;   /* 供 INA226 挂同一条总线 */

    ESP_LOGI(TAG, "Create CST816D touch panel IO");
    /* 注意：不用 IDF 的 esp_lcd_new_panel_io_i2c()。
       它把 I2C 事务超时硬编码成 -1（无限等待），电源板断电把总线拉死时
       触摸读会永久阻塞，把 LVGL 任务（连同屏幕/旋钮）一起卡死。
       这里用同样功能但带有限超时的实现。 */
    esp_lcd_panel_io_handle_t tp_io = touch_io_i2c_create(bus_handle,
                                                         ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
                                                         100000, TOUCH_I2C_TIMEOUT_MS);
    if (tp_io == NULL) {
        ESP_LOGE(TAG, "Touch panel IO init failed");
        return NULL;
    }

    ESP_LOGI(TAG, "Create CST816D touch panel");
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_WIDTH - 1,
        .y_max = LCD_HEIGHT - 1,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
    };
    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t err = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Touch panel init failed: %s", esp_err_to_name(err));
        return NULL;
    }
    return tp;
}

/* ============ 自定义 LVGL 触摸输入 ============ */
static void touchpad_read_cb(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    static lv_point_t last = {0, 0};
    static int fail_cnt = 0;
    esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t cnt = 0;

    /* 读取触摸原始坐标（芯片直接输出，无任何转换）
       读失败（如电源板断电把 I2C 拉死）时直接当成“无触摸”，
       不再把 LVGL 任务堵在无限等待的 I2C 事务里。 */
    if (esp_lcd_touch_read_data(tp_handle) != ESP_OK) {
        if (++fail_cnt == 3) {
            /* 持续失败时屏蔽触摸驱动的错误日志，否则每 30ms 刷一条 */
            ESP_LOGW(TAG, "touch read failing (I2C bus stuck?), mute driver log");
            esp_log_level_set("CST816S", ESP_LOG_NONE);
        }
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = last;
        return;
    }
    if (fail_cnt > 0) {
        if (fail_cnt >= 3) {
            esp_log_level_set("CST816S", ESP_LOG_ERROR);
            ESP_LOGW(TAG, "touch recovered");
        }
        fail_cnt = 0;
    }

    esp_lcd_touch_get_data(tp_handle, points, &cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);

    if (cnt > 0) {
        /* 实测：芯片 y=0~173(屏幕左右/横)、x=0~320(屏幕上下/竖)
           屏幕 320x172，需缩放：屏幕x = y×320/174，屏幕y = x×172/320 */
        last.x = (int32_t)points[0].y * 320 / 174;
        last.y = (int32_t)points[0].x * 172 / 320;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point = last;
}

/* ============ 编码器（EC11）→ LVGL 输入 ============ */
static void encoder_read_cb(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    static uint32_t last_press_tick = 0;

    /* 一次读取累计旋转格数，一次处理多格，快速旋转不会卡死 */
    int32_t diff = encoder_get_rotation();
    if (diff > 32) {
        diff = 32;
    } else if (diff < -32) {
        diff = -32;
    }

    bool phys = encoder_key_pressed();

    /* 按压缩放：LVGL 每 ~30ms 才读一次输入，快速短按可能被漏掉。
       物理按下后即使很快松开，也至少保持 PRESSED 60ms，确保 LVGL 捕获按下/松开边沿。 */
    uint32_t now = lv_tick_get();
    if (phys) {
        last_press_tick = now;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if ((uint32_t)(now - last_press_tick) < 60) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

    /* 可调电源屏调节模式：旋转直接调电压/电流，不参与 LVGL 组导航 */
    if (power_ui_is_adjusting()) {
        data->enc_diff = 0;
        power_ui_encoder_adjust((int)diff);
        return;
    }
    data->enc_diff = (int16_t)diff;

    /* 主界面手动导航：无选中→WiFi→Charge→Power→无选中 循环。
       旋转移动一格（含"不选中"状态），按键触发当前选中项。 */
    static bool s_prev_active = false;
    static int s_nav = 0;
    bool active = main_ui_is_active();
    if (active && !s_prev_active) {
        s_nav = 0;                        // 进入主界面 → 无选中状态
        main_ui_set_nav(0);
    }
    s_prev_active = active;

    bool rotating = (diff != 0);
    if (active) {
        if (rotating) {
            int dir = diff > 0 ? 1 : -1;  // 每帧只移动一步，避免快速旋转跳过
            data->enc_diff = 0;           // 手动导航，不交给 LVGL
            s_nav += dir;
            if (s_nav < 0) s_nav = 3;
            if (s_nav > 3) s_nav = 0;
            main_ui_set_nav(s_nav);
        }
    }
}

/* ============ SW3538 独立采集任务（两路） ============ */
static void sw3538_task(void *arg)
{
    (void)arg;
    /* 独立任务里周期读两路并缓存，即使 I2C 卡住也不影响主循环/屏幕 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        sw3538_data_t d;
        sw3538_read(0, &d);   /* 路1 (I2C1) */
        sw3538_read(1, &d);   /* 路2 (I2C0) */
    }
}

void app_main(void)
{
    /* 精简日志：全局 WARN 及以上（防运行时刷屏）；保留外设探测 INFO */
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("sw3538", ESP_LOG_INFO);
    esp_log_level_set("mp4248", ESP_LOG_INFO);
    esp_log_level_set("ina226", ESP_LOG_INFO);
    esp_log_level_set("lm75a", ESP_LOG_INFO);
    esp_log_level_set("fan", ESP_LOG_INFO);

    // 背光引脚初始化
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);

    // MP4248 EN（IO38）：常高给芯片供电/使能；输出开关用 OPERATION 寄存器（power 屏 I/O）
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << PIN_MP4248_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&en_cfg);
    gpio_set_level(PIN_MP4248_EN, 1);

    display_init();
    lcd_set_backlight(true);

    tp_handle = touch_init();

    /* I2C1(SDA41/SCL42)：SW3538 与 MP4248 共用一条总线（由这里统一创建） */
    i2c_master_bus_config_t i2c1_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_41,
        .scl_io_num = GPIO_NUM_42,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c1_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c1_cfg, &i2c1_bus));

    /* 初始化 I2C 外设（探测失败只打日志，不阻塞） */
    sw3538_init(0, i2c1_bus, I2C_BUS_ID_1);    /* 路1 充电器 SW3538 @0x3C (I2C1, 41/42) */
    sw3538_init(1, s_i2c0_bus, I2C_BUS_ID_0);  /* 路2 充电器 SW3538 @0x3C (I2C0, 39/40，与触摸/INA226 同总线) */
    mp4248_init(i2c1_bus);         /* 可调电源 MP4248 @0x67 (I2C1) */
    ina226_init(s_i2c0_bus);       /* 输出采样 INA226 @0x40 (I2C0) */
    lm75a_init(i2c1_bus);          /* 温度 LM75A @0x48 (I2C1) */
    fan_init();                    /* PWM 风扇：IO9(PWM) + IO10(FG) */

    /* 独立任务：I2C 读/写都在任务里做，不阻塞主循环（I2C 异常不影响屏幕） */
    xTaskCreate(sw3538_task, "sw3538", 4096, NULL, 5, NULL);
    xTaskCreate(mp4248_task, "mp4248", 4096, NULL, 5, NULL);
    xTaskCreate(ina226_task, "ina226", 4096, NULL, 5, NULL);
    xTaskCreate(lm75a_task, "lm75a", 4096, NULL, 5, NULL);
    xTaskCreate(fan_task, "fan", 4096, NULL, 5, NULL);

    // 初始化 WiFi（配置存 NVS，开机自动重连）
    ESP_LOGI(TAG, "Initialize WiFi");
    wifi_mgr_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    // 显示配置：172x320 竖屏面板，旋转为 320x172 横屏
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LVGL_H_RES * LVGL_DRAW_BUF_ROWS,
        .double_buffer = false,
        .hres = LVGL_H_RES,
        .vres = LVGL_V_RES,
        .rotation = {
            .swap_xy = true,      // 横屏：交换 X/Y（90° 旋转）
            .mirror_x = true,     // swap 后 MX 控制垂直方向
            .mirror_y = false,    // swap 后 MY 控制水平方向，false 修正 x 镜像
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,   // RGB565 字节序与模组一致，消除杂色
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    // 自定义 LVGL 触摸输入设备（在回调里做坐标转换）
    lv_indev_t *touch_indev = NULL;
    if (tp_handle) {
        touch_indev = lv_indev_create();
        lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch_indev, touchpad_read_cb);
        lv_indev_set_disp(touch_indev, disp);
        /* 长按：0.3s 判定后进入连续，每 60ms 触发一次（加减按钮长按连调） */
        lv_indev_set_long_press_time(touch_indev, 300);
        lv_indev_set_long_press_repeat_time(touch_indev, 60);
    }

    ESP_LOGI(TAG, "LVGL display: %dx%d, touch ready", LVGL_H_RES, LVGL_V_RES);

    /* 编码器输入（EC11）：初始化 + 注册为 LVGL encoder 输入设备 */
    encoder_init();
    lv_indev_t *enc_indev = lv_indev_create();
    lv_indev_set_type(enc_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(enc_indev, encoder_read_cb);
    lv_indev_set_disp(enc_indev, disp);

    /* 共享输入组：编码器焦点组，主界面/配网界面共用 */
    ui_group = lv_group_create();
    lv_indev_set_group(enc_indev, ui_group);
    /* 编码器按键长按：同样 0.3s 判定后每 60ms 连续触发 */
    lv_indev_set_long_press_time(enc_indev, 300);
    lv_indev_set_long_press_repeat_time(enc_indev, 60);

    /* ===== 开机显示主界面（时间/天气占位 + WiFi 标识） ===== */
    lvgl_port_lock(0);
    main_ui_init(ui_group);
    lvgl_port_unlock();

    /* LVGL 主循环：驱动 LVGL 并刷新各界面状态 */
    bool net_started = false;
    bool prev_connected = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));

        /* 联网边沿检测：首次连上初始化各网络服务，之后重连则重新对时 */
        bool connected = wifi_mgr_is_connected();
        if (connected && !prev_connected) {
            if (!net_started) {
                net_started = true;
                ESP_LOGI(TAG, "WiFi connected, start NTP + weather + web");
                main_ui_net_connected();
                net_time_init();
                weather_start();
                web_server_start();   /* 内置网页：浏览器访问 http://<设备IP>/ */
            } else {
                /* 重连：重新对时，并重新确认外网可达（否则 WiFi 图标一直带叹号） */
                ESP_LOGI(TAG, "WiFi reconnected, resync NTP");
                main_ui_net_connected();
                net_time_resync();
            }
        }
        prev_connected = connected;

        lvgl_port_lock(0);
        lv_timer_handler();
        prov_ui_update();
        main_ui_update();
        power_ui_update();
        net_time_update();
        lvgl_port_unlock();
    }
}
