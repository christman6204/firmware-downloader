/* app/sys_state.c
 *
 * 互斥量保护的全局系统状态实现。基于 UCOS-III OSMutex。
 *
 * ===== 互斥量保护规则 =====
 *
 * 本模块管理三类全局状态：数据源(DataSource)、固件类型(FwType)、传输锁(TransferLock)。
 * 三类状态被多个任务并发访问：
 *   - task_key.c (按键任务)     : SetDataSource / SetFwType (长按切换)
 *   - task_download.c (下载任务): GetDataSource / GetFwType / SetTransferLock
 *   - task_led_wdg.c (LED任务)  : GetLEDBlinkPeriod (只读)
 *
 * 保护策略：
 *   1. 单一互斥量 g_state_mutex 保护所有三类状态。
 *      为什么用单一锁而非每字段独立锁：
 *      - 状态字段间无因果依赖，升级为多锁无性能收益
 *      - 单一锁消除死锁风险（无锁序问题）
 *      - 临界区极短（读/写一个字），锁竞争概率低
 *   2. Get/Set 模式：Pend(阻塞) → 读/写局部变量 → Post(释放)
 *      - Pend 超时参数为 0（无限等待），不会失败
 *      - 局部变量复制后释放锁再 return，缩短临界区
 *   3. SysState_GetLEDBlinkPeriod 不在临界区内计算周期值，
 *      只在临界区内读取 g_data_source，计算（比较+赋值）在锁外完成。
 *
 * ===== 状态转移图 =====
 *
 *   上电 → Init()
 *           g_data_source     = SD_CARD
 *           g_fw_type         = FW_NORMAL
 *           g_transfer_locked = 0
 *
 *   数据源 (DataSource):
 *     SD_CARD  ←──B1_LONG──→  MCU_FLASH
 *     (LED 500ms)            (LED 1500ms)
 *     切换时检查: 切到SD_CARD时若SD_IsPresent==0 → 报错并保持原源
 *
 *   固件类型 (FwType):
 *     FW_NORMAL  ←──B2_LONG──→  FW_FACTORY
 *
 *   传输锁 (TransferLock):
 *     0 (空闲)  ──B2_SHORT→  1 (传输中)
 *     1 (传输中) ──传输结束/错误/终止→  0 (空闲)
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
