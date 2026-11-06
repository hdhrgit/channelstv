idf_component_register(SRCS "main.c" "board.c" "bridge.c" "h264dec.c" "player.c" "rotate.c" "setup.c" "stream.c" "ts.c" "tv.c" "ui.c" "web.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_ledc esp_driver_spi
                                esp_driver_usb_serial_jtag esp_driver_tsens esp_lcd esp_wifi esp_netif nvs_flash lwip
                                app_update esp_timer esp_http_server esp_http_client esp_adc json esp_pm esp_psram spi_flash videojet
                       EMBED_FILES "DejaVuSans.ttf"
                       EMBED_TXTFILES "index.html" "setup.html" "chips.html")
target_compile_definitions(${COMPONENT_LIB} PRIVATE CHANNELSTV_VERSION="${PROJECT_VER}")
