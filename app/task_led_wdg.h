/* app/task_led_wdg.h
 *
 * LED + 看门狗 + 蜂鸣器合并任务（50ms 周期，UCOS-III）。
 *
 * 任务职责：
 *   - 每 50ms 控制系统状态 LED（PB12）按 SysState_GetLEDBlinkPeriod() 闪烁
 *       SD 卡模式   : 500ms 周期  -> 每 250ms 翻转一次（50% 占空）
 *       MCU Flash 模: 1500ms 周期 -> 每 750ms 翻转一次（50% 占空）
 *   - 每 50ms 设置固件类型 LED（PA5）
 *       FW_FACTORY : 常亮
 *       FW_NORMAL  : 熄灭
 *   - 每 2000ms 喂 IWDG（IWDG 超时 4s，2s 喂狗 = 50% 余量）
 *   - 蜂鸣器状态机（50ms tick 驱动，非阻塞），接收外部 Buzzer_Request() 调用
 *       命令：BOOT/SHORT/LONG/ERROR/OK
 *
 * TX/RX LED（PA12 / PC10）由 USART 中断 / 下载任务直接控制，本任务不触碰。
 *
 * 依赖：
 *   - UCOS-III OSTimeDlyHMSM（os.h，500Hz tick -> 2ms/tick；50ms = 25 tick，无量化误差）
 *   - BSP_LED_On/Off/Toggle (Task 2, bsp_gpio.c)  LED 低电平有效
 *   - BSP_Buzzer_On/Off (Task 2, bsp_gpio.c)     蜂鸣器 PA3 高电平有效
 *   - BSP_IWDG_Feed (Task 2, bsp_iwdg.c)          IWDG 4s 超时
 *   - SysState_GetLEDBlinkPeriod / SysState_GetFwType (Task 5, sys_state.c)
 *   - LED_SYS_PORT/PIN、LED_FW_PORT/PIN (bsp_gpio.h，经 bsp.h -> includes.h 可见)
 *   - FW_FACTORY 枚举 (sys_state.h，与 app_cfg.h 的 FW_TYPE_FACTORY 值一致)
 *
 * 任务实体（栈 + OSTaskCreate）由 Task 10 (app.c) 负责，本头文件只提供入口声明。
 */
#ifndef __TASK_LED_WDG_H
#define __TASK_LED_WDG_H

#include "os.h"

/* ---- Buzzer 命令（合并到此任务的 50ms 状态机） ---- */
typedef enum {
    BUZZER_CMD_NONE  = 0,
    BUZZER_CMD_BOOT,      /* 上电：短鸣 0.5s */
    BUZZER_CMD_SHORT,     /* 短鸣 0.5s */
    BUZZER_CMD_LONG,      /* 长鸣 1.5s */
    BUZZER_CMD_ERROR,     /* 连续短鸣 4 声 (单声 0.5s, 间隔 0.5s) */
    BUZZER_CMD_OK         /* 传输完成：长鸣 1.5s */
} BuzzerCmd_t;

void Buzzer_Request(BuzzerCmd_t cmd);   /* 外部调用：请求蜂鸣器模式 */

void AppTask_LED_WDG(void *p_arg);

#endif /* __TASK_LED_WDG_H */
