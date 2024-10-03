#include "driver/uart.h"

#ifndef UART_LISTEN_H
#define UART_LISTEN_H

#ifdef __cplusplus
#include <stdio.h>
#include <functional>
#include <list>

typedef std::function<void(const uint8_t *, size_t)> uart_callback_t;
std::list<uart_callback_t>::iterator uart_listen_add_callback(uart_port_t uart_num, uart_callback_t callback);
void uart_listen_remove_callback(uart_port_t uart_num, std::list<uart_callback_t>::iterator callback_iterator);
extern "C" {
#endif

typedef struct 
{
    uart_port_t uart_num;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
} uart_listen_config_t;
void uart_listen(void *arg);

#ifdef __cplusplus
}
#endif

#endif