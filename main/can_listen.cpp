
#include "can_listen.h"

std::list<can_callback_t> can_callback_list[SOC_TWAI_CONTROLLER_NUM];
extern "C" {
    void can_listen(void *arg)
    {
        can_listen_config_t *config = (can_listen_config_t *)arg;

        twai_message_t rx_buffer;
        while (1) 
        {
            // Read and write data to functions
            if(twai_receive_v2(config->can_handle, &rx_buffer, pdMS_TO_TICKS(100)) != ESP_OK) continue;

            const std::list<can_callback_t>::iterator begin = can_callback_list[config->can_num].begin();
            const std::list<can_callback_t>::iterator end = can_callback_list[config->can_num].end();
            std::list<can_callback_t>::iterator next = begin;
            while(next != end)
            {
                (*next)(&rx_buffer);
                next++;
            }
        }
    }
}

std::list<can_callback_t>::iterator can_listen_add_callback(int can_num, can_callback_t callback)
{
    return can_callback_list[can_num].insert(can_callback_list[can_num].begin(), callback);
}
void can_listen_remove_callback(int can_num, std::list<can_callback_t>::iterator callback_iterator)
{
    can_callback_list[can_num].erase(callback_iterator);
}