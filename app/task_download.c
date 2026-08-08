/* app/task_download.c
 *
 * 固件下载状态机任务实现（核心任务，Task 9）。详见 task_download.h 顶部说明。
 *
 * ===== 状态机流程图（ASCII）=====
 *
 *   IDLE ───B2_SHORT──> SD_CHECK ───成功──> SEND_START ───OK──>
 *    ^                    │ 失败              │  WAIT/超时
 *    │                    v                   v   (重试)
 *    │                  IDLE              SEND_START (重试)
 *    │
 *    │   SEND_START ───OK──> SEND_DATA ───OK(offset>=total)──>
 *    │     │                  │  │  │
 *    │     │                  │  │  └── 写错误(170/175/176/177)
 *    │     │                  │  │         └── error_retry>6 -> TERMINATE
 *    │     │                  │  │             否则: exp_offset 重定位
 *    │     │                  │  └── VER_ERR(171) -> TERMINATE
 *    │     │                  │  └── 超时/unknown -> retry
 *    │     │                  └── retry_cnt>6 -> TERMINATE
 *    │     │
 *    │     └── WAIT/超时 -> TERMINATE (retry>6)
 *    │     └── 其它错误 -> TERMINATE (立即)
 *    │
 *    │   SEND_COMPLETE ─OK──> SEND_UPDATE ─OK──> IDLE (成功)
 *    │     │ 失败(重试)          │ 失败(蜂鸣)
 *    │     └── retry>6 ->        └── IDLE
 *    │         TERMINATE
 *    │
 *    └── TERMINATE ───发送Terminate帧+错误蜂鸣──> IDLE
 *
 * ===== 每个状态职责 =====
 *
 *   IDLE           : 阻塞等待按键事件队列，B2_SHORT 启动下载，B1_LONG 切换数据源，
 *                    B2_LONG 切换固件类型。传输锁忽略所有按键（无声无事件）。
 *
 *   SD_CHECK       : 探测数据源（SD卡/MCU Flash），读取固件版本（文件前2字节）、
 *                    整包 CRC32、文件大小。SD源需 close+reopen 重置 fptr，
 *                    MCU Flash 源直接指针读取 300KB。
 *
 *   SEND_START     : 发送 Start 帧（fw_type + fw_ver + fw_size + fw_crc +
 *                    verify_code 58902），等待应答。WAIT(100)=设备忙，延时1s重试；
 *                    超时重试；其它错误立即终止（含鉴权失败）。
 *
 *   SEND_DATA      : 分段发送固件（每段 APP_SEGMENT_SIZE=256B），包含 seg_crc 和
 *                    offset。SD源保证 fptr==offset（重试重定位用 f_lseek）。
 *                    写错误(170/175/176/177)=段级重试(error_retry)，从设备期望地址
 *                    重发；VER_ERR(171)=立即终止；超时=命令级重试(retry_cnt)。
 *
 *   SEND_COMPLETE  : 发送 Complete 帧（整包 fw_size + fw_crc），设备做最终校验。
 *
 *   SEND_UPDATE    : 发送 Update 帧（update_type=2/0），触发对端固件替换。
 *                    无论成败均释放锁+关闭SD文件+回IDLE。
 *
 *   SEND_TERMINATE : 发送 Terminate 帧通知对端中止，释放所有资源回 IDLE。
 *
 * ===== 错误处理策略 =====
 *
 *   - 双层重试：retry_cnt（命令级，上限6）管理超时/WAIT/读失败；
 *              error_retry（段级，上限6）管理写错误。
 *   - 全局超时(6min)：每次 SEND_* 结尾检查，防止无限重试（ostick 自然回绕安全）。
 *   - SD源重定位：f_lseek 替代 close+reopen+丢弃字节，O(1) 而非 O(n)。
 *   - 期望地址越界保护：exp_offset >= fw_total_size 立即终止，防止 remaining 下溢。
 *   - 版本不匹配：STATUS_VER_ERR 直接终止，不重试（固件版本冲突不可恢复）。
 *
 * 关键点：
 *   - DL_SendAndWait：发送一帧 -> 阻塞等待 g_usart1_rx_sem
 *     (APP_CMD_TIMEOUT_TICKS=500，即 1s @500Hz tick) -> 解析应答。
 *     超时返回 0xFF，解析失败返回 0xFE，成功返回 status 字段。
 *   - g_reply_content 必须为 FRAME_MAX_CMD_LEN (300) 字节：Proto_ParseReply
 *     会将其作为 in-place 临时缓冲复用（Task 5 review note）。
 *   - 段大小 APP_SEGMENT_SIZE(256)；MCU Flash 源固定 300KB。
 *   - TX LED (PA12) 在每个数据段发送时翻转一次（BSP_LED_Toggle）。
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
#include "task_led_wdg.h"
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

    /* ---- 调试: dump 发送帧 (全部字节, 单次打印) ---- */
    {
        char hexbuf[FRAME_MAX_TOTAL * 3 + 24];
        int pos = 0;
        pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "[DWN] TX %uB:", (unsigned)frame_len);
        for (uint16_t i = 0u; i < frame_len && pos < (int)sizeof(hexbuf) - 4; i++) {
            pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, " %02X", frame[i]);
        }
        BSP_USART2_Printf("%s\r\n", hexbuf);
    }

    BSP_USART1_RecvStart();                 /* 复位 RX 缓冲，使能 RXNE       */
    BSP_USART1_Send(frame, frame_len);      /* 阻塞发送整帧                  */

    /* 等待 RX 完成信号量，超时 = APP_CMD_TIMEOUT_TICKS (500 = 1s) */
    OSSemPend(&g_usart1_rx_sem,
              APP_CMD_TIMEOUT_TICKS,
              OS_OPT_PEND_BLOCKING,
              (CPU_TS *)0,
              &err);
    if (err == OS_ERR_TIMEOUT) {
        BSP_USART2_Printf("[DWN] RX TIMEOUT (1s no reply)\r\n");
        return 0xFFu;                        /* 命令超时                      */
    }
    /* err == OS_ERR_NONE 或其它（如已被删除） -- 尽力解析已收到的字节 */

    rx_len = BSP_USART1_GetRecvLen();
    rx_data = BSP_USART1_GetRecvBuf();

    /* ---- 调试: dump 接收帧 (全部字节, 单次打印) ---- */
    {
        char hexbuf[USART1_RX_BUF_SIZE * 3 + 24];
        int pos = 0;
        pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "[DWN] RX %uB:", (unsigned)rx_len);
        for (uint16_t i = 0u; i < rx_len && pos < (int)sizeof(hexbuf) - 4; i++) {
            pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, " %02X", rx_data[i]);
        }
        BSP_USART2_Printf("%s\r\n", hexbuf);
    }

    /* Proto_ParseReply 借用 content 作为 in-place cmd 解析缓冲 */
    ret = Proto_ParseReply(rx_data, rx_len, &rtype, &rstatus, content, &rclen);

    if (ret != PROTO_OK) {
        BSP_USART2_Printf("[DWN] Parse FAIL ret=%u\r\n", (unsigned)ret);
    }

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
            BSP_USART2_Printf("[DWN] Key event: %lu\r\n", (unsigned long)evt);

            if (evt == KEY_EVT_B1_LONG) {
                /* 切换数据源：SD 卡 <-> MCU Flash */
                DataSource_t cur = SysState_GetDataSource();
                DataSource_t next = (cur == DATA_SRC_SD_CARD)
                                    ? DATA_SRC_MCU_FLASH
                                    : DATA_SRC_SD_CARD;
                BSP_USART2_Printf("[DWN] Switch source: %s -> %s\r\n",
                                  (cur == DATA_SRC_SD_CARD) ? "SD" : "FLASH",
                                  (next == DATA_SRC_SD_CARD) ? "SD" : "FLASH");
                if (next == DATA_SRC_SD_CARD) {
                    if (!SD_IsPresent()) {
                        /* SD 卡不在：报错并保持当前数据源 */
                        BSP_USART2_Printf("[DWN] SD not present, abort switch\r\n");
                        Buzzer_Request(BUZZER_CMD_ERROR);
                        break;
                    }
                }
                SysState_SetDataSource(next);
            }
            else if (evt == KEY_EVT_B2_LONG) {
                /* 切换固件类型：普通 <-> 工厂 */
                FwType_t cur = SysState_GetFwType();
                FwType_t next = (cur == FW_NORMAL) ? FW_FACTORY : FW_NORMAL;
                BSP_USART2_Printf("[DWN] Switch fw type: %s -> %s\r\n",
                                  (cur == FW_NORMAL) ? "Normal" : "Factory",
                                  (next == FW_NORMAL) ? "Normal" : "Factory");
                SysState_SetFwType(next);
            }
            else if (evt == KEY_EVT_B2_SHORT) {
                /* 启动下载：短鸣反馈 -> 加锁 -> 进入 SD_CHECK */
                BSP_USART2_Printf("[DWN] === Download START ===\r\n");
                Buzzer_Request(BUZZER_CMD_SHORT);
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
                /* ---- SD 卡源: 扫描最新固件 ---- */
                if (SD_FindLatestFirmware() != SD_OK) {
                    BSP_USART2_Printf("[DWN] No firmware found on SD\r\n");
                    Buzzer_Request(BUZZER_CMD_ERROR);
                    SysState_SetTransferLock(0u);
                    state = DL_STATE_IDLE;
                    break;
                }
                /* 版本号从文件名解析 (g_fw_ver)，不再读文件头 */
                fw_ver = g_fw_ver;

                /* 打开选中的文件，读全部内容算 CRC */
                if (SD_FileOpen() != SD_OK) {
                    Buzzer_Request(BUZZER_CMD_ERROR);
                    SysState_SetTransferLock(0u);
                    state = DL_STATE_IDLE;
                    break;
                }
                fw_total_size = SD_FileGetSize();

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
                            break;
                        }
                        CRC32_Update(g_seg_buf, br);
                        remaining -= (uint32_t)br;
                    }
                    fw_crc = CRC32_GetResult();
                }

                /* 关闭后重开，准备顺序传输（fptr 归零） */
                SD_FileClose();
                if (SD_FileOpen() != SD_OK) {
                    Buzzer_Request(BUZZER_CMD_ERROR);
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

                /* 版本为 0xFFFF 表示未写入固件，禁止传输 */
                if (fw_ver == 0xFFFFu) {
                    BSP_USART2_Printf("[DWN] Flash: version=0xFFFF (no firmware), aborting.\r\n");
                    Buzzer_Request(BUZZER_CMD_ERROR);
                    SysState_SetTransferLock(0u);
                    state = DL_STATE_IDLE;
                    break;
                }

                fw_crc = CRC32_Calc(flash_ptr, fw_total_size);
                /* MCU Flash 源无 fptr 概念，sd_fptr 不使用 */
            }

            fw_type_code = (SysState_GetFwType() == FW_NORMAL)
                           ? FW_TYPE_NORMAL : FW_TYPE_FACTORY;
            offset      = 0u;
            retry_cnt   = 0u;
            error_retry = 0u;

            BSP_USART2_Printf("[DWN] Starting download: src=%s, fw=%s, ver=0x%04X, size=%lu, crc=0x%08lX\r\n",
                              (src == DATA_SRC_SD_CARD) ? "SD_CARD" : "MCU_FLASH",
                              (fw_type_code == FW_TYPE_NORMAL) ? "Normal" : "Factory",
                              (unsigned int)fw_ver,
                              (unsigned long)fw_total_size,
                              (unsigned long)fw_crc);

            state = DL_STATE_SEND_START;
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_START：发送 Start 帧，等待 ACK                              */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_START:
        {
            BSP_USART2_Printf("[DWN] Sending start cmd...\r\n");

            Proto_BuildStartCmd(fw_type_code, fw_ver, fw_total_size, fw_crc,
                                g_tx_buf, &len);
            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_START_OK) {
                BSP_USART2_Printf("[DWN] Start: OK (%u)\r\n",
                                  (unsigned int)STATUS_START_OK);
                state      = DL_STATE_SEND_DATA;
                retry_cnt  = 0u;
                error_retry = 0u;
            }
            else if (status == STATUS_WAIT) {
                retry_cnt++;
                BSP_USART2_Printf("[DWN] Start: WAIT (%u) retry %u/%u\r\n",
                                  (unsigned int)STATUS_WAIT,
                                  (unsigned int)retry_cnt,
                                  (unsigned int)APP_CMD_MAX_RETRY);
                /* 设备忙：等待 1s 后重试 */
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    BSP_USART2_Printf("[DWN] Retries exhausted, terminating\r\n");
                    state = DL_STATE_SEND_TERMINATE;
                } else {
                    OSTimeDlyHMSM(0u, 0u, 1u, 0u,
                                  OS_OPT_TIME_HMSM_STRICT, &err);
                }
            }
            else if (status == 0xFFu) {
                retry_cnt++;
                BSP_USART2_Printf("[DWN] Start: TIMEOUT retry %u/%u\r\n",
                                  (unsigned int)retry_cnt,
                                  (unsigned int)APP_CMD_MAX_RETRY);
                /* 命令超时 */
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    BSP_USART2_Printf("[DWN] Retries exhausted, terminating\r\n");
                    state = DL_STATE_SEND_TERMINATE;
                }
            }
            else {
                BSP_USART2_Printf("[DWN] Start: unexpected status %u, terminating\r\n",
                                  (unsigned int)status);
                /* 其它错误 / 解析失败 (0xFE) / 鉴权失败等：直接终止 */
                state = DL_STATE_SEND_TERMINATE;
            }

            if (DL_CheckGlobalTimeout(start_tick)) {
                BSP_USART2_Printf("[DWN] GLOBAL TIMEOUT (elapsed > 6min), terminating\r\n");
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
               此处用 f_lseek 重定位（替代 close+reopen+丢弃前缀的字节扫描）。 */
            if (src == DATA_SRC_SD_CARD && sd_fptr != offset) {
                if (SD_FileSeek(offset) == 0u) {
                    sd_fptr = offset;
                } else {
                    /* seek 失败：按读错误处理，sd_fptr 不更新，下轮重定位/重试 */
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

            BSP_USART2_Printf("[DWN] Seg 0x%08lX: %luB (%.1f%%)\r\n",
                              (unsigned long)offset, (unsigned long)seg_size,
                              (float)(offset * 100.0 / (double)fw_total_size));

            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_DATA_OK) {
                offset     += seg_size;    /* sd_fptr == offset 重新对齐 */
                BSP_USART2_Printf("[DWN] Seg 0x%08lX: OK (addr=0x%08lX)\r\n",
                                  (unsigned long)(offset - seg_size),
                                  (unsigned long)offset);
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
                error_retry++;
                BSP_USART2_Printf("[DWN] Seg 0x%08lX: ERR %u, retry %u/%u @ 0x%08lX\r\n",
                                  (unsigned long)offset,
                                  (unsigned int)status,
                                  (unsigned int)error_retry,
                                  (unsigned int)APP_ERROR_MAX_RETRY,
                                  (unsigned long)offset);
                /* 写错误：从设备期望地址重试（无期望地址则重试当前段）。
                   sd_fptr 已越过 offset，下轮 SEND_DATA 顶部会重定位。 */
                if (error_retry > APP_ERROR_MAX_RETRY) {
                    BSP_USART2_Printf("[DWN] Retries exhausted, terminating\r\n");
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
                BSP_USART2_Printf("[DWN] Seg 0x%08lX: VERSION MISMATCH, terminating\r\n",
                                  (unsigned long)offset);
                /* 版本不匹配：直接终止 */
                state = DL_STATE_SEND_TERMINATE;
            }
            else if (status == 0xFFu) {
                /* 命令超时：offset 不变，下轮重定位重读同一段 */
                retry_cnt++;
                BSP_USART2_Printf("[DWN] Seg 0x%08lX: TIMEOUT retry %u/%u\r\n",
                                  (unsigned long)offset,
                                  (unsigned int)retry_cnt,
                                  (unsigned int)APP_CMD_MAX_RETRY);
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    BSP_USART2_Printf("[DWN] Retries exhausted, terminating\r\n");
                    state = DL_STATE_SEND_TERMINATE;
                }
            }
            else {
                /* 其它错误 / 解析失败：命令级重试 */
                retry_cnt++;
                BSP_USART2_Printf("[DWN] Seg 0x%08lX: unexpected status %u, retry %u/%u\r\n",
                                  (unsigned long)offset,
                                  (unsigned int)status,
                                  (unsigned int)retry_cnt,
                                  (unsigned int)APP_CMD_MAX_RETRY);
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    BSP_USART2_Printf("[DWN] Retries exhausted, terminating\r\n");
                    state = DL_STATE_SEND_TERMINATE;
                }
            }

            if (DL_CheckGlobalTimeout(start_tick)) {
                BSP_USART2_Printf("[DWN] GLOBAL TIMEOUT (elapsed > 6min), terminating\r\n");
                state = DL_STATE_SEND_TERMINATE;
            }
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_COMPLETE：发送 Complete 帧                                  */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_COMPLETE:
        {
            BSP_USART2_Printf("[DWN] Sending complete cmd...\r\n");

            Proto_BuildCompleteCmd(fw_type_code, fw_ver, fw_total_size, fw_crc,
                                   g_tx_buf, &len);
            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_COMPLETE_OK) {
                BSP_USART2_Printf("[DWN] Complete: OK (%u)\r\n",
                                  (unsigned int)STATUS_COMPLETE_OK);
                state      = DL_STATE_SEND_UPDATE;
                retry_cnt  = 0u;
            }
            else {
                BSP_USART2_Printf("[DWN] Complete: FAIL status=%u\r\n",
                                  (unsigned int)status);
                retry_cnt++;
                if (retry_cnt > APP_CMD_MAX_RETRY) {
                    BSP_USART2_Printf("[DWN] Retries exhausted, terminating\r\n");
                    state = DL_STATE_SEND_TERMINATE;
                }
            }

            if (DL_CheckGlobalTimeout(start_tick)) {
                BSP_USART2_Printf("[DWN] GLOBAL TIMEOUT (elapsed > 6min), terminating\r\n");
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

            BSP_USART2_Printf("[DWN] Sending update cmd (type=%u)...\r\n",
                              (unsigned int)update_type);

            Proto_BuildUpdateCmd(update_type, g_tx_buf, &len);
            status = DL_SendAndWait(g_tx_buf, len, &rtype, &status,
                                    g_reply_content, &clen);

            if (status == STATUS_UPDATE_OK) {
                BSP_USART2_Printf("[DWN] Update: OK (%u) - SUCCESS!\r\n",
                                  (unsigned int)STATUS_UPDATE_OK);
                Buzzer_Request(BUZZER_CMD_OK);      /* 传输完成 */
            }
            else {
                BSP_USART2_Printf("[DWN] Update: FAIL status=%u\r\n",
                                  (unsigned int)status);
                Buzzer_Request(BUZZER_CMD_ERROR);   /* 更新失败 */
            }

            SysState_SetTransferLock(0u);
            if (src == DATA_SRC_SD_CARD) {
                SD_FileClose();
            }
            BSP_USART2_Printf("[DWN] Transfer ended, lock released\r\n");
            state = DL_STATE_IDLE;
            break;
        }

        /*-------------------------------------------------------------------*/
        /* SEND_TERMINATE：发送 Terminate 帧中止本次下载                    */
        /*-------------------------------------------------------------------*/
        case DL_STATE_SEND_TERMINATE:
        {
            BSP_USART2_Printf("[DWN] Terminating transfer (ver=0x%04X, size=%lu)\r\n",
                              (unsigned int)fw_ver, (unsigned long)fw_total_size);

            DL_SendTerminate(fw_ver, fw_total_size);
            Buzzer_Request(BUZZER_CMD_ERROR);

            SysState_SetTransferLock(0u);
            if (src == DATA_SRC_SD_CARD) {
                SD_FileClose();
            }
            BSP_USART2_Printf("[DWN] Transfer ended, lock released\r\n");
            state = DL_STATE_IDLE;
            break;
        }

        default:
            SysState_SetTransferLock(0u);
            state = DL_STATE_IDLE;
            break;

        } /* end switch(state) */

        /* 1ms 让出 CPU，使其它任务（按键 / LED / 蜂鸣器）得以调度 */
        OSTimeDlyHMSM(0u, 0u, 0u, 1u,
                      OS_OPT_TIME_HMSM_STRICT,
                      &err);
    }
}


