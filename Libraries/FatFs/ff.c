/* Libraries/FatFs/ff.c
 *
 * ============================================================================
 *                       STUB - 请用官方 FatFs R0.15 ff.c 替换
 * ============================================================================
 *
 * 本文件是 FatFs 文件系统层的桩实现：所有 f_* API 立即返回 FR_NOT_READY
 * (或 FR_NO_FILE)，目的是让本工程能链接通过并提供完整 API 符号。
 *
 * 它 **不会真正解析 FAT16/FAT32 文件系统**，因此 SD_FileOpen / SD_FileRead
 * 等上层接口在桩下无法返回真实数据。
 *
 * 正确使用方式：
 *   1. 从 http://elm-chan.org/fsw/ff/00index_e.html 下载 FatFs R0.15。
 *   2. 用官方源码包中的 ff.c 替换本文件（保留本目录下的 diskio.c -- 它是
 *      针对本工程 SD 卡 SPI 模式编写的适配层，是真实代码，不要覆盖）。
 *   3. 如需要，可一并替换 ff.h / ffconf.h / integer.h / diskio.h 为官方
 *      版本（本目录提供的版本与 R0.15 一致，二进制兼容，无需替换亦可）。
 *
 * 本桩保留全部需要的 API 符号 (f_mount/f_open/f_read/f_close/f_size/f_stat)
 * 以便 app/sd_card.c 能链接通过；运行时上层调用会因返回 FR_NOT_READY 而
 * 走错误路径。
 * ============================================================================
 */
#include "ff.h"
#include "diskio.h"

/*---------------------------------------------------------------------------*/
/* f_mount: 桩 -- 直接调用 disk_initialize 探测介质；不挂载任何 FAT 卷        */
/*---------------------------------------------------------------------------*/
FRESULT f_mount (FATFS* fs, const TCHAR* path, BYTE opt)
{
    (void)path;
    (void)opt;

    if (fs == (FATFS*)0) {
        /* 卸载卷：桩下不维护挂载状态，直接成功 */
        return FR_OK;
    }

    /* 介质探测：调用 disk_initialize 触发 SD 卡 SPI 模式初始化 */
    if (disk_initialize(0) & STA_NOINIT) {
        fs->fs_type = 0;
        return FR_NOT_READY;
    }

    /* 真正的 FAT 解析在桩里不做：返回 FR_NO_FILESYSTEM 让上层知道 */
    fs->fs_type = 0;
    return FR_NO_FILESYSTEM;
}

/*---------------------------------------------------------------------------*/
/* f_open: 桩 -- 始终找不到文件                                              */
/*---------------------------------------------------------------------------*/
FRESULT f_open (FIL* fp, const TCHAR* path, BYTE mode)
{
    (void)fp;
    (void)path;
    (void)mode;
    return FR_NOT_READY;
}

/*---------------------------------------------------------------------------*/
/* f_read: 桩 -- 永远读不到数据                                              */
/*---------------------------------------------------------------------------*/
FRESULT f_read (FIL* fp, void* buff, UINT btr, UINT* br)
{
    (void)fp;
    (void)buff;
    (void)btr;
    if (br) {
        *br = 0u;
    }
    return FR_NOT_READY;
}

/*---------------------------------------------------------------------------*/
/* f_close: 桩 -- 直接成功（无资源需要释放）                                  */
/*---------------------------------------------------------------------------*/
FRESULT f_close (FIL* fp)
{
    (void)fp;
    return FR_OK;
}

/*---------------------------------------------------------------------------*/
/* f_size: 桩 -- 返回 0                                                      */
/*---------------------------------------------------------------------------*/
FSIZE_t f_size (FIL* fp)
{
    if (fp == (FIL*)0) {
        return 0u;
    }
    return 0u;
}

/*---------------------------------------------------------------------------*/
/* f_stat: 桩 -- 始终找不到文件                                              */
/*---------------------------------------------------------------------------*/
FRESULT f_stat (const TCHAR* path, FILINFO* fno)
{
    (void)path;
    if (fno) {
        fno->fsize = 0u;
        fno->fdate = 0u;
        fno->ftime = 0u;
        fno->fattrib = 0u;
        fno->fname[0] = '\0';
    }
    return FR_NO_FILE;
}
