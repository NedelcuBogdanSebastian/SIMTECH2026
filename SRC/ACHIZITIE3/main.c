/*************************************************************************************
    Copyright (C) 2024 Nedelcu Bogdan Sebastian
    This code is free software: you can redistribute it and/or modify it 
    under the following conditions:
    1. The use, distribution, and modification of this file are permitted for any 
       purpose, provided that the following conditions are met:
    2. Any redistribution or modification of this file must retain the original 
       copyright notice, this list of conditions, and the following attribution:
       "Original work by Nedelcu Bogdan Sebastian."
    3. The original author provides no warranty regarding the functionality or fitness 
       of this software for any particular purpose. Use it at your own risk.
    By using this software, you agree to retain the name of the original author in any 
    derivative works or distributions.
    ------------------------------------------------------------------------
    This code is provided as-is, without any express or implied warranties.
**************************************************************************************/

/*
    PB1 - HEARTBEAT signal, down for 500 Us, then up.
    PB2 - WeAct BluePill LED
    PB5 - DS18B20 digital thermometer for ambient temperature.
    PB10(TX), PB11(RX) - Connection with modules is over USART3.

    The CRC16 is implemented as a look-up table.

    Relays Connector Configuration
    ==============================

    GND | GND
     A7 | A6     (K1 | K2)
     A5 | A4     (K3 | K4)
     A3 | A2     (K5 | K6)
     A1 | *A0    (K7 | *K8) <- PA0 NU ESTE FOLOSIT AICI
     !!!!!!!!!!!!! ATENTIE PA0 ESTE BUTONUL !!!!!!!!!!!!

    Module Configuration (stored in FLASH, programmed via Modbus)
    =============================================================
    Master writes module types into HR[71..89], then writes 1235 to HR[91].
    HR[71] = module 1 type
    HR[72] = module 2 type
    ...
    HR[89] = module 19 type  (write 0 to mark end of list)

    Module types:
      1 = DIGITAL WITH CRS  (sends /CAn\)
      2 = DIGITAL NO CRS    (sends /DAn\)
      3 = 4-20mA            (sends /IAn\)
      4 = PT100 MAX31865    (sends /TAn\)
      0 = end of list

    Address suffix n is hex (0..F), counting modules of the same type.
    Example: 3x type 1, 2x type 4 sends /CA0\,/CA1\,/CA2\,/TA0\,/TA1\

    On trigger (HR[91]=1235): saves config to FLASH, confirms with HR[92]=5321.
    On error:                 HR[92]=1111 (Empty list, Nothing to save)
                              HR[92]=2222 (Flash erase error)
                              HR[92]=3333 (Flash write error)
							  HR[92]=4444 (Invalid module type) 
							  
    On startup:               loads config from FLASH automatically.
    Debug:                    HR[93]=moduleCount after save.

    FLASH layout (last 1KB page):
      64KB  device: 0x0800FC00
      128KB device: 0x0801FC00
      [0]     = moduleCount (1..19)
      [1..19] = module types (1..4)
*/

#include "string.h"
#include "stm32f10x.h"
#include "main.h"
#include "mbutils.h"
#include "mb.h"

#define LED_ON       GPIOB->BRR = GPIO_Pin_2
#define LED_OFF      GPIOB->BSRR = GPIO_Pin_2
#define LED_TOGGLE   GPIOB->ODR ^= GPIO_Pin_2

#define MAX_REG      70

// dec -> hex, hex -> dec
#define TO_HEX(i) (i <= 9 ? '0' + i : 'A' - 10 + i)
#define TO_DEC(i) (i <= '9'? i - '0': i - 'A' + 10)
#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))

volatile uint32_t TimingDelay;

void Delay (volatile uint32_t nTime) {
    TimingDelay = nTime;
    while(TimingDelay != 0);
}
void USART3_Putch (unsigned char ch);
void USART3_Print (char s[]);
void USART3_Init (void);

volatile uint8_t Modbus_Request_Flag;

u16 usRegHoldingBuf[100+1]; // 0..99 holding registers
u8  usRegCoilBuf[64/8+1];   // 0..64  coils

void writeCoil (uint8_t coil_index, uint8_t state) {
    uint8_t coil_offset=coil_index/8;
    if (state == 1)
        usRegCoilBuf[coil_offset] |= (1<<(coil_index%8));
    else usRegCoilBuf[coil_offset] &= ~(1<<(coil_index%8));
}

uint8_t getCoil (uint8_t coil_index) {
    uint8_t coil_byte=usRegCoilBuf[coil_index/8];
    if (coil_byte & (1<<(coil_index%8))) return 1;
    else return 0;
}

void writeHoldingRegister (uint8_t reg_index, uint16_t reg_val) {
    usRegHoldingBuf[reg_index] = reg_val;
}

uint16_t readHoldingRegister (uint8_t reg_index) {
    return usRegHoldingBuf[reg_index];
}

/*
CRC Generation from the MODBUS Specification V1.02:
1. Load a 16bit register with FFFF hex (all 1's). Call this the CRC register.
2. Exclusive OR the first 8bit byte of the message with the low order byte of the 16 bit CRC register, putting the result in the
CRC register.
3. Shift the CRC register one bit to the right (toward the LSB), zero filling the MSB.
Extract and examine the LSB.
4. (If the LSB was 0): Repeat Step 3 (another shift).
(If the LSB was 1): Exclusive OR the CRC register with the polynomial value 0xA001 (1010 0000 0000 0001).
5. Repeat Steps 3 and 4 until 8 shifts have been performed. When this is done, a complete 8bit byte will have been processed.
6. Repeat Steps 2 through 5 for the next 8bit byte of the message. Continue doing this until all bytes have been processed.
7. The final content of the CRC register is the CRC value.
8. When the CRC is placed into the message, its upper and lower bytes must be swapped.

uint16_t crc16_update(uint16_t crc, uint8_t a) {
    uint8_t i;
    crc ^= a;
    for (i = 0; i < 8; ++i) {
        if (crc & 1) crc = (crc >> 1) ^ 0xA001;
        else crc = (crc >> 1);
    }
    return crc;
}

 Below we use look-up table for speed
 */
#define POLY 0xA001  // Polynomial used in CRC-16

uint16_t crc16_table[256];

// Generate the CRC-16 lookup table
void generateCRC16Table (void) {
    uint16_t crc;
    for (int i = 0; i < 256; i++) {
        crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ POLY;
            } else {
                crc = crc >> 1;
            }
        }
        crc16_table[i] = crc;
    }
}

uint16_t crc16_update (uint16_t crc, uint8_t data) {
    uint8_t tableIndex;
    tableIndex = (uint8_t)(crc ^ data);
    crc = (crc >> 8) ^ crc16_table[tableIndex];
    return crc;
}

// Initialise DWT counter
volatile uint32_t *DWT_CONTROL = (volatile uint32_t *)0xE0001000;
volatile uint32_t *DWT_CYCCNT  = (volatile uint32_t *)0xE0001004;
volatile uint32_t *SCB_DEMCR   = (volatile uint32_t *)0xE000EDFC;

void DWT_Enable (void) {
    *SCB_DEMCR = *SCB_DEMCR | 0x01000000;
    *DWT_CYCCNT = 0;
    *DWT_CONTROL = *DWT_CONTROL | 1;
}

void DWT_Delay (uint32_t us) {  // Microseconds
    uint32_t delayTicks = us * (SystemCoreClock / 1000000);
    uint32_t startTick = *DWT_CYCCNT;
    while (*DWT_CYCCNT - startTick < delayTicks);
}

uint8_t DS18B20_Init (void) {
    GPIOB->BRR = GPIO_Pin_5;   // Pull bus down for 500 Us (min. 480 Us)
    DWT_Delay(500);
    GPIOB->BSRR = GPIO_Pin_5;  // Release bus
    DWT_Delay(70);

    if (!(GPIOB->IDR & GPIO_Pin_5)) {
        DWT_Delay(430);
        return 0;
    } else {
        DWT_Delay(430);
        return 1;
    }
}

void DS18B20_Write (uint8_t data) {
    for (int i=0; i<8; i++) {
        if ((data & (1<<i)) != 0) {
            GPIOB->BRR = GPIO_Pin_5;
            DWT_Delay(15);
            GPIOB->BSRR = GPIO_Pin_5;
            DWT_Delay(60);
        } else {
            GPIOB->BRR = GPIO_Pin_5;
            DWT_Delay(60);
            GPIOB->BSRR = GPIO_Pin_5;
            DWT_Delay(5);
        }
    }
}

uint8_t DS18B20_Read (void) {
    uint8_t value=0;
    for (int i=0;i<8;i++) {
        GPIOB->BRR = GPIO_Pin_5;
        DWT_Delay(5);
        GPIOB->BSRR = GPIO_Pin_5;
        DWT_Delay(5);
        if (GPIOB->IDR & GPIO_Pin_5) {
            value |= 1<<i;
        }
        DWT_Delay(55);
    }
    return value;
}

uint16_t DS18B20_GetTemperature (void) {
    uint8_t check=2, temp_l=0, temp_h=0;

    check = DS18B20_Init();
    if (check == 1) return 5555;

    DS18B20_Write(0xCC);  // Skip ROM
    DS18B20_Write(0x44);  // Convert T

    // This hack works, there is not needed an 800ms delay !!!
    // Tested and proved
    DWT_Delay(800);

    DS18B20_Init();
    DS18B20_Write(0xCC);  // Skip ROM
    DS18B20_Write(0xBE);  // Read Scratchpad command

    temp_l = DS18B20_Read();
    temp_h = DS18B20_Read();
    return (temp_h<<8)|temp_l;
}

void HEARTBEAT(void) {
    GPIOB->BRR = GPIO_Pin_1;   // Pull heart beat pin down for 500 us
    DWT_Delay(500);
    GPIOB->BSRR = GPIO_Pin_1;  // Release heart beat pin
}

/*************************************************************
 *  Module configuration - persistent storage in FLASH
 *
 *  FLASH layout (last 1KB page):
 *    64KB  device: 0x0800FC00
 *    128KB device: 0x0801FC00
 *
 *    [0]     = moduleCount (1..15)
 *    [1..15] = module types (1..4)
 *************************************************************/

#define CONFIG_FLASH_PAGE_ADDR  0x0800FC00  // Change to 0x0801FC00 for 128KB device
#define CONFIG_MAX_MODULES      19

uint8_t moduleList[CONFIG_MAX_MODULES];  // Runtime module type list
uint8_t moduleCount = 0;                 // Total number of active modules


// Read HR[71..89], validate types, count entries, save to FLASH
// Returns: 0 on success, 1 if empty, 2 on erase failure, 3 on write failure, 4 on invalid module type
uint8_t Config_SaveToFlash (void) {
    uint8_t  i;
    uint16_t data[CONFIG_MAX_MODULES + 1];  // count + up to 19 types
    uint16_t current_type;

    // Validate and count entries in HR[71..89]
    uint8_t count = 0;
    for (i = 0; i < CONFIG_MAX_MODULES; i++) {
        current_type = readHoldingRegister(71 + i);

        if (current_type == 0) {
            break; // 0 marks the end of the module sequence configuration
        }

        // CRUCIAL GUARD: Reject save if the user passes any invalid module configuration type
        if (current_type < 1 || current_type > 4) {
            return 4; // Return validation error code
        }

        count++;
    }
    if (count == 0) return 1;  // Nothing to save

    // Build flash data layout: [0]=count, [1..count]=types
    data[0] = count;
    for (i = 0; i < count; i++) {
        data[i+1] = readHoldingRegister(71 + i);
    }

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);

    if (FLASH_ErasePage(CONFIG_FLASH_PAGE_ADDR) != FLASH_COMPLETE) {
        FLASH_Lock();
        return 2;  // Erase failed
    }

    for (i = 0; i <= count; i++) {  // Write count+1 halfwords
        if (FLASH_ProgramHalfWord(CONFIG_FLASH_PAGE_ADDR + i*2, data[i]) != FLASH_COMPLETE) {
            FLASH_Lock();
            return 3;  // Write failed
        }
    }

    FLASH_Lock();
    return 0;  // Success
}


// Load config from FLASH into moduleList[] and moduleCount
// Returns moduleCount if valid config found, 0 if no valid config
uint8_t Config_LoadFromFlash (void) {
    uint16_t *flash = (uint16_t *)CONFIG_FLASH_PAGE_ADDR;
    uint8_t  i, count;

    count = (uint8_t)flash[0];
    if (count == 0 || count > CONFIG_MAX_MODULES) return 0;  // Invalid or blank flash

    moduleCount = count;
    for (i = 0; i < moduleCount; i++) {
        moduleList[i] = (uint8_t)flash[i+1];
    }

    return moduleCount;  // Success
}

int main(void) {
    GPIO_InitTypeDef GPIO_InitStructure;

    uint8_t i, j, k, ch, bit;
    uint16_t CRC16_val, val;
    uint16_t temperature;
    char RECEIVED_DATA[30] = {0};
    uint8_t data_gathering_active_flag, character_counter;
    uint8_t module_counter;
    uint8_t data_size, error_flag;
    uint8_t co_index, hr_index;
    uint8_t module_type;
    uint16_t pt100_temperature;
    uint16_t CRS_counter;

    if(SysTick_Config(72000)) { // 1 Ms interrupt 72MHz/72000 = 1000
        while (1);
    }

    /************************************************************
    *   Enable the use of internal DWT for counting
    *************************************************************/
    DWT_Enable();

    /************************************************************
    *   Initialise led on PB2 (WEACT BluePill is used)
    *************************************************************/
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB , ENABLE);
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /************************************************************
    *   Initialise PB1 as heart beat bus for RESETTER module
    *************************************************************/
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB , ENABLE);
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    HEARTBEAT();

    /************************************************************
    * Initialise relay pins on GPIOA (WeAct BluePill)
    *************************************************************/
    RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOA , ENABLE);
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7 | GPIO_Pin_6 | GPIO_Pin_5 | GPIO_Pin_4 |
                                  GPIO_Pin_3 | GPIO_Pin_2 | GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // All relays OFF
    GPIOA->BSRR = GPIO_Pin_7 | GPIO_Pin_6 | GPIO_Pin_5 | GPIO_Pin_4 |
                  GPIO_Pin_3 | GPIO_Pin_2 | GPIO_Pin_1;

    /************************************************************
    *   Initialise PB5 as control pin for DS18B20
    *************************************************************/
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB , ENABLE);
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /************************************************************
    *   Initialise USART3 peripheral
    *************************************************************/
    USART3_Init();

    /************************************************************
    *   Generate CRC16 look-up table
    *************************************************************/
    generateCRC16Table();

    /************************************************************
    *   Initialise protocol stack in RTU mode
    *************************************************************/
    // MB_RTU, Device ID: 2, USART port: 1
    eMBInit(MB_RTU, 2, 1, 19200, MB_PAR_NONE);
    // Enable the Modbus Protocol Stack.
    eMBEnable();

    /************************************************************
    *   Load module configuration from FLASH
    *************************************************************/
    moduleCount = Config_LoadFromFlash();
    writeHoldingRegister(93, moduleCount);  // Debug: module count

    for (i = 0; i < moduleCount; i++) {
    	writeHoldingRegister(71+i, moduleList[i]);
    }


    /************************************************************
    *   Config IWDG watch dog
    *************************************************************/
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_32);   // 32KHz(LSI) / 32 = 1KHz
    IWDG_SetReload(500);                    // Timeout ~500ms
    IWDG_ReloadCounter();
    IWDG_Enable();

    co_index = 1;  // Coils start at zero in RMMS
    hr_index = 2;  // HR[1] is ambient temperature, data starts at HR[2]
    error_flag = 0;
    module_counter = 0;
    character_counter = 0;
    data_gathering_active_flag = 0;  // Start in Send ID mode

    while(1) {
        // Reload IWDG counter
        IWDG_ReloadCounter();

        eMBPoll();

        // We have 2 modes:   1. Send ID        ( data_gathering_active_flag = 0 )
        //                    2. Data gathering ( data_gathering_active_flag = 1 )

        // Mode 2: Data gathering / Listening phase
        if (data_gathering_active_flag == 1) {

        	// Test if we have received data and store to data buffer
			if (USART3->SR & USART_FLAG_RXNE) {
				ch = USART3->DR & 0xff;

				if (character_counter < 29) {
					RECEIVED_DATA[character_counter] = ch;
				}

				// Prevent counter from climbing out of buffer bounds
				if (character_counter < sizeof(RECEIVED_DATA) - 1) {
					character_counter++;
				}
			}

            if (TimingDelay == 0) { // 100ms window has expired

                HEARTBEAT();

                //  DIGITAL WITH CRS = 1, length of data is 11
                //  3 bytes DATA + 4 bytes CRS COUNTER + 4 bytes CRC
                //  11 values as 1 bit each + 16 bit counter

                //  DIGITAL NO CRS = 2, length of data is 7
                //  3 bytes DATA + 4 bytes CRC
                //  12 values as 1 bit each

                //  4-20mA = 3, length of data is 22
                //  18 bytes DATA + 4 bytes CRC
                //  6 values as 3 bytes each

                //  PT100RTD = 4, (with MAX31865), length of data is 12
                //  8 bytes DATA + 4 bytes CRC
                //  4 values as 2 bytes each

                data_size = 5;  // Safe fallback - just feeds the CRC check
                if (module_type == 1) data_size = 11;  // DGT_CRS
                if (module_type == 2) data_size = 7;   // DGT
                if (module_type == 3) data_size = 22;  // 4-20mA
                if (module_type == 4) data_size = 12;  // PT100

                data_gathering_active_flag = 0;
                character_counter = 0;
                error_flag = 0;

                // Reset CRC
                CRC16_val = 0xFFFF;

                // Compute CRC of data
                for (i = 1; i <= data_size-4; i++) {
                    CRC16_val = crc16_update(CRC16_val, RECEIVED_DATA[i]);
                }

                // Test if computed CRC = received CRC, set error flag if mismatch
                if (TO_HEX(((CRC16_val & 0xF000) >> 12)) != RECEIVED_DATA[data_size-3]) error_flag = 1;
                if (TO_HEX(((CRC16_val & 0x0F00) >> 8))  != RECEIVED_DATA[data_size-2]) error_flag = 1;
                if (TO_HEX(((CRC16_val & 0x00F0) >> 4))  != RECEIVED_DATA[data_size-1]) error_flag = 1;
                if (TO_HEX(((CRC16_val & 0x000F)))       != RECEIVED_DATA[data_size])   error_flag = 1;

                // Even on CRC error we must still increment indexes
                // so we don't override another module's registers
                if (error_flag == 1) {

                    if (module_type == 1 || module_type == 2) {
                        for (i = 1; i < 4; i++) {
                            for (j = 0; j < 4; j++) {
                                if ((module_type == 1) && (i == 3) && (j == 3)) continue;
                                writeCoil(co_index, 0);
                                if (co_index < 63) co_index++;
                            }
                        }
                        if (module_type == 1) {
                            writeHoldingRegister(hr_index, 0);
                            if (hr_index < MAX_REG) hr_index++;
                        }
                    }

                    if (module_type == 3) {
                        for (i = 0; i < 6; i++) {
                            writeHoldingRegister(hr_index, 0);
                            if (hr_index < MAX_REG) hr_index++;
                        }
                    }

                    if (module_type == 4) {
                        for (i = 0; i < 4; i++) {
                            writeHoldingRegister(hr_index, 0);
                            if (hr_index < MAX_REG) hr_index++;
                        }
                    }
                }

                // If CRC check is OK proceed analysing data and storing to Modbus stack
                if (error_flag == 0) {

                    if (module_type == 1 || module_type == 2) {
                        for (i = 1; i < 4; i++) {
                            val = TO_DEC(RECEIVED_DATA[i]);
                            for (j = 0; j < 4; j++) {
                                if ((module_type == 1) && (i == 3) && (j == 3)) continue;
                                bit = (val >> (3 - j)) & 0x01;
                                writeCoil(co_index, bit);
                                if (co_index < 63) co_index++;
                            }
                        }
                        if (module_type == 1) {
                            CRS_counter = (TO_DEC(RECEIVED_DATA[4]) << 12) |
                                          (TO_DEC(RECEIVED_DATA[5]) << 8)  |
                                          (TO_DEC(RECEIVED_DATA[6]) << 4)  |
                                          (TO_DEC(RECEIVED_DATA[7]));
                            writeHoldingRegister(hr_index, CRS_counter);
                            if (hr_index < MAX_REG) hr_index++;
                        }
                    }

                    if (module_type == 3) {
                        k = 1;
                        for (i = 0; i < 6; i++) {
                            val = (TO_DEC(RECEIVED_DATA[k]) & 0xF);
                            val = (val << 4) | (TO_DEC(RECEIVED_DATA[k+1]) & 0xF);
                            val = (val << 4) | (TO_DEC(RECEIVED_DATA[k+2]) & 0xF);
                            k += 3;
                            writeHoldingRegister(hr_index, val);
                            if (hr_index < MAX_REG) hr_index++;
                        }
                    }

                    if (module_type == 4) {
                        k = 1;
                        for (i = 0; i < 4; i++) {
                            pt100_temperature = (TO_DEC(RECEIVED_DATA[k]) & 0xF);
                            pt100_temperature = (pt100_temperature << 4) | (TO_DEC(RECEIVED_DATA[k+1]) & 0xF);
                            writeHoldingRegister(hr_index, pt100_temperature);
                            if (hr_index < MAX_REG) hr_index++;
                            k += 2;
                        }
                    }
                }

                // Advance to next module only after its 100ms window has fully expired
                if (moduleCount > 0) {
                    module_counter++;
                }
            }

        } else {

            // Clear received data buffer
            memset(RECEIVED_DATA, 0, 30);

            // Send ID string to current module based on its type from moduleList
            // Address suffix is hex (0..F), counting same-type modules before this one
            if (module_counter < moduleCount) {
                module_type = moduleList[module_counter];

                // Count how many modules of this same type appear before this one
                uint8_t type_index = 0;
                for (i = 0; i < module_counter; i++) {
                    if (moduleList[i] == module_type) type_index++;
                }

                char type_letter;
                switch (module_type) {
                    case 1: type_letter = 'C'; break;  // DIGITAL WITH CRS
                    case 2: type_letter = 'D'; break;  // DIGITAL NO CRS
                    case 3: type_letter = 'I'; break;  // 4-20mA
                    case 4: type_letter = 'T'; break;  // PT100
                    default: type_letter = 'X'; break;
                }

                char id_str[6];
                id_str[0] = '/';
                id_str[1] = type_letter;
                id_str[2] = 'A';
                id_str[3] = TO_HEX(type_index);  // hex suffix 0..F, supports up to 15 of each type
                id_str[4] = '\\';
                id_str[5] = '\0';

                USART3_Print(id_str);

                // Switch to data gathering mode and start 100ms window
                data_gathering_active_flag = 1;
                TimingDelay = 100;

            } else {
                // End of round: all configured modules have been polled
                // Update relays, read temperature, handle programming trigger
                LED_TOGGLE;
                HEARTBEAT();
                IWDG_ReloadCounter();

                // Feature to program modules order
                // Master writes module types into HR[71..90] (RMMS 70..89)
                // then writes 1235 to HR[91] (RMMS 90)
                if (readHoldingRegister(91) == 1235) {
                    writeHoldingRegister(91, 0);
                    uint8_t res = Config_SaveToFlash();
                    if (res == 0) {
                        HEARTBEAT();
                        IWDG_ReloadCounter();
                        moduleCount = Config_LoadFromFlash();
                        writeHoldingRegister(92, 5321);         // Confirm success
                        writeHoldingRegister(93, moduleCount);  // Debug: module count

                        // Reload module type mapping to Modbus, when we changed something !!!
						for (i = 0; i < CONFIG_MAX_MODULES; i++) {
							writeHoldingRegister(71 + i, 0);
						}
						for (i = 0; i < moduleCount; i++) {
							writeHoldingRegister(71 + i, moduleList[i]);
						}
                    } else {
                        if (res == 1) writeHoldingRegister(92, 1111);  // Empty list, Nothing to save
                        if (res == 2) writeHoldingRegister(92, 2222);  // Flash erase error
                        if (res == 3) writeHoldingRegister(92, 3333);  // Flash write error
                        if (res == 4) writeHoldingRegister(92, 4444);  // Invalid module type
                    }
                }

                // Only 7 relays - PA0 is the button on the WeAct BluePill !!!!
                if (getCoil(51) == 0) GPIOA->BRR = GPIO_Pin_7; else GPIOA->BSRR = GPIO_Pin_7;
                if (getCoil(52) == 0) GPIOA->BRR = GPIO_Pin_6; else GPIOA->BSRR = GPIO_Pin_6;
                if (getCoil(53) == 0) GPIOA->BRR = GPIO_Pin_5; else GPIOA->BSRR = GPIO_Pin_5;
                if (getCoil(54) == 0) GPIOA->BRR = GPIO_Pin_4; else GPIOA->BSRR = GPIO_Pin_4;
                if (getCoil(55) == 0) GPIOA->BRR = GPIO_Pin_3; else GPIOA->BSRR = GPIO_Pin_3;
                if (getCoil(56) == 0) GPIOA->BRR = GPIO_Pin_2; else GPIOA->BSRR = GPIO_Pin_2;
                if (getCoil(57) == 0) GPIOA->BRR = GPIO_Pin_1; else GPIOA->BSRR = GPIO_Pin_1;

                // Reset cycle counters for next round
                module_counter = 0;
                co_index = 1;
                hr_index = 2;  // HR[1] is the ambient temperature

                // Temperature reading - always runs, address fixed at HR[1]
                temperature = DS18B20_GetTemperature();
                if (temperature == 5555) {
                    writeHoldingRegister(1, 0);
                } else {
                    // Cast to signed first for correct two's complement handling
                    int16_t temp_c = (int16_t)temperature >> 4;  // 1/16C units -> degrees
                    uint16_t result = (uint16_t)(temp_c + 100);  // Add 100 offset for sign
                    writeHoldingRegister(1, result);
                }

                // No-config idle guard: non-blocking 30ms pad keeps Modbus and watchdog alive
                // module_type = 0 safely bypasses all module data parsing blocks
                if (moduleCount == 0) {
                    TimingDelay = 30;
                    data_gathering_active_flag = 1;
                    module_type = 0;
                }
            }
        }
    }
}

void USART3_Putch (unsigned char ch) {
    USART_SendData( USART3, ch);
    while( USART_GetFlagStatus( USART3, USART_FLAG_TC) == RESET){}
}

void USART3_Print (char s[]) {
    int i=0;
    while( i < 64) {
        if( s[i] == '\0') break;
        USART3_Putch( s[i++]);
    }
}

void USART3_Init (void) {
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;

    RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOB,ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);

    // Configure USART3 Tx (PB10) as alternate function push-pull
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // Configure USART3 Rx (PB11) as input floating
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // USART3 configuration
    USART_InitStructure.USART_BaudRate = 19200;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

    USART_Init(USART3, &USART_InitStructure);
    USART_Cmd(USART3, ENABLE);
}
