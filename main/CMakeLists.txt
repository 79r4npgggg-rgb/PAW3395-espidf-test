idf_component_register(
    SRCS "main.c" "paw3395.c" "raw_web.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_rom esp_event esp_http_server esp_netif esp_wifi nvs_flash
)
