idf_component_register(
        SRCS "main.c" "ble_config_uuid.c" "nvs_dta.c"
        INCLUDE_DIRS "."
        PRIV_REQUIRES bt spi_flash driver nvs_flash esp_adc

)
