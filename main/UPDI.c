#include "UPDI.h"

uint32_t UPDI_MaxWaitLoopCnt = 1500;
bool UPDI_wasLastWrite = false;

//set UPDI output pin
__attribute__((weak)) void UPDI_Set(bool val) {}

//get UPDI input pin
__attribute__((weak)) bool UPDI_Get() { return false; }

//delay 1/10 bit length
__attribute__((weak)) void UPDI_LoopDelay() {}

//delay 1 bit length
__attribute__((weak)) void UPDI_BitDelay() {}

//delay 1/2 bit length
__attribute__((weak)) void UPDI_HalfBitDelay() {}

//read UPDI byte
__attribute__((weak)) bool UPDI_Read(uint8_t *val)
{
    uint32_t i = 0;
    uint8_t b = 0;
    bool parity = 0;
    UPDI_LoopDelay();
    while(UPDI_Get() && i++ < UPDI_MaxWaitLoopCnt) UPDI_LoopDelay();
    if(i > UPDI_MaxWaitLoopCnt)
        return false;
    UPDI_HalfBitDelay();
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val = b;
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val |= b << 1;
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val |= b << 2;
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val |= b << 3;
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val |= b << 4;
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val |= b << 5;
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val |= b << 6;
    UPDI_BitDelay();
    if((b = UPDI_Get())) parity = !parity;
    *val |= b << 7;
    UPDI_BitDelay();
    parity = parity == UPDI_Get();
    UPDI_HalfBitDelay();
    return parity;
}

//write UPDI byte
__attribute__((weak)) bool UPDI_Write(uint8_t val)
{
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    uint8_t i = 1;
    UPDI_Set(0);
    UPDI_BitDelay();
    bool parity = 0;
    for(i = 0; i < 8; i++)
    {
        const bool bit = val & (0x1 << i);
        if(bit) parity = !parity;
        UPDI_Set(bit);
        UPDI_BitDelay();
    }
    UPDI_Set(parity);
    UPDI_BitDelay();
    UPDI_Get();
    return true;
}

//UPDI send Idle
__attribute__((weak)) void UPDI_Idle()
{
    UPDI_Get();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
}

//UPDI send Break
__attribute__((weak)) void UPDI_Break()
{
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_Set(0);
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
    UPDI_BitDelay();
}

//read and verify ack. Attribute weak for different implmentations where ACK can be disabled
__attribute__((weak)) bool UPDI_ReadAck()
{
    uint8_t ack;
    if(!UPDI_Read(&ack)) return false;
    return ack == 0x40;
}

//UPDI send Synch
void UPDI_Synch()
{
    UPDI_Write(0x55);
}

//UPDI Load Byte from Data Space
bool UPDI_LDSB(uint32_t address, uint8_t *val) 
{
    UPDI_Synch();
    if(address < 256)
    {
        UPDI_Write(0x00);
        UPDI_Write(address);
    } 
    else if (address < 65536)
    {
        UPDI_Write(0x04);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
    }
    else
    {
        UPDI_Write(0x08);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
        UPDI_Write(address >> 16);
    }
    if(!UPDI_Read(val)) return false;
    return true;
}

//UPDI Load Word from Data Space
bool UPDI_LDSW(uint32_t address, uint16_t *val) 
{
    UPDI_Synch();
    if(address < 256)
    {
        UPDI_Write(0x01);
        UPDI_Write(address);
    } 
    else if (address < 65536)
    {
        UPDI_Write(0x05);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
    }
    else
    {
        UPDI_Write(0x09);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
        UPDI_Write(address >> 16);
    }
    if(!UPDI_Read(((uint8_t *)val))) return false;
    if(!UPDI_Read(((uint8_t *)val) + 1)) return false;
    return true;
}

//UPDI Store Byte to Data Space
bool UPDI_STSB(uint32_t address, uint8_t val) 
{
    UPDI_Synch();
    if(address < 256)
    {
        UPDI_Write(0x40);
        UPDI_Write(address);
    } 
    else if (address < 65536)
    {
        UPDI_Write(0x44);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
    }
    else
    {
        UPDI_Write(0x48);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
        UPDI_Write(address >> 16);
    }
    if(!UPDI_ReadAck()) return false;
    UPDI_Write(val);
    if(!UPDI_ReadAck()) return false;
    return true;
}

//UPDI Store Word to Data Space
bool UPDI_STSW(uint32_t address, uint16_t val) 
{
    UPDI_Synch();
    if(address < 256)
    {
        UPDI_Write(0x41);
        UPDI_Write(address);
    } 
    else if (address < 65536)
    {
        UPDI_Write(0x45);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
    }
    else
    {
        UPDI_Write(0x49);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
        UPDI_Write(address >> 16);
    }
    if(!UPDI_ReadAck()) return false;
    UPDI_Write(val);
    UPDI_Write(val >> 8);
    if(!UPDI_ReadAck()) return false;
    return true;
}

//UPDI Load Pointer as Byte
bool UPDI_LDPTRB(uint8_t *address) 
{
    UPDI_Synch();
    UPDI_Write(0x28);
    if(!UPDI_Read(address)) return false;
    return true;
}

//UPDI Load Pointer as Word
bool UPDI_LDPTRW(uint16_t *address) 
{
    UPDI_Synch();
    UPDI_Write(0x29);
    if(!UPDI_Read(((uint8_t *)address))) return false;
    if(!UPDI_Read(((uint8_t *)address) + 1)) return false;
    return true;
}

//UPDI Load Pointer as Integer (3 bytes)
bool UPDI_LDPTRI(uint32_t *address) 
{
    UPDI_Synch();
    UPDI_Write(0x2A);
    if(!UPDI_Read(((uint8_t *)address))) return false;
    if(!UPDI_Read(((uint8_t *)address) + 1)) return false;
    if(!UPDI_Read(((uint8_t *)address) + 2)) return false;
    return true;
}

//UPDI Load Byte
bool UPDI_LDB(uint8_t *val) 
{
    UPDI_Synch();
    UPDI_Write(0x20);
    if(!UPDI_Read(val)) return false;
    return true;
}

//UPDI Load Byte and Increment pointer
bool UPDI_LDBI(uint8_t *val) 
{
    UPDI_Synch();
    UPDI_Write(0x24);
    if(!UPDI_Read(val)) return false;
    return true;
}

//UPDI Load Word
bool UPDI_LDW(uint16_t *val) 
{
    UPDI_Synch();
    UPDI_Write(0x21);
    if(!UPDI_Read(((uint8_t *)val))) return false;
    if(!UPDI_Read(((uint8_t *)val) + 1)) return false;
    return true;
}

//UPDI Load Word and Increment pointer
bool UPDI_LDWI(uint16_t *val) 
{
    UPDI_Synch();
    UPDI_Write(0x25);
    if(!UPDI_Read(((uint8_t *)val))) return false;
    if(!UPDI_Read(((uint8_t *)val) + 1)) return false;
    return true;
}

//UPDI Store Pointer
bool UPDI_STPTR(uint32_t address)
{
    UPDI_Synch();
    if(address < 256)
    {
        UPDI_Write(0x68);
        UPDI_Write(address);
    } 
    else if (address < 65536)
    {
        UPDI_Write(0x69);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
    }
    else
    {
        UPDI_Write(0x6A);
        UPDI_Write(address);
        UPDI_Write(address >> 8);
        UPDI_Write(address >> 16);
    }
    if(!UPDI_ReadAck()) return false;
    return true;
}

//UPDI Store Byte
bool UPDI_STB(uint8_t val)
{
    UPDI_Synch();
    UPDI_Write(0x60);
    UPDI_Write(val);
    if(!UPDI_ReadAck()) return false;
    return true;
}

//UPDI Store Byte and Increment pointer
bool UPDI_STBI(uint8_t val)
{
    UPDI_Synch();
    UPDI_Write(0x64);
    UPDI_Write(val);
    if(!UPDI_ReadAck()) return false;
    return true;
}

//UPDI Store Word
bool UPDI_STW(uint16_t val)
{
    UPDI_Synch();
    UPDI_Write(0x61);
    UPDI_Write(val);
    UPDI_Write(val >> 8);
    if(!UPDI_ReadAck()) return false;
    return true;
}

//UPDI Store Word and Increment pointer
bool UPDI_STWI(uint16_t val)
{
    UPDI_Synch();
    UPDI_Write(0x65);
    UPDI_Write(val);
    UPDI_Write(val >> 8);
    if(!UPDI_ReadAck()) return false;
    return true;
}

//UPDI Load Data from Control and Status register space
bool UPDI_LDCS(uint8_t address, uint8_t *val) 
{
    UPDI_Synch();
    UPDI_Write(0x80 | (address & 0xF));
    if(!UPDI_Read(val)) return false;
    return true;
}

//UPDI Store Data to Control and Status register space
void UPDI_STCS(uint8_t address, uint8_t val) 
{
    UPDI_Synch();
    UPDI_Write(0xC0 | (address & 0xF));
    UPDI_Write(val);
}

//UPDI Repeat the following command a number of times
void UPDI_Repeat(uint8_t times)
{
    UPDI_Synch();
    UPDI_Write(0xA0);
    UPDI_Write(times);
}

void UPDI_KeySend64(uint8_t *key)
{
    UPDI_Synch();
    UPDI_Write(0xE0);
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
}

void UPDI_KeySend128(uint8_t *key)
{
    UPDI_Synch();
    UPDI_Write(0xE1);
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
    UPDI_Write(*(key++));
}

bool UPDI_KeyGet64(uint8_t *key)
{
    UPDI_Synch();
    UPDI_Write(0xE4);
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    return true;
}

bool UPDI_KeyGet128(uint8_t *key)
{
    UPDI_Synch();
    UPDI_Write(0xE5);
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    if(!UPDI_Read(key++)) return false;
    return true;
}

bool UPDI_LD(uint32_t address, uint8_t *val, uint16_t length)
{
    if(length == 0) return false;
    while(length > 513)
    {
        if(!UPDI_LD(address, val, 512)) return false;
        address += 512;
        val += 512;
        length -= 512;
    }
    if(length == 0) return true;
    if(length == 1) return UPDI_LDSB(address, val);
    if(length == 2) return UPDI_LDSW(address, ((uint16_t *)val));
    if(!UPDI_STPTR(address)) return false;
    UPDI_Repeat((length/2)-1);
    if(!UPDI_LDWI((uint16_t *)val)) return false;
    val+=2;
    length-=2;
    while(length/2 > 0)
    {
        if(!UPDI_Read(val++)) return false;
        if(!UPDI_Read(val++)) return false;
        length-=2;
    }
    if(length > 0)
    {
        if(!UPDI_LDB(val)) return false;
    }
    return true;
}

bool UPDI_ST(uint32_t address, uint8_t *val, uint16_t length)
{
    if(length == 0) return false;
    while(length > 513)
    {
        if(!UPDI_ST(address, val, 512)) return false;
        address += 512;
        val += 512;
        length -= 512;
    }
    if(length == 0) return true;
    if(length == 1) return UPDI_STSB(address, *val);
    if(length == 2) return UPDI_STSW(address, *((uint16_t *)val));
    if(!UPDI_STPTR(address)) return false;
    UPDI_Repeat((length/2)-1);
    if(!UPDI_STWI(*((uint16_t *)val))) return false;
    val+=2;
    length-=2;
    while(length/2 > 0)
    {
        UPDI_Write(*(val++));
        UPDI_Write(*(val++));
        if(!UPDI_ReadAck()) return false;
        length-=2;
    }
    if(length > 0)
    {
        if(!UPDI_STB(*val)) return false;
    }
    return true;
}