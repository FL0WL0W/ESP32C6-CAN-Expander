#include "driver/gpio.h"
#include "driver/uart.h"


#ifndef ATTINY_UPDI_H
#define ATTINY_UPDI_H

#ifdef __cplusplus
extern "C" {
#endif

#define UPDI_RX_BUFFER_LENGTH 1024
extern uart_port_t UPDI_uart_num;
extern uint8_t UPDI_rx_buffer[];
extern volatile size_t UPDI_rx_buffer_index;
extern volatile size_t UPDI_rx_buffer_length;

bool UPDI_Program(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin, uint8_t *data, uint32_t length);
bool UPDI_Enable(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin);
bool UPDI_Reset();

#ifdef __cplusplus
}
#endif

#endif