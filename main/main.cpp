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
#include "sock_uart.h"
#include "mount.h"
#include "http_server.h"
#include <ATTiny_UPDI.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "ExpanderMain.h"
#include "EmbeddedIOServiceCollection.h"
#include "Variable.h"
#include "CallBack.h"
#include "Config.h"
#include "CommunicationHandler_Prefix.h"
#include "CommunicationHandlers/CommunicationHandler_EFIGenie.h"
#include "AnalogService_Expander.h"
#include "DigitalService_Expander.h"
#include "ATTiny427ExpanderUpdateService.h"
#include "AnalogService_ATTiny427Expander.h"
#include "DigitalService_ATTiny427Expander.h"
#include "Esp32IdfAnalogService.h"
#include "Esp32IdfDigitalService.h"
#include "Esp32IdfTimerService.h"
#include "Esp32IdfPwmService.h"
#include "Esp32IdfCANService.h"
#include "Esp32IdfCommunicationService_Socket.h"
#include "Esp32IdfCommunicationService_WebSocket.h"

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

//read UPDI byte
extern "C" bool UPDI_Read(uint8_t *val)
{
    uint8_t timeout = 0;
    while(UPDI_rx_buffer_length < 1 && timeout++ < 100)
        vTaskDelay(1);
    if(timeout > 99){
        return false;
    }
    // ESP_LOGE("UPDI", "read %x %d %d %d", UPDI_rx_buffer[UPDI_rx_buffer_index], UPDI_rx_buffer_index, UPDI_rx_buffer_length, timeout);
    *val = UPDI_rx_buffer[UPDI_rx_buffer_index++];
    UPDI_rx_buffer_length--;
    UPDI_rx_buffer_index %= UPDI_RX_BUFFER_LENGTH;
    return true;
}

//write UPDI byte
extern "C" bool UPDI_Write(uint8_t val)
{
    if(!(uart_write_bytes(UPDI_uart_num, &val, 1) > 0))
        return false;
    uint8_t verify;
    if(!UPDI_Read(&verify))
        return false;
    return verify == val;
}

//UPDI send Break
extern "C" void UPDI_Break()
{
    uint32_t baudrate = 100000;

    uart_wait_tx_done(UPDI_uart_num, pdMS_TO_TICKS(30)); //wait for all bytes to be flushed
    uart_get_baudrate(UPDI_uart_num, &baudrate);
    uart_set_baudrate(UPDI_uart_num, 300);
    UPDI_Write(0);  // send a zero byte
    uart_wait_tx_done(UPDI_uart_num, pdMS_TO_TICKS(30)); // wait for 0 byte to finish before restore normal baudrate
    uart_set_baudrate(UPDI_uart_num, baudrate); // set baudrate back to normal after break is sent
}

extern "C" void UPDI_Idle()
{
    uint32_t baudrate = 100000;
    uart_get_baudrate(UPDI_uart_num, &baudrate);
    esp_rom_delay_us(15000000/baudrate);
}

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
    ICommunicationService *_communicationService;
    CommunicationHandler_EFIGenie *_efiGenieHandler;
    ExpanderMain *_expanderMain;
    Variable *loopTime;
    uint32_t prev;

    bool loadConfig()
    {
        const char * configPath = "/SPIFFS/config.bin";
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

    bool expandermain_write(void *destination, const void *data, size_t length) {
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

    bool expandermain_quit() {
        if(_expanderMain != 0)
        {
            delete _expanderMain;
            _expanderMain = 0;
        }
        return true;
    }

    bool expandermain_start() {
        if(_expanderMain == 0)
        {
            if(!loadConfig())
                return false;

            size_t configSize = 0;
            _expanderMain = new ExpanderMain(&_config, configSize, &_embeddedIOServiceCollection, _variableMap);

            _expanderMain->Setup();
        }
        return true;
    }

    void Setup() 
    {
        if(!loadConfig())
            return;
        if(_config == 0)
            return;
        _variableMap = new GeneratorMap<Variable>();

        size_t _configSize = 0;
        _expanderMain = new ExpanderMain(reinterpret_cast<void*>(_config), _configSize, &_embeddedIOServiceCollection, _variableMap);

        _efiGenieHandler = new CommunicationHandler_EFIGenie(_variableMap, expandermain_write, expandermain_quit, expandermain_start, _config);
        // ESP_LOGI("ASDF", "_config %p ", _efiGenieHandler->_config);
        _communicationService->RegisterReceiveCallBack([](communication_send_callback_t send, const void *data, size_t length){ return _efiGenieHandler->Receive(send, data, length);});

        _expanderMain->Setup();
        loopTime = _variableMap->GenerateValue(250);
    }
    void Loop() 
    {
        if(_expanderMain != 0) {
            const tick_t now = _embeddedIOServiceCollection.TimerService->GetTick();
            *loopTime = (float)(now-prev) / _embeddedIOServiceCollection.TimerService->GetTicksPerSecond();
            prev = now;
            _expanderMain->Loop();
        }
    }

    Esp32IdfAnalogService *_esp32AnalogService;
    Esp32IdfDigitalService *_esp32DigitalService;

    ATTiny427Expander_Registers _attinyRegisters(SPI);
    ATTiny427ExpanderUpdateService *_attinyUpdateService;
    AnalogService_ATTiny427Expander *_attinyAnalogService;
    DigitalService_ATTiny427Expander *_attinyDigitalService;

    uint32_t transactionCount = 0;
    uint32_t prevTransactionCount = 0;
    spi_device_handle_t attinySPI;
    DMA_ATTR uint8_t inBuffer[1024];
    DMA_ATTR uint8_t outBuffer[1024];

    void IRAM_ATTR attinyTransactionCB(spi_transaction_t *t)
    {
        _attinyUpdateService->Receive(inBuffer, t->rxlength / 8);
        _attinyDigitalService->Update();

        t->length = _attinyUpdateService->Transmit(outBuffer) * 8;
        t->rxlength = 0;
        spi_device_queue_trans(attinySPI, t, 0);
        transactionCount++;
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
        
        // vTaskDelay(pdMS_TO_TICKS(1000));
        // size_t attinyload_bytes = attinyload_end - attinyload_start;
        // uint8_t *attinyload = (uint8_t *)calloc(attinyload_bytes, sizeof(uint8_t));
        // memcpy(attinyload, attinyload_start, attinyload_bytes);
        // UPDI_Program(1, 4, 5, attinyload, attinyload_bytes);
        
        // xTaskCreate(echo_task, "echo_task", 2048, NULL, 10, NULL);

        // mount_sd("/SD");
        start_http_server();

        _esp32AnalogService = new Esp32IdfAnalogService();
        _esp32DigitalService = new Esp32IdfDigitalService();

        _attinyUpdateService = new ATTiny427ExpanderUpdateService(&_attinyRegisters);
        _attinyAnalogService = new AnalogService_ATTiny427Expander(&_attinyRegisters);
        _attinyDigitalService = new DigitalService_ATTiny427Expander(&_attinyRegisters);

        _embeddedIOServiceCollection.AnalogService = new AnalogService_Expander(_esp32AnalogService, _attinyAnalogService);
        _embeddedIOServiceCollection.DigitalService = new DigitalService_Expander(_esp32DigitalService, _attinyDigitalService);
        _embeddedIOServiceCollection.TimerService = new Esp32IdfTimerService();
        const Esp32IdfCANServiceChannelConfig canconfigs[2] 
        {
            {
                .t_config = TWAI_TIMING_CONFIG_500KBITS(),
                .f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL(),
                .g_config = TWAI_GENERAL_CONFIG_DEFAULT_V2(0, (gpio_num_t)9, (gpio_num_t)8, TWAI_MODE_NORMAL)
            },
            {
                .t_config = TWAI_TIMING_CONFIG_500KBITS(),
                .f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL(),
                .g_config = TWAI_GENERAL_CONFIG_DEFAULT_V2(1, (gpio_num_t)3, (gpio_num_t)2, TWAI_MODE_NORMAL)
            }
        };
        _embeddedIOServiceCollection.CANService = new Esp32IdfCANService(canconfigs);
        _communicationService = new Esp32IdfCommunicationService_WebSocket(server, "/EFIGenieCommunication");

        mount_spiffs("/SPIFFS");
        register_file_handler_http_server("/SPIFFS");

        spi_bus_config_t attinybuscfg = {
            .mosi_io_num = ATTINY_MOSI,
            .miso_io_num = ATTINY_MISO,
            .sclk_io_num = ATTINY_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 1024
        };
        spi_device_interface_config_t attinydevcfg = {
            .command_bits = 0,
            .address_bits = 0,
            .dummy_bits = 0,
            .mode = 0,                  //SPI mode 0
            .clock_speed_hz = 2400000,  //Clock out at 2.4 MHz. Theoretical 2.5Mhz doesn't work when ATTiny is using internal oscillator. running any faster loses MSB
            .spics_io_num = ATTINY_CS,  //CS pin
            .flags = SPI_DEVICE_POSITIVE_CS,
            .queue_size = 7,            //We want to be able to queue 7 transactions at a time
            .post_cb = attinyTransactionCB
        };
        //Initialize the SPI bus
        ret = spi_bus_initialize(SPI2_HOST, &attinybuscfg, SPI_DMA_CH_AUTO);
        ESP_ERROR_CHECK(ret);
        //Attach the LCD to the SPI bus
        ret = spi_bus_add_device(SPI2_HOST, &attinydevcfg, &attinySPI);
        ESP_ERROR_CHECK(ret);

        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.rx_buffer = inBuffer;
        t.tx_buffer = outBuffer;
        t.length = t.rxlength = 0;
        attinyTransactionCB(&t);

        Setup();
        while (1)
        {          
            vTaskDelay(1);

            Loop();
        }
    }
}