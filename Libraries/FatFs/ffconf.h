/* Libraries/FatFs/ffconf.h
 *
 * FatFs 配置（按 task-4-brief Step 2 关键配置）。
 *
 * 取值：
 *   FF_DEBUG = 1    使能调试输出，ff.c 内部通过 BSP_USART2_Printf 打印 FAT 跟踪信息。
 *                    调试输出仅影响串口输出，不改变 FAT 解析逻辑；关闭后零开销。
 *   FF_FS_READONLY = 1   只读文件系统（下载器只需读 APP.bin）
 *   FF_FS_MINIMIZE = 0   保留 f_lseek（下载状态机重试重定位用，见 SD_FileSeek）
 *   FF_USE_LFN     = 0   不使用长文件名（APP.bin 为 8.3 名）
 *   FF_VOLUMES     = 1   单卷（仅 SD 卡）
 *   FF_MAX_SS      = 512 SD 卡固定 512 字节扇区
 *
 * 其余选项沿用 FatFs R0.15 默认值，保持与官方源码兼容。
 * 用户用官方 ff.c 替换桩时无需修改本文件。
 */
#ifndef _FFCONF
#define FFCONF_DEF  80286   /* FatFs R0.15 — 必须匹配 ff.h 的 FF_DEFINED */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY      1   /* 1 = 禁用写 API（f_write/f_unlink 等）。注：f_lseek 在只读下仍可用 */
#define FF_FS_MINIMIZE      0   /* 0 = 保留 f_lseek（重试重定位用）；f_sync/f_chdir 等未使用 */
#define FF_USE_STRFUNC      0   /* 0 = 不启用 f_gets/f_putc 等 */
#define FF_USE_FIND         0
#define FF_USE_MKFS         0
#define FF_USE_FORWARD      0

/*---------------------------------------------------------------------------/
/ Namespace/Locale Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE        437
#define FF_USE_LFN          0   /* 0 = 仅 8.3 文件名（APP.bin） */
#define FF_MAX_LFN          255 /* FF_USE_LFN>0 时生效，保留默认 */
#define FF_LFN_UNICODE      0   /* 0 = ANSI/OEM */
#define FF_LFN_BUF          255
#define FF_SFN_BUF          12
#define FF_STRF_ENCODE      3

/*---------------------------------------------------------------------------/
/ System/Drive Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES          1   /* 单卷：SD 卡 */
#define FF_STR_VOLUME_ID    0
#define FF_VOLUME_STRS      "RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_MULTI_PARTITION  0
#define FF_MIN_SS           512
#define FF_MAX_SS           512 /* 固定 512 字节扇区 */
#define FF_LBA64            0
#define FF_USE_TRIM         0
#define FF_FS_TINY          0   /* 0 = FIL 自带数据缓冲（更省事，吃一点 RAM） */
#define FF_FS_EXFAT         0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_NORTC         1   /* 1 = 不使用实时时钟，固定时间戳 */
#define FF_NORTC_MON        1
#define FF_NORTC_MDAY       1
#define FF_NORTC_YEAR       2024
#define FF_FS_NOFSINFO      0
#define FF_FS_LOCK          0
#define FF_FS_RPATH         0   /* 0 = 不支持相对路径（仅用根目录 APP.bin） */
#define FF_FS_REENTRANT     1   /* 1 = UCOS-III 互斥量保护（ff_ucos3.c 实现钩子） */

/* ---- 调试 ---- */
#define FF_DEBUG            1   /* 1 = 使能 ff.c 内 FAT 跟踪输出（BSP_USART2_Printf） */

#endif /* _FFCONF */
