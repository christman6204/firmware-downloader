/* Libraries/FatFs/diskio.h
 *
 * FatFs R0.15 风格的 disk I/O 接口声明。
 *
 * 实现在 diskio.c，对接 bsp_spi_sd.c（SD 卡 SPI 模式块读）。
 * 用户用官方 FatFs R0.15 ff.c 替换桩时无需修改本文件。
 */
#ifndef _DISKIO_DEFINED
#define _DISKIO_DEFINED

#include "integer.h"
#include "ffconf.h"

/* Status of Disk Functions */
typedef BYTE    DSTATUS;

/* Results of Disk Functions */
typedef enum {
    RES_OK = 0,     /* 0: Successful */
    RES_ERROR,      /* 1: R/W Error */
    RES_WRPRT,      /* 2: Write Protected */
    RES_NOTRDY,     /* 3: Not Ready */
    RES_PARERR      /* 4: Invalid Parameter */
} DRESULT;

/* Disk Status Bits (DSTATUS) */
#define STA_NOINIT      0x01    /* Drive not initialized */
#define STA_NODISK      0x02    /* No medium in the drive */
#define STA_PROTECT     0x04    /* Write protected */

/* Command code for disk_ioctl function */
#define CTRL_SYNC           0   /* Complete pending write process (needed at FF_FS_READONLY == 0) */
#define GET_SECTOR_COUNT    1   /* Get media size (number of sectors) */
#define GET_SECTOR_SIZE     2   /* Get sector size */
#define GET_BLOCK_SIZE      3   /* Get erase block size */
#define CTRL_TRIM           4   /* Inform device that the sectors can be erased */

/* LBA type (default = DWORD; LBA64 would use QWORD) */
#if FF_LBA64
typedef QWORD LBA_t;
#else
typedef DWORD LBA_t;
#endif

/* --------------------------------------- */
/* Prototypes for disk control functions   */
/* --------------------------------------- */
DSTATUS disk_initialize (BYTE pdrv);
DSTATUS disk_status     (BYTE pdrv);
DRESULT disk_read       (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count);
DRESULT disk_write      (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count);
DRESULT disk_ioctl      (BYTE pdrv, BYTE cmd, void* buff);

DWORD   get_fattime     (void);

#endif /* _DISKIO_DEFINED */
