/* app/task_buzzer.c
 *
 * 蜂鸣器非阻塞控制任务实现。详见 task_buzzer.h 顶部说明。
 *
 * 关键点：
 *   - 任务仅在 OSQPend 上阻塞；执行 Buzzer_Beep 时调用 OSTimeDlyHMSM
 *     让出 CPU（非忙等），系统其它任务照常调度。
 *   - 指针即数值：从 OSQPend 取回的 p_msg 直接强转为命令值。
 *   - 队列容量 APP_BUZZER_Q_SIZE=8，命令在队列满时由发送侧处理
 *     （本任务不关心发送失败）。
 *
 * Tick = 500Hz (2ms/tick)，OSTimeDlyHMSM 内部完成 ms->tick 换算，
 * 500ms/1500ms 均为 tick 整数倍，无量化误差。
 */
#include "includes.h"
#include "task_buzzer.h"

/*---------------------------------------------------------------------------*/
/* 全局对象定义（Task 10 中由 OSQCreate 初始化）                              */
/*---------------------------------------------------------------------------*/
OS_Q g_buzzer_cmd_q;

/*---------------------------------------------------------------------------*/
/* Buzzer_Beep：单次鸣响                                                      */
/*   on_ms  : 蜂鸣器持续鸣响毫秒数                                            */
/*   off_ms : 鸣响结束后静音毫秒数（0 表示不留间隔）                          */
/*---------------------------------------------------------------------------*/
static void Buzzer_Beep(uint32_t on_ms, uint32_t off_ms)
{
    OS_ERR err;

    BSP_Buzzer_On();
    OSTimeDlyHMSM(0u, 0u, 0u, on_ms,
                  OS_OPT_TIME_HMSM_STRICT,
                  &err);
    BSP_Buzzer_Off();

    if (off_ms > 0u) {
        OSTimeDlyHMSM(0u, 0u, 0u, off_ms,
                      OS_OPT_TIME_HMSM_STRICT,
                      &err);
    }
}

/*---------------------------------------------------------------------------*/
/* AppTask_Buzzer：蜂鸣器任务主体                                             */
/*   p_arg : 未使用（UCOS-III 任务入口约定）                                  */
/*---------------------------------------------------------------------------*/
void AppTask_Buzzer(void *p_arg)
{
    OS_ERR       err;
    void        *p_msg;
    OS_MSG_SIZE  msg_size;
    uint32_t     cmd;
    uint8_t      i;

    (void)p_arg;

    while (DEF_TRUE) {
        /* 阻塞等待命令，timeout=0 表示无限等待 */
        p_msg = OSQPend(&g_buzzer_cmd_q,
                        0u,
                        OS_OPT_PEND_BLOCKING,
                        &msg_size,
                        (CPU_TS *)0,
                        &err);

        if (err != OS_ERR_NONE) {
            /* 队列未创建 / 被删除 / pend abort 等异常：静音并重试 */
            BSP_Buzzer_Off();
            continue;
        }

        /* 指针即数值：直接强转得到命令码（见头文件约定） */
        cmd = (uint32_t)(CPU_ADDR)p_msg;

        switch (cmd) {
            case BUZZER_CMD_BOOT:
            case BUZZER_CMD_SHORT:
                Buzzer_Beep(500u, 0u);
                break;

            case BUZZER_CMD_LONG:
            case BUZZER_CMD_OK:
                Buzzer_Beep(1500u, 0u);
                break;

            case BUZZER_CMD_ERROR:
                /* 连续 4 声短鸣，单声 0.5s，间隔 0.5s */
                for (i = 0u; i < 4u; i++) {
                    Buzzer_Beep(500u, 500u);
                }
                break;

            default:
                /* 未知命令：忽略，确保蜂鸣器处于关闭状态 */
                BSP_Buzzer_Off();
                break;
        }
    }
}
