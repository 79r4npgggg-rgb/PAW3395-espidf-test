idf_component_register(
    SRCS "main.c" "paw3395.c" "raw_web.c" "motion_ble.c" "sd_logger.c"
         "motion_wifi.c" "motion_webserver.c" "motion_logger.c" "motion_protocol.c"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html" "web/style.css" "web/app.js"
    PRIV_REQUIRES bt esp_driver_gpio esp_driver_sdspi esp_driver_spi esp_rom esp_event
                  esp_http_server esp_netif esp_timer esp_wifi fatfs lwip nvs_flash
)
