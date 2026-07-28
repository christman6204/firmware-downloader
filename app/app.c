/*
*********************************************************************************************************
*                                            APPLICATION
*
*                                       STM32F103VE + uC/OS-III
*
* Filename      : app.c
* Description   : 干净 RTOS 底座 + PE0 闪灯测试任务。
*                 原产品业务逻辑已剥离归档至 old_product/。
*********************************************************************************************************
*/

#include <includes.h>


/*
*********************************************************************************************************
*                                             LOCAL DEFINES
*********************************************************************************************************
*/

#define  BLINK_DLY_MS    500u          /* 闪灯周期 ms（亮灭各 500ms）               */


/*
*********************************************************************************************************
*                                                 TCB
*********************************************************************************************************
*/

OS_TCB   AppTaskStartTCB;
OS_TCB   AppTaskBlinkTCB;


/*
*********************************************************************************************************
*                                                STACKS
*********************************************************************************************************
*/

__align(8) static  CPU_STK  AppTaskStartStk[APP_TASK_START_STK_SIZE];
__align(8) static  CPU_STK  AppTaskBlinkStk[APP_TASK_BLINK_STK_SIZE];


/*
*********************************************************************************************************
*                                         FUNCTION PROTOTYPES
*********************************************************************************************************
*/

static  void  AppTaskStart  (void *p_arg);
static  void  AppTaskBlink  (void *p_arg);


/*
*********************************************************************************************************
*                                                main()
*********************************************************************************************************
*/

int  main (void)
{
    OS_ERR  err;


    OSInit(&err);

    /* 起始任务（负责 BSP/CPU/SysTick 初始化）                                    */
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

    /* PE0 闪灯测试任务                                                           */
    OSTaskCreate((OS_TCB     *)&AppTaskBlinkTCB,
                 (CPU_CHAR   *)"App Task Blink",
                 (OS_TASK_PTR ) AppTaskBlink,
                 (void       *) 0,
                 (OS_PRIO     ) APP_TASK_BLINK_PRIO,
                 (CPU_STK    *)&AppTaskBlinkStk[0],
                 (CPU_STK_SIZE) APP_TASK_BLINK_STK_SIZE / 10,
                 (CPU_STK_SIZE) APP_TASK_BLINK_STK_SIZE,
                 (OS_MSG_QTY  ) 0u,
                 (OS_TICK     ) 0u,
                 (void       *) 0,
                 (OS_OPT      )(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR     *)&err);

    OSStart(&err);
}


/*
*********************************************************************************************************
*                                          STARTUP TASK
*********************************************************************************************************
*/

static  void  AppTaskStart (void *p_arg)
{
    OS_ERR       err;
    CPU_INT32U   cpu_clk_freq;
    CPU_INT32U   cnts;

    (void)p_arg;

    BSP_Init();                                    /* 板级初始化（含 PE0 LED）          */
    CPU_Init();

    cpu_clk_freq = BSP_CPU_ClkFreq();
    cnts = cpu_clk_freq / (CPU_INT32U)OSCfg_TickRate_Hz;
    OS_CPU_SysTickInit(cnts);                      /* SysTick 500Hz                    */

    Mem_Init();

#if OS_CFG_STAT_TASK_EN > 0u
    OSStatTaskCPUUsageInit(&err);
#endif

    CPU_IntDisMeasMaxCurReset();

    OSSchedRoundRobinCfg(DEF_ENABLED, 0, &err);

    while (1) {
        OSTimeDlyHMSM(0, 0, 1, 0,
                       OS_OPT_TIME_HMSM_STRICT,
                       &err);                     /* 1s 空闲循环                       */
    }
}


/*
*********************************************************************************************************
*                                          BLINK TASK (TEST)
*
* Description : PE0 闪灯测试任务，500ms 亮 / 500ms 灭循环。
*               验证 RTOS + BSP_Init + GPIO 正常后，后续可删除此任务或改为业务任务。
*********************************************************************************************************
*/

static  void  AppTaskBlink (void *p_arg)
{
    OS_ERR  err;

    (void)p_arg;

    while (1) {
        LED_ON();
        OSTimeDlyHMSM(0, 0, 0, BLINK_DLY_MS,
                       OS_OPT_TIME_HMSM_STRICT,
                       &err);
        LED_OFF();
        OSTimeDlyHMSM(0, 0, 0, BLINK_DLY_MS,
                       OS_OPT_TIME_HMSM_STRICT,
                       &err);
    }
}
