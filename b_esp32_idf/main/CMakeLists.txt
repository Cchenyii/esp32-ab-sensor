idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_system esp_app_format freertos sensor_protocol uart_link network_link cloud_link ota_service)
