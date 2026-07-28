/* Libraries/FatFs/ff_ucos3.c
 *
 * FatFs R0.15 重入保护 —— UCOS-III 互斥量适配层。
 *
 * 当 ffconf.h 中 FF_FS_REENTRANT == 1 时，FatFs 内部所有卷级操作与文件
 * 对象共享操作会通过以下 4 个钩子互斥保护：
 *
 *   ff_mutex_create(vol)  创建互斥量（挂载时调用）
 *   ff_mutex_delete(vol)  删除互斥量（卸载时调用）
 *   ff_mutex_take(vol)    获取互斥量（操作前调用，成功返回 1）
 *   ff_mutex_give(vol)    释放互斥量（操作后调用）
 *
 * vol 参数：
 *   0 .. FF_VOLUMES-1  卷级互斥量（FF_VOLUMES == 1 时有 1 个）
 *   FF_VOLUMES          系统级互斥量（全局排他锁，如文件共享表写）
 *
 * 本文件仅在 FF_FS_REENTRANT == 1 时编译；关闭重入时链接器不会引用这些符号。
 */

#include "ff.h"       /* FF_VOLUMES, ff_mutex_* 声明              */
#include <stdio.h>     /* snprintf                                   */
#include "os.h"       /* OSMutexCreate/Del/Pend/Post, OS_ERR       */

/* ---- 互斥量数组（vol 索引）----------------------------------------------- */
/* FF_VOLUMES==1 时：g_mtx[0] = 卷 0, g_mtx[1] = 系统锁              */

static OS_MUTEX  g_fatfs_mutex[FF_VOLUMES + 1u];
static BYTE      g_mtx_created[FF_VOLUMES + 1u];  /* 简单追踪（避免重复创建） */

/* ---- ff_mutex_create ------------------------------------------------------ */

int ff_mutex_create (int vol)
{
    OS_ERR  err;
    char    name[12];

    if (vol < 0 || (unsigned int)vol > (unsigned int)FF_VOLUMES) {
        return 0;                                      /* 参数非法 */
    }
    if (g_mtx_created[vol] != 0u) {
        return 1;                                      /* 已创建 */
    }

    /* 构造名称 "FatFS0" / "FatFS1" / "FatFSSys" */
    {
        int n = snprintf(name, sizeof(name),
                         (vol < (int)FF_VOLUMES) ? "FatFS%d" : "FatFSSys",
                         vol);
        (void)n;
    }

    OSMutexCreate(&g_fatfs_mutex[vol], (CPU_CHAR *)name, &err);
    if (err != OS_ERR_NONE) {
        return 0;
    }
    g_mtx_created[vol] = 1u;
    return 1;
}

/* ---- ff_mutex_delete ------------------------------------------------------ */

void ff_mutex_delete (int vol)
{
    OS_ERR err;

    if (vol < 0 || (unsigned int)vol > (unsigned int)FF_VOLUMES) {
        return;
    }
    if (g_mtx_created[vol] == 0u) {
        return;
    }

    OSMutexDel(&g_fatfs_mutex[vol], OS_OPT_DEL_ALWAYS, &err);
    g_mtx_created[vol] = 0u;
}

/* ---- ff_mutex_take -------------------------------------------------------- */

int ff_mutex_take (int vol)
{
    OS_ERR err;

    if (vol < 0 || (unsigned int)vol > (unsigned int)FF_VOLUMES) {
        return 0;
    }
    if (g_mtx_created[vol] == 0u) {
        return 0;
    }

    OSMutexPend(&g_fatfs_mutex[vol], (OS_TICK)0u, OS_OPT_PEND_BLOCKING,
                (CPU_TS *)0, &err);
    return (err == OS_ERR_NONE) ? 1 : 0;
}

/* ---- ff_mutex_give -------------------------------------------------------- */

void ff_mutex_give (int vol)
{
    OS_ERR err;

    if (vol < 0 || (unsigned int)vol > (unsigned int)FF_VOLUMES) {
        return;
    }
    if (g_mtx_created[vol] == 0u) {
        return;
    }

    OSMutexPost(&g_fatfs_mutex[vol], OS_OPT_POST_NONE, &err);
}
