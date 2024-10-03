
#include "sock_can.h"
#include "can_listen.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "time.h"
#include "sys/time.h"

typedef struct
{
    sock_can_config_t *sock_can_config;
    int sock;
} sock_can_read_config_t;

enum socket_can_state_t {
    STATE_NO_BUS = 0,
    STATE_RAW = 2
};

//only doing rawmode implementation for now since thats what socketcandcl uses
void sock_can_read(void *arg)
{
    sock_can_read_config_t *config = (sock_can_read_config_t *)arg;
    socket_can_state_t state = STATE_NO_BUS;

    auto can_callback_iterator = can_listen_add_callback(config->sock_can_config->can_num, [&](const twai_message_t *message) {
        switch(state) {
            case STATE_RAW: 
                break;
            default:
            //TODO filters
                return;
        }
        struct timeval tv;
        gettimeofday(&tv, NULL);
        char socketcanmsg[64];
        if(message->extd) {
            sprintf(socketcanmsg, "< frame %08X %lld.%06ld ", (unsigned int)message->identifier, tv.tv_sec, tv.tv_usec);
        } else {
            sprintf(socketcanmsg, "< frame %03X %lld.%06ld ", (unsigned int)message->identifier, tv.tv_sec, tv.tv_usec);
        }

        for (uint8_t i = 0; i < message->data_length_code; i++) {
            sprintf(socketcanmsg + strlen(socketcanmsg), "%02X ",  message->data[i]);
        }

        sprintf(socketcanmsg + strlen(socketcanmsg), ">");
        send(config->sock, socketcanmsg, strlen(socketcanmsg), 0);
    });

    int len;
    uint8_t rx_buffer[1500];
    while(1)
    {
        len = recv(config->sock, rx_buffer, sizeof(rx_buffer), 0);
        if (len == 0 || len == -1) {
            ESP_LOGW("SOCK_CAN", "Connection closed");
            goto sock_can_read_cleanup;
        } else if (len < 0) {
            ESP_LOGE("SOCK_CAN", "Error occurred during receiving: errno %d", len);
        } else {
            if(strncmp((const char *)rx_buffer, "< echo >", 8) == 0) {
                send(config->sock, rx_buffer, len, 0);
            } else if(strncmp((const char *)rx_buffer, "< open can", 10) == 0) {
                //TODO move can callback stuff in here
            } else if(strncmp((const char *)rx_buffer, "< rawmode >", 11) == 0) {
                state = STATE_RAW;
            } else {
            }
        }
    }

sock_can_read_cleanup:
    can_listen_remove_callback(config->sock_can_config->can_num, can_callback_iterator);
    shutdown(config->sock, 0);
    close(config->sock);
    vTaskDelete(NULL);
}

extern "C" {
    void sock_can(void *arg)
    {
        sock_can_config_t *config = (sock_can_config_t *)arg;

        int keepAlive = 1;
        int keepIdle = 5;
        int keepInterval = 5;
        int keepCount = 3;
        int noDelay = 1;
        struct sockaddr_storage dest_addr;

        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(config->port);

        int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock < 0) 
        {
            ESP_LOGE("SOCK_CAN", "Unable to create socket: errno %d", errno);
            vTaskDelete(NULL);
            return;
        }
        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) 
        {
            ESP_LOGE("SOCK_CAN", "Socket unable to bind: errno %d", errno);
            goto sock_uart_cleanup;
        }

        err = listen(listen_sock, 1);
        if (err != 0) 
        {
            ESP_LOGE("SOCK_CAN", "Error occurred during listen: errno %d", errno);
            goto sock_uart_cleanup;
        }

        while (1) 
        {
            struct sockaddr_storage source_addr;
            socklen_t addr_len = sizeof(source_addr);
            int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (sock < 0) {
                ESP_LOGE("SOCK_CAN", "Unable to accept connection: errno %d", errno);
                continue;
            }

            // Set tcp keepalive option
            setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
            // set no delay
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(int));

            //greeting
            const char *greeting = "< hi >";
            send(sock, greeting, 6, 0);
            
            sock_can_read_config_t sock_can_read_config = 
            {
                .sock_can_config = config,
                .sock = sock
            };
            xTaskCreate(sock_can_read, "sock_can_read", 4096, &sock_can_read_config, 10, NULL);
        }

sock_uart_cleanup:
        close(listen_sock);
        vTaskDelete(NULL);
    }
}