/*
*********************************************************************************************************
*                                            APPLICATION
*
*                                       STM32F103VET6 + uC/OS-III
*
* Filename      : app.c
* Description   : 固件下载器主应用 - 系统集成（5 任务 + IPC + 上电自检）。
*                 起始任务负责 BSP/CPU/SysTick/Mem 初始化、系统状态初始化、
*                 SD 卡探测、创建 IPC 队列与 4 个业务任务，最后发出上电
*                 提示音并进入 1s 监控循环（不删除，保留为最低业务优先级监控）。
*
* 任务清单：
*   AppTaskStart    prio 26  272 stk   启动 + 监控
*   AppTaskDownload prio 20 1024 stk   下载状态机（核心业务）
*   AppTaskBuzzer   prio 24  256 stk   蜂鸣器（队列驱动，非阻塞）
*   AppTaskKey      prio 25  256 stk   按键扫描（20ms 消抖 + 短/长按）
*   AppTaskLED_WDG  prio 28  256 stk   LED 指示 + IWDG 喂狗
*
* IPC：
*   g_key_event_q    OS_Q  8 deep  Key -> Download  （指针即数值）
*   g_buzzer_cmd_q   OS_Q  8 deep  Key/Download -> Buzzer（指针即数值）
*   g_usart1_rx_sem  OS_SEM        在 BSP_USART_Init 中创建（Task 3）
*********************************************************************************************************
*/

#include <includes.h>


/*
*********************************************************************************************************
*                                                 TCB
*********************************************************************************************************
*/

OS_TCB   AppTaskStartTCB;
OS_TCB   AppTaskKeyTCB;
OS_TCB   AppTaskBuzzerTCB;
OS_TCB   AppTaskLedWdgTCB;
OS_TCB   AppTaskDownloadTCB;


/*
*********************************************************************************************************
*                                                STACKS
*********************************************************************************************************
*/

__align(8) static  CPU_STK  AppTaskStartStk    [APP_TASK_START_STK_SIZE];
__align(8) static  CPU_STK  AppTaskKeyStk      [APP_TASK_KEY_STK_SIZE];
__align(8) static  CPU_STK  AppTaskBuzzerStk   [APP_TASK_BUZZER_STK_SIZE];
__align(8) static  CPU_STK  AppTaskLedWdgStk   [APP_TASK_LED_WDG_STK_SIZE];
__align(8) static  CPU_STK  AppTaskDownloadStk [APP_TASK_DOWNLOAD_STK_SIZE];


/*
*********************************************************************************************************
*                                         FUNCTION PROTOTYPES
*********************************************************************************************************
*/

static  void  AppTaskStart  (void *p_arg);


/*
*********************************************************************************************************
*                                                main()
*********************************************************************************************************
*/

int  main (void)
{
    OS_ERR  err;


    OSInit(&err);

    /* 起始任务（负责 BSP/CPU/SysTick 初始化 + 创建业务任务）                */
    OSTaskCreate((OS_TCB     *)&AppTaskStartTCB,
                 (CPU_CHAR   *)"App Task Start",
                 (OS_TASK_PTR ) AppTaskStart,
                 (void       *) 0,
                 (OS_PRIO     ) APP_TASK_START_PRIO,
                 (CPU_STK    *)&AppTaskStartStk[0],
                 (CPU_STK_SIZE) APP_TASK_START_STK_SIZE / 10,
                 (CPU_STK_SIZE) APP_TASK_START_STK_SIZE,
                 (OS_MSG_QTY  ) 5u,
                 (OS_TICK     ) 0u,
                 (void       *) 0,
                 (OS_OPT      )(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR     *)&err);

    OSStart(&err);

    while (1) {                                     /* 永不到达                          */
    }
}


/*
*********************************************************************************************************
*                                          STARTUP TASK
*
* Description : 系统启动任务。完成全部初始化后创建 4 个业务任务、发出上电
*               提示音，然后进入 1s 空闲循环（不删除，保留为最低业务优先级
*               监控任务）。
*********************************************************************************************************
*/

static  void  AppTaskStart (void *p_arg)
{
    OS_ERR       err;
    CPU_INT32U   cpu_clk_freq;
    CPU_INT32U   cnts;

    (void)p_arg;

    /* ---- 1. BSP / CPU / SysTick / Mem ---- */
    BSP_Init();                                    /* GPIO + IWDG + USART + SPI2         */
    CPU_Init();

    cpu_clk_freq = BSP_CPU_ClkFreq();
    cnts         = cpu_clk_freq / (CPU_INT32U)OSCfg_TickRate_Hz;
    OS_CPU_SysTickInit(cnts);                      /* SysTick 500Hz                      */

    Mem_Init();

#if OS_CFG_STAT_TASK_EN > 0u
    OSStatTaskCPUUsageInit(&err);                  /* 统计任务基线                       */
#endif

    CPU_IntDisMeasMaxCurReset();

    OSSchedRoundRobinCfg(DEF_ENABLED, 0u, &err);   /* 使能时间片轮转                     */

    /* ---- 2. 系统状态初始化（互斥量 + 默认 SD_CARD / NORMAL / 解锁） ---- */
    SysState_Init();

    /* ---- 3. SD 卡初始化（失败仅警告，不阻塞启动） ---- */
    if (SD_Init() != SD_OK) {
        BSP_USART2_Printf("Warning: SD card not detected.\r\n");
    }

    /* ---- 4. 创建 IPC 对象 ---- */
    /* g_usart1_rx_sem 由 BSP_USART_Init 创建（Task 3），此处不再创建        */
    OSQCreate(&g_key_event_q,
              "Key Q",
              APP_KEY_Q_SIZE,
              &err);

    OSQCreate(&g_buzzer_cmd_q,
              "Buzzer Q",
              APP_BUZZER_Q_SIZE,
              &err);

    /* ---- 5. 创建业务任务（按优先级从高到低：Download 20 -> Buzzer 24 ->
     *        Key 25 -> LED_WDG 28）                                       */
    OSTaskCreate((OS_TCB     *)&AppTaskDownloadTCB,
                 (CPU_CHAR   *)"Download",
                 (OS_TASK_PTR ) AppTask_Download,
                 (void       *) 0,
                 (OS_PRIO     ) APP_TASK_DOWNLOAD_PRIO,
                 (CPU_STK    *)&AppTaskDownloadStk[0],
                 (CPU_STK_SIZE) APP_TASK_DOWNLOAD_STK_SIZE / 10,
                 (CPU_STK_SIZE) APP_TASK_DOWNLOAD_STK_SIZE,
                 (OS_MSG_QTY  ) 0u,
                 (OS_TICK     ) 0u,
                 (void       *) 0,
                 (OS_OPT      )(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR     *)&err);

    OSTaskCreate((OS_TCB     *)&AppTaskBuzzerTCB,
                 (CPU_CHAR   *)"Buzzer",
                 (OS_TASK_PTR ) AppTask_Buzzer,
                 (void       *) 0,
                 (OS_PRIO     ) APP_TASK_BUZZER_PRIO,
                 (CPU_STK    *)&AppTaskBuzzerStk[0],
                 (CPU_STK_SIZE) APP_TASK_BUZZER_STK_SIZE / 10,
                 (CPU_STK_SIZE) APP_TASK_BUZZER_STK_SIZE,
                 (OS_MSG_QTY  ) 0u,
                 (OS_TICK     ) 0u,
                 (void       *) 0,
                 (OS_OPT      )(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR     *)&err);

    OSTaskCreate((OS_TCB     *)&AppTaskKeyTCB,
                 (CPU_CHAR   *)"Key",
                 (OS_TASK_PTR ) AppTask_Key,
                 (void       *) 0,
                 (OS_PRIO     ) APP_TASK_KEY_PRIO,
                 (CPU_STK    *)&AppTaskKeyStk[0],
                 (CPU_STK_SIZE) APP_TASK_KEY_STK_SIZE / 10,
                 (CPU_STK_SIZE) APP_TASK_KEY_STK_SIZE,
                 (OS_MSG_QTY  ) 0u,
                 (OS_TICK     ) 0u,
                 (void       *) 0,
                 (OS_OPT      )(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR     *)&err);

    OSTaskCreate((OS_TCB     *)&AppTaskLedWdgTCB,
                 (CPU_CHAR   *)"LED+WDG",
                 (OS_TASK_PTR ) AppTask_LED_WDG,
                 (void       *) 0,
                 (OS_PRIO     ) APP_TASK_LED_WDG_PRIO,
                 (CPU_STK    *)&AppTaskLedWdgStk[0],
                 (CPU_STK_SIZE) APP_TASK_LED_WDG_STK_SIZE / 10,
                 (CPU_STK_SIZE) APP_TASK_LED_WDG_STK_SIZE,
                 (OS_MSG_QTY  ) 0u,
                 (OS_TICK     ) 0u,
                 (void       *) 0,
                 (OS_OPT      )(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR     *)&err);

    /* ---- 6. 上电提示音（指针即数值：BUZZER_CMD_BOOT 当作指针投递） ---- */
    OSQPost(&g_buzzer_cmd_q,
            (void *)(CPU_ADDR)BUZZER_CMD_BOOT,
            sizeof(uint32_t),
            OS_OPT_POST_FIFO,
            &err);

    BSP_USART2_Printf("FW Downloader V2.0 Ready.\r\n");

    /* ---- 7. 启动任务进入 1s 监控循环（不删除） ---- */
    while (DEF_TRUE) {
        OSTimeDlyHMSM(0, 0, 1, 0,
                       OS_OPT_TIME_HMSM_STRICT,
                       &err);
    }
}
