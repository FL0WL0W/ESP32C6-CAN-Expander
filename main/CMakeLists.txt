file(GLOB_RECURSE USER_CSRCS ./*.c)
file(GLOB_RECURSE USER_CXXSRCS ./*.cpp)
set(SRCS ${USER_CSRCS} ${USER_CXXSRCS})# ${EmbeddedIOServiceHardwareAbstraction_SRCS})

idf_component_register(SRCS "main.cpp" "http_server.cpp" "Esp32IdfCommunicationService_WebSocket.cpp" "${SRCS}"
                       INCLUDE_DIRS "."# "${EmbeddedIOServicesHardwareAbstraction_Path}/include"
                       PRIV_REQUIRES EFIGenie esp_driver_gpio esp_driver_gptimer esp_adc esp_driver_spi esp_driver_uart esp_ringbuf spiffs esp_http_server fatfs esp_wifi nvs_flash driver)