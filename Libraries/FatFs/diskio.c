/* Libraries/FatFs/diskio.c
 *
 * FatFs R0.15 disk I/O 适配层。
 *
 * 本文件是本任务的真正集成工作：将 FatFs 的 disk_* API 接到 bsp_spi_sd.c
 * 的 SD 卡 SPI 模式块读驱动上。FatFs 上层 (ff.c) 通过 disk_read 读 SD 卡。
 *
 * 重要：用户从 FatFs R0.15 源码包替换 ff.c 时，请勿覆盖本文件 —— 它是
 * 针对本工程 SD 卡 (SPI 模式) 写的适配，与官方包附带的 diskio 模板不同。
 *
 * 限制：
 *   - 仅支持 1 个物理卷 (pdrv == 0)，对应 SD 卡。
 *   - 只读 (FF_FS_READONLY == 1)，disk_write 立即返回 RES_WRPRT。
 *   - disk_ioctl 仅实现 GET_SECTOR_SIZE / GET_SECTOR_COUNT / GET_BLOCK_SIZE
 *     / CTRL_SYNC（GET_SECTOR_COUNT 返回 0，FatFs 在 FF_FS_READONLY+最小化
 *     下不会用到卷容量，仅 f_size 需要从 FAT 项推导）。
 *
 * 依赖（已在 bsp_spi_sd.h / bsp_gpio.h 中提供）：
 *   - BSP_SPI2_Init()  : SPI2 + GPIO 配置
 *   - SD_SPI_Init()    : SD 卡 SPI 模式初始化 (CMD0 -> ACMD41 带 HCS 位)
 *   - SD_SPI_ReadBlock(): CMD17 单块读 (512 字节，SDHC 下按扇区号寻址)
 *   - SD_BLOCK_SIZE = 512
 */
#include "ff.h"         /* 间接引入 integer.h / ffconf.h / diskio.h */
#include "diskio.h"
#include "bsp_spi_sd.h" /* SD 卡块读驱动 */
#include "bsp_usart.h"   /* BSP_USART2_Printf for sector0 debug */
#include "bsp_gpio.h"   /* BSP_SD_CS_Low/High（SD_SPI_Init 内部已用） */

/* 单卷状态：b7 = 已初始化标志 (位掩码用 STA_NOINIT 取反) */
static DSTATUS g_sd_stat = STA_NOINIT;

/* MBR 分区映射：逻辑卷 0 = 物理驱动器 0, 第一个分区 */
PARTITION VolToPart[FF_VOLUMES] = {
    {0, 1},  /* pdrv=0, 分区 1 */
};

/*---------------------------------------------------------------------------*/
/* disk_initialize: 挂载时由 f_mount (实际是 f_open 路径) 触发                */
/*---------------------------------------------------------------------------*/
DSTATUS disk_initialize (BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT;          /* 本工程只支持 1 卷 */
    }

    /* SPI2 + SD 卡 SPI 模式初始化 */
    BSP_SPI2_Init();
    if (SD_SPI_Init() == 0u) {
        g_sd_stat &= ~STA_NOINIT;   /* 清除未初始化标志 */
    } else {
        g_sd_stat |= STA_NOINIT;    /* 保持未就绪 */
    }

    return g_sd_stat;
}

/*---------------------------------------------------------------------------*/
/* disk_status: 返回当前介质状态                                              */
/*---------------------------------------------------------------------------*/
DSTATUS disk_status (BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT;
    }
    return g_sd_stat;
}

/*---------------------------------------------------------------------------*/
/* disk_read: 多块读 —— 循环调用 SD_SPI_ReadBlock                             */
/*   buff   : 目标缓冲（由 FatFs 提供，FF_MAX_SS = 512 字节对齐足够）         */
/*   sector : LBA 起始扇区号                                                  */
/*   count  : 连续扇区数                                                      */
/*   地址约定：SD_SPI_Init 中 ACMD41 设置了 HCS 位 (0x40000000)，使卡进入    */
/*     SDHC 模式（>=2GB 卡）。SDHC 下 CMD17 接收扇区号，故直接传 FatFs 的 LBA */
/*     给 SD_SPI_ReadBlock。SDSC (<=2GB) 卡需改为 sector * SD_BLOCK_SIZE。   */
/*---------------------------------------------------------------------------*/
DRESULT disk_read (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv != 0) {
        return RES_PARERR;
    }
    if (g_sd_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }
    if (!buff || count == 0u) {
        return RES_PARERR;
    }

    for (UINT i = 0u; i < count; i++) {
        /* SDHC: ACMD41 已置 HCS 位 -> CMD17 接收扇区号，直接传 LBA。
           SDSC (<=2GB) 卡则需改为 (sector+i) * SD_BLOCK_SIZE 字节地址。 */
        if (SD_SPI_ReadBlock((uint32_t)(sector + i), buff + (i * SD_BLOCK_SIZE)) != 0u) {
            return RES_ERROR;
        }
    }

    /* 调试：扇区 0 首 16 字节 + 偏移 0x1BE 分区表首字节 */
    if (sector == 0u && count > 0u) {
        BSP_USART2_Printf("[SD] sector0[0..15]:"
                          " %02X %02X %02X %02X %02X %02X %02X %02X"
                          " %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                          buff[0], buff[1], buff[2], buff[3],
                          buff[4], buff[5], buff[6], buff[7],
                          buff[8], buff[9], buff[10], buff[11],
                          buff[12], buff[13], buff[14], buff[15]);
        BSP_USART2_Printf("[SD] sector0[0x1BE]: %02X %02X %02X %02X"
                          "  (partition type=0x%02X)\r\n",
                          buff[0x1BE], buff[0x1BF], buff[0x1C0], buff[0x1C1],
                          buff[0x1C2]);
    }

    return RES_OK;
}

/*---------------------------------------------------------------------------*/
/* disk_write: 只读配置下不可写                                              */
/*---------------------------------------------------------------------------*/
#if !FF_FS_READONLY
DRESULT disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    (void)pdrv; (void)buff; (void)sector; (void)count;
    return RES_WRPRT;
}
#endif

/*---------------------------------------------------------------------------*/
/* disk_ioctl: 提供给 FatFs 的卷信息                                         */
/*---------------------------------------------------------------------------*/
DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv != 0) {
        return RES_PARERR;
    }
    if (g_sd_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        /* 只读，无需同步；SD_SPI_ReadBlock 内部已 CS_High 结束事务 */
        break;

    case GET_SECTOR_SIZE:
        *(WORD*)buff = (WORD)SD_BLOCK_SIZE;
        break;

    case GET_BLOCK_SIZE:
        /* 擦除块大小（个扇区），无意义对只读返回 1 */
        *(DWORD*)buff = 1u;
        break;

    case GET_SECTOR_COUNT:
        /* 卷容量未知（CMD17 不读 CSD）；返回 0，FatFs 在最小化+只读下不会用到 */
        *(DWORD*)buff = 0u;
        break;

    default:
        return RES_PARERR;
    }

    return RES_OK;
}

/*---------------------------------------------------------------------------*/
/* get_fattime: 返回固定时间戳（FF_FS_NORTC == 1 时不会被调用）              */
/*   FatFs 时间戳格式 (DWORD)：                                              */
/*     bit31:25 年 (0..127, 0=1980)                                          */
/*     bit24:21 月 (1..12)                                                   */
/*     bit20:16 日 (1..31)                                                   */
/*     bit15:11 时 (0..23)                                                   */
/*     bit10:5  分 (0..59)                                                   */
/*     bit4:0   秒/2 (0..29)                                                 */
/*   这里返回 2024-01-01 00:00:00。                                          */
/*---------------------------------------------------------------------------*/
DWORD get_fattime (void)
{
    return ((DWORD)(2024u - 1980u) << 25)
         | ((DWORD)1u << 21)
         | ((DWORD)1u << 16);
}
