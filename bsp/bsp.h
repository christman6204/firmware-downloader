/*
*********************************************************************************************************
*                                        BOARD SUPPORT PACKAGE
*
*                                       STM32F103VE + uC/OS-III
*
* Filename      : bsp.h
*********************************************************************************************************
*/

#ifndef  BSP_PRESENT
#define  BSP_PRESENT

#ifdef   BSP_MODULE
#define  BSP_EXT
#else
#define  BSP_EXT  extern
#endif

#include "stm32f10x.h"
#include  <os.h>


/*
*********************************************************************************************************
*                                          LED DEFINITIONS
*********************************************************************************************************
*/

#define  LED_PORT               GPIOE
#define  LED_PIN                GPIO_Pin_0
#define  LED_RCC                RCC_APB2Periph_GPIOE
#define  LED_ON()               GPIO_ResetBits(LED_PORT, LED_PIN)   /* 低电平亮 */
#define  LED_OFF()              GPIO_SetBits(LED_PORT, LED_PIN)     /* 高电平灭 */
#define  LED_TOGGLE()           GPIO_WriteBit(LED_PORT, LED_PIN,    \
                                   (BitAction)(1 - GPIO_ReadOutputDataBit(LED_PORT, LED_PIN)))


/*
*********************************************************************************************************
*                                            GLOBAL VARIABLES
*********************************************************************************************************
*/

BSP_EXT CPU_INT32U  cpu_clk_freq;


/*
*********************************************************************************************************
*                                           FUNCTION PROTOTYPES
*********************************************************************************************************
*/

void         BSP_Init                    (void);
void         BSP_LED_Init                (void);
CPU_INT32U   BSP_CPU_ClkFreq             (void);
void         SoftReset                   (void);

#endif
