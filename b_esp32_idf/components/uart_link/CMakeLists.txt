idf_component_register(SRCS "uart_link.cpp" "session_gate.cpp" "session_wire.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_common
                       PRIV_REQUIRES sensor_protocol network_link esp_driver_uart esp_hw_support esp_timer freertos ota_service)
