/* app/task_download.c
 *
 * 固件下载状态机任务实现（核心任务，Task 9）。详见 task_download.h 顶部说明。
 *
 * 关键点：
 *   - 状态机：IDLE -> SD_CHECK -> SEND_START -> SEND_DATA -> SEND_COMPLETE
 *             -> SEND_UPDATE -> IDLE；任一阶段错误超限 / 全局超时 ->
 *             SEND_TERMINATE -> IDLE。
 *   - DL_SendAndWait：发送一帧 -> 阻塞等待 g_usart1_rx_sem
 *     (APP_CMD_TIMEOUT_TICKS=500，即 1s @500Hz tick) -> 解析应答。
 *     超时返回 0xFF，解析失败返回 0xFE，成功返回 status 字段。
 *   - g_reply_content 必须为 FRAME_MAX_CMD_LEN (300) 字节：Proto_ParseReply
 *     会将其作为 in-place 临时缓冲复用（Task 5 review note）。
 *   - 全局超时：APP_GLOBAL_TIMEOUT_MIN(6) * 60 * 500 = 180000 tick = 6min。
 *     每次 SEND_* 状态结尾检查；IDLE/SD_CHECK 不计入（start_tick 在
 *     KEY_EVT_B2_SHORT 触发时记录）。
 *   - 重试计数：
 *       retry_cnt   —— 命令级（超时 / WAIT / 未知错误），上限 APP_CMD_MAX_RETRY(6)。
 *       error_retry —— 段级（170/175/176/177 写错误），上限 APP_ERROR_MAX_RETRY(6)。
 *   - 段大小 APP_SEGMENT_SIZE(256)；MCU Flash 源固定 300KB。
 *   - TX LED (PA12) 在每个数据段发送时翻转一次（BSP_LED_Toggle）。
 *   - 指针即数值：g_key_event_q 接收、g_buzzer_cmd_q 发送均按此约定。
 *
 * 字节序：fw_ver / fw_size / fw_crc / seg_size / seg_crc / offset 均小端；
 *         SD 卡前 2 字节 = fw_ver（小端）。
 *
 * 依赖：见 task_download.h。无 PC 端测试（依赖 UCOS-III / BSP），
 *       仅保证 C 逻辑与设计文档一致。
 */
#include "includes.h"
#include "task_download.h"
#include "task_key.h"
#include "task_buzzer.h"
#include "protocol.h"
#include "crc32.h"
#include "sd_card.h"
#include "sys_state.h"
#include "bsp_usart.h"
#include "bsp_gpio.h"
#include "app_cfg.h"
#include <string.h>

/*---------------------------------------------------------------------------*/
/* 状态机状态                                                                 */
/*---------------------------------------------------------------------------*/
typedef enum {
    DL_STATE_IDLE,
    DL_STATE_SD_CHECK,
    DL_STATE_SEND_START,
    DL_STATE_SEND_DATA,
    DL_STATE_SEND_COMPLETE,
    DL_STATE_SEND_UPDATE,
    DL_STATE_SEND_TERMINATE
} DL_State_t;

/*---------------------------------------------------------------------------*/
/* 静态缓冲                                                                    */
/*   g_tx_buf         : 帧组装输出（>= FRAME_MAX_TOTAL=320）                 */
/*   g_reply_content  : 应答解析 in-place 缓冲（>= FRAME_MAX_CMD_LEN=300）   */
/*   g_seg_buf        : 单段固件数据（APP_SEGMENT_SIZE=256）                 */
/*---------------------------------------------------------------------------*/
static uint8_t g_tx_buf[FRAME_MAX_TOTAL];
static uint8_t g_reply_content[FRAME_MAX_CMD_LEN];
static uint8_t g_seg_buf[APP_SEGMENT_SIZE];

/*---------------------------------------------------------------------------*/
/* DL_Beep：向蜂鸣器任务投递命令（指针即数值，见 task_buzzer.h）              */
/*---------------------------------------------------------------------------*/
static void DL_Beep(uint32_t cmd)
{
    OS_ERR err;
    OSQPost(&g_buzzer_cmd_q,
            (void *)(CPU_ADDR)cmd,
            sizeof(uint32_t),
            OS_OPT_POST_FIFO,
            &err);
    /* 队列满时忽略：最坏丢失一次鸣响提示，不影响下载流程 */
}

/*---------------------------------------------------------------------------*/
/* DL_SendAndWait：发送一帧 -> 等待应答 -> 解析                              */
/*   frame       : 待发送帧字节                                               */
/*   frame_len   : 帧长度                                                     */
/*   reply_type  : 输出应答 cmd[0]（可空）                                    */
/*   status      : 输出应答 cmd[1]（可空）                                    */
/*   content     : 输出应答正文缓冲（须 >= FRAME_MAX_CMD_LEN，作为 in-place  */
/*                 临时缓冲被 Proto_ParseReply 复用）                         */
/*   content_len : 输出应答正文长度（可空）                                   */
/*   返回: 0xFF = 超时; 0xFE = 解析失败; 其它 = status 字段                   */
/*---------------------------------------------------------------------------*/
static uint8_t DL_SendAndWait(const uint8_t *frame, uint16_t frame_len,
                              uint8_t *reply_type, uint8_t *status,
                              uint8_t *content, uint16_t *content_len)
{
    OS_ERR       err;
    uint16_t     rclen = 0u;
    uint8_t      rtype = 0u;
    uint8_t      rstatus = 0u;
    uint16_t     rx_len;
    const uint8_t *rx_data;
    uint8_t      ret;

    BSP_USART1_RecvStart();                 /* 复位 RX 缓冲，使能 RXNE       */
    BSP_USART1_Send(frame, frame_len);      /* 阻塞发送整帧                  */

    /* 等待 RX 完成信号量，超时 = APP_CMD_TIMEOUT_TICKS (500 = 1s) */
    OSSemPend(&g_usart1_rx_sem,
              APP_CMD_TIMEOUT_TICKS,
              OS_OPT_PEND_BLOCKING,
              (CPU_TS *)0,
              &err);
    if (err == OS_ERR_TIMEOUT) {
        return 0xFFu;                        /* 命令超时                      */
    }
    /* err == OS_ERR_NONE 或其它（如已被删除） -- 尽力解析已收到的字节 */

    rx_len = BSP_USART1_GetRecvLen();
    rx_data = BSP_USART1_GetRecvBuf();

    /* Proto_ParseReply 借用 content 作为 in-place cmd 解析缓冲 */
    ret = Proto_ParseReply(rx_data, rx_len, &rtype, &rstatus, content, &rclen);

    if (reply_type)   *reply_type   = rtype;
    if (status)       *status       = rstatus;
    if (content_len)  *content_len  = rclen;

    return (ret == PROTO_OK) ? rstatus : 0xFEu;   /* 0xFE = 解析失败          */
}

/*---------------------------------------------------------------------------*/
/* DL_SendTerminate：发送 Terminate 帧（无视应答，仅通知对端中止）            */
/*---------------------------------------------------------------------------*/
static void DL_SendTerminate(uint16_t fw_ver, uint32_t fw_size)
{
    uint16_t len = 0u;
    uint8_t  dummy_status;
    uint8_t  rtype;
    uint16_t dummy_clen;

    Proto_BuildTerminateCmd(fw_ver, fw_size, g_tx_buf, &len);
    DL_SendAndWait(g_tx_buf, len, &rtype, &dummy_status,
                   g_reply_content, &dummy_clen);
    /* 应答结果丢弃：本端已决定终止 */
}

/*---------------------------------------------------------------------------*/
/* DL_CheckGlobalTimeout：检查全局 6min 超时                                  */
/*   start_tick : 下载开始时刻（OSTimeGet 返回值）                            */
/*   返回 1 = 已超时                                                          */
/*   6min = APP_GLOBAL_TIMEOUT_MIN(6) * 60 * 500 = 180000 tick              */
/*---------------------------------------------------------------------------*/
static uint8_t DL_CheckGlobalTimeout(uint32_t start_tick)
{
    OS_ERR   err;
    uint32_t now;
    uint32_t elapsed;

    now = OSTimeGet(&err);
    /* OSTick 为 uint32_t 自然回绕；此处用差值，单次下载 << 2^32 tick 安全 */
    elapsed = now - start_tick;
    if (elapsed > (uint32_t)((uint32_t)APP_GLOBAL_TIMEOUT_MIN * 60u * 500u)) {
        return 1u;
    }
    return 0u;
}

/*---------------------------------------------------------------------------*/
/* AppTask_Download：下载状态机任务主体                                      */
/*   p_arg : 未使用（UCOS-III 任务入口约定）                                  */
/*---------------------------------------------------------------------------*/
void AppTask_Download(void *p_arg)
{
    OS_ERR        err;
    OS_MSG_SIZE   msg_size;
    void         *p_msg;
    uint32_t      evt;

    DL_State_t    state         = DL_STATE_IDLE;
    DataSource_t  src           = DATA_SRC_SD_CARD;
    uint8_t       fw_type_code  = FW_TYPE_NORMAL;
    uint16_t      fw_ver        = 0u;
    uint32_t      fw_total_size = 0u;
    uint32_t      fw_crc        = 0u;
    uint32_t      offset        = 0u;
    uint16_t      retry_cnt     = 0u;
    uint16_t      error_retry   = 0u;
    uint32_t      start_tick    = 0u;
    uint32_t      sd_fptr       = 0u;   /* SD 文件已读字节偏移（用于重试重定位） */

    /* SEND_* 状态共用的临时变量 */
    uint16_t      len           = 0u;
    uint8_t       status        = 0u;
    uint8_t       rtype         = 0u;
    uint16_t      clen          = 0u;

    (void)p_arg;

    while (DEF_TRUE) {

        switch (state) {

        /*-------------------------------------------------------------------*/
        /* IDLE：阻塞等待按键事件                                            */
        /*-------------------------------------------------------------------*/
        case DL_STATE_IDLE:
        {
            p_msg = OSQPend(&g_key_event_q,
                            0u,
                            OS_OPT_PEND_BLOCKING,
                            &msg_size,
                            (CPU_TS *)0,
                            &err);
            if (err != OS_ERR_NONE) {
                /* 队列异常：忽略，继续等待 */
                break;
            }
            evt = (uint32_t)(CPU_ADDR)p_msg;

            if (evt == KEY_EVT_B1_LONG) {
                /* 切换数据源：SD 卡 <-> MCU Flash */
                DataSource_t cur = SysState_GetDataSource();
                DataSource_t next = (cur == DATA_SRC_SD_CARD)
                                    ? DATA_SRC_MCU_FLASH
                                    : DATA_SRC_SD_CARD;
                if (next == DATA_SRC_SD_CARD) {
                    if (!SD_IsPresent()) {
                        /* SD 卡不在：报错并保持当前数据源 */
                        DL_Beep(BUZZER_CMD_ERROR);
                        break;
                    }
                }
                SysState_SetDataSource(next);
            }
            else if (evt == KEY_EVT_B2_LONG) {
                /* 切换固件类型：普通 <-> 工厂 */
                FwType_t cur = SysState_GetFwType();
                FwType_t next = (cur == FW_NORMAL) ? FW_FACTORY : FW_NORMAL;
                SysState_SetFwType(next);
            }
            else if (evt == KEY_EVT_B2_SHORT) {
                /* 启动下载：短鸣反馈 -> 加锁 -> 进入 SD_CHECK */
                DL_Beep(BUZZER_CMD_SHORT);
                SysState_SetTransferLock(1u);
                start_tick = OSTimeGet(&err);
                state = DL_STATE_SD_CHECK;
            }
            /* 其它事件码忽略 */
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SD_CHECK：探测数据源，读取 fw_ver/fw_size/fw_crc                   */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SD_CHECK:
        {
            src = SysState_GetDataSource();

            if (src == DATA_SRC_SD_CARD) {
                /* ---- SD 卡源 ---- */
                if (SD_FileOpen() != SD_OK) {
                    DL_Beep(BUZZER_CMD_ERROR);
                    SysState_SetTransferLock(0u);
                    state = DL_STATE_IDLE;
                    break;
                }
                fw_total_size = SD_FileGetSize();

                /* fw_ver = 文件前 2 字节（小端） */
                {
                    uint8_t  ver_buf[2] = {0u, 0u};
                    uint16_t br_ver = 0u;
                    if (SD_FileRead(ver_buf, 2u, &br_ver) != SD_OK || br_ver != 2u) {
                        DL_Beep(BUZZER_CMD_ERROR);
                        SD_FileClose();
                        SysState_SetTransferLock(0u);
                        state = DL_STATE_IDLE;
                        break;
                    }
                    fw_ver = (uint16_t)((uint16_t)ver_buf[0]
                              | ((uint16_t)ver_buf[1] << 8));
                }

                /* 关闭后重开，从头计算整包 CRC32 */
                SD_FileClose();
                if (SD_FileOpen() != SD_OK) {
                    DL_Beep(BUZZER_CMD_ERROR);
                    SysState_SetTransferLock(0u);
                    state = DL_STATE_IDLE;
                    break;
                }
                {
                    uint32_t remaining = fw_total_size;
                    CRC32_Reset();
                    while (remaining > 0u) {
                        uint32_t chunk = (remaining > (uint32_t)APP_SEGMENT_SIZE)
                                         ? (uint32_t)APP_SEGMENT_SIZE
                                         : remaining;
                        uint16_t br = 0u;
                        if (SD_FileRead(g_seg_buf, (uint16_t)chunk, &br) != SD_OK
                            || br == 0u) {
                            break;   /* 读错误 / 提前 EOF：CRC 基于已读部分 */
                        }
                        CRC32_Update(g_seg_buf, br);
                        remaining -= (uint32_t)br;
                    }
                    fw_crc = CRC32_GetResult();
                }

                /* 关闭后重开，准备顺序传输（fptr 归零） */
                SD_FileClose();
                if (SD_FileOpen() != SD_OK) {
                    DL_Beep(BUZZER_CMD_ERROR);
                    SysState_SetTransferLock(0u);
                    state = DL_STATE_IDLE;
                    break;
                }
                sd_fptr = 0u;   /* fptr 已归零，与 offset=0 对齐 */
            }
            else {
                /* ---- MCU Flash 源 ---- */
                const uint8_t *flash_ptr = (const uint8_t *)APP_FLASH_APP_ADDR;

                fw_total_size = APP_FLASH_APP_MAX_SIZE;   /* 300KB */
                fw_ver = (uint16_t)((uint16_t)flash_ptr[0]
                          | ((uint16_t)flash_ptr[1] << 8));
                fw_crc = CRC32_Calc(flash_ptr, fw_total_size);
                /* MCU Flash 源无 fptr 概念，sd_fptr 不使用 */
            }

            fw_type_code = (SysState_GetFwType() == FW_NORMAL)
                           ? FW_TYPE_NORMAL : FW_TYPE_FACTORY;
            offset      = 0u;
            retry_cnt   = 0u;
            error_retry = 0u;
            state = DL_STATE_SEND_START;
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_START：发送 Start 帧，等待 ACK                              */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_START:
        {
            Proto_BuildStartCmd(fw_type_code, fw_ver, fw_total_size, fw_crc,
                                g_tx_buf, &len);
            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_START_OK) {
                state      = DL_STATE_SEND_DATA;
                retry_cnt  = 0u;
                error_retry = 0u;
            }
            else if (status == STATUS_WAIT) {
                /* 设备忙：等待 1s 后重试 */
                retry_cnt++;
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    state = DL_STATE_SEND_TERMINATE;
                } else {
                    OSTimeDlyHMSM(0u, 0u, 1u, 0u,
                                  OS_OPT_TIME_HMSM_STRICT, &err);
                }
            }
            else if (status == 0xFFu) {
                /* 命令超时 */
                retry_cnt++;
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    state = DL_STATE_SEND_TERMINATE;
                }
            }
            else {
                /* 其它错误 / 解析失败 (0xFE) / 鉴权失败等：直接终止 */
                state = DL_STATE_SEND_TERMINATE;
            }

            if (DL_CheckGlobalTimeout(start_tick)) {
                state = DL_STATE_SEND_TERMINATE;
            }
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_DATA：分段发送固件数据                                      */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_DATA:
        {
            uint32_t remaining = fw_total_size - offset;
            uint32_t seg_size  = (remaining > (uint32_t)APP_SEGMENT_SIZE)
                                 ? (uint32_t)APP_SEGMENT_SIZE
                                 : remaining;
            uint32_t seg_crc;

            /* SD 源：确保 fptr 对齐 offset。正常流程 sd_fptr == offset；
               任何重试（超时 / 写错误 / 读失败）后 sd_fptr 会越过 offset，
               此处重定位。无 f_lseek 封装，靠重开 + 丢弃前缀实现。 */
            if (src == DATA_SRC_SD_CARD && sd_fptr != offset) {
                uint32_t skip = offset;
                SD_FileClose();
                (void)SD_FileOpen();
                sd_fptr = 0u;
                while (skip >= (uint32_t)APP_SEGMENT_SIZE) {
                    uint16_t br = 0u;
                    if (SD_FileRead(g_seg_buf,
                                    (uint16_t)APP_SEGMENT_SIZE,
                                    &br) != SD_OK || br == 0u) {
                        break;
                    }
                    skip     -= (uint32_t)br;
                    sd_fptr  += (uint32_t)br;
                }
                if (skip > 0u) {
                    uint16_t br = 0u;
                    (void)SD_FileRead(g_seg_buf, (uint16_t)skip, &br);
                    sd_fptr += (uint32_t)br;
                }
            }

            /* 取一段数据到 g_seg_buf */
            if (src == DATA_SRC_SD_CARD) {
                uint16_t br = 0u;
                if (SD_FileRead(g_seg_buf, (uint16_t)seg_size, &br) != SD_OK
                    || br != (uint16_t)seg_size) {
                    /* 读失败：按命令级重试处理（sd_fptr 不更新，下轮重定位） */
                    retry_cnt++;
                    if (retry_cnt > APP_CMD_MAX_RETRY) {
                        state = DL_STATE_SEND_TERMINATE;
                    }
                    if (DL_CheckGlobalTimeout(start_tick)) {
                        state = DL_STATE_SEND_TERMINATE;
                    }
                    break;
                }
                sd_fptr += (uint32_t)br;   /* fptr 已前进 seg_size */
            }
            else {
                memcpy(g_seg_buf,
                       (const void *)(APP_FLASH_APP_ADDR + offset),
                       (size_t)seg_size);
            }

            seg_crc = CRC32_Calc(g_seg_buf, seg_size);

            Proto_BuildDataCmd(fw_type_code, fw_ver, seg_size, seg_crc,
                               offset, g_seg_buf, (uint16_t)seg_size,
                               g_tx_buf, &len);

            BSP_LED_Toggle(LED_TX_PORT, LED_TX_PIN);   /* TX 活动指示 */

            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_DATA_OK) {
                offset     += seg_size;    /* sd_fptr == offset 重新对齐 */
                retry_cnt   = 0u;
                error_retry = 0u;
                if (offset >= fw_total_size) {
                    state = DL_STATE_SEND_COMPLETE;
                }
            }
            else if (status == STATUS_ADDR_ERR
                  || status == STATUS_WRITE_FAIL
                  || status == STATUS_NO_PERM
                  || status == STATUS_WRITE_FAIL2) {
                /* 写错误：从设备期望地址重试（无期望地址则重试当前段）。
                   sd_fptr 已越过 offset，下轮 SEND_DATA 顶部会重定位。 */
                error_retry++;
                if (error_retry > APP_ERROR_MAX_RETRY) {
                    state = DL_STATE_SEND_TERMINATE;
                }
                else if (clen >= 4u) {
                    uint32_t exp_offset = (uint32_t)g_reply_content[0]
                                | ((uint32_t)g_reply_content[1] << 8)
                                | ((uint32_t)g_reply_content[2] << 16)
                                | ((uint32_t)g_reply_content[3] << 24);
                    /* 设备返回的期望地址越界（含 == fw_total_size 会使
                       remaining 下溢）-> 终止，避免读到包尾之外 */
                    if (exp_offset >= fw_total_size) {
                        state = DL_STATE_SEND_TERMINATE;
                        break;
                    }
                    offset = exp_offset;
                }
                /* clen < 4：offset 不变，重试同一段（下轮重定位） */
            }
            else if (status == STATUS_VER_ERR) {
                /* 版本不匹配：直接终止 */
                state = DL_STATE_SEND_TERMINATE;
            }
            else if (status == 0xFFu) {
                /* 命令超时：offset 不变，下轮重定位重读同一段 */
                retry_cnt++;
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    state = DL_STATE_SEND_TERMINATE;
                }
            }
            else {
                /* 其它错误 / 解析失败：命令级重试 */
                retry_cnt++;
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    state = DL_STATE_SEND_TERMINATE;
                }
            }

            if (DL_CheckGlobalTimeout(start_tick)) {
                state = DL_STATE_SEND_TERMINATE;
            }
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_COMPLETE：发送 Complete 帧                                  */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_COMPLETE:
        {
            Proto_BuildCompleteCmd(fw_type_code, fw_ver, fw_total_size, fw_crc,
                                   g_tx_buf, &len);
            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_COMPLETE_OK) {
                state      = DL_STATE_SEND_UPDATE;
                retry_cnt  = 0u;
            }
            else {
                retry_cnt++;
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    state = DL_STATE_SEND_TERMINATE;
                }
            }

            if (DL_CheckGlobalTimeout(start_tick)) {
                state = DL_STATE_SEND_TERMINATE;
            }
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_UPDATE：发送 Update 帧，触发对端固件更新                     */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_UPDATE:
        {
            uint8_t update_type = (SysState_GetFwType() == FW_FACTORY)
                                  ? FW_UPDATE_FACTORY
                                  : FW_UPDATE_NORMAL;

            Proto_BuildUpdateCmd(update_type, g_tx_buf, &len);
            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_UPDATE_OK) {
                DL_Beep(BUZZER_CMD_OK);      /* 传输完成 */
            }
            else {
                DL_Beep(BUZZER_CMD_ERROR);   /* 更新失败 */
            }

            SysState_SetTransferLock(0u);
            if (src == DATA_SRC_SD_CARD) {
                SD_FileClose();
            }
            state = DL_STATE_IDLE;
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_TERMINATE：发送 Terminate 帧中止本次下载                    */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_TERMINATE:
        {
            DL_SendTerminate(fw_ver, fw_total_size);
            DL_Beep(BUZZER_CMD_ERROR);

            SysState_SetTransferLock(0u);
            if (src == DATA_SRC_SD_CARD) {
                SD_FileClose();
            }
            state = DL_STATE_IDLE;
            break;
        }

        default:
            state = DL_STATE_IDLE;
            break;

        } /* end switch(state) */

        /* 1ms 让出 CPU，使其它任务（按键 / LED / 蜂鸣器）得以调度 */
        OSTimeDlyHMSM(0u, 0u, 0u, 1u,
                      OS_OPT_TIME_HMSM_STRICT,
                      &err);
    }
}


