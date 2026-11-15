idf_component_register(
    SRCS "main.c" "dht11.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_adc
        esp_http_client
        esp-tls
        mbedtls
        esp_driver_gpio
        esp_timer
        esp_rom
        lwip
)