idf_component_register(
    SRCS "app_main.c" "ld2410_task.c" "gps_task.c" "imu_task.c" "wifi_mgr.c" "http_srv.c" "live.c" "rec_srv.c" "sntp_sync.c" "rgb_led.c" "status_led_task.c"
    INCLUDE_DIRS "."
    REQUIRES core esp_driver_uart esp_driver_gpio esp_driver_i2c esp_driver_rmt esp_timer nvs_flash freertos esp_hw_support heap log
             esp_wifi esp_netif esp_event esp_http_server lwip esp_system
    EMBED_TXTFILES "web_page.html"
    PRIV_REQUIRES espressif__mdns)
