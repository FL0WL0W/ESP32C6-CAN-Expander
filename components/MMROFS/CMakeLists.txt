idf_component_register( SRCS "MMROFS.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES vfs esp_partition spi_flash)
