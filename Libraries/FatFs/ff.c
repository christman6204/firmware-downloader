/* Libraries/FatFs/ff.c
 *
 * 最小只读 FAT16/FAT32 文件系统实现。
 *
 * 实现：f_mount / f_open / f_read / f_lseek / f_close / f_stat / f_size。
 * 盘驱动：diskio.c (disk_initialize / disk_read / disk_status)。
 * 扇区缓存：复用 FATFS.win[512] + winsect（R0.15 FF_FS_TINY==0 模式）。
 *
 * 仅支持：根目录 8.3 短文件名、FA_READ 只读、512 字节扇区、单卷。
 */

#include "ff.h"
#include "diskio.h"
#include <string.h>

/* ---- 调试输出（FF_DEBUG 在 ffconf.h 中定义） ------------------------------- */
#ifdef FF_DEBUG
#include "bsp_usart.h"   /* BSP_USART2_Printf                 */
#define FF_PRINTF(fmt, ...)  BSP_USART2_Printf("[FAT] " fmt "\r\n", ##__VA_ARGS__)
#else
#define FF_PRINTF(fmt, ...)  ((void)0)
#endif

/* ---- 内部常量 ------------------------------------------------------------- */
#define FS_FAT16         2u
#define FS_FAT32         3u
#define FAT16_MASK       0xFFFFu
#define FAT32_MASK       0x0FFFFFFFu
#define EOC_FAT16        0xFFF8u
#define EOC_FAT32        0x0FFFFFF8u
#define WS_INVALID       0xFFFFFFFFu
#define DIR_ENTRY_SIZE   32u

/* ---- 全局（单卷） --------------------------------------------------------- */

static FATFS *g_cur_fs = (FATFS *)0x0;    /* 最后挂载的卷 */

/* ---- 小端读取 ------------------------------------------------------------- */

static WORD  ld_word (const BYTE *p) { return (WORD)p[0] | ((WORD)p[1] << 8); }
static DWORD ld_dword(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8)
                                             | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24); }

/* ---- 扇区缓存：读一个扇区到 fs->win -------------------------------------- */

static FRESULT move_window(FATFS *fs, DWORD sector)
{
    if (sector == fs->winsect) { return FR_OK; }
    if (disk_read(fs->pdrv, fs->win, sector, 1u) != RES_OK) {
        fs->winsect = WS_INVALID;
        return FR_DISK_ERR;
    }
    fs->winsect = sector;
    return FR_OK;
}

/* ---- 读取 FAT 条目（clust → next） --------------------------------------- */

static FRESULT get_fat(FATFS *fs, DWORD clust, DWORD *next)
{
    DWORD off, sec;

    if (fs->fs_type == FS_FAT16) {
        off = clust * 2u;
    } else {
        off = clust * 4u;
    }
    sec = fs->fatbase + (off >> 9);            /* / 512 */

    {
        FRESULT fr = move_window(fs, sec);
        if (fr != FR_OK) { return fr; }
    }

    {
        const BYTE *p = &fs->win[off & 511u];
        if (fs->fs_type == FS_FAT16) {
            *next = ((DWORD)p[0] | ((DWORD)p[1] << 8)) & FAT16_MASK;
        } else {
            *next = ((DWORD)p[0] | ((DWORD)p[1] << 8)
                   | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24)) & FAT32_MASK;
        }

        /* 仅当检测到 EOC 时打印簇链信息 */
        {
            DWORD eoc = (fs->fs_type == FS_FAT32) ? EOC_FAT32 : EOC_FAT16;
            if (*next >= eoc) {
                FF_PRINTF("get_fat: FAT chain clust %lu -> next=%lu (EOC, >=0x%lX)",
                          (unsigned long)clust, (unsigned long)*next,
                          (unsigned long)eoc);
            }
        }
    }
    return FR_OK;
}

/* ---- cluster number → 数据区扇区号 ---------------------------------------- */

static DWORD clust2sect(FATFS *fs, DWORD clust)
{
    return fs->database + (clust - 2u) * (DWORD)fs->csize;
}

/* ---- 8.3 文件名生成 ------------------------------------------------------ */

static void name_to_83(const TCHAR *nm, BYTE out[11])
{
    BYTE i;

    memset(out, ' ', 11u);

    for (i = 0u; i < 8u && nm[i] != '\0' && nm[i] != '.'; i++) {
        BYTE c = (BYTE)nm[i];
        if (c >= 'a' && c <= 'z') { c -= ('a' - 'A'); }
        out[i] = c;
    }
    while (*nm != '\0' && *nm != '.') { nm++; }
    if (*nm == '.') { nm++; }
    for (i = 0u; i < 3u && nm[i] != '\0'; i++) {
        BYTE c = (BYTE)nm[i];
        if (c >= 'a' && c <= 'z') { c -= ('a' - 'A'); }
        out[8u + i] = c;
    }
}

/* ---- 跳过无效目录条目 ---------------------------------------------------- */

static int valid_entry(const BYTE *dir)
{
    if (dir[0] == 0x00u)           { return 0; }   /* 结束标记 */
    if (dir[0] == 0xE5u)           { return 0; }   /* 已删除 */
    if (dir[11u] == 0x0Fu)         { return 0; }   /* 长文件名 */
    if (dir[11u] & 0x08u)          { return 0; }   /* 卷标 */
    return 1;
}

/* ---- 扫描 FAT16 根目录（连续扇区） ---------------------------------------- */

static FRESULT find_file_fat16(FATFS *fs, const BYTE name[11], BYTE **p_entry)
{
    DWORD sect  = fs->dirbase;
    DWORD end   = sect + ((DWORD)fs->n_rootdir * DIR_ENTRY_SIZE + 511u) / 512u;
    WORD  n     = fs->n_rootdir;

    FF_PRINTF("find_file_fat16: scanning root dir, sector base=%lu, entries=%u",
              (unsigned long)sect, (unsigned int)n);

    for (; sect < end && n > 0u; sect++) {
        FRESULT fr = move_window(fs, sect);
        BYTE   *dir;
        WORD    di;

        if (fr != FR_OK) { return fr; }
        for (di = 0u, dir = fs->win; di < (512u / DIR_ENTRY_SIZE) && n > 0u; di++, dir += DIR_ENTRY_SIZE) {
            n--;
            if (!valid_entry(dir)) { continue; }
            if (memcmp(dir, name, 11u) == 0) {
                *p_entry = dir;
                return FR_OK;
            }
        }
    }
    return FR_NO_FILE;
}

/* ---- 扫描 FAT32 根目录（簇链） -------------------------------------------- */

static FRESULT find_file_fat32(FATFS *fs, const BYTE name[11], BYTE **p_entry)
{
    DWORD clust = fs->dirbase;

    FF_PRINTF("find_file_fat32: scanning root dir, start clust=%lu",
              (unsigned long)clust);

    for (;;) {
        BYTE  ci;
        DWORD sect_base = clust2sect(fs, clust);

        for (ci = 0u; ci < fs->csize; ci++) {
            FRESULT fr = move_window(fs, sect_base + (DWORD)ci);
            BYTE   *dir = fs->win;
            WORD    di;

            if (fr != FR_OK) { return fr; }
            for (di = 0u, dir = fs->win; di < (512u / DIR_ENTRY_SIZE); di++, dir += DIR_ENTRY_SIZE) {
                if (!valid_entry(dir)) { continue; }
                if (memcmp(dir, name, 11u) == 0) {
                    *p_entry = dir;
                    return FR_OK;
                }
            }
        }
        {   /* 下一簇 */
            DWORD next;
            FRESULT fr = get_fat(fs, clust, &next);
            if (fr != FR_OK) { return fr; }
            if (next >= EOC_FAT32) { break; }
            clust = next;
        }
    }
    return FR_NO_FILE;
}

/* ---- 根目录文件查找（分发 FAT16/FAT32） ----------------------------------- */

static FRESULT dir_find(FATFS *fs, const BYTE name[11], BYTE **p_entry)
{
    FF_PRINTF("dir_find: searching, fs_type=%s, dirbase=%lu",
              (fs->fs_type == FS_FAT32) ? "FAT32" : "FAT16",
              (unsigned long)fs->dirbase);
    if (fs->fs_type == FS_FAT32) { return find_file_fat32(fs, name, p_entry); }
    return find_file_fat16(fs, name, p_entry);
}

/* ---- 跟随簇链到文件内指定字节偏移 ----------------------------------------- */

static FRESULT walk_chain(FATFS *fs, DWORD start_clust, DWORD ofs,
                          DWORD *clust_out, DWORD *sect_out)
{
    DWORD clust    = start_clust;
    DWORD bytes_per_cluster = (DWORD)fs->csize * 512u;

    while (ofs >= bytes_per_cluster) {
        DWORD next;
        FRESULT fr = get_fat(fs, clust, &next);
        if (fr != FR_OK) { return fr; }
        if (next >= (fs->fs_type == FS_FAT32 ? EOC_FAT32 : EOC_FAT16)) {
            return FR_INT_ERR;
        }
        clust = next;
        ofs  -= bytes_per_cluster;
    }
    *clust_out = clust;
    *sect_out  = clust2sect(fs, clust) + (ofs / 512u);
    return FR_OK;
}

/* ========================================================================= */
/*                           公共 API                                         */
/* ========================================================================= */

/* f_mount: 挂载卷，解析 Boot Sector BPB */
FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt)
{
    (void)path;
    (void)opt;

    if (fs == (FATFS *)0x0) { g_cur_fs = (FATFS *)0x0; return FR_OK; }  /* 卸载 */

    /* 初始化盘驱动 */
    if (disk_initialize(0) & STA_NOINIT) { return FR_NOT_READY; }

    fs->pdrv    = 0u;
    fs->winsect = WS_INVALID;

    /* 读引导扇区 */
    if (disk_read(0, fs->win, 0u, 1u) != RES_OK) { return FR_NOT_READY; }

    /* 引导签名 */
    if (ld_word(&fs->win[510u]) != 0xAA55u) { return FR_NO_FILESYSTEM; }

    /* 扇区大小（本实现仅支持 512） */
    if (ld_word(&fs->win[11u]) != 512u)      { return FR_NO_FILESYSTEM; }

    {
        DWORD fat_size, total_sect, data_sectors, root_dir_sectors;
        WORD  reserved, root_ents;

        fs->csize    = fs->win[13u];
        reserved     = ld_word(&fs->win[14u]);
        fs->n_fats   = fs->win[16u];
        root_ents    = ld_word(&fs->win[17u]);
        fat_size     = (DWORD)ld_word(&fs->win[22u]);     /* 每 FAT 扇区数 */
        total_sect   = (DWORD)ld_word(&fs->win[19u]);     /* 总扇区数 */

        /* FAT32 扩展字段 */
        if (fat_size == 0u) {
            fat_size               = ld_dword(&fs->win[36u]);   /* BPB_FATSz32 */
            fs->dirbase            = ld_dword(&fs->win[44u]);   /* BPB_RootClus */
            fs->n_rootdir          = 0u;
            if (total_sect == 0u) { total_sect = ld_dword(&fs->win[32u]); }
        } else {
            fs->dirbase   = (DWORD)reserved + (DWORD)fs->n_fats * fat_size;
            fs->n_rootdir = root_ents;
            if (total_sect == 0u) { total_sect = ld_dword(&fs->win[32u]); }
        }

        root_dir_sectors = ((DWORD)root_ents * 32u + 511u) / 512u;
        fs->database  = (DWORD)reserved + (DWORD)fs->n_fats * fat_size + root_dir_sectors;
        fs->fatbase   = (DWORD)reserved;
        fs->fsize     = fat_size;

        data_sectors  = total_sect - fs->database;
        fs->n_fatent  = data_sectors / (DWORD)fs->csize + 2u;

        /* 簇数 → 类型 */
        {
            DWORD n_clusters = data_sectors / (DWORD)fs->csize;
            if (n_clusters < 4085u) { fs->fs_type = 1u; }         /* FAT12（不常见） */
            else if (n_clusters < 65525u) { fs->fs_type = FS_FAT16; }
            else { fs->fs_type = FS_FAT32; }
        }

        FF_PRINTF("f_mount: media=0x%02X, bps=%u, spc=%u, rsvd=%u, fats=%u, "
                  "root_ents=%u, fat_sz=%lu, tot_sect=%lu, db=%lu, fatbase=%lu, "
                  "dirbase=%lu, n_fatent=%lu, type=%s",
                  (unsigned int)fs->win[21u],
                  (unsigned int)ld_word(&fs->win[11u]),
                  (unsigned int)fs->csize,
                  (unsigned int)reserved,
                  (unsigned int)fs->n_fats,
                  (unsigned int)root_ents,
                  (unsigned long)fat_size,
                  (unsigned long)total_sect,
                  (unsigned long)fs->database,
                  (unsigned long)fs->fatbase,
                  (unsigned long)fs->dirbase,
                  (unsigned long)fs->n_fatent,
                  (fs->fs_type == FS_FAT32) ? "FAT32"
                    : (fs->fs_type == FS_FAT16) ? "FAT16" : "FAT12");
    }

    g_cur_fs = fs;
    return FR_OK;
}

/* f_open: 打开根目录文件（仅读） */
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    FATFS *fs = g_cur_fs;
    BYTE   name_83[11u], *entry = (BYTE *)0x0;
    FRESULT fr;
    DWORD  start_clust;

    if (fs == (FATFS *)0x0 || fp == (FIL *)0x0)  { return FR_NOT_READY; }
    if (fs->fs_type == 0u || fs->fs_type > FS_FAT32) { return FR_NOT_READY; }
    if ((mode & FA_READ) == 0u)                 { return FR_WRITE_PROTECTED; }

    fp->obj        = fs;
    fp->flag       = 0u;
    fp->err        = 0u;
    fp->buf        = (BYTE *)0x0;

    name_to_83(path, name_83);

    FF_PRINTF("f_open: searching for \"%s\"", path);

    fr = dir_find(fs, name_83, &entry);
    if (fr != FR_OK) {
        FF_PRINTF("f_open: \"%s\" not found (fr=%d)", path, (int)fr);
        return (fr == FR_NO_FILE) ? FR_NO_FILE : fr;
    }

    start_clust = (DWORD)ld_word(&entry[20u]);                       /* 高位 */
    start_clust = (start_clust << 16u) | (DWORD)ld_word(&entry[26u]); /* 低位 */
    fp->fsize      = ld_dword(&entry[28u]);

    FF_PRINTF("f_open: \"%s\" found, start_clust=%lu, fsize=%lu",
              path, (unsigned long)start_clust, (unsigned long)fp->fsize);
    fp->fptr       = 0u;
    fp->clust      = start_clust;
    fp->start_clust = start_clust;
    fp->sect       = clust2sect(fs, start_clust);
    fp->flag      |= FA_READ;

    return FR_OK;
}

/* f_read: 读取文件数据 */
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    FATFS *fs;
    DWORD  bpc;
    UINT   rc = 0u;
    BYTE  *dst = (BYTE *)buff;

    if (br) { *br = 0u; }
    if (fp == (FIL *)0x0 || (fp->flag & FA_READ) == 0u) { return FR_INVALID_OBJECT; }

    fs  = fp->obj;
    if (fs->fs_type == 0u) { return FR_NOT_READY; }
    if (fp->fptr >= (DWORD)fp->fsize) { return FR_OK; }

    /* 钳位到文件尾 */
    {
        DWORD remain = (DWORD)fp->fsize - (DWORD)fp->fptr;
        if ((DWORD)btr > remain) { btr = (UINT)remain; }
    }
    bpc = (DWORD)fs->csize * 512u;

{   /* f_read loop counter for debug */
    UINT loop_cnt = 0u;
    while (btr > 0u) {
        DWORD sector = clust2sect(fs, fp->clust)
                     + ((fp->fptr % bpc) / 512u);
        WORD  off     = (WORD)(fp->fptr & 511u);
        UINT  chunk   = (512u - (UINT)off);
        BYTE  tmp[512u];

        if (chunk > btr) { chunk = btr; }

        /* 调试输出：前5次每次输出，之后每50次输出一次，避免刷屏 */
        if (loop_cnt < 5u || (loop_cnt % 50u) == 0u) {
            FF_PRINTF("f_read: offset=%lu, chunk=%u bytes, sector=%lu, clust=%lu",
                      (unsigned long)fp->fptr, (unsigned int)chunk,
                      (unsigned long)sector, (unsigned long)fp->clust);
        }
        loop_cnt++;

        if (disk_read(fs->pdrv, tmp, sector, 1u) != RES_OK) { return FR_DISK_ERR; }
        memcpy(dst, &tmp[off], (size_t)chunk);

        dst     += chunk;
        rc      += chunk;
        fp->fptr += (DWORD)chunk;
        btr     -= chunk;

        /* 跨簇后更新 clust */
        if (fp->fptr < (DWORD)fp->fsize && (fp->fptr % bpc) == 0u) {
            DWORD next;
            FRESULT fr = get_fat(fs, fp->clust, &next);
            if (fr != FR_OK) { return fr; }
            if (next >= (fs->fs_type == FS_FAT32 ? EOC_FAT32 : EOC_FAT16)) {
                break;
            }
            fp->clust = next;
        }
    }
} /* end loop_cnt scope */

    if (br) { *br = rc; }
    return FR_OK;
}

/* f_lseek: 移动文件读指针 */
FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    FATFS *fs;
    DWORD  offset;

    if (fp == (FIL *)0x0 || (fp->flag & FA_READ) == 0u) { return FR_INVALID_OBJECT; }

    fs  = fp->obj;
    offset = (DWORD)ofs;
    if (offset > (DWORD)fp->fsize) { offset = (DWORD)fp->fsize; }

    FF_PRINTF("f_lseek: seek to offset=%lu (fsize=%lu)",
              (unsigned long)offset, (unsigned long)fp->fsize);

    {   /* Re-walk 簇链从 start_clust 到新偏移 */
        DWORD clust, sect;
        FRESULT fr = walk_chain(fs, fp->start_clust, offset, &clust, &sect);
        if (fr != FR_OK) { return fr; }
        fp->clust = clust;
        fp->sect  = sect;
    }
    fp->fptr = offset;
    return FR_OK;
}

/* f_close: 关闭文件 */
FRESULT f_close(FIL *fp)
{
    if (fp == (FIL *)0x0) { return FR_INVALID_OBJECT; }
    FF_PRINTF("f_close");
    fp->flag = 0u;
    return FR_OK;
}

/* f_size: 返回文件大小 */
FSIZE_t f_size(FIL *fp)
{
    if (fp == (FIL *)0x0) { return 0u; }
    return fp->fsize;
}

/* f_stat: 获取文件信息（查询） */
FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    FATFS *fs = g_cur_fs;
    BYTE   name_83[11u], *entry = (BYTE *)0x0;
    FRESULT fr;

    if (fno == (FILINFO *)0x0) { return FR_INVALID_PARAMETER; }
    if (fs == (FATFS *)0x0 || fs->fs_type == 0u) { return FR_NOT_READY; }

    name_to_83(path, name_83);
    fr = dir_find(fs, name_83, &entry);
    if (fr != FR_OK) {
        fno->fsize   = 0u;
        fno->fdate   = 0u;
        fno->ftime   = 0u;
        fno->fattrib = 0u;
        fno->fname[0] = '\0';
        FF_PRINTF("f_stat: \"%s\" not found (fr=%d)", path, (int)fr);
        return fr;
    }

    fno->fsize   = ld_dword(&entry[28u]);
    FF_PRINTF("f_stat: \"%s\" found, size=%lu", path, (unsigned long)fno->fsize);
    fno->fdate   = ld_word(&entry[24u]);
    fno->ftime   = ld_word(&entry[22u]);
    fno->fattrib = entry[11u];

    /* 重建可读文件名（修剪尾随空格） */
    {
        char *n, *e;
        char  tmp_buf[13];
        memcpy(tmp_buf, entry, 8u);
        tmp_buf[8u] = '\0';
        n = tmp_buf + 7u;
        while (n >= tmp_buf && *n == ' ') { *n-- = '\0'; }
        n = tmp_buf + strlen(tmp_buf);
        *n++ = '.';
        e = (char *)entry + 8u;
        memcpy(n, e, 3u);
        n[3u] = '\0';
        n += 2u;
        while (n >= tmp_buf && *n == ' ') { *n-- = '\0'; }
        memcpy(fno->fname, tmp_buf, 13u);
    }

    return FR_OK;
}
