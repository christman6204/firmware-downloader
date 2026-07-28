/* Libraries/FatFs/integer.h
 *
 * FatFs R0.15 风格的整数类型别名。
 *
 * 注意：本文件为占位实现，供本任务桩 ff.c 与 diskio.c 编译。
 * 当用户从 FatFs R0.15 源码包替换 ff.c 时，可一并替换本文件为
 * 官方 integer.h（两者类型定义一致，无冲突）。
 */
#ifndef _INTEGER
#define _INTEGER

#include <stdint.h>

typedef int             INT;
typedef unsigned int    UINT;

typedef signed char     CHAR;
typedef unsigned char   UCHAR;
typedef unsigned char   BYTE;

typedef short           SHORT;
typedef unsigned short  USHORT;
typedef unsigned short  WORD;
typedef unsigned short  WCHAR;

typedef long            LONG;
typedef unsigned long   DWORD;

/* 64-bit 整数（FF_LBA64 / 大文件场景使用；ARMCC 与 GCC 均原生支持） */
typedef uint64_t        QWORD;

#endif /* _INTEGER */
