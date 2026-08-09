/* bsp/bsp_usart.c */
#include "bsp_usart.h"

OS_SEM g_usart1_rx_sem;

static uint8_t  g_rx_buf[USART1_RX_BUF_SIZE];
static volatile uint16_t g_rx_len = 0;
static volatile uint8_t  g_rx_flags = 0u;   /* 帧结束时的 SR 错误标志 (FE/ORE/NE) */
static volatile uint32_t g_rx_max_gap_ms = 0u;  /* 相邻字节最大间隔 (ms) */
static uint8_t  g_rx_byte;

/* ---- 接收超时计时 (TIM3, 10ms 周期) ----
   设备回复可能不连续。判定"一帧结束"的条件:
     1) 累积字节 > RX_FRAME_READY_LEN (128), 或
     2) 超过 RX_TIMEOUT_MS (300ms) 没有新数据
   满足任一条件即上报应用层 (OSSemPost)。*/
#define RX_TIMEOUT_MS       200u     /* 超时阈值 ms（设备回复字节间隔可达 >80ms） */
#define RX_TIMEOUT_TICKS    (RX_TIMEOUT_MS / 10u)   /* TIM3 10ms/tick -> 20 */
#define RX_FRAME_READY_LEN  128u     /* 字节数阈值 */
static volatile uint16_t g_rx_timeout = 0u;   /* 递减计数器, 0 表示已超时 */

static void BSP_TIM3_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_InitStruct;
    NVIC_InitTypeDef        NVIC_InitStruct;

    /* TIM3 时钟 = APB1*2 = 72MHz。预分频 7199 -> 10kHz, 重载 99 -> 10ms */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    TIM_InitStruct.TIM_Period        = 99;        /* 100 计数 = 10ms */
    TIM_InitStruct.TIM_Prescaler     = 7199;      /* 72MHz/7200 = 10kHz */
    TIM_InitStruct.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_InitStruct.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_InitStruct);

    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);

    NVIC_InitStruct.NVIC_IRQChannel                   = TIM3_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority  = 1;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority         = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd                 = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    TIM_Cmd(TIM3, ENABLE);
}

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
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IPU;         /* 上拉抑制空闲噪声 */
    GPIO_Init(GPIOA, &GPIO_InitStruct);

    USART_InitStruct.USART_BaudRate            = 38400;   /* USART1 下载通讯波特率 */
    USART_InitStruct.USART_WordLength           = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits             = USART_StopBits_1;
    USART_InitStruct.USART_Parity               = USART_Parity_No;
    USART_InitStruct.USART_HardwareFlowControl  = USART_HardwareFlowControl_None;
    USART_InitStruct.USART_Mode                 = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &USART_InitStruct);

    /* 仅使能接收中断。帧结束判定改用 TIM3 超时 + 字节数阈值，
       不用 IDLE 中断（否则线路空闲时 IDLE 标志常置位，ISR 死循环）。 */
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
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

    /* 启动接收超时计时器 (TIM3, 10ms 周期) */
    BSP_TIM3_Init();
    g_rx_timeout = 0u;
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
    OS_ERR err;
    g_rx_len = 0;
    g_rx_timeout = 0u;
    g_rx_flags = 0u;
    g_rx_max_gap_ms = 0u;
    /* 清除残留信号量计数，确保 OSSemPend 真正等到新数据或超时 */
    OSSemSet(&g_usart1_rx_sem, 0u, &err);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
}

uint8_t BSP_USART1_GetRxFlags(void)
{
    return g_rx_flags;
}

uint32_t BSP_USART1_GetRxMaxGapMs(void)
{
    return g_rx_max_gap_ms;
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

/* USART1 中断处理（在 stm32f10x_it.c 中调用）
   接收字节时重置 300ms 超时计数器；超过 128 字节立即上报。 */
void BSP_USART1_IRQHandler(void)
{
    OS_ERR err;

    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        g_rx_byte = (uint8_t)USART_ReceiveData(USART1);
        if (g_rx_len < USART1_RX_BUF_SIZE) {
            g_rx_buf[g_rx_len++] = g_rx_byte;
        }
        /* 收到新字节: 重置超时计数 */
        g_rx_timeout = RX_TIMEOUT_TICKS;

        /* 测量相邻字节间隔 (ISR 中 OSTimeGet 安全) */
        {
            OS_ERR t_err;
            static uint32_t last_tick = 0u;
            uint32_t now = OSTimeGet(&t_err);
            if (g_rx_len == 1u) {
                last_tick = now;             /* 首字节 */
            } else {
                uint32_t gap_ms = (now - last_tick) * 2u;   /* 500Hz -> 2ms */
                if (gap_ms > g_rx_max_gap_ms) {
                    g_rx_max_gap_ms = gap_ms;
                }
                last_tick = now;
            }
        }

        /* 累积超过阈值: 立即上报应用层 */
        if (g_rx_len > RX_FRAME_READY_LEN) {
            g_rx_flags = (uint8_t)(USART1->SR & (USART_FLAG_ORE | USART_FLAG_NE | USART_FLAG_FE));
            USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
            OSSemPost(&g_usart1_rx_sem, OS_OPT_POST_1, &err);
        }
    }

    /* 溢出错误 (ORE): 读 SR 再读 DR 清除，防止 RXNE 挂死 */
    if (USART_GetFlagStatus(USART1, USART_FLAG_ORE) != RESET) {
        (void)USART1->SR;
        (void)USART1->DR;
    }
}

/* TIM3 超时中断处理（在 stm32f10x_it.c 中调用）
   每 10ms 递减计数；归零且收到过数据则上报应用层。 */
void BSP_USART1_TIMEOUT_Handler(void)
{
    OS_ERR err;

    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

        if (g_rx_timeout > 0u) {
            g_rx_timeout--;
            if (g_rx_timeout == 0u && g_rx_len > 0u) {
                /* 300ms 无新数据, 帧结束 */
                g_rx_flags = (uint8_t)(USART1->SR & (USART_FLAG_ORE | USART_FLAG_NE | USART_FLAG_FE));
                USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
                OSSemPost(&g_usart1_rx_sem, OS_OPT_POST_1, &err);
            }
        }
    }
}
