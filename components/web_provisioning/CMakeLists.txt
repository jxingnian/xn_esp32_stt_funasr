idf_component_register(
    SRCS "src/web_provisioning.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi nvs_flash spiffs esp_netif
)
