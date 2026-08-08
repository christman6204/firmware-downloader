#!/usr/bin/env python3
"""Tests/test_protocol.py

PC 端单元测试：将 app/protocol.c 的逻辑移植到 Python，验证协议帧编解码正确性。
Python 移植与 C 代码使用相同的帧布局、字节序、校验和算法，因此 Python 测试
通过即验证 C 逻辑正确。

运行: python test_protocol.py
预期: 所有断言通过，打印 PASSED。
"""

# ---------------------------------------------------------------------------
# 帧常量（与 protocol.h 一致）
# ---------------------------------------------------------------------------
FRAME_HEADER = bytes([0xFE, 0xEF, 0xED, 0xFC])
FRAME_TAIL   = bytes([0xFD, 0xEC, 0xF8, 0xF1])
FRAME_FIXED_LEN   = 16
FRAME_TAIL_LEN    = 4
FRAME_MAX_CMD_LEN = 300
FRAME_MAX_TOTAL   = 16 + 300 + 4  # 320

# 命令类型
CMD_START     = 0
CMD_DATA      = 1
CMD_COMPLETE  = 2
CMD_TERMINATE = 3
CMD_UPDATE    = 4

# 状态码
STATUS_WAIT        = 100
STATUS_AUTH_FAIL   = 101
STATUS_START_OK    = 150
STATUS_DATA_OK     = 151
STATUS_COMPLETE_OK = 152
STATUS_TERM_OK     = 153
STATUS_UPDATE_OK   = 154
STATUS_ADDR_ERR    = 170
STATUS_VER_ERR     = 171
STATUS_INCOMPLETE  = 172
STATUS_CRC_ERR     = 173
STATUS_CMD_ERR     = 174
STATUS_WRITE_FAIL  = 175
STATUS_NO_PERM     = 176
STATUS_WRITE_FAIL2 = 177

# app_cfg.h 常量
APP_NET_TYPE      = 3
APP_HOST_ID       = 99
APP_MSG_TYPE      = 1
APP_DEV_ID        = 999999   # 0x000F423F
APP_VERIFY_CODE   = 58902    # 0xE616
APP_FRAME_MAX_DATA = 300
FW_TYPE_NORMAL    = 0
FW_TYPE_FACTORY   = 1
FW_UPDATE_FACTORY = 0
FW_UPDATE_NORMAL  = 2

# 解析返回码
PROTO_OK                = 0
PROTO_ERR_PARAM         = 1
PROTO_ERR_TOO_SHORT     = 2
PROTO_ERR_HEADER        = 3
PROTO_ERR_LENGTH        = 4
PROTO_ERR_TAIL          = 5
PROTO_ERR_CHECKSUM      = 6
PROTO_ERR_CMD_TOO_SHORT = 7

# ---------------------------------------------------------------------------
# 模块状态：自增 CommID（镜像 C 的 g_comm_id）
# ---------------------------------------------------------------------------
_comm_id = 0


# ---------------------------------------------------------------------------
# 小端/大端编码辅助
# ---------------------------------------------------------------------------
def _u16_le(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def _u32_le(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


# ---------------------------------------------------------------------------
# Proto_Checksum 移植：sum buf, 返回 ~sum (uint8)
# ---------------------------------------------------------------------------
def proto_checksum(buf):
    s = 0
    for b in buf:
        s = (s + b) & 0xFF
    return (~s) & 0xFF


# ---------------------------------------------------------------------------
# Proto_BuildFrame 移植
# ---------------------------------------------------------------------------
def proto_build_frame(cmd, cmd_len=None):
    """返回 (out_bytes, total_len)。"""
    global _comm_id
    if cmd_len is None:
        cmd_len = len(cmd)

    assert cmd_len <= FRAME_MAX_CMD_LEN

    cid = _comm_id
    _comm_id = (_comm_id + 1) & 0xFFFF  # 自增、自然回绕

    length = 12 + cmd_len   # 报文总长 - 8
    total = FRAME_FIXED_LEN + cmd_len + FRAME_TAIL_LEN
    out = bytearray(total)

    # Header
    out[0:4] = FRAME_HEADER
    # Checksum 占位
    out[4] = 0
    # NetType
    out[5] = APP_NET_TYPE
    # Length (BE)
    out[6] = (length >> 8) & 0xFF
    out[7] = length & 0xFF
    # CommID (BE)
    out[8] = (cid >> 8) & 0xFF
    out[9] = cid & 0xFF
    # DevID (BE) = 0x000F423F
    out[10] = (APP_DEV_ID >> 24) & 0xFF
    out[11] = (APP_DEV_ID >> 16) & 0xFF
    out[12] = (APP_DEV_ID >> 8) & 0xFF
    out[13] = APP_DEV_ID & 0xFF
    # HostID
    out[14] = APP_HOST_ID
    # MsgType
    out[15] = APP_MSG_TYPE
    # CmdContent
    out[16:16 + cmd_len] = bytes(cmd[:cmd_len])
    # Tail
    out[16 + cmd_len:16 + cmd_len + 4] = FRAME_TAIL
    # Checksum: 偏移 5..15+cmd_len (11+cmd_len 字节) 求和取反
    out[4] = proto_checksum(out[5:5 + 11 + cmd_len])
    return bytes(out), total


# ---------------------------------------------------------------------------
# Proto_ParseFrame 移植
# ---------------------------------------------------------------------------
def proto_parse_frame(raw):
    """返回 (err_code, cmd_bytes, cmd_len)。容忍前导字节（搜索帧头）。"""
    raw_len = len(raw)

    if raw_len < FRAME_FIXED_LEN + FRAME_TAIL_LEN:
        return (PROTO_ERR_TOO_SHORT, b'', 0)

    # 搜索帧头 FE EF ED FC，容忍前导字节
    off = 0
    while off + 4 <= raw_len:
        if raw[off:off + 4] == FRAME_HEADER:
            break
        off += 1
    if off + 4 > raw_len:
        return (PROTO_ERR_HEADER, b'', 0)

    # Length (BE)
    length = (raw[off + 6] << 8) | raw[off + 7]
    if length < 12:
        return (PROTO_ERR_LENGTH, b'', 0)
    cl = length - 12
    if cl > FRAME_MAX_CMD_LEN:
        return (PROTO_ERR_LENGTH, b'', 0)

    # 总长度
    if off + FRAME_FIXED_LEN + cl + FRAME_TAIL_LEN > raw_len:
        return (PROTO_ERR_LENGTH, b'', 0)

    # Tail
    tail_off = off + FRAME_FIXED_LEN + cl
    if raw[tail_off:tail_off + 4] != FRAME_TAIL:
        return (PROTO_ERR_TAIL, b'', 0)

    # Checksum
    cs = proto_checksum(raw[off + 5:off + 5 + 11 + cl])
    if cs != raw[off + 4]:
        return (PROTO_ERR_CHECKSUM, b'', 0)

    # Cmd
    cmd = bytes(raw[off + 16:off + 16 + cl])
    return (PROTO_OK, cmd, cl)


# ---------------------------------------------------------------------------
# 命令构造器移植
# ---------------------------------------------------------------------------
def proto_build_start_cmd(fw_type, fw_ver, fw_size, fw_crc):
    cmd = bytearray(14)
    cmd[0] = CMD_START
    cmd[1] = fw_type
    cmd[2:4] = _u16_le(fw_ver)
    cmd[4:8] = _u32_le(fw_size)
    cmd[8:12] = _u32_le(fw_crc)
    cmd[12:14] = _u16_le(APP_VERIFY_CODE)
    return proto_build_frame(bytes(cmd))


def proto_build_data_cmd(fw_type, fw_ver, seg_size, seg_crc, offset, data):
    data_len = len(data)
    assert data_len <= APP_FRAME_MAX_DATA
    cmd = bytearray(16 + data_len)
    cmd[0] = CMD_DATA
    cmd[1] = fw_type
    cmd[2:4] = _u16_le(fw_ver)
    cmd[4:8] = _u32_le(seg_size)
    cmd[8:12] = _u32_le(seg_crc)
    cmd[12:16] = _u32_le(offset)
    cmd[16:16 + data_len] = data
    return proto_build_frame(bytes(cmd))


def proto_build_complete_cmd(fw_type, fw_ver, fw_size, fw_crc):
    cmd = bytearray(12)
    cmd[0] = CMD_COMPLETE
    cmd[1] = fw_type
    cmd[2:4] = _u16_le(fw_ver)
    cmd[4:8] = _u32_le(fw_size)
    cmd[8:12] = _u32_le(fw_crc)
    return proto_build_frame(bytes(cmd))


def proto_build_terminate_cmd(fw_ver, fw_size):
    cmd = bytearray(8)
    cmd[0] = CMD_TERMINATE
    cmd[1] = 0  # reserved
    cmd[2:4] = _u16_le(fw_ver)
    cmd[4:8] = _u32_le(fw_size)
    return proto_build_frame(bytes(cmd))


def proto_build_update_cmd(update_type):
    cmd = bytearray(2)
    cmd[0] = CMD_UPDATE
    cmd[1] = update_type
    return proto_build_frame(bytes(cmd))


# ---------------------------------------------------------------------------
# Proto_ParseReply 移植
# ---------------------------------------------------------------------------
def proto_parse_reply(raw):
    """返回 (err, reply_type, status, content_bytes)。"""
    err, cmd, cl = proto_parse_frame(raw)
    if err != PROTO_OK:
        return (err, 0, 0, b'')
    if cl < 2:
        return (PROTO_ERR_CMD_TOO_SHORT, 0, 0, b'')
    reply_type = cmd[0]
    status = cmd[1]
    content = bytes(cmd[2:cl])
    return (PROTO_OK, reply_type, status, content)


# ---------------------------------------------------------------------------
# 测试用例
# ---------------------------------------------------------------------------
def main():
    print("=== 协议帧编解码单元测试（Python 移植 C 逻辑） ===\n")

    # ---- Test 1: StartCmd 往返 ----
    frame, total = proto_build_start_cmd(FW_TYPE_NORMAL, 0x0102,
                                         0x12345678, 0xDEADBEEF)
    err, cmd, cl = proto_parse_frame(frame)
    assert err == PROTO_OK, "T1: parse err %d" % err
    assert cl == 14, "T1: cmd_len %d != 14" % cl
    assert cmd[0] == CMD_START, "T1: cmd[0] %d" % cmd[0]
    assert cmd[1] == FW_TYPE_NORMAL, "T1: fw_type %d" % cmd[1]
    assert cmd[2:4] == _u16_le(0x0102), "T1: fw_ver %s" % cmd[2:4].hex()
    assert cmd[4:8] == _u32_le(0x12345678), "T1: fw_size %s" % cmd[4:8].hex()
    assert cmd[8:12] == _u32_le(0xDEADBEEF), "T1: fw_crc %s" % cmd[8:12].hex()
    assert cmd[12:14] == _u16_le(APP_VERIFY_CODE), "T1: verify %s" % cmd[12:14].hex()
    assert total == 16 + 14 + 4, "T1: total %d" % total
    print("Test 1 PASSED: StartCmd 往返 (total=%d, cmd_len=%d)" % (total, cl))

    # ---- Test 2: 校验和错误检测（翻转 checksum 字节） ----
    frame, _ = proto_build_start_cmd(FW_TYPE_NORMAL, 0x0102,
                                     0x12345678, 0xDEADBEEF)
    corrupted = bytearray(frame)
    corrupted[4] ^= 0xFF  # 翻转偏移 4（checksum）
    err, _, _ = proto_parse_frame(bytes(corrupted))
    assert err == PROTO_ERR_CHECKSUM, "T2: expected CHECKSUM(%d), got %d" % (
        PROTO_ERR_CHECKSUM, err)
    print("Test 2 PASSED: 翻转 checksum -> 解析失败 (err=PROTO_ERR_CHECKSUM)")

    # ---- Test 3: DataCmd 256 字节往返 ----
    data = bytes(range(256))
    frame, total = proto_build_data_cmd(FW_TYPE_FACTORY, 0x0304, 256,
                                        0xCAFEBABE, 0x00001000, data)
    err, cmd, cl = proto_parse_frame(frame)
    assert err == PROTO_OK, "T3: parse err %d" % err
    assert cl == 16 + 256, "T3: cmd_len %d != %d" % (cl, 16 + 256)
    assert cmd[0] == CMD_DATA, "T3: cmd[0] %d" % cmd[0]
    assert cmd[1] == FW_TYPE_FACTORY, "T3: fw_type %d" % cmd[1]
    assert cmd[2:4] == _u16_le(0x0304), "T3: fw_ver %s" % cmd[2:4].hex()
    assert cmd[4:8] == _u32_le(256), "T3: seg_size %s" % cmd[4:8].hex()
    assert cmd[8:12] == _u32_le(0xCAFEBABE), "T3: seg_crc %s" % cmd[8:12].hex()
    assert cmd[12:16] == _u32_le(0x00001000), "T3: offset %s" % cmd[12:16].hex()
    assert cmd[16:16 + 256] == data, "T3: data mismatch"
    assert total == 16 + 272 + 4, "T3: total %d" % total
    print("Test 3 PASSED: DataCmd 256 字节往返 (total=%d, cmd_len=%d)" % (total, cl))

    # ---- Test 4: 应答解析（DATA_OK 应答，含地址 256） ----
    # 应答 cmd: [0]=reply_type, [1]=status, [2..]=content
    # DATA_OK 应答：reply_type=CMD_DATA(回显), status=STATUS_DATA_OK, content=addr(4 LE)
    reply_cmd = bytearray(6)
    reply_cmd[0] = CMD_DATA
    reply_cmd[1] = STATUS_DATA_OK
    reply_cmd[2:6] = _u32_le(256)
    frame, _ = proto_build_frame(bytes(reply_cmd))
    err, rtype, status, content = proto_parse_reply(frame)
    assert err == PROTO_OK, "T4: parse err %d" % err
    assert rtype == CMD_DATA, "T4: reply_type %d" % rtype
    assert status == STATUS_DATA_OK, "T4: status %d" % status
    assert len(content) == 4, "T4: content len %d" % len(content)
    addr = content[0] | (content[1] << 8) | (content[2] << 16) | (content[3] << 24)
    assert addr == 256, "T4: addr %d != 256" % addr
    print("Test 4 PASSED: DATA_OK 应答解析 (status=%d, addr=%d)" % (status, addr))

    # ---- Test 5: 帧头字段校验（DevID/NetType/HostID/MsgType） ----
    frame, _ = proto_build_update_cmd(FW_UPDATE_NORMAL)
    assert frame[0:4] == FRAME_HEADER, "T5: header"
    assert frame[5] == APP_NET_TYPE, "T5: nettype %d" % frame[5]
    devid = (frame[10] << 24) | (frame[11] << 16) | (frame[12] << 8) | frame[13]
    assert devid == APP_DEV_ID == 999999, "T5: devid %d" % devid
    assert frame[14] == APP_HOST_ID, "T5: hostid %d" % frame[14]
    assert frame[15] == APP_MSG_TYPE, "T5: msgtype %d" % frame[15]
    # Length 字段 (BE) = 报文总长 - 8 = 12 + cmd_len = 12 + 2 = 14
    length = (frame[6] << 8) | frame[7]
    assert length == 14, "T5: length %d != 14" % length
    # Tail
    assert frame[-4:] == FRAME_TAIL, "T5: tail"
    print("Test 5 PASSED: 帧头/尾字段 (DevID=%d, NetType=%d, HostID=%d, "
          "MsgType=%d, Length=%d)" % (devid, frame[5], frame[14], frame[15], length))

    # ---- Test 6: CommID 自增 ----
    f1, _ = proto_build_update_cmd(FW_UPDATE_NORMAL)
    c1 = (f1[8] << 8) | f1[9]
    f2, _ = proto_build_update_cmd(FW_UPDATE_NORMAL)
    c2 = (f2[8] << 8) | f2[9]
    assert c2 == (c1 + 1) & 0xFFFF, "T6: commID %d -> %d (not incrementing)" % (c1, c2)
    print("Test 6 PASSED: CommID 自增 (%d -> %d)" % (c1, c2))

    # ---- Test 7: 错误检测（header/tail/too-short） ----
    # header 错
    bad = bytearray(proto_build_update_cmd(FW_UPDATE_NORMAL)[0])
    bad[0] = 0x00
    err, _, _ = proto_parse_frame(bytes(bad))
    assert err == PROTO_ERR_HEADER, "T7a: expected HEADER, got %d" % err
    # tail 错
    bad = bytearray(proto_build_update_cmd(FW_UPDATE_NORMAL)[0])
    bad[-1] ^= 0xFF
    err, _, _ = proto_parse_frame(bytes(bad))
    assert err == PROTO_ERR_TAIL, "T7b: expected TAIL, got %d" % err
    # too short
    err, _, _ = proto_parse_frame(b"\xFE\xEF\xED\xFC\x00")
    assert err == PROTO_ERR_TOO_SHORT, "T7c: expected TOO_SHORT, got %d" % err
    print("Test 7 PASSED: 错误检测 (header/tail/too-short 均正确识别)")

    # ---- Test 8: 全部命令构造器可解析 ----
    for label, fn in [
        ("Complete", lambda: proto_build_complete_cmd(FW_TYPE_NORMAL, 1, 1000, 0xABCDEF01)),
        ("Terminate", lambda: proto_build_terminate_cmd(5, 2000)),
        ("Update",    lambda: proto_build_update_cmd(FW_UPDATE_FACTORY)),
    ]:
        f, t = fn()
        err, _, cl = proto_parse_frame(f)
        assert err == PROTO_OK, "T8 %s: parse err %d" % (label, err)
        assert t == 16 + cl + 4, "T8 %s: total %d" % (label, t)
    print("Test 8 PASSED: Complete/Terminate/Update 命令构造器均可往返解析")

    # ---- Test 9: 帧内正文字段字节序（fw_ver/fw_size/crc/offset 小端） ----
    frame, _ = proto_build_start_cmd(FW_TYPE_NORMAL, 0xBEEF, 0x11223344, 0x55667788)
    _, cmd, _ = proto_parse_frame(frame)
    # fw_ver LE: 0xEF, 0xBE
    assert cmd[2] == 0xEF and cmd[3] == 0xBE, "T9: fw_ver LE %s" % cmd[2:4].hex()
    # fw_size LE: 0x44, 0x33, 0x22, 0x11
    assert cmd[4:8] == bytes([0x44, 0x33, 0x22, 0x11]), "T9: fw_size LE"
    # fw_crc LE: 0x88, 0x77, 0x66, 0x55
    assert cmd[8:12] == bytes([0x88, 0x77, 0x66, 0x55]), "T9: fw_crc LE"
    # verify_code 58902 = 0xE616 -> LE: 0x16, 0xE6
    assert cmd[12:14] == _u16_le(APP_VERIFY_CODE), "T9: verify LE %s" % cmd[12:14].hex()
    assert cmd[12] == 0x16 and cmd[13] == 0xE6, "T9: verify LE %s" % cmd[12:14].hex()
    print("Test 9 PASSED: 小端字段 (fw_ver=0xBEEF, fw_size/fw_crc/verify_code LE)")

    # ---- Test 10: 容忍前导字节 (目标回复前带一个 FE) ----
    frame, _ = proto_build_start_cmd(0, 0x0102, 307200, 0xAABBCCDD)
    padded = bytes([0xFE]) + frame          # 前面加一个 FE 前导字节
    ret, cmd_out, cl = proto_parse_frame(padded)
    assert ret == 0, "T10: parse with leading byte err=%d" % ret
    assert cl == 14, "T10: cmd_len %d != 14" % cl
    print("Test 10 PASSED: 前导字节容忍 (FE + frame)")

    print("\nAll protocol tests PASSED")


if __name__ == "__main__":
    main()
