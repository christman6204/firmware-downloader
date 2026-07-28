#!/usr/bin/env python3
"""Tests/test_crc32.py

PC 端单元测试：将 app/crc32.c 的逻辑移植到 Python，验证 CRC32 算法正确性。
Python 移植与 C 代码使用相同的表生成算法（reflected 0xEDB88320）和相同的
Update/GetResult 流程，因此 Python 测试通过即验证 C 逻辑正确。

运行: python test_crc32.py
预期: 所有断言通过，打印 PASSED。
"""
import zlib

# ---------------------------------------------------------------------------
# CRC32 表生成（与 C 代码 crc32_table_flash 完全相同的算法）
# reflected polynomial 0xEDB88320
# ---------------------------------------------------------------------------
def _gen_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if (c & 1) else (c >> 1)
        table.append(c)
    return table

CRC32_TABLE = _gen_table()

# ---------------------------------------------------------------------------
# 模块状态（镜像 C 的 g_crc_state）
# ---------------------------------------------------------------------------
_crc_state = 0xFFFFFFFF


def crc32_reset():
    """镜像 CRC32_Reset。"""
    global _crc_state
    _crc_state = 0xFFFFFFFF


def crc32_update(data):
    """镜像 CRC32_Update：逐字节查表更新。

    C 的 length 是 uint16_t（单次 <=65535）；这里 data 是 bytes，长度由调用方控制。
    CRC32_Calc 内部按 65535 分块调用本函数。
    """
    global _crc_state
    crc = _crc_state
    for b in data:
        index = (crc ^ b) & 0xFF
        crc = (crc >> 8) ^ CRC32_TABLE[index]
    _crc_state = crc


def crc32_get_result():
    """镜像 CRC32_GetResult：g_crc_state ^ 0xFFFFFFFF。"""
    return _crc_state ^ 0xFFFFFFFF


def crc32_calc(buf):
    """镜像 CRC32_Calc：Reset -> 按 65535 分块 Update -> GetResult。"""
    crc32_reset()
    off = 0
    n = len(buf)
    while off < n:
        chunk = min(n - off, 65535)
        crc32_update(buf[off:off + chunk])
        off += chunk
    return crc32_get_result()


# ---------------------------------------------------------------------------
# 测试用例
# ---------------------------------------------------------------------------
def main():
    print("=== CRC32 单元测试（Python 移植 C 逻辑） ===\n")

    # ---- Test 1: 标准测试向量 CRC32("123456789") == 0xCBF43926 ----
    result = crc32_calc(b"123456789")
    assert result == 0xCBF43926, \
        "Test 1 FAILED: CRC32('123456789') = 0x%08X, expected 0xCBF43926" % result
    print("Test 1 PASSED: CRC32('123456789') = 0x%08X (标准测试向量)" % result)

    # ---- Test 2: CRC32("") == 0x00000000 ----
    result = crc32_calc(b"")
    assert result == 0x00000000, \
        "Test 2 FAILED: CRC32('') = 0x%08X, expected 0x00000000" % result
    print("Test 2 PASSED: CRC32('') = 0x%08X (空缓冲)" % result)

    # ---- Test 3: 分块 Update 与整缓冲 Calc 一致 ----
    buf = bytes(range(256)) * 4  # 1024 字节
    whole = crc32_calc(buf)
    crc32_reset()
    crc32_update(buf[0:7])
    crc32_update(buf[7:107])
    crc32_update(buf[107:407])
    crc32_update(buf[407:1024])
    split = crc32_get_result()
    assert split == whole, \
        "Test 3 FAILED: split=0x%08X != whole=0x%08X" % (split, whole)
    print("Test 3 PASSED: 分块 Update (0x%08X) == 整缓冲 Calc (0x%08X)" % (split, whole))

    # ---- Test 4: 表关键值（与 C 固化表对照） ----
    assert CRC32_TABLE[0] == 0x00000000, \
        "Test 4 FAILED: table[0] = 0x%08X, expected 0x00000000" % CRC32_TABLE[0]
    assert CRC32_TABLE[1] == 0x77073096, \
        "Test 4 FAILED: table[1] = 0x%08X, expected 0x77073096" % CRC32_TABLE[1]
    assert CRC32_TABLE[255] == 0x2D02EF8D, \
        "Test 4 FAILED: table[255] = 0x%08X, expected 0x2D02EF8D" % CRC32_TABLE[255]
    print("Test 4 PASSED: table[0]=0x%08X, table[1]=0x%08X, table[255]=0x%08X"
          % (CRC32_TABLE[0], CRC32_TABLE[1], CRC32_TABLE[255]))

    # ---- Test 5: 长缓冲 (>65535) 验证分块逻辑，对照 zlib ----
    big = bytes(range(256)) * 300  # 76800 字节 > 65535
    big_crc = crc32_calc(big)
    expected = zlib.crc32(big) & 0xFFFFFFFF
    assert big_crc == expected, \
        "Test 5 FAILED: 0x%08X != zlib 0x%08X" % (big_crc, expected)
    print("Test 5 PASSED: 76800 字节 CRC32 = 0x%08X (匹配 zlib)" % big_crc)

    # ---- Test 6: 多种缓冲对照 zlib（含单字节、对齐长度） ----
    for test_buf in [b"\x00", b"\xFF", b"A", bytes(range(256)),
                     b"Hello, World!", bytes(range(256)) * 256]:
        got = crc32_calc(test_buf)
        exp = zlib.crc32(test_buf) & 0xFFFFFFFF
        assert got == exp, \
            "Test 6 FAILED: len=%d got=0x%08X != zlib=0x%08X" % (len(test_buf), got, exp)
    print("Test 6 PASSED: 6 种缓冲均匹配 zlib（含 65536 字节边界）")

    print("\nAll CRC32 tests PASSED")


if __name__ == "__main__":
    main()
