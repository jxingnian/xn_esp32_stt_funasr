# WiFi 组件 CMake 配置（独立组件）

idf_component_register(
    SRCS
        "src/wifi_manager.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        esp_netif
)
