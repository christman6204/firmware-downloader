/* bsp/bsp_usart.c */
#include "bsp_usart.h"

OS_SEM g_usart1_rx_sem;

static uint8_t  g_rx_buf[USART1_RX_BUF_SIZE];
static volatile uint16_t g_rx_len = 0;
static uint8_t  g_rx_byte;

void BSP_USART_Init(void)
{
    GPIO_InitTypeDef   GPIO_InitStruct;
    USART_InitTypeDef  USART_InitStruct;
    NVIC_InitTypeDef   NVIC_InitStruct;
    OS_ERR err;

    /* ---- USART1: PA9=TX, PA10=RX ---- */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    /* GPIOA 时钟已在 BSP_GPIO_Init 中开启 */

    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_9;           /* TX */
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin  = GPIO_Pin_10;           /* RX */
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStruct);

    USART_InitStruct.USART_BaudRate            = 38400;   /* USART1 下载通讯波特率 */
    USART_InitStruct.USART_WordLength           = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits             = USART_StopBits_1;
    USART_InitStruct.USART_Parity               = USART_Parity_No;
    USART_InitStruct.USART_HardwareFlowControl  = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode                 = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &USART_InitStruct);

    /* 使能接收中断 + 空闲中断 */
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_ITConfig(USART1, USART_IT_IDLE, ENABLE);
    USART_Cmd(USART1, ENABLE);

    NVIC_InitStruct.NVIC_IRQChannel                   = USART1_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority  = 1;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority         = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd                 = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    /* ---- USART2: PD5=TX, PD6=RX ---- */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    /* GPIOD 时钟已在 BSP_GPIO_Init 中开启 */

    /* 使能 AFIO 时钟并应用 USART2 部分重映射：PA2/PA3 -> PD5/PD6。
       不重映射时 USART2 默认在 PA2/PA3，会导致 PD5/PD6 上无信号，
       且 PA3（蜂鸣器）会被 USART2_TX 占用，与 GPIO 输出冲突。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_USART2, ENABLE);

    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_5;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOD, &GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin  = GPIO_Pin_6;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOD, &GPIO_InitStruct);

    USART_InitStruct.USART_BaudRate = 115200;
    USART_Init(USART2, &USART_InitStruct);
    USART_Cmd(USART2, ENABLE);

    /* 创建接收信号量 */
    OSSemCreate(&g_usart1_rx_sem, "USART1 RX", 0, &err);
}

void BSP_USART1_Send(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        USART_SendData(USART1, buf[i]);
    }
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
}

void BSP_USART1_RecvStart(void)
{
    g_rx_len = 0;
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
}

uint16_t BSP_USART1_GetRecvLen(void) { return g_rx_len; }
const uint8_t *BSP_USART1_GetRecvBuf(void) { return g_rx_buf; }

void BSP_USART2_Printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    /* vsnprintf 在输出被截断时返回"本应写出的长度"（可能 > sizeof(buf)），
       直接用于循环会读越界栈缓冲。按实际缓冲大小钳位。 */
    if (len < 0) len = 0;
    if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
    for (int i = 0; i < len; i++) {
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
        USART_SendData(USART2, (uint8_t)buf[i]);
    }
}

/* USART1 中断处理（在 stm32f10x_it.c 中调用） */
void BSP_USART1_IRQHandler(void)
{
    OS_ERR err;

    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        g_rx_byte = (uint8_t)USART_ReceiveData(USART1);
        if (g_rx_len < USART1_RX_BUF_SIZE) {
            g_rx_buf[g_rx_len++] = g_rx_byte;
        }
    }

    if (USART_GetITStatus(USART1, USART_IT_IDLE) != RESET) {
        (void)USART1->SR;
        (void)USART1->DR;   /* 清 IDLE 标志 */
        USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
        OSSemPost(&g_usart1_rx_sem, OS_OPT_POST_1, &err);
    }
}
