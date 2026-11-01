# MDPS —— ESP32-S3 多路电源监控与可调电源面板

Copyright (C) 2026 The Fool

基于 ESP32-S3 + LVGL 9 的桌面电源控制终端：一块 320×172 横屏，触摸 + 编码器双操作，管理两路 SW3538 充电器（4 个口）、一路 MP4248 可调电源、INA226 输出采样、LM75A 测温与 PWM 风扇，并内置 Web 页面远程读写。

## 功能

- **三个界面**（底部导航切换）
  - 主页：时间 / 日期 / 天气图标与温度、WiFi 状态（右上角，点击进入配网）
  - 充电：C1 / A1 / C2 / A2 四口实时功率与电压电流
  - 可调电源：SET 设定值（编码器或 +/- 调节）、ACT 实测值、`I/O` 输出使能
- **双输入**：CST816D 电容触摸 + EC11 编码器（旋转调值、按下确认、长按连调）
- **联网**：WiFi 配网（结果存 NVS，开机自动重连）、NTP 对时、open-meteo 天气
- **内置 Web 服务**：浏览器访问 `http://<设备IP>/` 查看/控制全部通道
- **温控风扇**：LM75A 测温，PWM 调速 + FG 转速反馈，支持手动占空比

## 硬件要求

| 项目 | 规格 |
|---|---|
| MCU | ESP32-S3（本项目验证于 16MB Flash 模组） |
| 屏幕 | ST7789，240×320 面板，可见区 172×320，SPI 接口 |
| 触摸 | CST816D / CST816S，I2C |
| 编码器 | EC11（A/B/按键） |
| 电源外设 | SW3538 ×2（0x3C）、MP4248（0x67）、INA226（0x40）、LM75A（0x48） |
| 风扇 | 4 线 PWM 风扇（PWM + FG） |

> Flash 必须 ≥ 16MB：分区表按 16MB 规划（app 4MB + 12MB storage）。换板子请同步修改 `partitions.csv` 与 `sdkconfig` 中的 `CONFIG_ESPTOOLPY_FLASHSIZE_*`。

### 引脚定义

全部集中在 `main/main.c`、`main/encoder.c`、`main/fan.c` 顶部宏里，按实际接线修改即可。

| 用途 | 信号 | GPIO |
|---|---|---|
| LCD (SPI2) | SCLK / MOSI / CS / DC / BL | 6 / 15 / 7 / 16 / 17（背光低电平点亮） |
| LCD | RST | 未接（复用模组 EN） |
| 触摸 (I2C0) | SDA / SCL / RST / INT | 39 / 40 / 4 / 5 |
| 外设 (I2C1) | SDA / SCL | 41 / 42 |
| 编码器 | A / B / KEY | 13 / 14 / 12 |
| 风扇 | PWM / FG | 9 / 10 |
| MP4248 | EN（常高使能） | 38 |

I2C 地址分配：`I2C0(39/40)` = 触摸 + INA226(0x40) + SW3538#1(0x3C)；`I2C1(41/42)` = SW3538#0(0x3C) + MP4248(0x67) + LM75A(0x48)。

## 软件环境

| 组件 | 版本 | 说明 |
|---|---|---|
| ESP-IDF | **v6.0.2** | `main/CMakeLists.txt` 依赖 v6 拆分出的 `esp_driver_*` 组件，v5.x 无法直接编译 |
| lvgl/lvgl | 9.4.0 | 由组件管理器自动下载 |
| espressif/esp_lvgl_port | 2.9.0 | 显示/触摸/LVGL 任务封装 |
| espressif/esp_lcd_touch_cst816s | 1.1.2 | 触摸驱动 |

版本已在 `dependencies.lock` 中锁定，构建时按锁文件解析。

## 目录结构

```
.
├── CMakeLists.txt
├── CMakePresets.json        # default / production 两套预设
├── partitions.csv           # 16MB flash 自定义分区表
├── sdkconfig                # 完整工程配置（务必随仓库提交）
├── sdkconfig.defaults       # 关键配置增量（target/频率/字体/触摸等）
├── dependencies.lock        # 组件版本锁
├── release/                 # 预编译固件（可选，随 Release 附件分发，不入库）
└── main/
    ├── main.c               # 显示/触摸/I2C 初始化，LVGL 主循环
    ├── main_ui.c            # 主页：时间/天气/WiFi
    ├── power_ui.c           # 充电页 + 可调电源页（含 EN 开关）
    ├── prov_ui.c            # WiFi 配网界面
    ├── web_server.c         # 内置 HTTP 服务与单页 HTML
    ├── sw3538.c / mp4248.c / ina226.c / lm75a.c / fan.c   # 外设驱动
    ├── wifi_mgr.c / net_time.c / weather.c                # 联网
    ├── encoder.c / touch_io.c / i2c_health.c              # 输入与总线健康
    └── wx_icons/            # 天气图标（LVGL 图像描述符）
```

## 编译

### 环境要求

| 项 | 要求 |
|---|---|
| ESP-IDF | **v6.0.2**（必须 v6.x） |
| Python | 3.9+（随 ESP-IDF 安装，无需单独配置） |
| 网络 | 首次构建需联网下载组件（约 160MB） |
| 磁盘 | 构建目录约 400MB |

### 命令行编译

```bash
# 1. 进入 ESP-IDF 环境（关键：让 cmake/ninja/工具链进入 PATH）
#    Windows：开始菜单打开 "ESP-IDF PowerShell" / "ESP-IDF CMD"，
#    或在 cmd 中执行 export.bat（位于你的 esp-idf 安装目录根下）
idf.py --version          # 应输出 v6.0.2

# 2. 获取源码
git clone <repo-url> MDPS
cd MDPS

# 3. 构建（直接 build，不要 set-target）
idf.py build
```

> **不要执行 `idf.py set-target esp32s3`**。该命令会触发 fullclean，并把仓库里的 `sdkconfig` 重命名为 `sdkconfig.old`，配置回退到精简的 `sdkconfig.defaults`，导致 LVGL 内存、刷新周期等设置丢失。`sdkconfig` 中已固定 `CONFIG_IDF_TARGET="esp32s3"`，直接 build 即可。

### VSCode（ESP-IDF 插件）

1. 文件 → 打开文件夹，选择工程根目录（含 `CMakeLists.txt` 的那层，**不是** `main/`）
2. 状态栏确认 IDF 版本 `v6.0.2`、target `esp32s3`（首次可能需执行 `ESP-IDF: Select current ESP-IDF version`）
3. 点击 **Build**（用 ESP-IDF 的按钮，不要用 CMake Tools 的）
4. 若提示 `cmake was not found`，说明当前窗口未选择 IDF 版本，按第 2 步处理

首次构建会从 components.espressif.com 下载 LVGL 等组件（约 160MB，需联网）。若下载失败，可设置镜像后重试：

```bash
export IDF_COMPONENT_REGISTRY_URL=https://components.espressif.cn
```

## 烧录

### 方式一：直接烧预编译固件（无需 ESP-IDF）

从 Releases 下载 `MDPS_esp32s3_full.bin`（约 1.9MB，已从 0x0 起合并 bootloader + 分区表 + 应用；仓库 `release/` 目录下同名的就是它），任选工具：

| 工具 | 操作 |
|---|---|
| [Flash Download Tool](https://www.espressif.com/en/support/download/other-tools)（Windows GUI） | 芯片 ESP32-S3，加载 bin，地址 `0x0`，SPI 80MHz / DIO / 16MB，选 COM 口后 START |
| esptool.py（跨平台） | `pip install esptool` 后执行下方命令 |
| [espflash](https://github.com/esp-rs/espflash)（单文件 CLI） | `espflash write-bin 0x0 MDPS_esp32s3_full.bin` |
| 浏览器（Chrome/Edge + Web Serial） | 配合 [esp-web-tools](https://esphome.github.io/esp-web-tools/) 一键烧录，无需安装任何软件 |

```bash
esptool.py --chip esp32s3 -p COM4 -b 460800 write_flash 0x0 MDPS_esp32s3_full.bin
```

### 方式二：从源码烧录

```bash
idf.py -p COM4 flash monitor
```

烧录地址（由 `build/flasher_args.json` 生成，勿手改）：

| 文件 | 地址 |
|---|---|
| `bootloader/bootloader.bin` | `0x0` |
| `partition_table/partition-table.bin` | `0x8000` |
| `MDPS.bin`（应用） | `0x10000` |

烧录参数：**ESP32-S3 / DIO / 80MHz / 16MB**。

自行生成发布用合并固件：

```bash
esptool.py --chip esp32s3 merge_bin -o MDPS_esp32s3_full.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/MDPS.bin
```

恢复出厂（清空 WiFi 配网与电源设定）：

```bash
esptool.py --chip esp32s3 -p COM4 erase_flash
```

## 内置 Web 接口

设备联网后，用浏览器打开 `http://<设备IP>/`。接口均为 GET：

| 接口 | 参数 | 说明 |
|---|---|---|
| `/api/status` | — | 全部状态 JSON（输出电压/电流、设定值、EN、温度、风扇、4 口数据） |
| `/api/set` | `v`(mV)、`i`(mA)、`en`(0/1)，均可省略 | 设置可调电源 |
| `/api/fan` | `pct`(0~100 或 `auto`) | 风扇手动占空比 / 恢复自动温控 |

示例：`http://192.168.1.50/api/set?v=12000&i=2000&en=1`

## 常见问题

- **编译报找不到 `esp_driver_gpio` / `esp_driver_i2c`**：IDF 版本过低，请使用 v6.0.2。ESP-IDF v5.x 中这些驱动仍属于 `driver` 组件。
- **烧录后花屏/马赛克**：SPI 时钟已降到 20MHz（`main.c` 中 `pclk_hz`）；换屏请先确认 `esp_lcd_panel_invert_color` 与 `rgb_ele_order` 设置。
- **触摸无响应**：CST816D 在未触发中断前不响应 I2C，已通过 `CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID` 跳过读 ID；若换了 CST816S 可关闭该选项。
- **触摸坐标偏移**：坐标映射与旋转在 `main/touch_io.c` 中处理。
- **首次构建下载组件很慢**：属于正常现象，组件缓存在 `~/.espressif`，后续构建不再重复下载。

## 许可证

本项目以 **GNU General Public License v3.0** 发布，许可证全文见 [LICENSE](LICENSE)。

你可以自由使用、修改和再分发，但衍生作品必须以相同的 GPL-3.0 协议开源，并保留原始版权声明。GPL 不提供任何担保。

第三方组件遵循各自协议：LVGL（MIT）、esp_lvgl_port / esp_lcd_touch_cst816s（Apache-2.0）、ESP-IDF（Apache-2.0）。
