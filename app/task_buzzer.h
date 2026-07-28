/* app/task_buzzer.h
 *
 * 蜂鸣器非阻塞控制任务（UCOS-III OS_Q 驱动）。
 *
 * 任务职责：
 *   阻塞等待 g_buzzer_cmd_q 上的鸣响命令，按命令类型执行对应的蜂鸣
 *   模式（短鸣 / 长鸣 / 错误四声 / 上电 / 传输完成）。其它任务（按键、
 *   下载等）通过 OSQPost 投递命令，自身不阻塞。
 *
 * 命令值：
 *   BUZZER_CMD_BOOT     上电短鸣 0.5s
 *   BUZZER_CMD_SHORT    短鸣 0.5s
 *   BUZZER_CMD_LONG     长鸣 1.5s
 *   BUZZER_CMD_ERROR    连续短鸣 4 声 (单声 0.5s, 间隔 0.5s)
 *   BUZZER_CMD_OK       传输完成长鸣 1.5s
 *
 * ---------------------------------------------------------------------------
 * OSQPost / OSQPend 消息约定 —— 指针即数值（pointer-as-value）
 * ---------------------------------------------------------------------------
 * UCOS-III 的 OSQPost 第二个参数 p_void 是指针，但本模块约定：
 * “把命令值本身当作指针投递”，不在发送侧分配任何缓冲区。这样既避免
 * 了“发送局部变量地址、返回前变量已被回收”的悬挂指针风险，也免去动
 * 态内存。命令值范围远小于 4GB 地址空间，足以区分。
 *
 *   发送方（按键任务 Task 6 / 下载任务 Task 9 等）：
 *
 *       OS_ERR  err;
 *       OSQPost(&g_buzzer_cmd_q,
 *               (void *)(CPU_ADDR)BUZZER_CMD_SHORT,
 *               sizeof(uint32_t),
 *               OS_OPT_POST_FIFO,
 *               &err);
 *
 *   接收方（本任务）：
 *
 *       void    *p_msg = OSQPend(...);
 *       uint32_t cmd   = (uint32_t)(CPU_ADDR)p_msg;
 *
 * 注：msg_size 形参仍传 sizeof(uint32_t) 以满足 OSQPost 的 API 语义，
 *     实际不依赖该字段解码命令（指针本身即命令）。
 *
 * 依赖：
 *   - UCOS-III OS_Q（Task 1 已在 os_cfg.h 使能 OS_CFG_Q_EN=1）
 *   - bsp_gpio.h 的 BSP_Buzzer_On/Off（Task 2，PA3 高电平有效）
 *   - 任务实体与队列的创建由 Task 10 (app.c) 负责，本头文件只提供声明
 *     与命令码，不创建任何内核对象。
 */
#ifndef __TASK_BUZZER_H
#define __TASK_BUZZER_H

#include "os.h"

/*==================== 命令码 ====================*/
#define BUZZER_CMD_BOOT     1u  /* 上电：短鸣 0.5s */
#define BUZZER_CMD_SHORT    2u  /* 短鸣 0.5s */
#define BUZZER_CMD_LONG     3u  /* 长鸣 1.5s */
#define BUZZER_CMD_ERROR    4u  /* 连续短鸣 4 声 (单声 0.5s, 间隔 0.5s) */
#define BUZZER_CMD_OK       5u  /* 传输完成：长鸣 1.5s */

/*==================== 全局对象（Task 10 中 OSQCreate） ====================*/
extern OS_Q g_buzzer_cmd_q;

/*==================== 任务入口 ====================*/
void AppTask_Buzzer(void *p_arg);

#endif /* __TASK_BUZZER_H */
