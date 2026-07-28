/* Libraries/FatFs/ff.h
 *
 * FatFs R0.15 公共 API 声明（最小子集，仅本工程用到的：f_mount/f_open/f_read/
 * f_close/f_size/f_stat）。
 *
 * 桩 ff.c 实现于同目录 ff.c（所有 f_* 返回 FR_NOT_READY / FR_NO_FILE），
 * 目的是让 app/sd_card.c 能编译链接。**桩下无法真正读 SD 卡上的 FAT 文件。**
 *
 * 升级到真实 FatFs：
 *   推荐用官方 FatFs R0.15 的 ff.c / ff.h / ffconf.h / integer.h / diskio.h
 *   整体覆盖本目录同名文件，仅保留本目录下的 diskio.c（SD 卡 SPI 适配层）。
 *   本头文件字段顺序近似 R0.15 但**不保证**与官方 ff.c 二进制兼容，因此
 *   务必连同 ff.c 一起替换，不要只替换 ff.c。
 */
#ifndef _FATFS
#define _FATFS

#include "integer.h"   /* FatFs 基本整数类型 */
#include "ffconf.h"    /* FatFs 配置选项 */
#include "diskio.h"    /* DSTATUS/DRESULT + disk_* 原型 */

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*
 *  Size/offset types
 *
 *  QWORD 在 integer.h 中已 typedef 为 uint64_t（diskio.h 的 LBA_t 在
 *  FF_LBA64 == 1 时使用）。FSIZE_t 在 FatFs R0.15 中默认 = DWORD
 *  (FF_LBA64 == 0)，与 LBA_t 同。
 *---------------------------------------------------------------------------*/
#ifndef FSIZE_t
#define FSIZE_t     DWORD
#endif

/* TCHAR */
#ifndef TCHAR
#define TCHAR      char
#endif

/*---------------------------------------------------------------------------*
 *  FRESULT
 *---------------------------------------------------------------------------*/
typedef enum {
    FR_OK = 0,              /* (0) Succeeded */
    FR_DISK_ERR,            /* (1) A hard error occurred in the low level disk I/O layer */
    FR_INT_ERR,             /* (2) Assertion failed */
    FR_NOT_READY,           /* (3) The physical drive cannot work */
    FR_NO_FILE,             /* (4) Could not find the file */
    FR_NO_PATH,             /* (5) Could not find the path */
    FR_INVALID_NAME,        /* (6) The path name format is invalid */
    FR_DENIED,              /* (7) Access denied due to prohibited access or directory full */
    FR_EXIST,               /* (8) Access denied due to prohibited access */
    FR_INVALID_OBJECT,      /* (9) The file/directory object is invalid */
    FR_WRITE_PROTECTED,     /* (10) The physical drive is write protected */
    FR_INVALID_DRIVE,       /* (11) The logical drive number is invalid */
    FR_NOT_ENABLED,         /* (12) The volume has no work area */
    FR_NO_FILESYSTEM,       /* (13) There is no valid FAT volume */
    FR_MKFS_ABORTED,        /* (14) The f_mkfs() aborted due to any problem */
    FR_TIMEOUT,             /* (15) Could not get a grant to access the volume within defined period */
    FR_LOCKED,              /* (16) The operation is rejected according to the file sharing policy */
    FR_NOT_ENOUGH_CORE,     /* (17) LFN working buffer could not be allocated */
    FR_TOO_MANY_OPEN_FILES, /* (18) Number of open files > FF_FS_LOCK */
    FR_INVALID_PARAMETER    /* (19) Given parameter is invalid */
} FRESULT;

/*---------------------------------------------------------------------------*
 *  File access mode / file seek origin / file attribute
 *---------------------------------------------------------------------------*/
#define FA_READ             0x01
#define FA_WRITE            0x02
#define FA_OPEN_EXISTING    0x00
#define FA_CREATE_NEW       0x04
#define FA_CREATE_ALWAYS    0x08
#define FA_OPEN_ALWAYS      0x10
#define FA_OPEN_APPEND      0x30

#define SEEK_SET            0
#define SEEK_CUR            1
#define SEEK_END            2

#define AM_RDO     0x01      /* Read only */
#define AM_HID     0x02      /* Hidden */
#define AM_SYS     0x04      /* System */
#define AM_DIR     0x10      /* Directory */
#define AM_ARC     0x20      /* Archive */

/*---------------------------------------------------------------------------*
 *  Filesystem object (FATFS)
 *
 *  字段命名/顺序近似官方 FatFs R0.15 FATFS，便于阅读。本工程仅用其作为
 *  f_mount 的承载结构。FF_FS_TINY == 0 -> win[] 大小 = FF_MAX_SS = 512。
 *  注意：不保证与官方 ff.c 二进制兼容，整体替换时连同 ff.h 一起换。
 *---------------------------------------------------------------------------*/
typedef struct {
    BYTE    fs_type;        /* Filesystem type (0, FS_FAT12, FS_FAT16, FS_FAT32 or FS_EXFAT) */
    BYTE    pdrv;           /* Associated physical drive */
    BYTE    n_fats;         /* Number of FATs (1 or 2) */
    BYTE    wflag;          /* win[] flag (b0:dirty) */
    BYTE    fsi_flag;       /* FSINFO flags (b7:disabled, b0:dirty) */
    WORD    id;             /* Volume mount ID */
    WORD    n_rootdir;      /* Number of root directory entries (FAT12/16) */
    WORD    csize;          /* Cluster size [sectors] */
#if FF_MAX_SS != FF_MIN_SS
    WORD    ssize;          /* Sector size (512, 1024, 2048 or 4096) */
#endif
    UINT    n_fatent;       /* Number of FAT entries (number of clusters + 2) */
    DWORD   fsize;          /* Number of sectors per FAT */
    LBA_t   volbase;        /* Volume base sector */
    LBA_t   fatbase;        /* FAT base sector */
    LBA_t   dirbase;        /* Root directory base sector/cluster */
    LBA_t   database;       /* Data base sector */
    LBA_t   winsect;        /* Current sector appearing in the win[] */
    BYTE    win[FF_MAX_SS]; /* Disk access window for Directory, FAT (and file data at tiny config) */
} FATFS;

/*---------------------------------------------------------------------------*
 *  File object (FIL)
 *
 *  桩用最小字段集（obj/flag/err/fptr/clust/sect/buf/blks）；real FatFs R0.15
 *  的 FIL 字段更多，整体替换时用官方 ff.h 覆盖。
 *---------------------------------------------------------------------------*/
typedef struct {
    FATFS*  obj;            /* Pointer to the owner filesystem object */
    BYTE    flag;           /* File status flags */
    BYTE    err;            /* Abortable error */
    FSIZE_t fptr;           /* File read/write pointer (0 on f_open) */
    FSIZE_t fsize;          /* File size in bytes (populated by f_open) */
    DWORD   clust;          /* Current cluster of fptr (valid once past cluster boundary) */
    DWORD   start_clust;    /* Starting cluster (for f_lseek: re-walk chain from here) */
    LBA_t   sect;           /* Current data sector */
    BYTE*   buf;            /* Pointer to the data buffer if FF_FS_TINY == 0 */
    UINT    blks;           /* Read/write size left in the cluster-boundary */
#if !FF_FS_READONLY
    UINT    lockid;         /* File lock ID (index of file semaphore, 0 = unused) */
#endif
} FIL;

/*---------------------------------------------------------------------------*
 *  File information (FILINFO)
 *---------------------------------------------------------------------------*/
typedef struct {
    FSIZE_t fsize;          /* File size */
    WORD    fdate;          /* Modified date */
    WORD    ftime;          /* Modified time */
    BYTE    fattrib;       /* File attribute */
    TCHAR   fname[13];      /* 8.3 file name (FF_USE_LFN == 0) */
} FILINFO;

/*---------------------------------------------------------------------------*
 *  FatFs API (only the subset this project needs)
 *---------------------------------------------------------------------------*/
FRESULT f_mount (FATFS* fs, const TCHAR* path, BYTE opt);
FRESULT f_open  (FIL* fp, const TCHAR* path, BYTE mode);
FRESULT f_read  (FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_lseek (FIL* fp, FSIZE_t ofs);   /* 移动读写指针（重试重定位用） */
FRESULT f_close (FIL* fp);
FSIZE_t f_size  (FIL* fp);
FRESULT f_stat  (const TCHAR* path, FILINFO* fno);

#ifdef __cplusplus
}
#endif

#endif /* _FATFS */
