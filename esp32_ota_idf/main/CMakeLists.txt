idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_client esp_https_ota bootloader_support cjson esp_driver_gpio esp_driver_uart vfs esp-tls
)
