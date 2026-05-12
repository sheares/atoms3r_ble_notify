idf_component_register(
    SRCS "main.c" "ble_nus.c" "gc9107.c" "lp5562.c" "bmi270.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_i2c esp_driver_spi esp_driver_gpio
    PRIV_REQUIRES esp_timer
)
