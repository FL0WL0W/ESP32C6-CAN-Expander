
#include "uart_listen.h"
#include "soc/uart_reg.h"
#include "freertos/semphr.h"
#include <map>

std::map<uint32_t, uart_callback_t> uart_callback_map[UART_NUM_MAX];
SemaphoreHandle_t uart_callback_map_semaphore[UART_NUM_MAX];

extern "C" {
    void uart_listen(void *arg)
    {
        uart_listen_config_t *config = (uart_listen_config_t *)arg;
        uart_callback_map_semaphore[config->uart_num] = xSemaphoreCreateBinary();
        xSemaphoreGive(uart_callback_map_semaphore[config->uart_num]);

        if(!uart_is_driver_installed(config->uart_num))
            ESP_ERROR_CHECK(uart_driver_install(config->uart_num, config->rx_buffer_size, config->tx_buffer_size, 0, NULL, ESP_INTR_FLAG_IRAM));
        ESP_ERROR_CHECK(uart_set_rx_full_threshold(config->uart_num, 1));

        uint8_t rx_buffer[config->rx_buffer_size];
        while (1) 
        {
            // Read data from the UART. have to do this weird read stuff below because read bytes tries to get all the bytes requested before timing out. not just what's available
            size_t len = uart_read_bytes(config->uart_num, rx_buffer, 1, pdMS_TO_TICKS(1000));
            size_t bufferedLen = 0;
            uart_get_buffered_data_len(config->uart_num, &bufferedLen);
            if(bufferedLen > 0) 
            {
                len += uart_read_bytes(config->uart_num, rx_buffer + 1, bufferedLen > (config->rx_buffer_size - 1)? (config->rx_buffer_size - 1): bufferedLen, pdMS_TO_TICKS(1000));
            }
            // Write data to functions
            if (len) 
            {
                if(xSemaphoreTake(uart_callback_map_semaphore[config->uart_num], pdMS_TO_TICKS(100)) != pdTRUE)
                    continue;
                const auto begin = uart_callback_map[config->uart_num].begin();
                const auto end = uart_callback_map[config->uart_num].end();
                auto next = begin;
                while(next != end)
                {
                    next->second(rx_buffer, len);
                    next++;
                }
                xSemaphoreGive(uart_callback_map_semaphore[config->uart_num]);
            }
        }
    }
}

uint32_t callback_id_iterator = 1;

uint32_t uart_listen_add_callback(uart_port_t uart_num, uart_callback_t callback)
{
    if(xSemaphoreTake(uart_callback_map_semaphore[uart_num], pdMS_TO_TICKS(1000)) != pdTRUE)
        return 0;
    uint32_t callback_id = callback_id_iterator++;
    uart_callback_map[uart_num].insert({callback_id, callback});
    xSemaphoreGive(uart_callback_map_semaphore[uart_num]);
    return callback_id;
}
void uart_listen_remove_callback(uart_port_t uart_num, uint32_t callback_id)
{
    if(callback_id == 0)
        return;
    if(xSemaphoreTake(uart_callback_map_semaphore[uart_num], pdMS_TO_TICKS(1000)) != pdTRUE)
        return;
    uart_callback_map[uart_num].erase(callback_id);
    xSemaphoreGive(uart_callback_map_semaphore[uart_num]);
}