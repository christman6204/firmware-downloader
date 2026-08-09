#!/usr/bin/env python3
"""
hex_utils.py — Intel HEX 解析库

提供 hex_to_binary(hex_path) -> (start_addr, data)

合并工具用:
  - 解析 Intel HEX (.hex) 文件
  - 返回 (起始地址, 连续字节数组)
  - 忽略 HEX 内部绝对地址偏移（合并工具统一从 0 放置）

Intel HEX 记录格式:
  :LLAAAATT[DD...]CC
    LL   = 数据字节数
    AAAA = 16位地址
    TT   = 记录类型: 00=数据, 01=EOF, 02=段地址, 04=扩展线性地址
    DD   = 数据字节
    CC   = 校验和 (所有字节和取反+1, 低8位为0)
"""

def _parse_hex_line(line):
    """解析一行 HEX 记录, 返回 (addr, type, data) 或 None (EOF/忽略)。"""
    line = line.strip()
    if not line or line[0] != ':':
        return None

    try:
        body = bytes.fromhex(line[1:])
    except ValueError:
        return None

    if len(body) < 5:
        return None

    byte_count = body[0]
    addr = (body[1] << 8) | body[2]
    rec_type = body[3]
    data = body[4:4 + byte_count]

    # 校验和验证: 所有字节(含校验和)和低8位应为0
    if sum(body) & 0xFF != 0:
        raise ValueError(f"HEX 校验和错误: {line}")

    return (addr, rec_type, data)


def hex_to_binary(hex_path):
    """解析 Intel HEX 文件, 返回 (start_addr, data)。

    - 起始地址 = 所有数据记录的最小绝对地址
    - data     = 从 start_addr 到最大地址的连续字节 (间隙填 0xFF)
    - 支持 16 位地址 + 扩展线性地址 (type 04) 记录
    """
    segments = []      # (abs_addr, data)
    upper_addr = 0     # 扩展线性地址 (type 04) 的高 16 位

    with open(hex_path, "r") as f:
        for line in f:
            parsed = _parse_hex_line(line)
            if parsed is None:
                continue

            addr, rec_type, data = parsed

            if rec_type == 0x00:
                # 数据记录: 绝对地址 = upper<<16 | addr
                abs_addr = (upper_addr << 16) | addr
                segments.append((abs_addr, data))
            elif rec_type == 0x01:
                # EOF
                break
            elif rec_type == 0x04:
                # 扩展线性地址 (ELA): data[0:2] = 高16位
                if len(data) >= 2:
                    upper_addr = (data[0] << 8) | data[1]
            elif rec_type == 0x02:
                # 扩展段地址 (USA): 段<<4
                if len(data) >= 2:
                    upper_addr = ((data[0] << 8) | data[1]) << 4
            # 类型 03/05 (起始地址) 忽略

    if not segments:
        raise ValueError(f"HEX 文件无有效数据: {hex_path}")

    # 计算起始/结束地址
    start = min(a for a, _ in segments)
    end = max(a + len(d) for a, d in segments)

    # 构建连续缓冲区 (间隙填 0xFF)
    buf = bytearray([0xFF] * (end - start))
    for a, d in segments:
        buf[a - start:a - start + len(d)] = d

    return start, bytes(buf)
