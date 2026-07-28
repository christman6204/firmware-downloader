/* bsp/bsp_spi_sd.c */
#include "bsp_spi_sd.h"
#include "bsp_gpio.h"

void BSP_SPI2_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    SPI_InitTypeDef  SPI_InitStruct;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    /* GPIOB 时钟已在 BSP_GPIO_Init 中开启 */

    /* PB13=SCK, PB15=MOSI: 复用推挽 */
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_13 | GPIO_Pin_15;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* PB14=MISO: 浮空输入 */
    GPIO_InitStruct.GPIO_Pin  = GPIO_Pin_14;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    SPI_InitStruct.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStruct.SPI_Mode              = SPI_Mode_Master;
    SPI_InitStruct.SPI_DataSize          = SPI_DataSize_8b;
    SPI_InitStruct.SPI_CPOL              = SPI_CPOL_High;
    SPI_InitStruct.SPI_CPHA              = SPI_CPHA_2Edge;
    SPI_InitStruct.SPI_NSS               = SPI_NSS_Soft;
    SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_256; /* 36MHz/256≈140kHz, SD规范要求初始化期<=400kHz */
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

/* SD 卡 SPI 模式初始化: CMD0 -> CMD8 -> ACMD41 -> 就绪 */
uint8_t SD_SPI_Init(void)
{
    uint8_t r1;
    uint16_t retry;

    BSP_SD_CS_High();
    for (int i = 0; i < 10; i++) SPI2_SendRecv(0xFF);

    /* CMD0: GO_IDLE_STATE */
    BSP_SD_CS_Low();
    retry = 100;
    do {
        SPI2_SendRecv(0x40); SPI2_SendRecv(0); SPI2_SendRecv(0);
        SPI2_SendRecv(0); SPI2_SendRecv(0); SPI2_SendRecv(0x95);
        r1 = SPI2_SendRecv(0xFF);
    } while (r1 != 0x01 && --retry);
    if (!retry) { BSP_SD_CS_High(); return 1; }

    /* CMD8: SEND_IF_COND (电压 3.3V, check pattern 0xAA)
       SD v2.0 规范要求在 ACMD41 前发 CMD8，否则卡可能忽略 HCS 位。
       - r1 == 0x01: SD v2.0，后续 ACMD41 的 HCS 位生效（支持 SDHC）
       - r1 == 0x05: SD v1.x（非法命令），HCS 位被忽略，按标准容量初始化
       SPI 模式下 CMD0/CMD8 需要正确 CRC7。 */
    SPI2_SendRecv(0x48);                        /* CMD8 = 0x40+8 */
    SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
    SPI2_SendRecv(0x01); SPI2_SendRecv(0xAA);   /* arg = 0x000001AA */
    SPI2_SendRecv(0x87);                        /* CRC7 for CMD8 with 0x1AA arg */
    r1 = SPI2_SendRecv(0xFF);                   /* R1 */
    if (r1 == 0x01) {
        /* SD v2.0: 读 4 字节 R7 应答体（电压窗口 + check pattern 回显） */
        SPI2_SendRecv(0xFF); SPI2_SendRecv(0xFF);
        SPI2_SendRecv(0xFF); SPI2_SendRecv(0xFF);
    }
    /* r1 == 0x05 (SD v1.x) 无 R7 体；其它异常值亦不读，直接进 ACMD41，
       由 ACMD41 重试循环兜底（不改变现有 HCS=1 的 ACMD41，v1.x 会忽略 HCS）。 */

    /* ACMD41: SD_SEND_OP_COND */
    retry = 1000;
    do {
        SPI2_SendRecv(0x77); SPI2_SendRecv(0); SPI2_SendRecv(0);
        SPI2_SendRecv(0); SPI2_SendRecv(0); SPI2_SendRecv(0xFF);
        SPI2_SendRecv(0xFF);
        SPI2_SendRecv(0x69); SPI2_SendRecv(0x40); SPI2_SendRecv(0);
        SPI2_SendRecv(0); SPI2_SendRecv(0); SPI2_SendRecv(0xFF);
        r1 = SPI2_SendRecv(0xFF);
    } while (r1 != 0x00 && --retry);

    BSP_SD_CS_High();
    SPI2_SendRecv(0xFF);
    if (r1 != 0x00) return 1;

    /* 初始化成功, 提速至 18MHz 用于后续读写 */
    SD_SPI_SetHighSpeed();
    return 0;
}

uint8_t SD_SPI_ReadBlock(uint32_t addr, uint8_t *buf)
{
    uint8_t r1;
    uint16_t retry;

    BSP_SD_CS_Low();
    SPI2_SendRecv(0x51);  /* CMD17 */
    SPI2_SendRecv((addr >> 24) & 0xFF);
    SPI2_SendRecv((addr >> 16) & 0xFF);
    SPI2_SendRecv((addr >> 8) & 0xFF);
    SPI2_SendRecv(addr & 0xFF);
    SPI2_SendRecv(0xFF);

    r1 = SPI2_SendRecv(0xFF);
    if (r1 != 0x00) { BSP_SD_CS_High(); return 1; }

    retry = 65535;
    while (SPI2_SendRecv(0xFF) != 0xFE && --retry);
    if (!retry) { BSP_SD_CS_High(); return 2; }

    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++)
        buf[i] = SPI2_SendRecv(0xFF);
    SPI2_SendRecv(0xFF); SPI2_SendRecv(0xFF);  /* CRC */

    BSP_SD_CS_High();
    SPI2_SendRecv(0xFF);
    return 0;
}
