/* app/task_led_wdg.c
 *
 * LED + 喂狗合并任务实现。详见 task_led_wdg.h 顶部说明。
 *
 * 关键点：
 *   - 50ms 任务周期：OSTimeDlyHMSM(0,0,0,50, OS_OPT_TIME_HMSM_STRICT, &err)。
 *     500Hz tick -> 2ms/tick，50ms = 25 tick，无量化误差。
 *   - 系统状态 LED 闪烁：tick_ms 每 50ms 累加一次，当 (tick_ms % half) == 0
 *     时翻转。half = period/2（SD: 250ms, Flash: 750ms），即 50% 占空比。
 *     tick_ms 在 3000ms 处回零（500 与 1500 的公倍数），保证两种模式下
 *     翻转节拍对齐且无累积漂移。
 *   - 固件类型 LED：每周期重设为常亮（FW_FACTORY）或熄灭（FW_NORMAL），
 *     幂等操作，无需状态跟踪。
 *   - 喂狗：wdg_ms 每 50ms +50，达到 2000ms 调用 BSP_IWDG_Feed() 并清零。
 *     IWDG 超时 4s，喂狗间隔 2s，留 50% 余量。
 *
 * 依赖：
 *   - BSP_LED_On/Off/Toggle (Task 2)            低电平有效
 *   - BSP_IWDG_Feed (Task 2)                    IWDG 4s 超时
 *   - SysState_GetLEDBlinkPeriod (Task 5)       返回 500/1500 ms
 *   - SysState_GetFwType (Task 5)               返回 FW_FACTORY/FW_NORMAL
 *   - LED_SYS_xxx, LED_FW_xxx 宏 (bsp_gpio.h)
 *   - FW_FACTORY 枚举 (sys_state.h)
 */
#include "includes.h"
#include "task_led_wdg.h"
#include "sys_state.h"

void AppTask_LED_WDG(void *p_arg)
{
    OS_ERR     err;
    uint16_t   tick_ms   = 0;    /* 累计 ms（每循环 +50），3000ms 回零 */
    uint16_t   wdg_ms    = 0;    /* 喂狗计时（每循环 +50，2000ms 喂一次） */
    (void)p_arg;

    while (1) {
        uint16_t period = SysState_GetLEDBlinkPeriod();  /* 500 或 1500 */
        uint16_t half   = period / 2;

        /* --- 系统状态 LED：按周期闪烁（50% 占空，half 处翻转） --- */
        if ((tick_ms % half) == 0) {
            BSP_LED_Toggle(LED_SYS_PORT, LED_SYS_PIN);
        }

        /* --- 固件类型 LED：出厂=常亮，普通=熄灭 --- */
        if (SysState_GetFwType() == FW_FACTORY)
            BSP_LED_On(LED_FW_PORT, LED_FW_PIN);
        else
            BSP_LED_Off(LED_FW_PORT, LED_FW_PIN);

        /* --- TX/RX LED 由 USART 中断直接控制，此处不处理 --- */

        /* --- 喂狗：每 2000ms（IWDG 4s 超时，50% 余量） --- */
        wdg_ms += 50;
        if (wdg_ms >= 2000) {
            BSP_IWDG_Feed();
            wdg_ms = 0;
        }

        tick_ms += 50;
        if (tick_ms >= 3000) tick_ms = 0;  /* 500 和 1500 的公倍数，防累积漂移 */

        OSTimeDlyHMSM(0, 0, 0, 50, OS_OPT_TIME_HMSM_STRICT, &err);
    }
}
