#include <ATTiny_UPDI.h>
#include <stdio.h>
#include <string.h>
#include <functional>
#include "driver/gpio.h"
#include "hal/gpio_hal.h"
#include "esp_log.h"
#include "UPDI.h"
#include "ICommunicationService.h"

#define UPDI_KEYNVMPROG 0x4E564D50726F6720
#define UPDI_KEYNVMERASE 0x4E564D4572617365

using namespace EmbeddedIOServices;

void _UPDI_error_check_failed(const char *file, int line, const char *function, const char *expression)
{
    ESP_LOGE("UPDI", "UPDI_ERROR_CHECK failed at %p", __builtin_return_address(0));
    ESP_LOGE("UPDI", "file: \"%s\" line %d\nfunc: %s\nexpression: %s", file, line, function, expression);
}

#define UPDI_ERROR_CHECK(x) do {                            \
        if (!(x)) {                                         \
            _UPDI_error_check_failed(__FILE__, __LINE__,    \
                                    __ASSERT_FUNC, #x);     \
            return false;                                   \
        }                                                   \
    } while(0)

uart_port_t UPDI_uart_num;
ICommunicationService *_updiCommService = nullptr;
communication_receive_callback_id_t _updiCallbackId = 0;
uint8_t UPDI_rx_buffer[UPDI_RX_BUFFER_LENGTH];
volatile size_t UPDI_rx_buffer_index = 0;
volatile size_t UPDI_rx_buffer_length = 0;

extern "C" {
    extern ICommunicationService *_uartCommServices[UART_NUM_MAX];
}

bool ack_disabled = false;
extern "C" bool UPDI_ReadAck()
{
    if(ack_disabled) return true;
    uint8_t ack;
    if(!UPDI_Read(&ack)) return false;
    return ack == 0x40;
}

bool UPDI_DisableAck()
{
    uint8_t CTRLA = 0x0C;
    UPDI_ERROR_CHECK(UPDI_LDCS(0x2, &CTRLA));
    CTRLA |= 0x08;
    UPDI_STCS(0x2, CTRLA);
    ack_disabled = true;
    uint8_t CTRLACheck = 0x0C;
    //clear read buffer
    uint8_t val;
    while(UPDI_Read(&val)) ;
    UPDI_ERROR_CHECK(UPDI_LDCS(0x2, &CTRLACheck));
    UPDI_ERROR_CHECK(CTRLACheck == CTRLA);
    return true;
}

bool UPDI_EnableAck()
{
    uint8_t CTRLA = 0x06;
    UPDI_ERROR_CHECK(UPDI_LDCS(0x2, &CTRLA));
    CTRLA &= !0x08;
    UPDI_STCS(0x2, CTRLA);
    ack_disabled = false;
    uint8_t CTRLACheck = 0x06;
    //clear read buffer
    uint8_t val;
    while(UPDI_Read(&val)) ;
    UPDI_ERROR_CHECK(UPDI_LDCS(0x2, &CTRLACheck));
    UPDI_ERROR_CHECK(CTRLACheck == CTRLA);
    return true;
}

bool UPDI_Enable(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin)
{
    // Get communication service from array
    if(uart_num >= UART_NUM_MAX || _uartCommServices[uart_num] == nullptr)
        return false;
    
    // Setup UPDI Communication Service
    if(_updiCommService != nullptr) {
        _updiCommService->UnRegisterReceiveCallBack(_updiCallbackId);
    }
    
    _updiCommService = _uartCommServices[uart_num];
    _updiCallbackId = _updiCommService->RegisterReceiveCallBack([](communication_send_callback_t send, const void *rx_buffer_ptr, size_t len) -> size_t {
        const uint8_t *rx_buffer = (const uint8_t *)rx_buffer_ptr;
        size_t index = 0;
        while(len > 0) {
            UPDI_rx_buffer[(UPDI_rx_buffer_index + UPDI_rx_buffer_length) % UPDI_RX_BUFFER_LENGTH] = rx_buffer[index];
            UPDI_rx_buffer_length++;
            len--;
            index++;
        }
        return index;
    });
    UPDI_rx_buffer_index = 0;
    UPDI_rx_buffer_length = 0;
    UPDI_uart_num = uart_num;

    // Setup UPDI UART and send 500ns pulse
    uart_config_t uart_config = {
        .baud_rate = 2000000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, 
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
    ESP_ERROR_CHECK(gpio_set_drive_capability(tx_pin, GPIO_DRIVE_CAP_0));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode(rx_pin, GPIO_FLOATING);
    UPDI_Write(0xFF);
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(1)); // wait for FF byte to finish before restore normal baudrate
    uart_config.baud_rate = 10000;
    uart_config.parity = UART_PARITY_EVEN;
    uart_config.stop_bits = UART_STOP_BITS_2;
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
    esp_rom_delay_us(196);

    //send NVMPROG Key to enable UPDI
    uint64_t key = UPDI_KEYNVMPROG;
    UPDI_KeySend64((uint8_t *)&key);
    UPDI_Break();
    UPDI_Break();
    UPDI_Idle();

    //clear read buffer. this could get filled by who knows what before we get to here
    uint8_t val;
    while(UPDI_Read(&val)) ;

    //remove guard time and verify
    ack_disabled = false;
    uint8_t CTRLA = 0x06;
    UPDI_STCS(0x2, CTRLA);
    UPDI_ERROR_CHECK(UPDI_LDCS(0x2, &CTRLA));
    UPDI_ERROR_CHECK(CTRLA == 0x06);

    //disable collision detection and verify
    uint8_t CTRLB = 0x08;
    UPDI_STCS(0x3, CTRLB);
    UPDI_ERROR_CHECK(UPDI_LDCS(0x3, &CTRLB));
    UPDI_ERROR_CHECK(CTRLB == 0x08);

    return true;
}

bool UPDI_Reset() 
{
    UPDI_STCS(0x8, 0x59);
    UPDI_STCS(0x8, 0x0);
    uint8_t ASI_SYS_STATUS = 0;
    uint32_t i = 0;
    UPDI_ERROR_CHECK(UPDI_LDCS(0xB, &ASI_SYS_STATUS));
    while((ASI_SYS_STATUS & 0xE) == 0 && i++<configTICK_RATE_HZ) 
    {
        vTaskDelay(1);
        UPDI_ERROR_CHECK(UPDI_LDCS(0xB, &ASI_SYS_STATUS));
    }
    return (ASI_SYS_STATUS & 0xE) != 0;
}

bool UPDI_EraseChip() 
{
    uint64_t key = UPDI_KEYNVMERASE;
    UPDI_KeySend64((uint8_t *)&key);
    esp_rom_delay_us(100);
    return UPDI_Reset();
}

bool UPDI_NVMProg() 
{
    uint64_t key = UPDI_KEYNVMPROG;
    UPDI_KeySend64((uint8_t *)&key);
    esp_rom_delay_us(100);
    UPDI_ERROR_CHECK(UPDI_Reset());

    uint8_t ASI_SYS_STATUS = 0;
    uint32_t i = 0;
    UPDI_ERROR_CHECK(UPDI_LDCS(0xB, &ASI_SYS_STATUS));
    while(!(ASI_SYS_STATUS & 0x8) && i++<configTICK_RATE_HZ/50) 
    {
        vTaskDelay(1);
        UPDI_ERROR_CHECK(UPDI_LDCS(0xB, &ASI_SYS_STATUS));
    }
    return ASI_SYS_STATUS & 0x8;
}

bool UPDI_WriteFlashOrEEPROM(uint32_t address, uint8_t *data, uint32_t length) 
{
    UPDI_ERROR_CHECK(UPDI_STSB(0x1000, 0x4));

    //program
    UPDI_DisableAck();
    uint32_t addressI = address;
    uint8_t *dataI = data;
    uint32_t lengthI = length;
    while(lengthI > 0) 
    {
        //wait for ready
        uint8_t NVMCTRL_STATUS = 0;
        uint32_t i = 0;
        UPDI_ERROR_CHECK(UPDI_LDSB(0x1002, &NVMCTRL_STATUS));
        while((NVMCTRL_STATUS & 0x3) != 0 && i++<1000) 
        {
            esp_rom_delay_us(100);
            UPDI_ERROR_CHECK(UPDI_LDSB(0x1002, &NVMCTRL_STATUS));
        }
        UPDI_ERROR_CHECK((NVMCTRL_STATUS & 0x3) == 0);

        uint32_t pagelength = 64 - (addressI % 64);
        if(lengthI < pagelength)
            pagelength = lengthI;
        
        UPDI_ERROR_CHECK(UPDI_ST(addressI, dataI, pagelength));

        UPDI_ERROR_CHECK(UPDI_STSB(0x1000, 0x3));

        addressI += pagelength;
        dataI += pagelength;
        lengthI -= pagelength;
    }
    UPDI_EnableAck();

    //verify
    uint8_t verifybuf[length];
    UPDI_ERROR_CHECK(UPDI_LD(address, verifybuf, length));
    addressI = address;
    if(memcmp(verifybuf, data, length) != 0)
    {
        ESP_LOGE("UPDI", "UPDI verification failed");
        for(int a = 0; a < length; a+=16) 
        {
            ESP_LOGE("UPDI", "%04X\t%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X| %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                (unsigned int)addressI,
                verifybuf[a],   verifybuf[a+1], verifybuf[a+2],  verifybuf[a+3],  verifybuf[a+4],  verifybuf[a+5],  verifybuf[a+6],  verifybuf[a+7],
                verifybuf[a+8], verifybuf[a+9], verifybuf[a+10], verifybuf[a+11], verifybuf[a+12], verifybuf[a+13], verifybuf[a+14], verifybuf[a+15],
                data[a],   data[a+1], data[a+2],  data[a+3],  data[a+4],  data[a+5],  data[a+6],  data[a+7],
                data[a+8], data[a+9], data[a+10], data[a+11], data[a+12], data[a+13], data[a+14], data[a+15]);
            addressI += 16;
        }
        return false;
    }

    // uint8_t verifybuf[512];
    // addressI = address;
    // dataI = data;
    // lengthI = length;
    // while(lengthI > 0) 
    // {
    //     uint32_t pagelength = 512 - (addressI % 512);
    //     if(lengthI < pagelength)
    //         pagelength = lengthI;

    //     UPDI_ERROR_CHECK(UPDI_LD(addressI, verifybuf, pagelength));

    //     if(memcmp(verifybuf, dataI, pagelength) != 0)
    //     {
    //         ESP_LOGE("UPDI", "UPDI verification failed @ %04X", (unsigned int)addressI);
    //         for(int a = 0; a < pagelength; a+=16) 
    //         {
    //             ESP_LOGE("UPDI", "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X| %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
    //                 verifybuf[a],   verifybuf[a+1], verifybuf[a+2],  verifybuf[a+3],  verifybuf[a+4],  verifybuf[a+5],  verifybuf[a+6],  verifybuf[a+7],
    //                 verifybuf[a+8], verifybuf[a+9], verifybuf[a+10], verifybuf[a+11], verifybuf[a+12], verifybuf[a+13], verifybuf[a+14], verifybuf[a+15],
    //                 dataI[a],   dataI[a+1], dataI[a+2],  dataI[a+3],  dataI[a+4],  dataI[a+5],  dataI[a+6],  dataI[a+7],
    //                 dataI[a+8], dataI[a+9], dataI[a+10], dataI[a+11], dataI[a+12], dataI[a+13], dataI[a+14], dataI[a+15]);
    //         }
    //         return false;
    //     }

    //     addressI += pagelength;
    //     dataI += pagelength;
    //     lengthI -= pagelength;
    // }

    return true;
}

bool UPDI_Program(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin, uint8_t *data, uint32_t length)
{
    ESP_LOGI("UPDI", "programming attiny");
    ESP_LOGI("UPDI", "enable");
    UPDI_ERROR_CHECK(UPDI_Enable(uart_num, tx_pin, rx_pin));
    ESP_LOGI("UPDI", "erase");
    if(!UPDI_EraseChip()) UPDI_ERROR_CHECK(UPDI_EraseChip());
    ESP_LOGI("UPDI", "nvm");
    if(!UPDI_NVMProg()) UPDI_ERROR_CHECK(UPDI_NVMProg());
    ESP_LOGI("UPDI", "flash");
    UPDI_ERROR_CHECK(UPDI_WriteFlashOrEEPROM(0x8000, data, length));
    ESP_LOGI("UPDI", "reset");
    UPDI_ERROR_CHECK(UPDI_Reset());
    ESP_LOGI("UPDI", "success!");
    return true;
}