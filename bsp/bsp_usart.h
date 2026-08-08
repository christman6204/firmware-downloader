/* bsp/bsp_usart.h */
#ifndef __BSP_USART_H
#define __BSP_USART_H

#include "stm32f10x.h"
#include "os.h"
#include <stdio.h>
#include <stdarg.h>

#define USART1_RX_BUF_SIZE  512u

extern OS_SEM g_usart1_rx_sem;

void           BSP_USART_Init(void);
void           BSP_USART1_Send(const uint8_t *buf, uint16_t len);
void           BSP_USART1_RecvStart(void);
uint16_t       BSP_USART1_GetRecvLen(void);
const uint8_t *BSP_USART1_GetRecvBuf(void);
void           BSP_USART2_Printf(const char *fmt, ...);
void           BSP_USART1_IRQHandler(void);
void           BSP_USART1_TIMEOUT_Handler(void);  /* TIM3 超时中断处理 */

#endif
