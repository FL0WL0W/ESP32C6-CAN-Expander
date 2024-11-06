#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "sdkconfig.h"
#include "esp_log.h"
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

#include "EmbeddedIOServiceCollection.h"
#include "ATTiny427ExpanderUpdateService.h"
#include "AnalogService_ATTiny427Expander.h"
#include "Esp32IdfDigitalService.h"
#include "Esp32IdfTimerService.h"
#include "Esp32IdfAnalogService.h"
#include "Esp32IdfPwmService.h"
#include "EFIGenieMain.h"
#include "Variable.h"
#include "CallBack.h"
#include "Config.h"
#include "CommunicationHandler_Prefix.h"
#include "CommunicationHandlers/CommunicationHandler_EFIGenie.h"

#define UPDI_UART_RX_PIN 15
#define UPDI_UART_TX_PIN 14

#define ATTINY_MISO 23
#define ATTINY_MOSI 7
#define ATTINY_CLK  6
#define ATTINY_CS   22

using namespace OperationArchitecture;
using namespace EmbeddedIOServices;
using namespace EmbeddedIOOperations;
using namespace Esp32;
using namespace EFIGenie;

extern "C"
{
    void wifi_init_softap()
    {
        esp_netif_create_default_wifi_ap();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        wifi_config_t wifi_config = {
            .ap = {
                .ssid = "EFIGenie-Expander",
                // .password = EXAMPLE_ESP_WIFI_PASS,
                .ssid_len = strlen("EFIGenie-Expander"),
                .authmode = WIFI_AUTH_WPA2_PSK,
                .max_connection = 5,
                .pmf_cfg = { .required = true }
            },
        };
        if (strlen((char *)wifi_config.ap.password) == 0) {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    void UPDI_Enable_Task(void *arg) {
        vTaskDelay(pdMS_TO_TICKS(100));
        UPDI_Enable((uart_port_t)1, (gpio_num_t)UPDI_UART_TX_PIN, (gpio_num_t)UPDI_UART_RX_PIN);
        vTaskDelete(NULL);
    }

    uint8_t updi_enabled = 0;
    bool UPDI_RX_Hook(const uint8_t *data, size_t len)
    {
        if(updi_enabled != 1) {
            xTaskCreate(UPDI_Enable_Task, "UPDI_Enable", 4096, 0, 5, NULL);
        }
        updi_enabled = 1;
        return true;
    }

    void *_config = 0;
    GeneratorMap<Variable> *_variableMap;
    EmbeddedIOServiceCollection _embeddedIOServiceCollection;
    CommunicationHandler_EFIGenie *_efiGenieHandler;
    EFIGenieMain *_engineMain;
    Variable *loopTime;
    uint32_t prev;

    bool loadConfig()
    {
        const char * configPath = "/SPIFFS/Config.bin";
        FILE *fd = NULL;
        struct stat file_stat;

        if(stat(configPath, &file_stat) == -1)
            return false;
        fd = fopen(configPath, "r");
        if(!fd)
            return false;
        free(_config);
        _config = malloc(file_stat.st_size);
        fread(_config, 1, file_stat.st_size, fd);
        fclose(fd);
        return true;
    }

    bool enginemain_write(void *destination, const void *data, size_t length) {
        if(reinterpret_cast<size_t>(destination) >= 0x20000000 && reinterpret_cast<size_t>(destination) <= 0x2000FA00)
        {
            std::memcpy(destination, data, length);
        }
        else if(reinterpret_cast<size_t>(destination) >= 0x8004000 && reinterpret_cast<size_t>(destination) <= 0x8008000)
        {
            //TODO Write to flash
        }

        return true;
    }

    bool enginemain_quit() {
        if(_engineMain != 0)
        {
            delete _engineMain;
            _engineMain = 0;
        }
        return true;
    }

    bool enginemain_start() {
        if(_engineMain == 0)
        {
            if(!loadConfig())
                return false;

            size_t configSize = 0;
            _engineMain = new EFIGenieMain(&_config, configSize, &_embeddedIOServiceCollection, _variableMap);

            _engineMain->Setup();
        }
        return true;
    }

    ATTiny427Expander_Registers _attinyRegisters(SPI);
    ATTiny427ExpanderUpdateService *_attinyUpdateService;

    void Setup() 
    {
        if(!loadConfig())
            return;
        if(_config == 0)
            return;
        _variableMap = new GeneratorMap<Variable>();

        if(_embeddedIOServiceCollection.DigitalService == 0)
            _embeddedIOServiceCollection.DigitalService = new Esp32IdfDigitalService();
        if(_embeddedIOServiceCollection.AnalogService == 0)
            _embeddedIOServiceCollection.AnalogService = new Esp32IdfAnalogService();
        if(_embeddedIOServiceCollection.TimerService == 0)
            _embeddedIOServiceCollection.TimerService = new Esp32IdfTimerService();
        
        // if(_embeddedIOServiceCollection.PwmService == 0)
        //     _embeddedIOServiceCollection.PwmService = new Esp32IdfPwmService();

        size_t _configSize = 0;
        _engineMain = new EFIGenieMain(reinterpret_cast<void*>(_config), _configSize, &_embeddedIOServiceCollection, _variableMap);

        _efiGenieHandler = new CommunicationHandler_EFIGenie(_variableMap, enginemain_write, enginemain_quit, enginemain_start, _config);

        _engineMain->Setup();
        loopTime = _variableMap->GenerateValue(250);
    }
    void Loop() 
    {
        const tick_t now = _embeddedIOServiceCollection.TimerService->GetTick();
        *loopTime = (float)(now-prev) / _embeddedIOServiceCollection.TimerService->GetTicksPerSecond();
        prev = now;
        // _cdcService->Flush();

        if(_engineMain != 0) {
            _engineMain->Loop();
        }
    }

    DMA_ATTR uint8_t inBuffer[1024];
    DMA_ATTR uint8_t outBuffer[1024];

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
            uart_listen_config[i].uart_num = (uart_port_t)i;
            uart_listen_config[i].rx_buffer_size = 2048;
            uart_listen_config[i].tx_buffer_size = 0;
            sprintf(uart_listen_name[i], "uart_listen_%d", i);
            xTaskCreate(uart_listen, uart_listen_name[i], 4096, &uart_listen_config[i], 10, NULL);
        }

        // uart_config_t UPDI_uart_config = {
        //     .baud_rate = 100000,
        //     .data_bits = UART_DATA_8_BITS,
        //     .parity    = UART_PARITY_EVEN,
        //     .stop_bits = UART_STOP_BITS_2, 
        //     .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        //     .source_clk = UART_SCLK_DEFAULT,
        // };

        // sock_uart_config_t UPDI_sock_uart_config = 
        // {
        //     .port = 8001,
        //     .sock_rx_buffer_size = 1440,
        //     .uart_num = (uart_port_t)1,
        //     .uart_config = &UPDI_uart_config,
        //     .tx_pin = (gpio_num_t)UPDI_UART_TX_PIN,
        //     .rx_pin = (gpio_num_t)UPDI_UART_RX_PIN,
        //     .sock_rx_hook = UPDI_RX_Hook
        // };

        // xTaskCreate(sock_uart, "UPDI_sock_uart", 4096, &UPDI_sock_uart_config, 5, NULL);

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

        // mount_sd("/SD");
        mount_spiffs("/SPIFFS");
        start_http_server("/SPIFFS");

        AnalogService_ATTiny427Expander *analogService = new AnalogService_ATTiny427Expander(&_attinyRegisters);

        spi_device_handle_t spi;
        spi_bus_config_t buscfg = {
            .mosi_io_num = ATTINY_MOSI,
            .miso_io_num = ATTINY_MISO,
            .sclk_io_num = ATTINY_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 1024
        };
        spi_device_interface_config_t devcfg = {
            .command_bits = 0,
            .address_bits = 0,
            .dummy_bits = 0,
            .mode = 0,                  //SPI mode 0
            .clock_speed_hz = 250000,  //Clock out at 2.5 MHz
            .spics_io_num = ATTINY_CS,  //CS pin
            .flags = SPI_DEVICE_POSITIVE_CS,
            .queue_size = 7,            //We want to be able to queue 7 transactions at a time
        };
        //Initialize the SPI bus
        ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        ESP_ERROR_CHECK(ret);
        //Attach the LCD to the SPI bus
        ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
        ESP_ERROR_CHECK(ret);

        _attinyUpdateService = new ATTiny427ExpanderUpdateService(&_attinyRegisters);
        _embeddedIOServiceCollection.AnalogService = new AnalogService_ATTiny427Expander(&_attinyRegisters);
        _embeddedIOServiceCollection.AnalogService->InitPin(7);

        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.rx_buffer = inBuffer;
        t.tx_buffer = outBuffer;
        t.length = _attinyUpdateService->Transmit(outBuffer) * 8;
        t.rxlength = 0;
        if(spi_device_polling_transmit(spi, &t) == ESP_OK)
            _attinyUpdateService->Receive(inBuffer, t.rxlength / 8);

        ESP_LOGI("ATTINY", "length: %d", t.rxlength / 8);

        // Setup();
        while (1)
        {
            memset(&t, 0, sizeof(t));
            t.rx_buffer = inBuffer;
            t.tx_buffer = outBuffer;
            t.length = _attinyUpdateService->Transmit(outBuffer) * 8;
            t.rxlength = 0;
            if(spi_device_polling_transmit(spi, &t) == ESP_OK)
                _attinyUpdateService->Receive(inBuffer, t.rxlength / 8);

            
            ESP_LOGI("ATTINY", "length: %d\t PA7: %f %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x", t.rxlength / 8, _embeddedIOServiceCollection.AnalogService->ReadPin(7),
                inBuffer[0], inBuffer[1], inBuffer[2], inBuffer[3], inBuffer[4], inBuffer[5], inBuffer[6], inBuffer[7], inBuffer[8], inBuffer[9],
                inBuffer[10], inBuffer[11], inBuffer[12], inBuffer[13], inBuffer[14], inBuffer[15], inBuffer[16]);

            // Loop();
            vTaskDelay(100);
        }
    }
}