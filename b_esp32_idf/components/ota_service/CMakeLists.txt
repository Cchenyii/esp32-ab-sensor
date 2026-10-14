idf_component_register(SRCS "ota_service.c" INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_update bootloader_support esp_app_format esp_http_server
                  esp_system esp_hw_support esp_timer esp_netif freertos mbedtls espressif__cjson
    EMBED_TXTFILES "ota_page.html")
