/* app/sd_card.c
 *
 * SD 卡应用层封装实现：使用 FatFs API (f_mount/f_open/f_read/f_close/f_size/
 * f_stat) 把下载状态机需要的"读 APP.bin"流程封装成简单状态接口。
 *
 * ===== FatFs 封装说明 =====
 *
 * 本模块是对 Libraries/FatFs/ff.c 的薄封装，为上层（task_download.c）提供
 * 不依赖 FatFs 头文件的简洁接口。封装决策：
 *
 * 1. 模块内部静态持有 FATFS + FIL 实例，上层只看到 uint8_t 错误码和
 *    uint32_t 返回值，无需引用 ff.h。
 * 2. SD_FileOpen 幂等：若文件已打开则直接返回 SD_OK（复用现有 FIL），
 *    避免重复 open 把 fptr 重置为零。task_download 在 SD_CHECK 阶段
 *    多次 Open/Close/Reopen 时利用此特性。
 * 3. 错误码翻译：FatFs FRESULT → SD_ERR_* 枚举，统一错误语义。
 *    FR_NOT_READY → SD_ERR_MOUNT，FR_NO_FILE → SD_ERR_NO_FILE 等。
 * 4. ready 标志独立于文件 open 状态 —— SD_IsPresent 仅检查 SD 卡+卷就绪，
 *    不检查文件是否打开。
 * 5. Seek 支持：SD_FileSeek 调用 f_lseek，供下载状态机重试重定位使用。
 *
 * ===== 文件读取流程图 =====
 *
 *   上电 → SD_Init()
 *          ├─ BSP_SPI2_Init()     // SPI2 硬件初始化
 *          ├─ SD_SPI_Init()       // SD 卡 SPI 模式（CMD0→CMD8→ACMD41）
 *          ├─ f_mount(&g_fs,"0:",1)  // 挂载 FAT 卷
 *          └─ g_ready = 1
 *
 *   下载触发 → SD_FileOpen()
 *              ├─ f_open(&g_file, "APP.bin", FA_READ|FA_OPEN_EXISTING)
 *              └─ g_opened = 1
 *
 *   SEND_DATA 循环：
 *     SD_FileGetSize() → f_size()
 *     SD_FileRead(buf, seg_size, &br) → f_read()
 *     SD_FileSeek(offset) → f_lseek()
 *
 *   传输结束/错误 → SD_FileClose()
 *                   └─ f_close() + g_opened = 0
 *
 * 设计：
 *   - 模块内部持有一个 FATFS 卷对象和一个 FIL 文件对象（静态分配，避免上层
 *     暴露 FatFs 类型）。
 *   - SD_Init: 调 BSP_SPI2_Init + SD_SPI_Init 初始化硬件与 SD 卡 SPI 模式，
 *     再 f_mount 挂载 FAT 卷。
 *   - SD_IsPresent: 仅返回模块内部就绪标志（不重新发 CMD0）。
 *   - SD_FileExists: f_stat("APP.bin") == FR_OK ?
 *   - SD_FileOpen/Read/GetSize/Close: 直接转发到 FatFs，做错误码翻译。
 *
 * 桩说明：Libraries/FatFs/ff.c 当前为桩（返回 FR_NOT_READY/FR_NO_FILE），
 * 因此 SD_Init 会返回 SD_ERR_MOUNT 直至用户用官方 FatFs R0.15 ff.c 替换。
 * 桩下 disk_initialize 仍会真正调用 SD_SPI_Init 完成 SD 卡硬件初始化。
 */
#include "sd_card.h"
#include "bsp_spi_sd.h"        /* BSP_SPI2_Init / SD_SPI_Init / SD_BLOCK_SIZE */
#include "bsp_usart.h"          /* BSP_USART2_Printf for [SD] debug */
#include "ff.h"                 /* FatFs API + FATFS/FIL 类型 */
#include "app_cfg.h"            /* APP_SD_BIN_FILENAME */

/*---------------------------------------------------------------------------*/
/* 模块静态状态                                                                */
/*---------------------------------------------------------------------------*/

static FATFS  g_fs;             /* 卷对象（含 512 字节 win[] 缓冲） */
static FIL    g_file;           /* 当前打开的文件对象 */
static uint8_t g_ready = 0u;    /* SD 卡+卷就绪标志 (1=就绪) */
static uint8_t g_opened = 0u;   /* 文件已打开标志 */

/*---------------------------------------------------------------------------*/
/* SD_Init                                                                    */
/*---------------------------------------------------------------------------*/
uint8_t SD_Init(void)
{
    FRESULT fr;

    /* 1) SD 卡 SPI 模式初始化（BSP_SPI2_Init 已在 BSP_Init 中调用） */
    if (SD_SPI_Init() != 0u) {
        g_ready = 0u;
        return SD_ERR_NO_CARD;
    }

    /* 2) 挂载 FAT 卷 (0: = 第一个卷) */
    fr = f_mount(&g_fs, "0:", 1u);
    if (fr != FR_OK) {
        g_ready = 0u;
        BSP_USART2_Printf("[SD] f_mount fail, FRESULT=%u\r\n", (unsigned int)fr);
        return SD_ERR_MOUNT;
    }

    g_ready = 1u;
    BSP_USART2_Printf("[SD] Mount OK, FAT%d\r\n",
                      (g_fs.fs_type == 2) ? 16 : 32);
    return SD_OK;
}

/*---------------------------------------------------------------------------*/
/* SD_IsPresent                                                               */
/* 返回 1 表示 SD 卡就绪。若之前未检测到卡，尝试重新初始化（支持热插拔）。     */
/*---------------------------------------------------------------------------*/
uint8_t SD_IsPresent(void)
{
    if (g_ready) { return 1u; }

    /* 重新尝试初始化：SD 卡可能刚插入或之前供电未稳定 */
    if (SD_Init() == SD_OK) {
        return 1u;
    }
    return 0u;
}

/*---------------------------------------------------------------------------*/
/* SD_FileExists: f_stat 查询根目录 APP.bin                                   */
/*---------------------------------------------------------------------------*/
uint8_t SD_FileExists(void)
{
    FILINFO fno;

    if (!g_ready) {
        return 0u;
    }
    if (f_stat(APP_SD_BIN_FILENAME, &fno) == FR_OK) {
        return 1u;
    }
    return 0u;
}

/*---------------------------------------------------------------------------*/
/* SD_FileOpen: 以只读方式打开 APP.bin                                        */
/*---------------------------------------------------------------------------*/
uint8_t SD_FileOpen(void)
{
    FRESULT fr;

    if (!g_ready) {
        return SD_ERR_NO_CARD;
    }
    if (g_opened) {
        /* 已打开 -- 直接复用，避免上层重复 Open 把 fptr 重置 */
        return SD_OK;
    }

    fr = f_open(&g_file, APP_SD_BIN_FILENAME, FA_READ | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        g_opened = 0u;
        if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
            return SD_ERR_NO_FILE;
        }
        return SD_ERR_OPEN;
    }

    g_opened = 1u;
    return SD_OK;
}

/*---------------------------------------------------------------------------*/
/* SD_FileRead: 从当前 fptr 读 btr 字节                                       */
/*---------------------------------------------------------------------------*/
uint8_t SD_FileRead(uint8_t *buf, uint16_t btr, uint16_t *br)
{
    FRESULT fr;
    UINT    br_actual = 0u;

    if (buf == (uint8_t*)0 || br == (uint16_t*)0 || btr == 0u) {
        return SD_ERR_PARAM;
    }
    *br = 0u;
    if (!g_opened) {
        return SD_ERR_OPEN;
    }

    fr = f_read(&g_file, buf, (UINT)btr, &br_actual);
    if (fr != FR_OK) {
        return SD_ERR_READ;
    }
    *br = (uint16_t)br_actual;
    return SD_OK;
}

/*---------------------------------------------------------------------------*/
/* SD_FileSeek: 重定位已打开文件的读写指针 (f_lseek)                          */
/*   供下载状态机重试重定位使用，替代 close+reopen+丢弃前缀的字节扫描。       */
/*---------------------------------------------------------------------------*/
uint8_t SD_FileSeek(uint32_t offset)
{
    if (g_ready == 0u) {
        return 1u;
    }
    if (g_opened == 0u) {
        return 1u;
    }
    if (f_lseek(&g_file, (FSIZE_t)offset) != FR_OK) {
        return 1u;
    }
    return 0u;
}

/*---------------------------------------------------------------------------*/
/* SD_FileGetSize: 已打开文件大小                                             */
/*---------------------------------------------------------------------------*/
uint32_t SD_FileGetSize(void)
{
    if (!g_opened) {
        return 0u;
    }
    return (uint32_t)f_size(&g_file);
}

/*---------------------------------------------------------------------------*/
/* SD_FileClose                                                               */
/*---------------------------------------------------------------------------*/
void SD_FileClose(void)
{
    if (g_opened) {
        (void)f_close(&g_file);
        g_opened = 0u;
    }
}

/*---------------------------------------------------------------------------*/
/* SD_SpeedTest: 块级读写测速                                                 */
/*   测试扇区: 10~13 (MBR 与分区1之间的保留区，不含文件系统数据，安全)          */
/*   写: 2048 字节(4扇区) pattern -> 读回校验                                   */
/*   读: 2048 字节(4扇区) 测速                                                 */
/*---------------------------------------------------------------------------*/
#include <string.h>
void SD_SpeedTest(void)
{
    #define SPEED_TEST_SECTOR  10u
    #define SPEED_TEST_NSEC    4u                          /* 4 扇区 = 2048 字节 */
    static uint8_t wbuf[SPEED_TEST_NSEC * SD_BLOCK_SIZE];  /* 2048B 写缓冲 */
    static uint8_t rbuf[SPEED_TEST_NSEC * SD_BLOCK_SIZE];  /* 2048B 读缓冲 */
    OS_ERR  err;
    uint32_t i, start, elapsed, bytes;

    BSP_USART2_Printf("\r\n[SD] ===== Speed Test (sectors %lu~%lu, %lu bytes) =====\r\n",
                      (unsigned long)SPEED_TEST_SECTOR,
                      (unsigned long)(SPEED_TEST_SECTOR + SPEED_TEST_NSEC - 1u),
                      (unsigned long)(SPEED_TEST_NSEC * SD_BLOCK_SIZE));

    /* ---- 填充测试 pattern ---- */
    for (i = 0u; i < sizeof(wbuf); i++) {
        wbuf[i] = (uint8_t)(i ^ 0xA5u);
    }

    /* ---- 写测速: 4 扇区 ---- */
    start = OSTimeGet(&err);
    for (i = 0u; i < SPEED_TEST_NSEC; i++) {
        if (SD_SPI_WriteBlock(SPEED_TEST_SECTOR + i, &wbuf[i * SD_BLOCK_SIZE]) != 0u) {
            BSP_USART2_Printf("[SD] Write fail @ sector %lu\r\n",
                              (unsigned long)(SPEED_TEST_SECTOR + i));
            return;
        }
    }
    {
        uint32_t w_elapsed = OSTimeGet(&err) - start;     /* 500Hz tick = 2ms/tick */
        uint32_t w_bytes = SPEED_TEST_NSEC * SD_BLOCK_SIZE;
        uint32_t w_kbs   = (w_bytes * 500u / w_elapsed) / 1024u;
        uint32_t w_frac  = ((w_bytes * 500u * 100u / w_elapsed) / 1024u) % 100u;
        BSP_USART2_Printf("[SD] Write: %lu bytes in %lu ms -> %lu.%02lu KB/s\r\n",
                          (unsigned long)w_bytes, (unsigned long)(w_elapsed * 2u),
                          (unsigned long)w_kbs, (unsigned long)w_frac);
    }

    /* ---- 读回校验 + 读测速 ---- */
    start = OSTimeGet(&err);
    for (i = 0u; i < SPEED_TEST_NSEC; i++) {
        if (SD_SPI_ReadBlock(SPEED_TEST_SECTOR + i, &rbuf[i * SD_BLOCK_SIZE]) != 0u) {
            BSP_USART2_Printf("[SD] Read fail @ sector %lu\r\n",
                              (unsigned long)(SPEED_TEST_SECTOR + i));
            return;
        }
    }
    elapsed = OSTimeGet(&err) - start;
    bytes = SPEED_TEST_NSEC * SD_BLOCK_SIZE;

    /* ---- 校验数据 ---- */
    if (memcmp(rbuf, wbuf, sizeof(wbuf)) != 0) {
        BSP_USART2_Printf("[SD] Verify FAIL (data mismatch)\r\n");
    } else {
        BSP_USART2_Printf("[SD] Verify OK\r\n");
    }

    /* ---- 读速度 ---- */
    {
        uint32_t r_kbs   = (bytes * 500u / elapsed) / 1024u;
        uint32_t r_frac  = ((bytes * 500u * 100u / elapsed) / 1024u) % 100u;
        BSP_USART2_Printf("[SD] Read:  %lu bytes in %lu ms -> %lu.%02lu KB/s\r\n",
                          (unsigned long)bytes, (unsigned long)(elapsed * 2u),
                          (unsigned long)r_kbs, (unsigned long)r_frac);
    }

    BSP_USART2_Printf("[SD] ===== Test Done =====\r\n");
}

/*---------------------------------------------------------------------------*/
/* SD_PrintInfo: 打印卡容量 + 分区信息                                        */
/*   CMD9 读 CSD -> 算总容量                                                  */
/*   读扇区0(MBR) -> 解析分区1                                                */
/*---------------------------------------------------------------------------*/
void SD_PrintInfo(void)
{
    uint8_t csd[16];
    uint32_t capacity_bytes = 0u;
    uint32_t capacity_mb = 0u;

    BSP_USART2_Printf("\r\n[SD] ===== Card Info =====\r\n");

    /* ---- 卡类型 ---- */
    BSP_USART2_Printf("[SD] Type: %s\r\n", SD_SPI_IsBlockAddr() ? "SDHC/SDXC" : "SDSC");

    /* ---- CMD9 读 CSD, 算容量 ---- */
    if (SD_SPI_ReadCSD(csd) != 0u) {
        BSP_USART2_Printf("[SD] CMD9 (ReadCSD) fail\r\n");
    } else {
        uint8_t csd_ver = (csd[0] >> 6) & 0x03u;   /* CSD_STRUCTURE */
        BSP_USART2_Printf("[SD] CSD version: %u\r\n", (unsigned)csd_ver);

        if (csd_ver == 0u) {
            /* CSD v1.0 (SDSC) */
            uint32_t read_bl_len = csd[5] & 0x0Fu;
            uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10)
                            | ((uint32_t)csd[7] << 2)
                            | ((uint32_t)csd[8] >> 6);
            uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03u) << 1)
                                 | ((uint32_t)csd[10] >> 7);
            uint32_t block_len = 1u << read_bl_len;
            uint32_t mult = 1u << (c_size_mult + 2u);
            uint32_t block_nr = (c_size + 1u) * mult;
            capacity_bytes = block_nr * block_len;
        } else if (csd_ver == 1u) {
            /* CSD v2.0 (SDHC/SDXC) */
            uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16)
                            | ((uint32_t)csd[8] << 8)
                            | (uint32_t)csd[9];
            capacity_bytes = (c_size + 1u) * 512u * 1024u;   /* 512KB 单位 */
        }
        capacity_mb = capacity_bytes / (1024u * 1024u);
        BSP_USART2_Printf("[SD] Capacity: %lu MB (%lu bytes, %lu sectors)\r\n",
                          (unsigned long)capacity_mb,
                          (unsigned long)capacity_bytes,
                          (unsigned long)(capacity_bytes / 512u));
    }

    /* ---- 读 MBR, 解析分区1 ---- */
    {
        static uint8_t mbr[SD_BLOCK_SIZE];
        if (SD_SPI_ReadBlock(0u, mbr) != 0u) {
            BSP_USART2_Printf("[SD] Read MBR fail\r\n");
        } else {
            uint8_t  ptype = mbr[0x1C2u];
            uint32_t p_start = (uint32_t)mbr[0x1C6u]
                             | ((uint32_t)mbr[0x1C7u] << 8)
                             | ((uint32_t)mbr[0x1C8u] << 16)
                             | ((uint32_t)mbr[0x1C9u] << 24);
            uint32_t p_size = (uint32_t)mbr[0x1CAu]
                            | ((uint32_t)mbr[0x1CBu] << 8)
                            | ((uint32_t)mbr[0x1CCu] << 16)
                            | ((uint32_t)mbr[0x1CDu] << 24);
            uint32_t p_mb = p_size / 2u / 1024u;            /* sectors*512/1024/1024 */
            BSP_USART2_Printf("[SD] Partition1: type=0x%02X start_LBA=%lu size=%lu sectors (%lu MB)\r\n",
                              (unsigned)ptype, (unsigned long)p_start,
                              (unsigned long)p_size, (unsigned long)p_mb);
        }
    }
    BSP_USART2_Printf("[SD] ========================\r\n");
}
