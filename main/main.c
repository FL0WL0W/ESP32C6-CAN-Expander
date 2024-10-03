#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "hal/gpio_hal.h"
#include "uart_listen.h"
#include "can_listen.h"
#include "sock_uart.h"
#include "sock_can.h"
#include "mount.h"
#include "http_server.h"
#include <ATTiny_UPDI.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define UPDI1_UART_RX_PIN 15
#define UPDI1_UART_TX_PIN 14

void wifi_init_softap()
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "EFIGenie-Expander",
            .ssid_len = strlen("EFIGenie-Expander"),
            // .password = EXAMPLE_ESP_WIFI_PASS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = true },
            .max_connection = 5
        },
    };
    if (strlen((char *)wifi_config.ap.password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void UPDI1_Enable(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(100));
    UPDI_Enable(1, UPDI1_UART_TX_PIN, UPDI1_UART_RX_PIN);
    vTaskDelete(NULL);
}

uint8_t updi_enabled = 0;
bool UPDI1_RX_Hook(const uint8_t *data, size_t len)
{
    if(updi_enabled != 1) {
        xTaskCreate(UPDI1_Enable, "UPDI1_Enable", 4096, 0, 5, NULL);
    }
    updi_enabled = 1;
    return true;
}

void app_main()
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //initialize net
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //initialize wifi
    wifi_init_softap();

    //install uart listen services
    uart_listen_config_t uart_listen_config[UART_NUM_MAX];
    char uart_listen_name[UART_NUM_MAX][16];
    for(uint8_t i = UART_NUM_0; i < UART_NUM_MAX; i++)
    {
        uart_listen_config[i].uart_num = i;
        uart_listen_config[i].rx_buffer_size = 2048;
        uart_listen_config[i].tx_buffer_size = 0;
        sprintf(uart_listen_name[i], "uart_listen_%d", i);
        xTaskCreate(uart_listen, uart_listen_name[i], 4096, &uart_listen_config[i], 10, NULL);
    }

    uart_config_t UPDI_uart_config = {
        .baud_rate = 100000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_2, 
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    sock_uart_config_t UPDI1_sock_uart_config = 
    {
        .port = 8001,
        .sock_rx_buffer_size = 1440,
        .uart_num = 1,
        .uart_config = &UPDI_uart_config,
        .rx_pin = UPDI1_UART_RX_PIN,
        .tx_pin = UPDI1_UART_TX_PIN,
        .sock_rx_hook = UPDI1_RX_Hook
    };

    xTaskCreate(sock_uart, "UPDI1_sock_uart", 4096, &UPDI1_sock_uart_config, 5, NULL);

    // //install can listen service
    // twai_general_config_t twai_general_config = {
    //     .controller_id = 0,
    //     .mode = TWAI_MODE_LISTEN_ONLY,
    //     .tx_io = 9,
    //     .rx_io = 8,
    //     .clkout_io = TWAI_IO_UNUSED,
    //     .bus_off_io = TWAI_IO_UNUSED,
    //     .tx_queue_len = 1000,
    //     .rx_queue_len = 1000,
    //     .alerts_enabled = TWAI_ALERT_NONE,
    //     .clkout_divider = 0,
    //     .intr_flags = ESP_INTR_FLAG_LEVEL1
    // };
    // twai_timing_config_t twai_timing_config = TWAI_TIMING_CONFIG_500KBITS();
    // twai_filter_config_t twai_filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    // twai_handle_t twai_handle;
    // twai_driver_install_v2(&twai_general_config, &twai_timing_config, &twai_filter_config, &twai_handle);
    // can_listen_config_t can_listen_config = {
    //     .can_num = 0,
    //     .can_handle = twai_handle
    // };

    // xTaskCreate(can_listen, "can_listen", 4096, &can_listen_config, 9, NULL);

    // sock_can_config_t sock_can_config = {
    //     .port = 7000,
    //     .can_num = 0,
    //     .can_handle = twai_handle,
    //     .can_general_config = &twai_general_config,
    //     .can_timing_config = &twai_timing_config,
    //     .can_filter_config = &twai_filter_config
    // };
    // xTaskCreate(can_listen, "sock_can", 4096, &sock_can_config, 4, NULL);
    
    // vTaskDelay(pdMS_TO_TICKS(1000));
    // size_t attinyload_bytes = attinyload_end - attinyload_start;
    // uint8_t *attinyload = (uint8_t *)calloc(attinyload_bytes, sizeof(uint8_t));
    // memcpy(attinyload, attinyload_start, attinyload_bytes);
    // UPDI_Program(1, 4, 5, attinyload, attinyload_bytes);
    
    // xTaskCreate(echo_task, "echo_task", 2048, NULL, 10, NULL);

    mount_sd("/SD");
    mount_spiffs("/SPIFFS");
    start_http_server("/SPIFFS");

    while(1) {
        vTaskDelay(1);
    }
}
