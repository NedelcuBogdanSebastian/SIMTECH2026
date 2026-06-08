/*************************************************************************************
    Copyright (C) 2025 Nedelcu Bogdan Sebastian
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
Bauds	  Bits/s	      Bit duration	  Speed	          Actual speed	  Actual byte duration
==============================================================================================
115200    115200 bits/s	  8.681 탎	      14400 bytes/s	  11520 bytes/s	  86.806 탎
250000    250000          4 탎            31250           31250	          32 탎
*/

/*
 IMPORTANT: LED PD7 overlaps with the reset function. To disable this function and use the pin as an IO-Pin,
 program with the WCH-LinkUtility and select "Disable mul-func,PD7 is used for IO function". As standard
 there should be selected "Enable mul-func,ignored pin status within 128us/1ms/12ms"

DIGITAL
=======
PD0 - LED
PD3 PD2 PC7 PC6 PC5 PC2
PD4 PD5 PD6 PD7 PA2 PA1

======================
USART = PC0 (TX3)
        PC1 (RX3)
*/

#include "debug.h"
#include "CRC16.h"

#define LED_TOGGLE       GPIOD->OUTDR ^= GPIO_Pin_0;
#define LED_OFF          GPIOD->BSHR = GPIO_Pin_0
#define LED_ON           GPIOD->BCR = GPIO_Pin_0

#define TO_HEX(n) ((n) < 10 ? '0' + (n) : 'A' + ((n) - 10))

volatile uint32_t TimingDelay = 0;

uint8_t RxBuffer[20] = {0};

volatile uint8_t RxCnt = 0;

volatile uint8_t USART_ChipID_Ready = 0;

uint16_t CRC16 = 0xFFFF;

/*********************************************************************
 * @fn      GPIO_Toggle_INIT
 *
 * @brief   LED pins config.
 *
 * @return  none
 */
void GPIO_Config(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD, ENABLE);

    // LED connected to PD0
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOD, &GPIO_InitStructure);

	// PD3 PD2 PC7 PC6 PC5 PC2
	// PD4 PD5 PD6 PD7 PA2 PA1

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_1;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7 | GPIO_Pin_6 | GPIO_Pin_5 | GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOC, &GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7 | GPIO_Pin_6 | GPIO_Pin_5 | GPIO_Pin_4 | GPIO_Pin_3 | GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOD, &GPIO_InitStructure);
}

void SysTick_Config(u_int64_t ticks)
{
    SysTick->SR &= ~(1 << 0);
    SysTick->CMP = ticks;
    SysTick->CNT = 0;
    SysTick->CTLR = 0xF;

    NVIC_EnableIRQ(SysTicK_IRQn);
    NVIC_SetPriority(SysTicK_IRQn, 15);
}

// Variables to store the debounced pin states
uint8_t PD3_state = 0;
uint8_t PD2_state = 0;
uint8_t PC7_state = 0;
uint8_t PC6_state = 0;
uint8_t PC5_state = 0;
uint8_t PC2_state = 0;
uint8_t PD4_state = 0;
uint8_t PD5_state = 0;
uint8_t PD6_state = 0;
uint8_t PD7_state = 0;
uint8_t PA2_state = 0;
uint8_t PA1_state = 0;

// Glitch counters for each pin
uint16_t PD3_glitch_counter = 0;
uint16_t PD2_glitch_counter = 0;
uint16_t PC7_glitch_counter = 0;
uint16_t PC6_glitch_counter = 0;
uint16_t PC5_glitch_counter = 0;
uint16_t PC2_glitch_counter = 0;
uint16_t PD4_glitch_counter = 0;
uint16_t PD5_glitch_counter = 0;
uint16_t PD6_glitch_counter = 0;
uint16_t PD7_glitch_counter = 0;
uint16_t PA2_glitch_counter = 0;
uint16_t PA1_glitch_counter = 0;

// Debounce threshold
#define DEBOUNCE_THRESHOLD 40000

/**
 * @brief Reads the state of specified GPIO pins and applies debounce logic.
 */
void FilterAndDebouncePinStates(void)
{
    // Read the input data registers (INDR) for all the relevant pins
    uint32_t pd = GPIOD->INDR;  // Read GPIOD input data (for PD3, PD2, PD4, PD5, PD6, PD7)
    uint32_t pc = GPIOC->INDR;  // Read GPIOC input data (for PC7, PC6, PC5, PC2)
    uint32_t pa = GPIOA->INDR;  // Read GPIOA input data (for PA2, PA1)

    // Check and debounce PD3
    if (PD3_state == 1 && ((pd & GPIO_Pin_3) == 0)) {  // PD3 is low (bitmask for Pin 3)
        if (++PD3_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD3_state = 0;
        }
    } else if (PD3_state == 0 && ((pd & GPIO_Pin_3) != 0)) {  // PD3 is high (bitmask for Pin 3)
        if (++PD3_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD3_state = 1;
        }
    } else {
        PD3_glitch_counter = 0;
    }

    // Check and debounce PD2
    if (PD2_state == 1 && ((pd & GPIO_Pin_2) == 0)) {
        if (++PD2_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD2_state = 0;
        }
    } else if (PD2_state == 0 && ((pd & GPIO_Pin_2) != 0)) {
        if (++PD2_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD2_state = 1;
        }
    } else {
        PD2_glitch_counter = 0;
    }

    // Check and debounce PC7
    if (PC7_state == 1 && ((pc & GPIO_Pin_7) == 0)) {
        if (++PC7_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC7_state = 0;
        }
    } else if (PC7_state == 0 && ((pc & GPIO_Pin_7) != 0)) {
        if (++PC7_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC7_state = 1;
        }
    } else {
        PC7_glitch_counter = 0;
    }

    // Check and debounce PC6
    if (PC6_state == 1 && ((pc & GPIO_Pin_6) == 0)) {
        if (++PC6_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC6_state = 0;
        }
    } else if (PC6_state == 0 && ((pc & GPIO_Pin_6) != 0)) {
        if (++PC6_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC6_state = 1;
        }
    } else {
        PC6_glitch_counter = 0;
    }

    // Check and debounce PC5
    if (PC5_state == 1 && ((pc & GPIO_Pin_5) == 0)) {
        if (++PC5_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC5_state = 0;
        }
    } else if (PC5_state == 0 && ((pc & GPIO_Pin_5) != 0)) {
        if (++PC5_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC5_state = 1;
        }
    } else {
        PC5_glitch_counter = 0;
    }

    // Check and debounce PC2
    if (PC2_state == 1 && ((pc & GPIO_Pin_2) == 0)) {
        if (++PC2_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC2_state = 0;
        }
    } else if (PC2_state == 0 && ((pc & GPIO_Pin_2) != 0)) {
        if (++PC2_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PC2_state = 1;
        }
    } else {
        PC2_glitch_counter = 0;
    }

    // Check and debounce PD4
    if (PD4_state == 1 && ((pd & GPIO_Pin_4) == 0)) {
        if (++PD4_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD4_state = 0;
        }
    } else if (PD4_state == 0 && ((pd & GPIO_Pin_4) != 0)) {
        if (++PD4_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD4_state = 1;
        }
    } else {
        PD4_glitch_counter = 0;
    }

    // Check and debounce PD5
    if (PD5_state == 1 && ((pd & GPIO_Pin_5) == 0)) {
        if (++PD5_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD5_state = 0;
        }
    } else if (PD5_state == 0 && ((pd & GPIO_Pin_5) != 0)) {
        if (++PD5_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD5_state = 1;
        }
    } else {
        PD5_glitch_counter = 0;
    }

    // Check and debounce PD6
    if (PD6_state == 1 && ((pd & GPIO_Pin_6) == 0)) {
        if (++PD6_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD6_state = 0;
        }
    } else if (PD6_state == 0 && ((pd & GPIO_Pin_6) != 0)) {
        if (++PD6_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD6_state = 1;
        }
    } else {
        PD6_glitch_counter = 0;
    }

    // Check and debounce PD7
    if (PD7_state == 1 && ((pd & GPIO_Pin_7) == 0)) {
        if (++PD7_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD7_state = 0;
        }
    } else if (PD7_state == 0 && ((pd & GPIO_Pin_7) != 0)) {
        if (++PD7_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PD7_state = 1;
        }
    } else {
        PD7_glitch_counter = 0;
    }

    // Check and debounce PA2
    if (PA2_state == 1 && ((pa & GPIO_Pin_2) == 0)) {
        if (++PA2_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PA2_state = 0;
        }
    } else if (PA2_state == 0 && ((pa & GPIO_Pin_2) != 0)) {
        if (++PA2_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PA2_state = 1;
        }
    } else {
        PA2_glitch_counter = 0;
    }

    // Check and debounce PA1
    if (PA1_state == 1 && ((pa & GPIO_Pin_1) == 0)) {
        if (++PA1_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PA1_state = 0;
        }
    } else if (PA1_state == 0 && ((pa & GPIO_Pin_1) != 0)) {
        if (++PA1_glitch_counter >= DEBOUNCE_THRESHOLD) {
            PA1_state = 1;
        }
    } else {
        PA1_glitch_counter = 0;
    }
}

/*********************************************************************
 * @fn      IWDG_Init
 *
 * @brief   Initializes IWDG. The internal LSI RC freq. = 128kHz
 *
 * @param   IWDG_Prescaler: specifies the IWDG Prescaler value.
 *            IWDG_Prescaler_4: IWDG prescaler set to 4.
 *            IWDG_Prescaler_8: IWDG prescaler set to 8.
 *            IWDG_Prescaler_16: IWDG prescaler set to 16.
 *            IWDG_Prescaler_32: IWDG prescaler set to 32.
 *            IWDG_Prescaler_64: IWDG prescaler set to 64.
 *            IWDG_Prescaler_128: IWDG prescaler set to 128.
 *            IWDG_Prescaler_256: IWDG prescaler set to 256.
 *          Reload: specifies the IWDG Reload value.
 *            This parameter must be a number between 0 and 0x0FFF.
 *
 * @return  none
 */
void IWDG_Feed_Init(u16 prer, u16 rlr)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(prer);
    IWDG_SetReload(rlr);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */
int main(void)
{
    int i;
    char buf[20];
	uint8_t octet, a, b, c, d;

	SystemCoreClockUpdate();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

	TimingDelay = 0;	
	SysTick_Config(SystemCoreClock/1000-1); // SysTick counts miliseconds

    /*
    // PD7 is by default is configured as MCU reset pin, you need to configure
    // it as GPIO by configuring it with WCH Link Programmer Utility or in the code
    FLASH_Unlock();
    FLASH_EraseOptionBytes();
    FLASH_UserOptionByteConfig(OB_STOP_NoRST, OB_STDBY_NoRST, OB_RST_NoEN, OB_PowerON_Start_Mode_USER);
    FLASH_Unlock();
    */

    USART_Printf_Init(19200);
    GPIO_Config();

    LED_OFF;
	for (i = 0; i < 10; i++)
	{
		TimingDelay = 40; while (TimingDelay != 0x00);
		LED_TOGGLE;
	}

    USART_ChipID_Ready = 0;

    // The internal LSI RC freq. = 128 kHz
    // => 128000 kHz / 128 prescaller = 1000 Hz
    // => (1000 + 1) / 1000 Hz = 1.001 second IWDG reset
    IWDG_Feed_Init(IWDG_Prescaler_128, 1000);

    while(1)
    {
        IWDG_ReloadCounter();  // Feed watchdog

    	FilterAndDebouncePinStates();

    	if (USART_ChipID_Ready == 1)
    	{
    		USART_ChipID_Ready = 0;

    		if ((RxBuffer[0] == 'D')&&(RxBuffer[1] == 'A')&&(RxBuffer[2] == 'E'))
    		{
    			LED_TOGGLE;

    			// PD3 PD2 PC7 PC6 PC5 PC2
    			// PD4 PD5 PD6 PD7 PA2 PA1

    		    i = 0;

    			a = ~PD3_state & 1;
    			b = ~PD2_state & 1;
    			c = ~PC7_state & 1;
    			d = ~PC6_state & 1;
    			octet = (a << 3) | (b << 2) | (c << 1) | d;
    			buf[i++] = TO_HEX(octet);

    			a = ~PC5_state & 1;
    			b = ~PC2_state & 1;
    			c = ~PD4_state & 1;
    			d = ~PD5_state & 1;
    			octet = (a << 3) | (b << 2) | (c << 1) | d;
    			buf[i++] = TO_HEX(octet);

    			a = ~PD6_state & 1;
    			b = ~PD7_state & 1;
    			c = ~PA2_state & 1;
				d = ~PA1_state & 1;  // Now using PA1 like the other pins
    			octet = (a << 3) | (b << 2) | (c << 1) | d;
    			buf[i++] = TO_HEX(octet);

    		    // Calculate CRC for the buffer
    			CRC16 = 0xFFFF;  // CRC initial value
    		    for (uint8_t j=0; j<i; j++)
    		    	CRC16 = crc16_update(CRC16, buf[j]);

    			// Append the CRC (4 digits in hex)
    			buf[i++] = TO_HEX((CRC16 & 0xF000) >> 12);
    			buf[i++] = TO_HEX((CRC16 & 0x0F00) >> 8);
    			buf[i++] = TO_HEX((CRC16 & 0x00F0) >> 4);
    			buf[i++] = TO_HEX(CRC16 & 0x000F);

    			buf[i++] = 0;    // Null-terminate the string

    			printf("%s\n", (char*)buf);  // Print the buffer contents
                /*
                printf("%d %d %d %d %d %d %d %d %d %d %d %d\n", // Print debug data
                    ~PD3_state & 1, ~PD2_state & 1, ~PC7_state & 1, ~PC6_state & 1, 
					~PC5_state & 1, ~PC2_state & 1, ~PD4_state & 1, ~PD5_state & 1,
                    ~PD6_state & 1, ~PD7_state & 1, ~PA2_state & 1, ~PA1_state & 1
                );
                */          
    		}
    	}
    }
}

void USART1_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void USART1_IRQHandler(void) {
    if(USART_GetITStatus(USART1, USART_IT_RXNE)) {
        uint8_t data = USART_ReceiveData(USART1);
        
        if (data == '/') {
            RxCnt = 0;
            return;
        }
        
        if (data == '\\') {
            USART_ChipID_Ready = 1;
            return;
        }
        
        if (RxCnt < 3) { // sizeof(RxBuffer)-1) {
            RxBuffer[RxCnt++] = data;
        }
    }
}

/*
void USART1_IRQHandler(void)
{
    if(USART_GetITStatus(USART1, USART_IT_RXNE))
    {
        uint8_t data = USART_ReceiveData(USART1);

        if(data == '/') { RxCnt = 0; return; }
        if(data == '\\') { USART_ChipID_Ready = 1; return; }
        if(RxCnt < RXBUF_SIZE-1) { RxBuffer[RxCnt++] = data; }
    }
}
*/