idf_component_register(
    SRCS "app_main.c" "ld2410_task.c" "wifi_mgr.c" "http_srv.c"
    INCLUDE_DIRS "."
    REQUIRES core esp_driver_uart esp_driver_gpio esp_timer nvs_flash freertos esp_hw_support heap log
             esp_wifi esp_netif esp_event esp_http_server lwip esp_system
    PRIV_REQUIRES espressif__mdns)
