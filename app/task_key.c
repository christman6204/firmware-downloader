/* app/task_key.c
 *
 * 按键扫描任务实现。详见 task_key.h 顶部说明。
 *
 * 关键点：
 *   - 20ms 周期扫描，软件消抖（电平变化后等待 ~1 个 20ms 周期确认）。
 *   - 长按阈值 APP_KEY_LONG_PRESS_TICKS（app_cfg.h 中 =1000，定义为
 *     2000ms / 2ms 即 1000 个 OS tick @ 500Hz = 2s）。press_ticks 每个扫描
 *     周期累加 APP_KEY_DEBOUNCE_TICKS(=10) 个 OS tick，达到阈值时触发长按
 *     事件（仅触发一次，long_fired 防重复；用 >= 以防扫描延迟漏判）。
 *   - 短按：释放时 press_ticks>0 且未触发长按 -> 短按事件（仅 Key2）。
 *   - 传输锁定（SysState_IsTransferLocked）：所有按键动作完全无响应
 *     （不鸣响、不投递事件）。锁定状态在动作触发时刻判定。
 *   - 指针即数值：OSQPost 第二参数直接强转事件/命令码（见 task_buzzer.h）。
 *   - 队列满时 OSQPost 失败由发送侧忽略（最坏情况丢失一次按键事件）。
 *
 * 依赖：
 *   - BSP_Key_Read (Task 2)            0=按下 1=释放
 *   - SysState_IsTransferLocked (Task 5)
 *   - g_buzzer_cmd_q + BUZZER_CMD_* (Task 8)
 *   - g_key_event_q（本模块定义，Task 10 中 OSQCreate 初始化）
 */
#include "includes.h"
#include "task_key.h"
#include "task_buzzer.h"
#include "sys_state.h"

/*---------------------------------------------------------------------------*/
/* 全局对象定义（Task 10 中由 OSQCreate 初始化）                              */
/*---------------------------------------------------------------------------*/
OS_Q g_key_event_q;

/*---------------------------------------------------------------------------*/
/* 按键状态                                                                   */
/*---------------------------------------------------------------------------*/
typedef struct {
    uint8_t  last_level;    /* 上次确认电平：0=按下 1=释放                  */
    uint16_t press_ticks;   /* 按下累计 OS tick（每扫描 +DEBOUNCE_TICKS）  */
    uint8_t  long_fired;    /* 长按事件是否已触发（防止重复触发）           */
    uint8_t  debouncing;    /* 消抖中标志                                   */
    uint8_t  debounce_cnt;  /* 消抖计数                                     */
} KeyState_t;

static KeyState_t g_key1;
static KeyState_t g_key2;

/*---------------------------------------------------------------------------*/
/* Key_Scan：单键扫描                                                         */
/*   ks     : 按键状态                                                        */
/*   key_id : KEY_ID_1 / KEY_ID_2                                             */
/*                                                                           */
/* 处理流程：                                                                 */
/*   1. 电平变化 -> 启动消抖（debouncing=1, debounce_cnt=1），更新 last_level */
/*   2. 消抖中   -> debounce_cnt++; 达到 2 时确认电平，继续后续处理          */
/*   3. 按下     -> press_ticks += DEBOUNCE_TICKS; >= 阈值且未触发 -> 长按  */
/*   4. 释放     -> 若 press_ticks>0 且未长按 -> 短按事件（仅 Key2，锁定忽略)*/
/*                 复位 press_ticks / long_fired                              */
/*---------------------------------------------------------------------------*/
static void Key_Scan(KeyState_t *ks, uint8_t key_id)
{
    uint8_t cur;
    OS_ERR  err;

    cur = BSP_Key_Read(key_id);   /* 0=按下, 1=释放 */

    /* --- 1. 电平变化：启动消抖 --- */
    if (cur != ks->last_level) {
        ks->debouncing   = 1u;
        ks->debounce_cnt = 1u;
        ks->last_level   = cur;
        return;
    }

    /* --- 2. 消抖确认：再等 1 个 20ms 周期 --- */
    if (ks->debouncing) {
        ks->debounce_cnt++;
        if (ks->debounce_cnt < 2u) {
            return;
        }
        ks->debouncing = 0u;
        /* 电平已确认，继续后续按下/释放处理 */
    }

    /* --- 3. 按下 --- */
    if (cur == 0u) {
        /* 每 20ms 扫描累加 10 个 OS tick（APP_KEY_DEBOUNCE_TICKS），
         * 使 press_ticks 单位与 APP_KEY_LONG_PRESS_TICKS（OS tick @500Hz）一致。 */
        ks->press_ticks += APP_KEY_DEBOUNCE_TICKS;
        if (ks->press_ticks >= APP_KEY_LONG_PRESS_TICKS && !ks->long_fired) {
            ks->long_fired = 1u;
            /* 传输锁定 -> 完全无响应（无声、无事件） */
            if (!SysState_IsTransferLocked()) {
                /* 长按蜂鸣（达阈值时刻） */
                OSQPost(&g_buzzer_cmd_q,
                        (void *)(CPU_ADDR)BUZZER_CMD_LONG,
                        sizeof(uint32_t),
                        OS_OPT_POST_FIFO,
                        &err);
                /* 长按事件：Key1=切换数据源, Key2=切换固件类型 */
                if (key_id == KEY_ID_1) {
                    OSQPost(&g_key_event_q,
                            (void *)(CPU_ADDR)KEY_EVT_B1_LONG,
                            sizeof(uint32_t),
                            OS_OPT_POST_FIFO,
                            &err);
                } else {  /* KEY_ID_2 */
                    OSQPost(&g_key_event_q,
                            (void *)(CPU_ADDR)KEY_EVT_B2_LONG,
                            sizeof(uint32_t),
                            OS_OPT_POST_FIFO,
                            &err);
                }
            }
        }
    }
    /* --- 4. 释放 --- */
    else {  /* cur == 1 */
        if (ks->press_ticks > 0u && !ks->long_fired) {
            /* 短按：仅 Key2 响应（Key1 短按无动作） */
            if (key_id == KEY_ID_2 && !SysState_IsTransferLocked()) {
                /* 短按蜂鸣（释放时刻） */
                OSQPost(&g_buzzer_cmd_q,
                        (void *)(CPU_ADDR)BUZZER_CMD_SHORT,
                        sizeof(uint32_t),
                        OS_OPT_POST_FIFO,
                        &err);
                /* 短按事件：启动下载 */
                OSQPost(&g_key_event_q,
                        (void *)(CPU_ADDR)KEY_EVT_B2_SHORT,
                        sizeof(uint32_t),
                        OS_OPT_POST_FIFO,
                        &err);
            }
        }
        /* 复位状态，准备下次按下 */
        ks->press_ticks = 0u;
        ks->long_fired  = 0u;
    }
}

/*---------------------------------------------------------------------------*/
/* AppTask_Key：按键扫描任务主体                                              */
/*   p_arg : 未使用（UCOS-III 任务入口约定）                                  */
/*---------------------------------------------------------------------------*/
void AppTask_Key(void *p_arg)
{
    OS_ERR err;

    (void)p_arg;

    /* 初始化按键状态：释放态，无计数 */
    g_key1.last_level   = 1u;
    g_key1.press_ticks  = 0u;
    g_key1.long_fired   = 0u;
    g_key1.debouncing   = 0u;
    g_key1.debounce_cnt = 0u;

    g_key2.last_level   = 1u;
    g_key2.press_ticks  = 0u;
    g_key2.long_fired   = 0u;
    g_key2.debouncing   = 0u;
    g_key2.debounce_cnt = 0u;

    while (DEF_TRUE) {
        Key_Scan(&g_key1, KEY_ID_1);
        Key_Scan(&g_key2, KEY_ID_2);
        OSTimeDlyHMSM(0u, 0u, 0u, 20u,
                      OS_OPT_TIME_HMSM_STRICT,
                      &err);
    }
}
