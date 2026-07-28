/* app/task_download.h
 *
 * 固件下载状态机任务（核心任务，Task 9）。
 *
 * 任务职责：
 *   - 在 IDLE 态阻塞等待按键事件队列 g_key_event_q (Task 6) 的事件。
 *   - 收到 KEY_EVT_B2_SHORT（Key2 短按）后启动一次下载流程：
 *       SD_CHECK -> SEND_START -> SEND_DATA(N) -> SEND_COMPLETE ->
 *       SEND_UPDATE -> IDLE
 *     任何阶段错误超过重试上限或全局超时，转 SEND_TERMINATE -> IDLE。
 *   - 收到 KEY_EVT_B1_LONG（Key1 长按）切换数据源（SD 卡 / MCU Flash），
 *     切到 SD 卡时若 SD_IsPresent 为假则报错并保持原数据源。
 *   - 收到 KEY_EVT_B2_LONG（Key2 长按）切换固件类型（普通 / 工厂）。
 *   - 通过 USART1（bsp_usart.c）发送协议帧、等待应答（g_usart1_rx_sem）。
 *   - TX LED (PA12) 在每个数据段发送时翻转一次。
 *   - 蜂鸣器反馈通过 g_buzzer_cmd_q (Task 8) 投递命令（指针即数值）。
 *
 * 协议流程（设计文档第 10 章）：
 *   Start(0) -> Execute(1) x N -> Complete(2) -> Update(4)
 *   错误时发送 Terminate(3) 终止。
 *
 * 依赖：
 *   - UCOS-III OS_Q / OS_SEM / OSTimeDlyHMSM / OSTimeGet
 *   - BSP_USART1_Send/RecvStart/GetRecvLen/GetRecvBuf + g_usart1_rx_sem (Task 4)
 *   - Proto_Build*Cmd / Proto_ParseReply (Task 5, protocol.c)
 *   - CRC32_Reset/Update/GetResult/Calc (Task 5, crc32.c)
 *   - SD_FileOpen/Read/GetSize/Close/IsPresent (Task 5, sd_card.c)
 *   - SysState_GetDataSource/SetDataSource/GetFwType/SetFwType/
 *     IsTransferLocked/SetTransferLock (Task 5, sys_state.c)
 *   - BSP_LED_Toggle + LED_TX_PORT/PIN (Task 2)
 *   - g_key_event_q + KEY_EVT_* (Task 6, task_key.h)
 *   - g_buzzer_cmd_q + BUZZER_CMD_* (Task 8, task_buzzer.h)
 *   - APP_* 配置宏 (app_cfg.h)
 *
 * 任务实体与 g_usart1_rx_sem 的创建由其它任务负责（bsp_usart.c 创建信号量，
 * Task 10 app.c 创建任务），本头文件只提供入口声明。
 */
#ifndef __TASK_DOWNLOAD_H
#define __TASK_DOWNLOAD_H

#include "os.h"

/*---------------------------------------------------------------------------*/
/* 任务入口                                                                   */
/*---------------------------------------------------------------------------*/
void AppTask_Download(void *p_arg);

#endif /* __TASK_DOWNLOAD_H */
