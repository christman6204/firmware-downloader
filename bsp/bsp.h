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
#include "bsp_gpio.h"
#include "bsp_iwdg.h"
#include "bsp_usart.h"


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
CPU_INT32U   BSP_CPU_ClkFreq             (void);
void         SoftReset                   (void);

#endif
