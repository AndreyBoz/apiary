idf_component_register(SRCS "sensors.c" INCLUDE_DIRS "." REQUIRES config esp_driver_gpio esp_driver_i2s esp_rom esp_timer freertos)
