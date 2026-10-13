set(BAMBU_SRCS
    "app_main.c"
    "app_state.c"
    "app_config.c"
    "display/ssd1306.c"
    "display/font.c"
    "display/ui.c"
    "net/wifi_manager.c"
    "net/portal.c"
    "net/bambu_api.c"
    "net/bambu_mqtt.c"
)

idf_component_register(SRCS ${BAMBU_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer
                                esp_http_server esp_http_client esp-tls mqtt cjson
                                esp_driver_i2c esp_driver_gpio mbedtls lwip)
