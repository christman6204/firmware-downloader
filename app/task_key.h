/* app/task_key.h
 *
 * 按键扫描任务（20ms 周期，UCOS-III）。
 *
 * 任务职责：
 *   - 每 20ms 扫描 Key1 (PB11) 与 Key2 (PE14)
 *   - 软件消抖（电平变化后等待 ~1 个 20ms 周期确认）
 *   - 短按 / 长按（>=2s）检测
 *   - 传输锁定期间禁止所有按键动作（无声、无事件）
 *   - 长按达阈值：投递 BUZZER_CMD_LONG + 按键事件
 *   - 短按释放：投递 BUZZER_CMD_SHORT + 按键事件（仅 Key2）
 *
 * 事件码：
 *   KEY_EVT_B1_LONG   按键1长按 -> 切换数据源（SD 卡 / MCU Flash）
 *   KEY_EVT_B2_SHORT  按键2短按 -> 启动下载
 *   KEY_EVT_B2_LONG   按键2长按 -> 切换固件类型（普通 / 工厂）
 *
 * ---------------------------------------------------------------------------
 * OSQPost 消息约定 -- 指针即数值（pointer-as-value）
 * ---------------------------------------------------------------------------
 * 指针即数值约定：把事件码本身当作指针投递，不在发送侧分配
 * 任何缓冲区。消费方（下载任务 Task 9）从 OSQPend 取回 p_msg 后直接强转：
 *
 *   void    *p_msg = OSQPend(...);
 *   uint32_t evt   = (uint32_t)(CPU_ADDR)p_msg;
 *
 * 依赖：
 *   - UCOS-III OS_Q（Task 1 已在 os_cfg.h 使能 OS_CFG_Q_EN=1）
 *   - BSP_Key_Read (Task 2, bsp_gpio.c)  0=按下 1=释放
 *   - SysState_IsTransferLocked (Task 5, sys_state.c)
 *   - Buzzer_Request + BUZZER_CMD_* (task_led_wdg.h)
 *   - APP_KEY_LONG_PRESS_TICKS (app_cfg.h)
 *
 * 任务实体与 g_key_event_q 的创建由 Task 10 (app.c) 负责，本头文件只
 * 提供声明与事件码，不创建任何内核对象。
 */
#ifndef __TASK_KEY_H
#define __TASK_KEY_H

#include "os.h"

/*==================== 按键事件码 ====================*/
#define KEY_EVT_NONE        0u
#define KEY_EVT_B1_LONG     1u  /* 按键1长按：切换数据源 */
#define KEY_EVT_B2_SHORT    2u  /* 按键2短按：启动下载 */
#define KEY_EVT_B2_LONG     3u  /* 按键2长按：切换固件类型 */

/*==================== 全局对象（Task 10 中 OSQCreate） ====================*/
extern OS_Q g_key_event_q;

/*==================== 任务入口 ====================*/
void AppTask_Key(void *p_arg);

#endif /* __TASK_KEY_H */
