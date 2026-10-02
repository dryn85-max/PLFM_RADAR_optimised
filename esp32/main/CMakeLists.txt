idf_component_register(
    SRCS "app_main.c" "ld2410_task.c"
    INCLUDE_DIRS "."
    REQUIRES core esp_driver_uart esp_timer nvs_flash freertos esp_hw_support heap log)
