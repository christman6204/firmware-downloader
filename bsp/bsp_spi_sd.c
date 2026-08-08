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
 *   └─ 成功 → SD_SPI_SetHighSpeed() 提速至 18MHz (Prescaler_2)
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
 *   - 模式: CPOL=1 (空闲高), CPHA=1 (第二沿采样) → SPI_Mode 3
 *   - 数据宽度: 8-bit
 *   - 位序: MSB first
 *   - NSS: 软件管理（CS 由 GPIO 手动控制，BSP_SD_CS_Low/High）
 *   - 速度: 初始 ~140kHz (256 分频) → 就绪后 18MHz (2 分频)
 *
 * ===== 超时规范 =====
 *
 *   所有 SD 超时基于 UCOS-III OSTimeGet (500Hz = 2ms/tick)，而非固定次数：
 *     - CMD0/CMD8 响应 : 100ms (50 ticks)
 *     - ACMD41 就绪    : 1s (500 ticks), 每 10ms 重试一次
 *     - CMD17 R1/Token : 100ms
 *
 *   SD Physical Layer Spec §7.2.2 要求 ACMD41 超时不短于 1s。
 */
#include "bsp_spi_sd.h"
#include "bsp_gpio.h"
#include "bsp_usart.h"   /* BSP_USART2_Printf for [SD] debug */
#include "os.h"           /* OSTimeGet / OSTimeDlyHMSM for SD timeouts */

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

    /* SPI Mode 3，初始 140kHz（SD 规范要求 ≤400kHz） */
    SPI_InitStruct.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStruct.SPI_Mode              = SPI_Mode_Master;
    SPI_InitStruct.SPI_DataSize          = SPI_DataSize_8b;
    SPI_InitStruct.SPI_CPOL              = SPI_CPOL_High;
    SPI_InitStruct.SPI_CPHA              = SPI_CPHA_2Edge;
    SPI_InitStruct.SPI_NSS               = SPI_NSS_Soft;
    SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_256;     /* ~140kHz (<400kHz SD spec) */
    SPI_InitStruct.SPI_FirstBit          = SPI_FirstBit_MSB;
    SPI_InitStruct.SPI_CRCPolynomial     = 7;
    SPI_Init(SPI2, &SPI_InitStruct);
    SPI_Cmd(SPI2, ENABLE);
}

/* SD 初始化完成后将 SPI2 提速至 18MHz (Prescaler_2) */
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

/*
 * SD 卡 SPI 模式初始化。
 *
 * UCOS-III 诊断结论：任务抢断破坏 SPI 时序 → R1=0xFF。
 * 每个 SPI 命令由 CPU_CRITICAL_ENTER/EXIT 保护（原子执行，无函数封装开销）。
 * ACMD41 每 10ms 重试一次，超时 1s。
 */
uint8_t SD_SPI_Init(void)
{
    OS_ERR   err;
    uint32_t start, deadline;
    CPU_SR_ALLOC();

    BSP_SD_CS_High();
    for (int i = 0; i < 10; i++) SPI2_SendRecv(0xFF);     /* ≥74 clocks */

    /* ---- CMD0: GO_IDLE_STATE ---- */
    CPU_CRITICAL_ENTER();
    {
        uint32_t cnt = 0xFFFu;
        uint8_t  r1  = 0xFFu;
        BSP_SD_CS_Low();
        SPI2_SendRecv(0x40);
        SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
        SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
        SPI2_SendRecv(0x95);
        do { r1 = SPI2_SendRecv(0xFF); } while (r1 != 0x01u && --cnt);
        BSP_SD_CS_High();
        SPI2_SendRecv(0xFF);
        if (r1 != 0x01u) {
            CPU_CRITICAL_EXIT();
            BSP_USART2_Printf("[SD] CMD0 fail\r\n");
            return 1;
        }
    }
    CPU_CRITICAL_EXIT();

    /* ---- CMD8: SEND_IF_COND ---- */
    CPU_CRITICAL_ENTER();
    {
        uint32_t cnt   = 0xFFFu;
        uint8_t  r7_r1 = 0xFFu;
        BSP_SD_CS_Low();
        SPI2_SendRecv(0x48);
        SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
        SPI2_SendRecv(0x01); SPI2_SendRecv(0xAA);
        SPI2_SendRecv(0x87);
        do { r7_r1 = SPI2_SendRecv(0xFF); } while (r7_r1 == 0xFFu && --cnt);
        if (r7_r1 == 0x01u) {
            (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);
            (void)SPI2_SendRecv(0xFF); (void)SPI2_SendRecv(0xFF);
        }
        BSP_SD_CS_High();
        SPI2_SendRecv(0xFF);
        BSP_USART2_Printf("[SD] CMD8 R1=0x%02X\r\n", r7_r1);
    }
    CPU_CRITICAL_EXIT();

    /* ---- ACMD41: SD_SEND_OP_COND (HCS=1, 最长 1s, 每 10ms 重试) ---- */
    start    = OSTimeGet(&err);
    deadline = start + 500u;                               /* 500 ticks = 1s */
    {
        uint8_t r1 = 0xFFu;
        do {
            CPU_CRITICAL_ENTER();
            {
                uint32_t cnt = 0xFFFu;
                BSP_SD_CS_Low();
                /* CMD55: APP_CMD 前缀，必须紧贴 ACMD41 发送 */
                SPI2_SendRecv(0x77);
                SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
                SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
                SPI2_SendRecv(0xFF);
                /* 轮询 CMD55 的 R1（等待卡处理完 APP_CMD 前缀） */
                {
                    uint32_t c55 = 0xFFFu;
                    uint8_t  r55 = 0xFFu;
                    do { r55 = SPI2_SendRecv(0xFF); } while (r55 == 0xFFu && --c55);
                }
                /* ACMD41: arg=0x40000000 (HCS=1) */
                SPI2_SendRecv(0x69);
                SPI2_SendRecv(0x40); SPI2_SendRecv(0x00);
                SPI2_SendRecv(0x00); SPI2_SendRecv(0x00);
                SPI2_SendRecv(0xFF);
                do { r1 = SPI2_SendRecv(0xFF); } while (r1 == 0xFFu && --cnt);
                BSP_SD_CS_High();
                SPI2_SendRecv(0xFF);
            }
            CPU_CRITICAL_EXIT();
            if (r1 == 0x00u) break;

            OSTimeDlyHMSM(0, 0, 0, 10u,
                          OS_OPT_TIME_HMSM_STRICT, &err);
        } while ((OSTimeGet(&err) - start) < (deadline - start));

        if (r1 != 0x00u) {
            BSP_USART2_Printf("[SD] ACMD41 timeout (1s)\r\n");
            return 1;
        }
    }

    SD_SPI_SetHighSpeed();
    BSP_USART2_Printf("[SD] Init OK\r\n");
    return 0;
}

/*
 * CMD17 读取单个 512 字节块。
 */
uint8_t SD_SPI_ReadBlock(uint32_t addr, uint8_t *buf)
{
    uint32_t cnt;
    uint8_t  b;
    CPU_SR_ALLOC();

    CPU_CRITICAL_ENTER();
    BSP_SD_CS_Low();
    SPI2_SendRecv(0x51);
    SPI2_SendRecv((uint8_t)(addr >> 24));
    SPI2_SendRecv((uint8_t)(addr >> 16));
    SPI2_SendRecv((uint8_t)(addr >> 8));
    SPI2_SendRecv((uint8_t)(addr));
    SPI2_SendRecv(0xFF);

    cnt = 0xFFFu;
    do { b = SPI2_SendRecv(0xFF); } while (b == 0xFFu && --cnt);
    if (b != 0x00u) { BSP_SD_CS_High(); CPU_CRITICAL_EXIT(); return 1; }

    cnt = 0xFFFu;
    do { b = SPI2_SendRecv(0xFF); } while (b == 0xFFu && --cnt);
    if (b != 0xFEu) { BSP_SD_CS_High(); CPU_CRITICAL_EXIT(); return 2; }

    for (uint16_t i = 0; i < SD_BLOCK_SIZE; i++)
        buf[i] = SPI2_SendRecv(0xFF);
    SPI2_SendRecv(0xFF); SPI2_SendRecv(0xFF);

    BSP_SD_CS_High();
    SPI2_SendRecv(0xFF);
    CPU_CRITICAL_EXIT();
    return 0;
}
