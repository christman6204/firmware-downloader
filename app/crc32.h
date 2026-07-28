/* app/crc32.h
 *
 * 标准 CRC32（Ethernet/zlib）：反射多项式 0xEDB88320，init=0xFFFFFFFF，
 * xorout=0xFFFFFFFF。查表实现，256 项表固化在 flash（const）。
 *
 * 设计文档第 12 章。本模块为纯逻辑，不依赖 SPL/HAL/UCOS，
 * 可在 PC 端用 Python 移植验证（Tests/test_crc32.py）。
 *
 * 用法：
 *   CRC32_Reset();
 *   CRC32_Update(data1, len1);
 *   CRC32_Update(data2, len2);
 *   crc = CRC32_GetResult();
 * 或一次性：
 *   crc = CRC32_Calc(buf, len);
 */
#ifndef __CRC32_H
#define __CRC32_H

#include <stdint.h>

/* 复位 CRC 状态：g_crc_state = 0xFFFFFFFF */
void     CRC32_Reset(void);

/* 增量更新：对 data[0..length-1] 逐字节查表更新 g_crc_state。
   length 为 uint16_t，单次最多 65535 字节；更长缓冲请分多次调用
   或使用 CRC32_Calc（内部按 65535 分块）。 */
void     CRC32_Update(const uint8_t *data, uint16_t length);

/* 取最终结果：g_crc_state ^ 0xFFFFFFFF */
uint32_t CRC32_GetResult(void);

/* 一次性计算：Reset -> 按 65535 分块 Update -> GetResult。
   适用于 len 可能超过 65535 的整包校验。 */
uint32_t CRC32_Calc(const uint8_t *buf, uint32_t len);

#endif /* __CRC32_H */
