/* bsp/bsp_spi_sd.c
 *
 * SD 卡 SPI 模式底层驱动。
 *
 * 本实现直接移植自经硬件验证的测试工程（bsp_spi_sdcard.c）的字节级时序，
 * 仅在外层包 UCOS-III 临界区（CPU_CRITICAL_ENTER/EXIT）以适配多任务环境。
 * 测试工程为裸机；多任务下若不在 SPI 事务期间关中断，任务抢断会破坏
 * SPI 字节时序导致 R1 误读 0xFF。
 *
 * ===== SPI 配置（与测试工程完全一致） =====
 *   - SPI2: SCK=PB13, MISO=PB14, MOSI=PB15, CS=PD8
 *   - Mode 3 (CPOL=High, CPHA=2Edge)
 *   - 速率: 初始 140kHz (Prescaler_256, ≤400kHz SD 规范) -> 就绪后 18MHz (Prescaler_2)
 *   - 8-bit, MSB first, NSS 软件管理
 *
 * ===== 时序要点 =====
 *   - SD_SendCmd: 仅发送 6 字节命令帧，不读响应
 *   - SD_GetResponse: 轮询最多 0xFFF 字节直到匹配（SD 规范 §7.2）
 *   - 每条命令的完整 CS-Low->收发->CS-High 周期均在临界区内
 */
#include "bsp_spi_sd.h"
#include "bsp_gpio.h"
#include "bsp_usart.h"   /* BSP_USART2_Printf for [SD] debug */
#include "os.h"           /* CPU_CRITICAL_ENTER/EXIT, OSTimeGet/Dly */

/* 卡类型：0=SDSC(字节地址), 1=SDHC(块地址)。
   CMD17 地址参数据此决定是否 ×512。 */
static uint8_t g_card_blockaddr = 0u;

/* ---- SPI 字节收发（与测试工程 SD_WriteByte 一致） ---- */
static uint8_t SPI2_SendRecv(uint8_t byte)
{
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);
    SPI_I2S_SendData(SPI2, byte);
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) == RESET);
    return (uint8_t)SPI_I2S_ReceiveData(SPI2);
}

/* ---- 发送 6 字节命令帧（不读响应，与测试工程 SD_SendCmd 一致） ---- */
static void SD_SendCmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    SPI2_SendRecv(cmd | 0x40u);
    SPI2_SendRecv((uint8_t)(arg >> 24));
    SPI2_SendRecv((uint8_t)(arg >> 16));
    SPI2_SendRecv((uint8_t)(arg >> 8));
    SPI2_SendRecv((uint8_t)(arg));
    SPI2_SendRecv(crc);
}

/* ---- 轮询响应字节（与测试工程 SD_GetResponse 一致：最多 0xFFF 次） ---- */
/* 返回 0=收到期望值, 1=超时 */
static uint8_t SD_GetResponse(uint8_t expected)
{
    uint32_t count = 0xFFFu;
    uint8_t  b;
    do { b = SPI2_SendRecv(0xFF); } while (b != expected && --count);
    return (b == expected) ? 0u : 1u;
}

void BSP_SPI2_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    SPI_InitTypeDef  SPI_InitStruct;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    /* GPIOB 时钟已在 BSP_GPIO_Init 中开启 */

    /* PB13=SCK: 复用推挽 */
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_13;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* PB15=MOSI: 复用推挽 */
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_15;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* PB14=MISO: 浮空输入（与测试工程一致） */
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_14;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* SPI Mode 3, 初始 140kHz（Prescaler_256，SD 规范要求初始化期 ≤400kHz） */
    SPI_InitStruct.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStruct.SPI_Mode              = SPI_Mode_Master;
    SPI_InitStruct.SPI_DataSize          = SPI_DataSize_8b;
    SPI_InitStruct.SPI_CPOL              = SPI_CPOL_High;
    SPI_InitStruct.SPI_CPHA              = SPI_CPHA_2Edge;
    SPI_InitStruct.SPI_NSS               = SPI_NSS_Soft;
    SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_256;      /* 140kHz (≤400kHz SD spec) */
    SPI_InitStruct.SPI_FirstBit          = SPI_FirstBit_MSB;
    SPI_InitStruct.SPI_CRCPolynomial     = 7;
    SPI_Init(SPI2, &SPI_InitStruct);
    SPI_Cmd(SPI2, ENABLE);
}

/* SD 初始化完成后提速至 18MHz (Prescaler_2) 用于正常读写。
   STM32 规范：修改 CR1.BR 前必须先 SPI_Cmd(DISABLE)。 */
void SD_SPI_SetHighSpeed(void)
{
    SPI_Cmd(SPI2, DISABLE);
    SPI2->CR1 &= ~SPI_CR1_BR;
    SPI2->CR1 |= SPI_BaudRatePrescaler_2;
    SPI_Cmd(SPI2, ENABLE);
}

/*
 * SD 卡 SPI 模式初始化（移植自测试工程 SD_Init + SD_GoIdleState + SD_GetCardType）
 *
 * 流程: 上电 80+ 时钟 -> CMD0(等0x01) -> CMD8(等0x01,读R7) ->
 *       CMD55+ACMD41 循环(等0x00) -> 就绪
 * 每个 CS-Low->收发->CS-High 事务均在临界区内（关中断防 RTOS 抢断）。
 * ACMD41 重试间隔 10ms（恢复中断让其他任务运行 + 给卡初始化时间），最长 1s。
 */
uint8_t SD_SPI_Init(void)
{
    OS_ERR   err;
    uint32_t start, deadline;
    CPU_SR_ALLOC();

    BSP_SD_CS_High();
    for (int i = 0; i < 10; i++) SPI2_SendRecv(0xFF);     /* ≥74 clocks */

    /* ---- CMD0: GO_IDLE_STATE (等 R1=0x01) ---- */
    CPU_CRITICAL_ENTER();
    BSP_SD_CS_Low();
    SD_SendCmd(0x00u, 0x00000000u, 0x95u);
    if (SD_GetResponse(0x01u)) {
        BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
        CPU_CRITICAL_EXIT();
        BSP_USART2_Printf("[SD] CMD0 fail\r\n");
        return 1;
    }
    BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
    CPU_CRITICAL_EXIT();

    /* ---- CMD8: SEND_IF_COND (arg=0x1AA, 等 R1=0x01, 读 4 字节 R7) ---- */
    CPU_CRITICAL_ENTER();
    BSP_SD_CS_Low();
    SD_SendCmd(0x08u, 0x000001AAu, 0x87u);
    {
        uint8_t r1;
        uint32_t c = 0xFFFu;
        do { r1 = SPI2_SendRecv(0xFF); } while (r1 == 0xFFu && --c);
        BSP_USART2_Printf("[SD] CMD8 R1=0x%02X\r\n", r1);
        if (r1 == 0x01u) {
            (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);
            (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);
        }
    }
    BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
    CPU_CRITICAL_EXIT();

    /* ---- ACMD41: CMD55 + ACMD41(HCS=1) 循环, 最长 1s, 每 10ms 重试 ---- */
    start    = OSTimeGet(&err);
    deadline = start + 500u;                               /* 500 ticks = 1s */
    {
        uint8_t acmd41_ok = 0u;
        do {
            CPU_CRITICAL_ENTER();
            BSP_SD_CS_Low();
            SD_SendCmd(0x37u, 0x00000000u, 0xFFu);         /* CMD55 */
            (void)SD_GetResponse(0x01u);                   /* CMD55 R1=0x01 */
            SD_SendCmd(0x29u, 0x40000000u, 0xFFu);         /* ACMD41, HCS=1 */
            acmd41_ok = (SD_GetResponse(0x00u) == 0u) ? 1u : 0u;
            BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
            CPU_CRITICAL_EXIT();

            if (acmd41_ok) break;
            OSTimeDlyHMSM(0, 0, 0, 10u, OS_OPT_TIME_HMSM_STRICT, &err);
        } while ((OSTimeGet(&err) - start) < (deadline - start));

        if (!acmd41_ok) {
            BSP_USART2_Printf("[SD] ACMD41 timeout (1s)\r\n");
            return 1;
        }
    }

    /* ---- CMD58: READ_OCR, 检查 CCS 位判断 SDHC/SDSC ----
       OCR bit30 (CCS): 1=SDHC/SDXC(块寻址), 0=SDSC(字节寻址)。
       SDSC 卡即使 ACMD41 设了 HCS=1 也保持字节寻址，CMD17 地址必须 ×512。 */
    CPU_CRITICAL_ENTER();
    BSP_SD_CS_Low();
    SD_SendCmd(0x3Au, 0x00000000u, 0xFFu);                 /* CMD58 */
    {
        uint8_t  r1;
        uint32_t c = 0xFFFu;
        uint8_t  ocr[4];
        do { r1 = SPI2_SendRecv(0xFF); } while (r1 == 0xFFu && --c);
        if (r1 == 0x00u) {
            ocr[0] = SPI2_SendRecv(0xFF); ocr[1] = SPI2_SendRecv(0xFF);
            ocr[2] = SPI2_SendRecv(0xFF); ocr[3] = SPI2_SendRecv(0xFF);
            g_card_blockaddr = (ocr[0] & 0x40u) ? 1u : 0u;  /* CCS = bit30 = ocr[0] bit6 */
            BSP_USART2_Printf("[SD] CMD58 OCR=%02X%02X%02X%02X  CCS=%u (%s)\r\n",
                              ocr[0], ocr[1], ocr[2], ocr[3],
                              g_card_blockaddr,
                              g_card_blockaddr ? "SDHC" : "SDSC");
        } else {
            /* CMD58 失败：保守按 SDSC 处理（字节寻址） */
            g_card_blockaddr = 0u;
            BSP_USART2_Printf("[SD] CMD58 fail R1=0x%02X, assume SDSC\r\n", r1);
        }
    }
    BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
    CPU_CRITICAL_EXIT();

    /* 初始化完成，提速至 18MHz 用于后续读写（SD 规范：初始化期 ≤400kHz，就绪后可提速） */
    SD_SPI_SetHighSpeed();
    BSP_USART2_Printf("[SD] Init OK (SPI -> 18MHz)\r\n");
    return 0;
}

/*
 * CMD17 读取单个 512 字节块（移植自测试工程 SD_ReadBlock）
 *   1. CS_Low + SendCmd(CMD17, addr, 0xFF)
 *   2. 等 R1=0x00
 *   3. 等 Data Token=0xFE
 *   4. 读 512 字节 + 2 字节 CRC
 *   5. CS_High + dummy
 * 全程临界区保护。
 */
uint8_t SD_SPI_ReadBlock(uint32_t addr, uint8_t *buf)
{
    CPU_SR_ALLOC();

    /* SDSC 卡用字节地址，SDHC 用块(LBA)地址 */
    if (!g_card_blockaddr) {
        addr *= SD_BLOCK_SIZE;
    }

    CPU_CRITICAL_ENTER();
    BSP_SD_CS_Low();
    SD_SendCmd(0x11u, addr, 0xFFu);                        /* CMD17 */

    if (SD_GetResponse(0x00u)) {                           /* R1 */
        BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
        CPU_CRITICAL_EXIT();
        BSP_USART2_Printf("[SD] CMD17 R1 timeout (addr=%lu)\r\n", (unsigned long)addr);
        return 1;
    }
    if (SD_GetResponse(0xFEu)) {                           /* Data Token */
        BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
        CPU_CRITICAL_EXIT();
        BSP_USART2_Printf("[SD] CMD17 token timeout (addr=%lu)\r\n", (unsigned long)addr);
        return 2;
    }

    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++)
        buf[i] = SPI2_SendRecv(0xFF);
    (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);  /* CRC16 */

    BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
    CPU_CRITICAL_EXIT();
    return 0;
}

/*
 * CMD24 写入单个 512 字节块。
 *   1. CS_Low + SendCmd(CMD24, addr, 0xFF)
 *   2. 等 R1=0x00
 *   3. 发 Data Token 0xFE + 512 字节数据 + 2 字节 CRC
 *   4. 读 Data Response (b & 0x1F == 0x05 表示接受)
 *   5. 等待 busy 完成（卡拉低 DO，读到非 0x00 即完成）
 *   6. CS_High + dummy
 */
uint8_t SD_SPI_WriteBlock(uint32_t addr, const uint8_t *buf)
{
    CPU_SR_ALLOC();
    uint8_t  resp;
    uint32_t cnt;

    /* SDSC 卡用字节地址 */
    if (!g_card_blockaddr) {
        addr *= SD_BLOCK_SIZE;
    }

    CPU_CRITICAL_ENTER();
    BSP_SD_CS_Low();
    SD_SendCmd(0x18u, addr, 0xFFu);                        /* CMD24 */

    if (SD_GetResponse(0x00u)) {                           /* R1 */
        BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
        CPU_CRITICAL_EXIT();
        BSP_USART2_Printf("[SD] CMD24 R1 timeout\r\n");
        return 1;
    }

    SPI2_SendRecv(0xFEu);                                  /* Data Token */
    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++)
        SPI2_SendRecv(buf[i]);
    SPI2_SendRecv(0xFF); SPI2_SendRecv(0xFF);              /* CRC16 (dummy) */

    /* Data Response: 等待 (b & 0x1F) == 0x05 (accepted) */
    cnt = 0xFFFu;
    do { resp = SPI2_SendRecv(0xFF); } while ((resp & 0x1Fu) != 0x05u && --cnt);
    if ((resp & 0x1Fu) != 0x05u) {
        BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
        CPU_CRITICAL_EXIT();
        BSP_USART2_Printf("[SD] CMD24 data resp=0x%02X\r\n", resp);
        return 2;
    }

    /* 等待 busy 完成：卡写 Flash 期间 DO 拉低(读 0x00)，完成后拉高(读 0xFF) */
    cnt = 0xFFFFFu;
    do { resp = SPI2_SendRecv(0xFF); } while (resp == 0x00u && --cnt);

    BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
    CPU_CRITICAL_EXIT();
    return 0;
}

/*
 * CMD9: SEND_CSD，读 16 字节 CSD 寄存器（不涉及块地址，无需 SDSC 转换）。
 * 用于查询卡容量、CSD 版本等。
 *   1. CS_Low + SendCmd(CMD9, 0, 0xFF)
 *   2. 等 R1=0x00
 *   3. 等 Data Token=0xFE
 *   4. 读 16 字节 CSD + 2 字节 CRC
 *   5. CS_High + dummy
 */
uint8_t SD_SPI_ReadCSD(uint8_t csd[16])
{
    CPU_SR_ALLOC();

    CPU_CRITICAL_ENTER();
    BSP_SD_CS_Low();
    SD_SendCmd(0x09u, 0x00000000u, 0xFFu);                  /* CMD9 */

    if (SD_GetResponse(0x00u)) {                            /* R1 */
        BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
        CPU_CRITICAL_EXIT();
        return 1;
    }
    if (SD_GetResponse(0xFEu)) {                            /* Data Token */
        BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
        CPU_CRITICAL_EXIT();
        return 2;
    }
    for (uint8_t i = 0; i < 16u; i++) {
        csd[i] = SPI2_SendRecv(0xFF);
    }
    (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);   /* CRC16 */

    BSP_SD_CS_High(); SPI2_SendRecv(0xFF);
    CPU_CRITICAL_EXIT();
    return 0;
}

/* 返回卡寻址模式：1=SDHC(块寻址), 0=SDSC(字节寻址) */
uint8_t SD_SPI_IsBlockAddr(void)
{
    return g_card_blockaddr;
}
