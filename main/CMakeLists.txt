idf_component_register(
    SRCS "main.c" "paw3395.c" "raw_web.c" "motion_ble.c" "sd_logger.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt esp_driver_gpio esp_driver_sdspi esp_driver_spi esp_rom esp_event
                  esp_http_server esp_netif esp_timer esp_wifi fatfs nvs_flash
)
