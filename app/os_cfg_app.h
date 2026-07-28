/*
************************************************************************************************************************
*                                                     uC/OS-III
*                                                The Real-Time Kernel
*
*                                  (c) Copyright 2009-2010; Micrium, Inc.; Weston, FL
*                          All rights reserved.  Protected by international copyright laws.
*
*                                       OS CONFIGURATION (APPLICATION SPECIFICS)
*
* File    : OS_CFG_APP.H
* By      : JJL
* Version : V3.01.2
*
* LICENSING TERMS:
* ---------------
*               uC/OS-III is provided in source form to registered licensees ONLY.  It is 
*               illegal to distribute this source code to any third party unless you receive 
*               written permission by an authorized Micrium representative.  Knowledge of 
*               the source code may NOT be used to develop a similar product.
*
*               Please help us continue to provide the Embedded community with the finest
*               software available.  Your honesty is greatly appreciated.
*
*               You can contact us at www.micrium.com.
************************************************************************************************************************
*/

#ifndef OS_CFG_APP_H
#define OS_CFG_APP_H

/*
************************************************************************************************************************
*                                                      CONSTANTS
************************************************************************************************************************
*/

                                                            /* --------------------- MISCELLANEOUS ------------------ */
#define  OS_CFG_MSG_POOL_SIZE            100u               //��Ϣ�������Ŀ/* Maximum number of messages                             */
#define  OS_CFG_ISR_STK_SIZE             256u               /* Stack size of ISR stack (number of CPU_STK elements)   */
#define  OS_CFG_TASK_STK_LIMIT_PCT_EMPTY  10u               /* Stack limit position in percentage to empty            */


                                                            /* ---------------------- IDLE TASK --------------------- */
#define  OS_CFG_IDLE_TASK_STK_SIZE       128u               /* Stack size (number of CPU_STK elements)                */


                                                            /* ------------------ ISR HANDLER TASK ------------------ */
#define  OS_CFG_INT_Q_SIZE               32u               //�жϴ���������еĴ�С/* Size of ISR handler task queue                         */
#define  OS_CFG_INT_Q_TASK_STK_SIZE      512u               //�жϴ��������ջ��С����λ��CPU_STK��/* Stack size (number of CPU_STK elements)                */


                                                            /* ------------------- STATISTIC TASK ------------------- */
#define  OS_CFG_STAT_TASK_PRIO            23u               /* Priority                                               */
#define  OS_CFG_STAT_TASK_RATE_HZ         10u               /* Rate of execution (10 Hz Typ.)                         */
#define  OS_CFG_STAT_TASK_STK_SIZE       128u               /* Stack size (number of CPU_STK elements)                */


                                                            /* ------------------------ TICKS ----------------------- */
#define  OS_CFG_TICK_RATE_HZ            500u               // ʱ�ӽ���Ƶ�� (10 to 1000 Hz)                    
#define  OS_CFG_TICK_TASK_PRIO            10u               // ʱ�ӽ������� OS_TickTask() �����ȼ�
#define  OS_CFG_TICK_TASK_STK_SIZE       128u               // ʱ�ӽ������� OS_TickTask() ��ջ�ռ��С����λ��CPU_STK��
#define  OS_CFG_TICK_WHEEL_SIZE           11u               // OSCfg_TickWheel ����Ĵ�С���Ƽ�ʹ����������/4����Ϊ����


                                                            /* ----------------------- TIMERS ----------------------- */
#define  OS_CFG_TMR_TASK_PRIO             22u               //��ʱ����������ȼ�
#define  OS_CFG_TMR_TASK_RATE_HZ          50u               //��ʱ����ʱ�� (һ�㲻�ܴ��� OS_CFG_TICK_RATE_HZ )  
#define  OS_CFG_TMR_TASK_STK_SIZE        256u               //��ʱ�������ջ�ռ��С����λ��CPU_STK��
#define  OS_CFG_TMR_WHEEL_SIZE            11u               // OSCfg_TmrWheel ����Ĵ�С���Ƽ�ʹ����������/4����Ϊ����

#endif









