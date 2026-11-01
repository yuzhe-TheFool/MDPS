# 自动收集 main 目录下所有 .c（含 wx_icons/ 子目录），避免新增文件后忘记登记
# 注意：新增/删除 .c 后需重新运行一次 cmake 配置（idf.py reconfigure 或 fullclean 后重建）
file(GLOB MAIN_SRCS "wx_icons.c" "web_server.c" "encoder.c" "main_ui.c" "power_ui.c" "main.c" "net_time.c" "wifi_mgr.c" "prov_ui.c" "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
file(GLOB WX_ICONS_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/wx_icons/*.c")

# IDF v6 把 driver 拆成 esp_driver_* 细分组件，需显式依赖
idf_component_register(SRCS ${MAIN_SRCS} ${WX_ICONS_SRCS}
                    INCLUDE_DIRS "."
                    REQUIRES "esp_lvgl_port" "esp_lcd_touch" "esp_lcd_touch_cst816s"
                             "esp_wifi" "nvs_flash" "esp_netif" "esp_event"
                             "esp_driver_gpio" "esp_driver_ledc"
                             "esp_driver_pcnt" "esp_driver_i2c"
                             "esp_timer"
                             "esp_http_client" "esp_http_server")


