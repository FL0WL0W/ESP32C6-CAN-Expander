#include <stdbool.h>
#include <stdint.h>

#ifndef UPDI_H
#define UPDI_H

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t UPDI_MaxWaitLoopCnt;

//set UPDI output pin
void UPDI_Set(bool val);

//get UPDI input pin
bool UPDI_Get();

//delay 1/10 bit length
void UPDI_LoopDelay();

//delay 1 bit length
void UPDI_BitDelay();

//delay 1/2 bit length
void UPDI_HalfBitDelay();

//write UPDI byte
bool UPDI_Write(uint8_t val);

//read UPDI byte
bool UPDI_Read(uint8_t *val);

//UPDI send Idle
void UPDI_Idle();

//UPDI send Break
void UPDI_Break();

//UPDI send Synch
void UPDI_Synch();

//UPDI Load Byte from Data Space
bool UPDI_LDSB(uint32_t address, uint8_t *val);

//UPDI Load Word from Data Space
bool UPDI_LDSW(uint32_t address, uint16_t *val);

//UPDI Store Byte to Data Space
bool UPDI_STSB(uint32_t address, uint8_t val);

//UPDI Store Word to Data Space
bool UPDI_STSW(uint32_t address, uint16_t val);

//UPDI Load Pointer as Byte
bool UPDI_LDPTRB(uint8_t *address);

//UPDI Load Pointer as Word
bool UPDI_LDPTRW(uint16_t *address);

//UPDI Load Pointer as Integer (3 bytes)
bool UPDI_LDPTRI(uint32_t *address);

//UPDI Load Byte
bool UPDI_LDB(uint8_t *val);

//UPDI Load Byte and Increment pointer
bool UPDI_LDBI(uint8_t *val);

//UPDI Load Word
bool UPDI_LDW(uint16_t *val);

//UPDI Load Word and Increment pointer
bool UPDI_LDWI(uint16_t *val);

//UPDI Store Pointer
bool UPDI_STPTR(uint32_t address);

//UPDI Store Byte
bool UPDI_STB(uint8_t val);

//UPDI Store Byte and Increment pointer
bool UPDI_STBI(uint8_t val);

//UPDI Store Word
bool UPDI_STW(uint16_t val);

//UPDI Store Word and Increment pointer
bool UPDI_STWI(uint16_t val);

//UPDI Load Data from Control and Status register space
bool UPDI_LDCS(uint8_t address, uint8_t *val);

//UPDI Store Data to Control and Status register space
void UPDI_STCS(uint8_t address, uint8_t val);

//UPDI Repeat the following command a number of times
void UPDI_Repeat(uint8_t times);

void UPDI_KeySend64(uint8_t *key);

void UPDI_KeySend128(uint8_t *key);

bool UPDI_KeyGet64(uint8_t *key);

bool UPDI_KeyGet128(uint8_t *key);

bool UPDI_LD(uint32_t address, uint8_t *val, uint16_t length);

bool UPDI_ST(uint32_t address, uint8_t *val, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif