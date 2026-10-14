if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../config/wifi_credentials.h")
    message(FATAL_ERROR "Create config/wifi_credentials.h from the example; keep it out of Git.")
endif()
idf_component_register(SRCS "network_link.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "../../config"
                       REQUIRES esp_common
                       PRIV_REQUIRES sensor_protocol esp_wifi esp_event esp_netif
                                     nvs_flash lwip esp_timer freertos mdns ota_service)
