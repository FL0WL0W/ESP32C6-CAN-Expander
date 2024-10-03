#include "driver/uart.h"
#include "driver/gpio.h"
#include <stdio.h>

#ifndef SOCK_UART_H
#define SOCK_UART_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct 
{
    uint16_t port;
    size_t sock_rx_buffer_size;
    uart_port_t uart_num;
    uart_config_t *uart_config;
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    bool(*sock_rx_hook)(const uint8_t *, size_t);
} sock_uart_config_t;

void sock_uart(void *arg);

#ifdef __cplusplus
}
#endif

#endif