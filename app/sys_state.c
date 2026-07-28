/* app/sys_state.c
 *
 * 互斥量保护的全局系统状态实现。基于 UCOS-III OSMutex。
 *
 * Get/Set 模式（设计文档要求）：
 *   OSMutexPend(&mutex, 0, OS_OPT_PEND_BLOCKING, NULL, &err);   // 阻塞等待
 *   <读/写局部变量>
 *   OSMutexPost(&mutex, OS_OPT_POST_NONE, &err);                // 释放
 *   return <值>;
 *
 * 默认状态：SD_CARD, NORMAL, unlocked=0。
 * 互斥量名："State Mutex"。
 *
 * 依赖 UCOS-III，无 PC 端测试；C 逻辑与设计文档一致。
 */
#include "sys_state.h"
#include "os.h"
#include "app_cfg.h"

/*---------------------------------------------------------------------------*/
/* 模块静态状态                                                                */
/*---------------------------------------------------------------------------*/
static OS_MUTEX     g_state_mutex;
static DataSource_t g_data_source;
static FwType_t     g_fw_type;
static uint8_t      g_transfer_locked;

/*---------------------------------------------------------------------------*/
/* SysState_Init                                                              */
/*---------------------------------------------------------------------------*/
void SysState_Init(void)
{
    OS_ERR err;

    OSMutexCreate(&g_state_mutex, (CPU_CHAR *)"State Mutex", &err);

    g_data_source     = DATA_SRC_SD_CARD;
    g_fw_type         = FW_NORMAL;
    g_transfer_locked = 0u;
}

/*---------------------------------------------------------------------------*/
/* 数据源                                                                     */
/*---------------------------------------------------------------------------*/
DataSource_t SysState_GetDataSource(void)
{
    OS_ERR       err;
    DataSource_t src;

    OSMutexPend(&g_state_mutex, 0u, OS_OPT_PEND_BLOCKING, (CPU_TS *)0, &err);
    src = g_data_source;
    OSMutexPost(&g_state_mutex, OS_OPT_POST_NONE, &err);
    return src;
}

void SysState_SetDataSource(DataSource_t src)
{
    OS_ERR err;

    OSMutexPend(&g_state_mutex, 0u, OS_OPT_PEND_BLOCKING, (CPU_TS *)0, &err);
    g_data_source = src;
    OSMutexPost(&g_state_mutex, OS_OPT_POST_NONE, &err);
}

/*---------------------------------------------------------------------------*/
/* 固件类型                                                                   */
/*---------------------------------------------------------------------------*/
FwType_t SysState_GetFwType(void)
{
    OS_ERR   err;
    FwType_t type;

    OSMutexPend(&g_state_mutex, 0u, OS_OPT_PEND_BLOCKING, (CPU_TS *)0, &err);
    type = g_fw_type;
    OSMutexPost(&g_state_mutex, OS_OPT_POST_NONE, &err);
    return type;
}

void SysState_SetFwType(FwType_t type)
{
    OS_ERR err;

    OSMutexPend(&g_state_mutex, 0u, OS_OPT_PEND_BLOCKING, (CPU_TS *)0, &err);
    g_fw_type = type;
    OSMutexPost(&g_state_mutex, OS_OPT_POST_NONE, &err);
}

/*---------------------------------------------------------------------------*/
/* 传输锁定                                                                   */
/*---------------------------------------------------------------------------*/
uint8_t SysState_IsTransferLocked(void)
{
    OS_ERR  err;
    uint8_t locked;

    OSMutexPend(&g_state_mutex, 0u, OS_OPT_PEND_BLOCKING, (CPU_TS *)0, &err);
    locked = g_transfer_locked;
    OSMutexPost(&g_state_mutex, OS_OPT_POST_NONE, &err);
    return locked;
}

void SysState_SetTransferLock(uint8_t locked)
{
    OS_ERR err;

    OSMutexPend(&g_state_mutex, 0u, OS_OPT_PEND_BLOCKING, (CPU_TS *)0, &err);
    g_transfer_locked = locked;
    OSMutexPost(&g_state_mutex, OS_OPT_POST_NONE, &err);
}

/*---------------------------------------------------------------------------*/
/* LED 闪烁周期：SD_CARD=500ms, MCU_FLASH=1500ms                              */
/*---------------------------------------------------------------------------*/
uint32_t SysState_GetLEDBlinkPeriod(void)
{
    OS_ERR       err;
    DataSource_t src;
    uint32_t     period;

    OSMutexPend(&g_state_mutex, 0u, OS_OPT_PEND_BLOCKING, (CPU_TS *)0, &err);
    src = g_data_source;
    OSMutexPost(&g_state_mutex, OS_OPT_POST_NONE, &err);

    if (src == DATA_SRC_SD_CARD) {
        period = 500u;
    } else {
        period = 1500u;
    }
    return period;
}
