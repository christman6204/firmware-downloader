/* app/protocol.c
 *
 * 下载器通信协议帧编解码实现。设计文档第 10 章。
 *
 * ===== 帧结构图 =====
 *
 *   0  1  2  3  4        5      6-7      8-9     10-13    14     15      16..15+N  16+N..19+N
 *  +-------------+------+-------+--------+--------+--------+-------+--------+---------+------------+
 *  |Header(4)    |CS(1) |NetType|Len(2BE)|CommID  |DevID   |HostID |MsgType |Cmd(N)   |Tail(4)     |
 *  |FE EF ED FC  |      |       |       |(2BE)   |(4BE)   |       |       |         |FD EC F8 F1 |
 *  +-------------+------+-------+--------+--------+--------+-------+--------+---------+------------+
 *
 *  CS = ~(sum of bytes[5..15+N]) & 0xFF       (对 11+N 字节求和后取反)
 *  Len = 12 + N                                (报文总长 - 8, 即帧头4+帧尾4)
 *  N   = cmd_len                               (命令内容长度)
 *
 * 固定字段：
 *   Header:  FE EF ED FC (小端写入 0xFCEDEFFE)
 *   NetType: 3（APP_NET_TYPE）
 *   HostID:  99（APP_HOST_ID）
 *   MsgType: 1（APP_MSG_TYPE）
 *   DevID:   999999 = 0x000F423F（APP_DEV_ID）
 *   Tail:    FD EC F8 F1
 *
 * ===== 字节序规则表 =====
 *
 *   字段         字节序  C类型      所在帧偏移  说明
 *   ------------ ------ ---------- ---------- ---------------------------
 *   Header       N/A    uint8[4]   0-3       固定魔数，无字节序概念
 *   Checksum     N/A    uint8      4         单字节，无字节序概念
 *   NetType      N/A    uint8      5         单字节
 *   Length       BE     uint16     6-7       长度 = 12+cmd_len (报文总长-8)
 *   CommID       BE     uint16     8-9       自增计数器
 *   DevID        BE     uint32     10-13     999999 = 0x000F423F
 *   HostID       N/A    uint8      14        固定值 99
 *   MsgType      N/A    uint8      15        固定值 1
 *   CmdContent   mixed  uint8[]    16..     内部字段按各自规则
 *   Tail         N/A    uint8[4]   16+N..    固定魔数
 *
 *  CmdContent 内部字段（LE=小端）：
 *   fw_ver       LE     uint16     Cmd[2-3]  固件版本
 *   fw_size      LE     uint32     Cmd[4-7]  固件总大小
 *   fw_crc       LE     uint32     Cmd[8-11] 整包 CRC32
 *   seg_size     LE     uint32     Cmd[4-7]  段大小 (DATA)
 *   seg_crc      LE     uint32     Cmd[8-11] 段 CRC32 (DATA)
 *   offset       LE     uint32     Cmd[12-15]段偏移 (DATA)
 *   verify_code  LE     uint16     Cmd[12-13]验证码 58902 (START)
 *   update_type  N/A    uint8      Cmd[1]    更新类型 (UPDATE)
 *
 * ===== 各命令字段表 =====
 *
 *   命令       Cmd大小  字段布局
 *   --------- -------- ---------------------------------------------------
 *   START      14B      [0]=CMD_START, [1]=fw_type, [2-3]=fw_ver(LE),
 *                        [4-7]=fw_size(LE), [8-11]=fw_crc(LE),
 *                        [12-13]=verify_code(LE)
 *   DATA       16B+data [0]=CMD_DATA, [1]=fw_type, [2-3]=fw_ver(LE),
 *                        [4-7]=seg_size(LE), [8-11]=seg_crc(LE),
 *                        [12-15]=offset(LE), [16..]=raw data
 *   COMPLETE   12B      [0]=CMD_COMPLETE, [1]=fw_type, [2-3]=fw_ver(LE),
 *                        [4-7]=fw_size(LE), [8-11]=fw_crc(LE)
 *   TERMINATE  8B       [0]=CMD_TERMINATE, [1]=0(reserved),
 *                        [2-3]=fw_ver(LE), [4-7]=fw_size(LE)
 *   UPDATE     2B       [0]=CMD_UPDATE, [1]=update_type
 *
 * 校验和（偏移 4）：对偏移 5..15+N 求和后取反（uint8）。
 *
 * CommID：模块静态计数器，每次 BuildFrame 自增、自然回绕（uint16_t）。
 *
 * 纯逻辑模块，PC 端 Python 移植验证（Tests/test_protocol.py）。
 */
#include "protocol.h"

/*---------------------------------------------------------------------------*/
/* 模块静态：自增 CommID（大端写入帧，回绕）                                  */
/*---------------------------------------------------------------------------*/
static uint16_t g_comm_id = 0u;

/*---------------------------------------------------------------------------*/
/* Proto_Checksum: sum buf[0..len-1], 返回 ~sum（uint8）                      */
/*---------------------------------------------------------------------------*/
uint8_t Proto_Checksum(const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    uint8_t  sum = 0u;

    for (i = 0u; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return (uint8_t)(~sum);
}

/*---------------------------------------------------------------------------*/
/* Proto_BuildFrame                                                           */
/*---------------------------------------------------------------------------*/
uint16_t Proto_BuildFrame(const uint8_t *cmd, uint16_t cmd_len,
                          uint8_t *out, uint16_t *out_len)
{
    uint16_t i;
    uint16_t total;
    uint16_t length;
    uint16_t cid;
    uint8_t  cs;

    if (cmd == (const uint8_t *)0 || out == (uint8_t *)0) {
        if (out_len != (uint16_t *)0) { *out_len = 0u; }
        return 0u;
    }
    if (cmd_len > FRAME_MAX_CMD_LEN) {
        if (out_len != (uint16_t *)0) { *out_len = 0u; }
        return 0u;
    }

    total  = (uint16_t)(FRAME_FIXED_LEN + cmd_len + FRAME_TAIL_LEN);
    length = (uint16_t)(12u + cmd_len);   /* 长度 = 报文总长 - 8 (帧头4+帧尾4) */
    cid    = g_comm_id;
    g_comm_id = (uint16_t)(g_comm_id + 1u);  /* 自增、自然回绕 */

    /* Header */
    out[0]  = FRAME_HEADER0;
    out[1]  = FRAME_HEADER1;
    out[2]  = FRAME_HEADER2;
    out[3]  = FRAME_HEADER3;

    /* Checksum 占位（后填） */
    out[4]  = 0u;

    /* NetType */
    out[5]  = APP_NET_TYPE;

    /* Length (BE) */
    out[6]  = (uint8_t)((length >> 8) & 0xFFu);
    out[7]  = (uint8_t)(length & 0xFFu);

    /* CommID (BE) */
    out[8]  = (uint8_t)((cid >> 8) & 0xFFu);
    out[9]  = (uint8_t)(cid & 0xFFu);

    /* DevID (BE) = APP_DEV_ID (999999 = 0x000F423F) */
    out[10] = (uint8_t)((APP_DEV_ID >> 24) & 0xFFu);
    out[11] = (uint8_t)((APP_DEV_ID >> 16) & 0xFFu);
    out[12] = (uint8_t)((APP_DEV_ID >> 8)  & 0xFFu);
    out[13] = (uint8_t)(APP_DEV_ID & 0xFFu);

    /* HostID */
    out[14] = APP_HOST_ID;

    /* MsgType */
    out[15] = APP_MSG_TYPE;

    /* CmdContent */
    for (i = 0u; i < cmd_len; i++) {
        out[FRAME_FIXED_LEN + i] = cmd[i];
    }

    /* Tail */
    out[FRAME_FIXED_LEN + cmd_len + 0u] = FRAME_TAIL0;
    out[FRAME_FIXED_LEN + cmd_len + 1u] = FRAME_TAIL1;
    out[FRAME_FIXED_LEN + cmd_len + 2u] = FRAME_TAIL2;
    out[FRAME_FIXED_LEN + cmd_len + 3u] = FRAME_TAIL3;

    /* Checksum: sum of out[5..15+cmd_len] (即 11+cmd_len 字节), 取反 */
    cs = Proto_Checksum(&out[5], (uint16_t)(11u + cmd_len));
    out[4] = cs;

    if (out_len != (uint16_t *)0) { *out_len = total; }
    return total;
}

/*---------------------------------------------------------------------------*/
/* Proto_ParseFrame                                                           */
/*---------------------------------------------------------------------------*/
uint8_t Proto_ParseFrame(const uint8_t *raw, uint16_t raw_len,
                         uint8_t *cmd, uint16_t *cmd_len)
{
    uint16_t length;
    uint16_t cl;
    uint16_t i;
    uint16_t off;       /* 帧头在 raw 中的偏移 */
    uint16_t tail_off;
    uint8_t  cs;

    if (raw == (const uint8_t *)0 || cmd == (uint8_t *)0 || cmd_len == (uint16_t *)0) {
        return PROTO_ERR_PARAM;
    }
    *cmd_len = 0u;

    /* 最小帧 = 16 + 0 + 4 = 20 */
    if (raw_len < (FRAME_FIXED_LEN + FRAME_TAIL_LEN)) {
        return PROTO_ERR_TOO_SHORT;
    }

    /* 搜索帧头 FE EF ED FC，容忍目标设备发送的前导字节 */
    /* 解析原则（按协议约定）：
       1) 必须有帧头 FE EF ED FC 和帧尾 FD EC F8 F1
       2) 帧头帧尾之间数据长度须符合报文中的长度字段
       3) 校验和须正确
       4) 不对通讯ID/设备ID/主机ID 做值判断（目标回复的 ID 可能与约定不同） */
    off = 0u;
    while (off + 4u <= raw_len) {
        if (raw[off] == FRAME_HEADER0 && raw[off + 1u] == FRAME_HEADER1 &&
            raw[off + 2u] == FRAME_HEADER2 && raw[off + 3u] == FRAME_HEADER3) {
            break;
        }
        off++;
    }
    if (off + 4u > raw_len) {
        return PROTO_ERR_HEADER;
    }

    /* Length (BE) = 报文总长 - 8 = 12 + cmd_len */
    length = (uint16_t)(((uint16_t)raw[off + 6u] << 8) | raw[off + 7u]);
    if (length < 12u) {
        return PROTO_ERR_LENGTH;
    }
    cl = (uint16_t)(length - 12u);
    if (cl > FRAME_MAX_CMD_LEN) {
        return PROTO_ERR_LENGTH;
    }

    /* 总长度检查：从帧头偏移起，raw 至少能容纳完整帧 */
    if (off + (uint16_t)(FRAME_FIXED_LEN + cl + FRAME_TAIL_LEN) > raw_len) {
        return PROTO_ERR_LENGTH;
    }

    /* Tail */
    tail_off = (uint16_t)(off + FRAME_FIXED_LEN + cl);
    if (raw[tail_off + 0u] != FRAME_TAIL0 || raw[tail_off + 1u] != FRAME_TAIL1 ||
        raw[tail_off + 2u] != FRAME_TAIL2 || raw[tail_off + 3u] != FRAME_TAIL3) {
        return PROTO_ERR_TAIL;
    }

    /* Checksum: 偏移 5..15+cl (11+cl 字节) 求和取反，与 raw[4] 比较 */
    cs = Proto_Checksum(&raw[off + 5u], (uint16_t)(11u + cl));
    if (cs != raw[off + 4u]) {
        return PROTO_ERR_CHECKSUM;
    }

    /* 提取 cmd */
    for (i = 0u; i < cl; i++) {
        cmd[i] = raw[off + FRAME_FIXED_LEN + i];
    }
    *cmd_len = cl;

    return PROTO_OK;
}

/*---------------------------------------------------------------------------*/
/* Proto_BuildStartCmd: cmd[14]                                               */
/*---------------------------------------------------------------------------*/
uint16_t Proto_BuildStartCmd(uint8_t fw_type, uint16_t fw_ver,
                             uint32_t fw_size, uint32_t fw_crc,
                             uint8_t *out, uint16_t *out_len)
{
    uint8_t cmd[14];

    cmd[0]  = CMD_START;
    cmd[1]  = fw_type;
    cmd[2]  = (uint8_t)(fw_ver & 0xFFu);              /* fw_ver LE */
    cmd[3]  = (uint8_t)((fw_ver >> 8) & 0xFFu);
    cmd[4]  = (uint8_t)(fw_size & 0xFFu);             /* fw_size LE */
    cmd[5]  = (uint8_t)((fw_size >> 8) & 0xFFu);
    cmd[6]  = (uint8_t)((fw_size >> 16) & 0xFFu);
    cmd[7]  = (uint8_t)((fw_size >> 24) & 0xFFu);
    cmd[8]  = (uint8_t)(fw_crc & 0xFFu);              /* fw_crc LE */
    cmd[9]  = (uint8_t)((fw_crc >> 8) & 0xFFu);
    cmd[10] = (uint8_t)((fw_crc >> 16) & 0xFFu);
    cmd[11] = (uint8_t)((fw_crc >> 24) & 0xFFu);
    cmd[12] = (uint8_t)(APP_VERIFY_CODE & 0xFFu);     /* verify_code LE */
    cmd[13] = (uint8_t)((APP_VERIFY_CODE >> 8) & 0xFFu);

    return Proto_BuildFrame(cmd, 14u, out, out_len);
}

/*---------------------------------------------------------------------------*/
/* Proto_BuildDataCmd: cmd[16+data_len]                                       */
/*---------------------------------------------------------------------------*/
uint16_t Proto_BuildDataCmd(uint8_t fw_type, uint16_t fw_ver,
                            uint32_t seg_size, uint32_t seg_crc,
                            uint32_t offset,
                            const uint8_t *data, uint16_t data_len,
                            uint8_t *out, uint16_t *out_len)
{
    uint8_t  cmd[16u + APP_FRAME_MAX_DATA];
    uint16_t i;

    if (data == (const uint8_t *)0 && data_len > 0u) {
        if (out_len != (uint16_t *)0) { *out_len = 0u; }
        return 0u;
    }
    if (data_len > APP_FRAME_MAX_DATA) {
        if (out_len != (uint16_t *)0) { *out_len = 0u; }
        return 0u;
    }

    cmd[0]  = CMD_DATA;
    cmd[1]  = fw_type;
    cmd[2]  = (uint8_t)(fw_ver & 0xFFu);              /* fw_ver LE */
    cmd[3]  = (uint8_t)((fw_ver >> 8) & 0xFFu);
    cmd[4]  = (uint8_t)(seg_size & 0xFFu);            /* seg_size LE */
    cmd[5]  = (uint8_t)((seg_size >> 8) & 0xFFu);
    cmd[6]  = (uint8_t)((seg_size >> 16) & 0xFFu);
    cmd[7]  = (uint8_t)((seg_size >> 24) & 0xFFu);
    cmd[8]  = (uint8_t)(seg_crc & 0xFFu);             /* seg_crc LE */
    cmd[9]  = (uint8_t)((seg_crc >> 8) & 0xFFu);
    cmd[10] = (uint8_t)((seg_crc >> 16) & 0xFFu);
    cmd[11] = (uint8_t)((seg_crc >> 24) & 0xFFu);
    cmd[12] = (uint8_t)(offset & 0xFFu);              /* offset LE */
    cmd[13] = (uint8_t)((offset >> 8) & 0xFFu);
    cmd[14] = (uint8_t)((offset >> 16) & 0xFFu);
    cmd[15] = (uint8_t)((offset >> 24) & 0xFFu);
    for (i = 0u; i < data_len; i++) {                 /* data */
        cmd[16u + i] = data[i];
    }

    return Proto_BuildFrame(cmd, (uint16_t)(16u + data_len), out, out_len);
}

/*---------------------------------------------------------------------------*/
/* Proto_BuildCompleteCmd: cmd[12]                                            */
/*---------------------------------------------------------------------------*/
uint16_t Proto_BuildCompleteCmd(uint8_t fw_type, uint16_t fw_ver,
                                uint32_t fw_size, uint32_t fw_crc,
                                uint8_t *out, uint16_t *out_len)
{
    uint8_t cmd[12];

    cmd[0]  = CMD_COMPLETE;
    cmd[1]  = fw_type;
    cmd[2]  = (uint8_t)(fw_ver & 0xFFu);              /* fw_ver LE */
    cmd[3]  = (uint8_t)((fw_ver >> 8) & 0xFFu);
    cmd[4]  = (uint8_t)(fw_size & 0xFFu);             /* fw_size LE */
    cmd[5]  = (uint8_t)((fw_size >> 8) & 0xFFu);
    cmd[6]  = (uint8_t)((fw_size >> 16) & 0xFFu);
    cmd[7]  = (uint8_t)((fw_size >> 24) & 0xFFu);
    cmd[8]  = (uint8_t)(fw_crc & 0xFFu);              /* fw_crc LE */
    cmd[9]  = (uint8_t)((fw_crc >> 8) & 0xFFu);
    cmd[10] = (uint8_t)((fw_crc >> 16) & 0xFFu);
    cmd[11] = (uint8_t)((fw_crc >> 24) & 0xFFu);

    return Proto_BuildFrame(cmd, 12u, out, out_len);
}

/*---------------------------------------------------------------------------*/
/* Proto_BuildTerminateCmd: cmd[8]                                            */
/*---------------------------------------------------------------------------*/
uint16_t Proto_BuildTerminateCmd(uint16_t fw_ver, uint32_t fw_size,
                                 uint8_t *out, uint16_t *out_len)
{
    uint8_t cmd[8];

    cmd[0] = CMD_TERMINATE;
    cmd[1] = 0u;                                       /* reserved */
    cmd[2] = (uint8_t)(fw_ver & 0xFFu);               /* fw_ver LE */
    cmd[3] = (uint8_t)((fw_ver >> 8) & 0xFFu);
    cmd[4] = (uint8_t)(fw_size & 0xFFu);              /* fw_size LE */
    cmd[5] = (uint8_t)((fw_size >> 8) & 0xFFu);
    cmd[6] = (uint8_t)((fw_size >> 16) & 0xFFu);
    cmd[7] = (uint8_t)((fw_size >> 24) & 0xFFu);

    return Proto_BuildFrame(cmd, 8u, out, out_len);
}

/*---------------------------------------------------------------------------*/
/* Proto_BuildUpdateCmd: cmd[2]                                               */
/*---------------------------------------------------------------------------*/
uint16_t Proto_BuildUpdateCmd(uint8_t update_type,
                              uint8_t *out, uint16_t *out_len)
{
    uint8_t cmd[2];

    cmd[0] = CMD_UPDATE;
    cmd[1] = update_type;

    return Proto_BuildFrame(cmd, 2u, out, out_len);
}

/*---------------------------------------------------------------------------*/
/* Proto_ParseReply                                                           */
/*---------------------------------------------------------------------------*/
uint8_t Proto_ParseReply(const uint8_t *raw, uint16_t raw_len,
                         uint8_t *reply_type, uint8_t *status,
                         uint8_t *content, uint16_t *content_len)
{
    uint8_t  err;
    uint16_t cl;
    uint16_t i;

    /* 借用 content 缓冲作为临时 cmd 解析缓冲（content 须 >= FRAME_MAX_CMD_LEN） */
    err = Proto_ParseFrame(raw, raw_len, content, &cl);
    if (err != PROTO_OK) {
        if (content_len != (uint16_t *)0) { *content_len = 0u; }
        return err;
    }
    if (cl < 2u) {
        if (content_len != (uint16_t *)0) { *content_len = 0u; }
        return PROTO_ERR_CMD_TOO_SHORT;
    }

    if (reply_type != (uint8_t *)0) { *reply_type = content[0]; }
    if (status != (uint8_t *)0)     { *status     = content[1]; }

    /* content[2..cl-1] 即应答正文；左移到 content[0..] */
    for (i = 2u; i < cl; i++) {
        content[i - 2u] = content[i];
    }
    if (content_len != (uint16_t *)0) { *content_len = (uint16_t)(cl - 2u); }

    return PROTO_OK;
}
