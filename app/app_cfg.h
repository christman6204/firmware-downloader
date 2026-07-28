/* app/app_cfg.h - 完整替换 */
#ifndef  __APP_CFG_H__
#define  __APP_CFG_H__

/* ==================== 任务优先级 ==================== */
/* 内核占用: Tick=10, Tmr=22, Stat=23, Idle=31 */
#define  APP_TASK_START_PRIO          26u
#define  APP_TASK_KEY_PRIO            25u
#define  APP_TASK_LED_WDG_PRIO        28u
#define  APP_TASK_DOWNLOAD_PRIO       20u

/* ==================== 任务栈（CPU_STK = 4字节） ==================== */
#define  APP_TASK_START_STK_SIZE      272u   /* 1088B */
#define  APP_TASK_KEY_STK_SIZE        256u   /* 1024B */
#define  APP_TASK_LED_WDG_STK_SIZE    256u   /* 1024B */
#define  APP_TASK_DOWNLOAD_STK_SIZE   1024u  /* 4096B */

/* ==================== IPC 参数 ==================== */
#define  APP_KEY_Q_SIZE               8u

/* ==================== 按键参数 ==================== */
#define  APP_KEY_DEBOUNCE_MS          20u
#define  APP_KEY_LONG_PRESS_MS        2000u
/* 500Hz tick -> 2ms/tick */
#define  APP_KEY_DEBOUNCE_TICKS       (APP_KEY_DEBOUNCE_MS / 2u)    /* 10 */
#define  APP_KEY_LONG_PRESS_TICKS     (APP_KEY_LONG_PRESS_MS / 2u)  /* 1000 */

/* ==================== 传输参数 ==================== */
#define  APP_SEGMENT_SIZE             256u
#define  APP_CMD_TIMEOUT_MS           1000u
#define  APP_CMD_TIMEOUT_TICKS        (APP_CMD_TIMEOUT_MS / 2u)     /* 500 */
#define  APP_CMD_MAX_RETRY            6u
#define  APP_GLOBAL_TIMEOUT_MIN       6u
#define  APP_ERROR_MAX_RETRY          6u
#define  APP_FRAME_MAX_DATA           300u

/* ==================== Flash 地址（设计文档） ==================== */
#define  APP_FLASH_BASE               0x08000000u
#define  APP_FLASH_BOOT_SIZE          0x20000u     /* 128KB */
#define  APP_FLASH_APP_ADDR           0x08020000u
#define  APP_FLASH_APP_MAX_SIZE       0x4B000u     /* 300KB */

/* ==================== SD 卡 ==================== */
#define  APP_SD_BIN_FILENAME          "APP.bin"

/* ==================== 协议常量 ==================== */
#define  APP_DEV_ID                   999999u
#define  APP_HOST_ID                  99u
#define  APP_NET_TYPE                 3u
#define  APP_MSG_TYPE                 1u
#define  APP_VERIFY_CODE              58902u

/* ==================== 固件类型编码 ==================== */
#define  FW_TYPE_NORMAL               0u   /* 传输命令 */
#define  FW_TYPE_FACTORY              1u
#define  FW_UPDATE_FACTORY            0u   /* 更新命令（故意不同） */
#define  FW_UPDATE_NORMAL             2u

/* ==================== 调试 ==================== */
#define  BSP_CFG_SER_COMM_SEL         BSP_SER_COMM_UART_02
#define  BSP_CFG_TS_TMR_SEL           2
#define  APP_TRACE_LEVEL              TRACE_LEVEL_INFO
#define  APP_TRACE                    BSP_Ser_Printf
#define  APP_TRACE_INFO(x)  ((APP_TRACE_LEVEL >= TRACE_LEVEL_INFO) ? (void)(APP_TRACE x) : (void)0)
#define  MCU_ID_ADDR                  0x1FFFF7E8u

#endif
