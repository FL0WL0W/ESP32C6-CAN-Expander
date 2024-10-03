#include "driver/twai.h"
#include "driver/gpio.h"
#include <stdio.h>

#ifndef SOCK_CAN_H
#define SOCK_CAN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct 
{
    uint16_t port;
    int can_num;
    twai_handle_t can_handle;
    twai_general_config_t *can_general_config;
    twai_timing_config_t *can_timing_config;
    twai_filter_config_t *can_filter_config;
} sock_can_config_t;

void sock_can(void *arg);

#ifdef __cplusplus
}
#endif

#endif