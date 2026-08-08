/* app/sd_card.h
 *
 * SD 卡应用层封装：基于 FatFs (f_mount/f_open/f_read/f_close/f_size/f_stat)
 * 暴露给下载状态机使用的简化接口。本层不依赖 SPL/HAL，仅依赖 FatFs API，
 * 便于在 PC 单元测试桩下复用。
 *
 * 文件名取自 app_cfg.h 的 APP_SD_BIN_FILENAME ("APP.bin")。
 * SD 卡块大小固定 512 字节，与 FatFs FF_MAX_SS 一致。
 */
#ifndef __SD_CARD_H
#define __SD_CARD_H

#include <stdint.h>

/* 错误码：上层（下载状态机）按非零即失败处理 */
#define SD_OK              0u
#define SD_ERR_NO_CARD     1u   /* SD 卡未初始化/未插卡 */
#define SD_ERR_MOUNT       2u   /* f_mount 失败 */
#define SD_ERR_NO_FILE     3u   /* APP.bin 不存在 */
#define SD_ERR_OPEN        4u   /* f_open 失败 */
#define SD_ERR_READ        5u   /* f_read 失败 */
#define SD_ERR_PARAM       6u   /* 参数非法 */

/* 一次读取的最大块数（按 SD_BLOCK_SIZE=512 对齐下载器分段） */
#define SD_READ_CHUNK      512u

/*---------------------------------------------------------------------------*/
/* 生命周期                                                                   */
/*---------------------------------------------------------------------------*/

/* 初始化：调用 BSP_SPI2_Init + SD_SPI_Init + f_mount。
   返回 SD_OK 或 SD_ERR_*。
   - SD_ERR_NO_CARD: SD_SPI_Init 失败 (CMD0/ACMD41 未通过)
   - SD_ERR_MOUNT  : f_mount 返回非 OK */
uint8_t SD_Init(void);

/* 探测 SD 卡是否存在（SD_SPI_Init 成功且 f_mount 成功后视为存在）。
   不做重初始化，只查询内部就绪标志。 */
uint8_t SD_IsPresent(void);

/* 扫描根目录，找到版本号最高的 IL_800_XXX_XXX.BIN 文件。
   成功返回 SD_OK，g_fw_filename / g_fw_ver 被填充。
   失败返回 SD_ERR_NO_FILE（没找到匹配文件）。
   版本号编码: fw_ver = major * 256 + sub */
uint8_t SD_FindLatestFirmware(void);

/* 查询是否已找到固件文件。
   返回 1 = 已找到，0 = 未找到/未就绪。 */
uint8_t SD_FileExists(void);

/*---------------------------------------------------------------------------*/
/* 文件读写                                                                    */
/*---------------------------------------------------------------------------*/

/* 打开 APP.bin（FA_READ）。
   返回 SD_OK 或 SD_ERR_OPEN / SD_ERR_NO_CARD。 */
uint8_t SD_FileOpen(void);

/* 读取已打开文件：从当前 fptr 读 btr 字节到 buf，实际读出 *br。
   返回 SD_OK 或 SD_ERR_READ / SD_ERR_PARAM。
   注意：本接口将 FatFs 的 UINT 映射为 uint16_t，因为下载器一次最多读
   APP_SEGMENT_SIZE (256) 字节，足以容纳。 */
uint8_t SD_FileRead(uint8_t *buf, uint16_t btr, uint16_t *br);

/* 将已打开文件的读写指针定位到 offset（转发到 FatFs f_lseek）。
   返回 0 = 成功，1 = 失败（未就绪 / 文件未打开 / f_lseek 失败）。
   用于下载状态机重试时重定位，替代 close+reopen+丢弃前缀的字节扫描。 */
uint8_t SD_FileSeek(uint32_t offset);

/* 返回已打开文件的大小 (字节)。未打开时返回 0。 */
uint32_t SD_FileGetSize(void);

/* 关闭已打开的文件。重复关闭安全。 */
void SD_FileClose(void);

/*---------------------------------------------------------------------------*/
/* 读写测速（块级，绕过 FatFs）                                                */
/*   读：连续读 N 个扇区，测平均读速度                                          */
/*   写：备份扇区 0 -> 写测试 pattern -> 读回校验 -> 恢复扇区 0，测平均写速度   */
/*   结果通过 USART2 输出 [SD] 速度报告                                        */
/*---------------------------------------------------------------------------*/
void SD_SpeedTest(void);

/* 打印 SD 卡信息：卡类型、CSD 版本、总容量、分区1信息（起始/大小/类型） */
void SD_PrintInfo(void);

#endif /* __SD_CARD_H */
