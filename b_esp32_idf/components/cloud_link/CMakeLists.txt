idf_component_register(SRCS "cloud_link.c" "threshold_store.c" "cloud_settings.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../config"
    PRIV_REQUIRES esp_driver_uart esp_system esp_timer nvs_flash
                  freertos network_link uart_link esp-tls mbedtls esp_netif
                  espressif__mqtt espressif__cjson ota_service)
