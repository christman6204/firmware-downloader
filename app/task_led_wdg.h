/* app/task_led_wdg.h
 *
 * LED + 看门狗合并任务（50ms 周期，UCOS-III）。
 *
 * 任务职责：
 *   - 每 50ms 控制系统状态 LED（PB12）按 SysState_GetLEDBlinkPeriod() 闪烁
 *       SD 卡模式   : 500ms 周期  -> 每 250ms 翻转一次（50% 占空）
 *       MCU Flash 模: 1500ms 周期 -> 每 750ms 翻转一次（50% 占空）
 *   - 每 50ms 设置固件类型 LED（PA5）
 *       FW_FACTORY : 常亮
 *       FW_NORMAL  : 熄灭
 *   - 每 2000ms 喂 IWDG（IWDG 超时 4s，2s 喂狗 = 50% 余量）
 *
 * TX/RX LED（PA12 / PC10）由 USART 中断 / 下载任务直接控制，本任务不触碰。
 *
 * 依赖：
 *   - UCOS-III OSTimeDlyHMSM（os.h，500Hz tick -> 2ms/tick；50ms = 25 tick，无量化误差）
 *   - BSP_LED_On/Off/Toggle (Task 2, bsp_gpio.c)  LED 低电平有效
 *   - BSP_IWDG_Feed (Task 2, bsp_iwdg.c)          IWDG 4s 超时
 *   - SysState_GetLEDBlinkPeriod / SysState_GetFwType (Task 5, sys_state.c)
 *   - LED_SYS_PORT/PIN、LED_FW_PORT/PIN (bsp_gpio.h，经 bsp.h -> includes.h 可见)
 *   - FW_FACTORY 枚举 (sys_state.h，与 app_cfg.h 的 FW_TYPE_FACTORY 值一致)
 *
 * 任务实体（栈 + OSTaskCreate）由 Task 10 (app.c) 负责，本头文件只提供入口声明。
 */
#ifndef __TASK_LED_WDG_H
#define __TASK_LED_WDG_H

void AppTask_LED_WDG(void *p_arg);

#endif /* __TASK_LED_WDG_H */
