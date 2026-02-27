file(GLOB_RECURSE USER_CSRCS ./*.c)
file(GLOB_RECURSE USER_CXXSRCS ./*.cpp)
set(SRCS ${USER_CSRCS} ${USER_CXXSRCS})

idf_component_register(SRCS "main.cpp" "${SRCS}" 
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES EFIGenie ATTiny_UPDI esp_driver_gpio esp_driver_gptimer esp_adc esp_driver_spi esp_driver_uart esp_ringbuf esp_http_server fatfs esp_wifi nvs_flash driver app_update spiffs MMROFS spi_flash)