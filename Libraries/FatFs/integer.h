/* Libraries/FatFs/integer.h
 *
 * FatFs 整数类型别名。
 *
 * 官方 ff.h（R0.15 起）通过 C99 <stdint.h> 内联定义 UINT/BYTE/WORD/DWORD/
 * QWORD/WCHAR。当 ff.h 先于本文件被包含时（FF_DEFINED 已定义），跳过所有
 * 类型定义避免重定义警告。
 * 当 diskio.c 仅含本文件（未先含 ff.h）时，本文件提供兼容类型。
 */
#ifndef _INTEGER
#define _INTEGER

#include <stdint.h>

#ifndef FF_DEFINED
/* 官方 ff.h 未包含时才定义（diskio.h → integer.h 但未 → ff.h 的场景） */
typedef unsigned int    UINT;
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef unsigned long   DWORD;
typedef uint64_t        QWORD;
typedef unsigned short  WCHAR;
#endif

/* 以下类型官方 ff.h 不定义，总是可用 */
typedef int             INT;
typedef signed char     CHAR;
typedef unsigned char   UCHAR;
typedef short           SHORT;
typedef unsigned short  USHORT;
typedef long            LONG;

#endif /* _INTEGER */
