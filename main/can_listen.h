#include "driver/twai.h"

#ifndef CAN_LISTEN_H
#define CAN_LISTEN_H

#ifdef __cplusplus
#include <stdio.h>
#include <functional>
#include <list>

typedef std::function<void(const twai_message_t *)> can_callback_t;
std::list<can_callback_t>::iterator can_listen_add_callback(int can_num, can_callback_t callback);
void can_listen_remove_callback(int can_num, std::list<can_callback_t>::iterator callback_iterator);
extern "C" {
#endif

typedef struct 
{
    int can_num;
    twai_handle_t can_handle;
} can_listen_config_t;
void can_listen(void *arg);

#ifdef __cplusplus
}
#endif

#endif