idf_component_register(SRCS "ble_config_uuid.c" "nvs_dta.c" "main.c" 
                    REQUIRES esp_driver_gpio esp_adc bt nvs_flash driver
                    PRIV_REQUIRES spi_flash
                    INCLUDE_DIRS "")
