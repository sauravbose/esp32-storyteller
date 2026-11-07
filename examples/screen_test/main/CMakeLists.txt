# Which C files to compile, and which ESP-IDF parts we use
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_driver_gpio)
