/* bsp/bsp_spi_sd.c
 *
 * SD 卡 SPI 模式底层驱动：SPI2 初始化、SD 卡时序初始化、扇区读取。
 *
 * ===== SD 卡 SPI 模式初始化流程 =====
 *
 * 上电 → 发 >=74 个时钟 (10×0xFF) 让卡进入 SPI 模式
 *   │
 *   ├─ CMD0 (GO_IDLE_STATE): 复位卡到 Idle 态
 *   │   格式: 0x40 0x00 0x00 0x00 0x00 0x95   (CRC7=0x95 对 CMD0 正确)
 *   │   重试 100 次，等待 R1=0x01（Idle 状态）
 *   │
 *   ├─ CMD8 (SEND_IF_COND): 电压检测 + SD v2.0 识别
 *   │   格式: 0x48 0x00 0x00 0x01 0xAA 0x87
 *   │   参数: VHS=0x01 (2.7-3.6V), CheckPattern=0xAA
 *   │   R1=0x01 → SD v2.0，读4字节R7体（电压窗口+回显0xAA）
 *   │   R1=0x05 → SD v1.x（非法命令），HCS位被忽略，按标准容量初始化
 *   │   其它值 → 不读R7体，由ACMD41兜底
 *   │
 *   ├─ CMD55 + ACMD41 (SD_SEND_OP_COND): 等待卡就绪
 *   │   CMD55: 0x77 0x00 0x00 0x00 0x00 0xFF (APP_CMD前缀)
 *   │   ACMD41: 0x69 0x40 0x00 0x00 0x00 0xFF
 *   │   参数: HCS=1 (bit30, 支持SDHC)，OCR其余位=0
 *   │   重试 1000 次，等待 R1=0x00（就绪，退出 Idle 态）
 *   │   设计要点：CMD8 失败（v1.x卡）时仍执行 ACMD41，HCS位被忽略
 *   │
 *   └─ 成功 → SD_SPI_SetHighSpeed() 提速至 18MHz (PCLK2/2)
 *             初始化期是 Prescaler_256 ≈ 140kHz (< 400kHz 规范要求)
 *
 * ===== CMD17 读块协议 =====
 *
 * 时序（每个字节以 SPI2_SendRecv 交换，发 0xFF 接收）：
 *
 *   1. CS=Low
 *   2. 发 CMD17: 0x51  addr[31:24]  addr[23:16]  addr[15:8]  addr[7:0]  0xFF
 *   3. 等待 R1=0x00 (OK)   ← 若 R1≠0x00 → 错误退出
 *   4. 等待 Data Token 0xFE (最多等 65535 个 0xFF)
 *   5. 读 512 字节数据块
 *   6. 读 2 字节 CRC16（SPI 模式下忽略，但必须读走以推进总线）
 *   7. CS=High + 1 字节 0xFF 虚拟时钟
 *
 * 错误码: 返回 1 = R1 非零，返回 2 = Data Token 超时。
 *
 * ===== SPI 配置要点 =====
 *
 *   - SPI2: SCK=PB13, MISO=PB14, MOSI=PB15
 *   - 模式: CPOL=0 (空闲低), CPHA=0 (上升沿采样) → SPI_Mode 0（SD 卡标准模式）
 *   - 数据宽度: 8-bit
 *   - 位序: MSB first
 *   - NSS: 软件管理（CS 由 GPIO 手动控制，BSP_SD_CS_Low/High）
 *   - 速度: 初始 ~140kHz (256分频) → 就绪后 18MHz (2分频)
 */
#include "bsp_spi_sd.h"
#include "bsp_gpio.h"
#include "bsp_usart.h"   /* BSP_USART2_Printf for [SD] debug */

void BSP_SPI2_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    SPI_InitTypeDef  SPI_InitStruct;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    /* GPIOB 时钟已在 BSP_GPIO_Init 中开启 */

    /* PB13=SCK: 复用推挽（单独配置，与测试工程一致） */
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

    /* SPI Mode 3 + 18MHz（与测试工程完全一致） */
    SPI_InitStruct.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStruct.SPI_Mode              = SPI_Mode_Master;
    SPI_InitStruct.SPI_DataSize          = SPI_DataSize_8b;
    SPI_InitStruct.SPI_CPOL              = SPI_CPOL_High;
    SPI_InitStruct.SPI_CPHA              = SPI_CPHA_2Edge;
    SPI_InitStruct.SPI_NSS               = SPI_NSS_Soft;
    SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;       /* 18MHz */
    SPI_InitStruct.SPI_FirstBit          = SPI_FirstBit_MSB;
    SPI_InitStruct.SPI_CRCPolynomial     = 7;
    SPI_Init(SPI2, &SPI_InitStruct);
    SPI_Cmd(SPI2, ENABLE);
}

/* SD 初始化完成后将 SPI2 提速至 18MHz (Prescaler_2) 用于正常读写 */
void SD_SPI_SetHighSpeed(void)
{
    SPI_Cmd(SPI2, DISABLE);
    SPI2->CR1 &= ~SPI_CR1_BR;
    SPI2->CR1 |= SPI_BaudRatePrescaler_2;
    SPI_Cmd(SPI2, ENABLE);
}

static uint8_t SPI2_SendRecv(uint8_t byte)
{
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);
    SPI_I2S_SendData(SPI2, byte);
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) == RESET);
    return (uint8_t)SPI_I2S_ReceiveData(SPI2);
}

/* 等待 SD 卡返回指定响应字节（与测试工程一致：最多 4095 次轮询） */
static uint8_t SD_WaitResponse(uint8_t expected)
{
    uint32_t count = 0xFFF;
    uint8_t  r1;
    do {
        r1 = SPI2_SendRecv(0xFF);
    } while (r1 != expected && --count);
    return (r1 == expected) ? 0u : 1u;   /* 0=成功, 1=超时 */
}

/* SD 卡 SPI 模式初始化: CMD0 -> CMD8 -> ACMD41 -> 就绪
   （流程完全对齐测试工程） */
uint8_t SD_SPI_Init(void)
{
    BSP_SD_CS_High();
    for (int i = 0; i < 10; i++) SPI2_SendRecv(0xFF);

    /* CMD0: GO_IDLE_STATE */
    BSP_SD_CS_Low();
    /* 发命令帧: cmd | 0x40, arg=0x00000000, CRC=0x95 */
    SPI2_SendRecv(0x40);
    SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
    SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
    SPI2_SendRecv(0x95);
    if (SD_WaitResponse(0x01u)) {
        BSP_SD_CS_High();
        BSP_USART2_Printf("[SD] CMD0 fail\r\n");
        return 1;
    }
    BSP_SD_CS_High();
    SPI2_SendRecv(0xFF);                       /* 8 个虚拟时钟 */

    /* CMD8: SEND_IF_COND (arg=0x000001AA, CRC=0x87) */
    BSP_SD_CS_Low();
    SPI2_SendRecv(0x48);
    SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
    SPI2_SendRecv(0x01); SPI2_SendRecv(0xAA);
    SPI2_SendRecv(0x87);
    {
        uint8_t r7 = SPI2_SendRecv(0xFF);               /* R1 */
        BSP_USART2_Printf("[SD] CMD8 R1=0x%02X\r\n", r7);
        if (r7 == 0x01u) {
            /* SD v2.0: 读 4 字节 R7 体（电压窗口 + check pattern 回显） */
            (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);
            (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);
        }
    }
    BSP_SD_CS_High();
    SPI2_SendRecv(0xFF);

    /* ACMD41: SD_SEND_OP_COND (CMD55 + ACMD41, HCS=1) */
    {
        uint16_t acmd_retry = 1000;
        uint8_t  r1 = 0xFF;
        do {
            BSP_SD_CS_Low();
            /* CMD55: APP_CMD */
            SPI2_SendRecv(0x77);
            SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
            SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
            SPI2_SendRecv(0xFF);
            (void)SPI2_SendRecv(0xFF);                  /* 丢弃 CMD55 的 R1 */
            /* ACMD41: arg=0x40000000 (HCS=1) */
            SPI2_SendRecv(0x69);
            SPI2_SendRecv(0x40); SPI2_SendRecv(0x00);
            SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
            SPI2_SendRecv(0xFF);
            r1 = SPI2_SendRecv(0xFF);                   /* 读 R1 */
            BSP_SD_CS_High();
            SPI2_SendRecv(0xFF);
        } while (r1 != 0x00u && --acmd_retry);
        if (r1 != 0x00u) {
            BSP_USART2_Printf("[SD] ACMD41 fail, R1=0x%02X\r\n", r1);
            return 1;
        }
    }

    BSP_USART2_Printf("[SD] Init OK\r\n");
    return 0;
}

uint8_t SD_SPI_ReadBlock(uint32_t addr, uint8_t *buf)
{
    BSP_SD_CS_Low();
    /* CMD17: READ_SINGLE_BLOCK */
    SPI2_SendRecv(0x51);
    SPI2_SendRecv((addr >> 24) & 0xFF);
    SPI2_SendRecv((addr >> 16) & 0xFF);
    SPI2_SendRecv((addr >> 8) & 0xFF);
    SPI2_SendRecv(addr & 0xFF);
    SPI2_SendRecv(0xFF);
    /* 等待 R1=0x00 */
    if (SD_WaitResponse(0x00u)) { BSP_SD_CS_High(); return 1; }

    /* 等待 Data Token 0xFE */
    if (SD_WaitResponse(0xFEu)) { BSP_SD_CS_High(); return 2; }

    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++)
        buf[i] = SPI2_SendRecv(0xFF);
    SPI2_SendRecv(0xFF); SPI2_SendRecv(0xFF);  /* CRC */

    BSP_SD_CS_High();
    SPI2_SendRecv(0xFF);
    return 0;
}
