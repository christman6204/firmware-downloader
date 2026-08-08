/* app/protocol.h
 *
 * 下载器通信协议帧编解码。设计文档第 10 章。
 *
 * 帧格式（大端 length/commID/devID，小端 fw_ver/fw_size/crc/offset/verify_code）：
 *   偏移 0  : Header(4)   = FE EF ED FC
 *   偏移 4  : Checksum(1) = ~(sum of bytes[5..15+N]) & 0xFF
 *   偏移 5  : NetType(1)  = APP_NET_TYPE (3)
 *   偏移 6  : Length(2,BE) = 12 + cmd_len  （报文总长 - 8, 即帧头4+帧尾4）
 *   偏移 8  : CommID(2,BE)= 自增、回绕
 *   偏移 10 : DevID(4,BE) = APP_DEV_ID (999999 = 0x000F423F)
 *   偏移 14 : HostID(1)   = APP_HOST_ID (99)
 *   偏移 15 : MsgType(1)  = APP_MSG_TYPE (1)
 *   偏移 16 : CmdContent(N)
 *   偏移 16+N : Tail(4)   = FD EC F8 F1
 *
 * 常量：FRAME_FIXED_LEN=16, FRAME_TAIL_LEN=4, FRAME_MAX_CMD_LEN=300,
 *       FRAME_MAX_TOTAL=16+300+4=320。
 *
 * 纯逻辑模块（仅依赖 stdint.h + app_cfg.h），PC 端用 Python 移植验证
 * （Tests/test_protocol.py）。
 */
#ifndef __PROTOCOL_H
#define __PROTOCOL_H

#include <stdint.h>
#include "app_cfg.h"

/*---------------------------------------------------------------------------*/
/* 帧常量                                                                     */
/*---------------------------------------------------------------------------*/
#define FRAME_HEADER0      0xFEu
#define FRAME_HEADER1      0xEFu
#define FRAME_HEADER2      0xEDu
#define FRAME_HEADER3      0xFCu
#define FRAME_TAIL0        0xFDu
#define FRAME_TAIL1        0xECu
#define FRAME_TAIL2        0xF8u
#define FRAME_TAIL3        0xF1u

#define FRAME_FIXED_LEN    16u   /* header..msgtype */
#define FRAME_TAIL_LEN     4u
#define FRAME_MAX_CMD_LEN  300u
#define FRAME_MAX_TOTAL    (FRAME_FIXED_LEN + FRAME_MAX_CMD_LEN + FRAME_TAIL_LEN)  /* 320 */

/*---------------------------------------------------------------------------*/
/* 命令类型（CmdContent 首字节）                                              */
/*---------------------------------------------------------------------------*/
#define CMD_START          0u
#define CMD_DATA           1u
#define CMD_COMPLETE       2u
#define CMD_TERMINATE      3u
#define CMD_UPDATE         4u

/*---------------------------------------------------------------------------*/
/* 状态码（应答帧 status 字段）                                               */
/*---------------------------------------------------------------------------*/
#define STATUS_WAIT        100u
#define STATUS_AUTH_FAIL   101u
#define STATUS_START_OK    150u
#define STATUS_DATA_OK     151u
#define STATUS_COMPLETE_OK 152u
#define STATUS_TERM_OK     153u
#define STATUS_UPDATE_OK   154u
#define STATUS_ADDR_ERR    170u
#define STATUS_VER_ERR     171u
#define STATUS_INCOMPLETE  172u
#define STATUS_CRC_ERR     173u
#define STATUS_CMD_ERR     174u
#define STATUS_WRITE_FAIL  175u
#define STATUS_NO_PERM     176u
#define STATUS_WRITE_FAIL2 177u

/*---------------------------------------------------------------------------*/
/* 解析返回码                                                                 */
/*---------------------------------------------------------------------------*/
#define PROTO_OK               0u
#define PROTO_ERR_PARAM        1u
#define PROTO_ERR_TOO_SHORT    2u
#define PROTO_ERR_HEADER       3u
#define PROTO_ERR_LENGTH       4u
#define PROTO_ERR_TAIL         5u
#define PROTO_ERR_CHECKSUM     6u
#define PROTO_ERR_CMD_TOO_SHORT 7u

/*---------------------------------------------------------------------------*/
/* 校验和：sum buf[0..len-1]，返回 ~sum（uint8）                              */
/*---------------------------------------------------------------------------*/
uint8_t  Proto_Checksum(const uint8_t *buf, uint16_t len);

/*---------------------------------------------------------------------------*/
/* 组装完整帧。                                                               */
/*   cmd:    命令内容（偏移 16 起）                                           */
/*   cmd_len:命令内容长度 N                                                   */
/*   out:    输出缓冲，须 >= 16+N+4 字节                                      */
/*   out_len:*out_len = 总长度（返回值也是总长度，0=失败）                    */
/*---------------------------------------------------------------------------*/
uint16_t Proto_BuildFrame(const uint8_t *cmd, uint16_t cmd_len,
                          uint8_t *out, uint16_t *out_len);

/*---------------------------------------------------------------------------*/
/* 解析完整帧。                                                               */
/*   raw:    原始字节                                                         */
/*   raw_len:原始长度                                                         */
/*   cmd:    输出命令内容缓冲（须 >= FRAME_MAX_CMD_LEN）                      */
/*   cmd_len:*cmd_len = 命令内容长度                                          */
/*   返回: 0=OK, 非0=错误码                                                   */
/*---------------------------------------------------------------------------*/
uint8_t  Proto_ParseFrame(const uint8_t *raw, uint16_t raw_len,
                          uint8_t *cmd, uint16_t *cmd_len);

/*---------------------------------------------------------------------------*/
/* 命令构造器（内部组装 cmd 后调用 BuildFrame）                               */
/*   返回总长度（0=失败），*out_len 同。                                      */
/*---------------------------------------------------------------------------*/

/* START: cmd[14] = [0]=CMD_START, [1]=fw_type,
          [2-3]=fw_ver LE, [4-7]=fw_size LE,
          [8-11]=fw_crc LE, [12-13]=verify_code(58902) LE */
uint16_t Proto_BuildStartCmd(uint8_t fw_type, uint16_t fw_ver,
                             uint32_t fw_size, uint32_t fw_crc,
                             uint8_t *out, uint16_t *out_len);

/* DATA: cmd[16+data_len] = [0]=CMD_DATA, [1]=fw_type,
         [2-3]=fw_ver LE, [4-7]=seg_size LE, [8-11]=seg_crc LE,
         [12-15]=offset LE, [16+]=data */
uint16_t Proto_BuildDataCmd(uint8_t fw_type, uint16_t fw_ver,
                            uint32_t seg_size, uint32_t seg_crc,
                            uint32_t offset,
                            const uint8_t *data, uint16_t data_len,
                            uint8_t *out, uint16_t *out_len);

/* COMPLETE: cmd[12] = [0]=CMD_COMPLETE, [1]=fw_type,
             [2-3]=fw_ver LE, [4-7]=fw_size LE, [8-11]=fw_crc LE */
uint16_t Proto_BuildCompleteCmd(uint8_t fw_type, uint16_t fw_ver,
                                uint32_t fw_size, uint32_t fw_crc,
                                uint8_t *out, uint16_t *out_len);

/* TERMINATE: cmd[8] = [0]=CMD_TERMINATE, [1]=0(reserved),
              [2-3]=fw_ver LE, [4-7]=fw_size LE */
uint16_t Proto_BuildTerminateCmd(uint16_t fw_ver, uint32_t fw_size,
                                 uint8_t *out, uint16_t *out_len);

/* UPDATE: cmd[2] = [0]=CMD_UPDATE, [1]=update_type */
uint16_t Proto_BuildUpdateCmd(uint8_t update_type,
                              uint8_t *out, uint16_t *out_len);

/*---------------------------------------------------------------------------*/
/* 应答解析：ParseFrame 后拆 cmd 为                                           */
/*   cmd[0]=reply_type, cmd[1]=status, cmd[2..]=content                      */
/*   *content_len = cmd_len - 2                                               */
/*   返回 0=OK, 非0=错误码                                                    */
/*---------------------------------------------------------------------------*/
uint8_t  Proto_ParseReply(const uint8_t *raw, uint16_t raw_len,
                          uint8_t *reply_type, uint8_t *status,
                          uint8_t *content, uint16_t *content_len);

#endif /* __PROTOCOL_H */
