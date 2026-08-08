/* ffunicode.c — 最小 Unicode 转换实现
 *
 * FatFs R0.15 启用 LFN (FF_USE_LFN > 0) 时需要 3 个 Unicode 函数。
 * 完整实现在官方 ffunicode.c（含数千字节码页表），但本工程文件名
 * 仅用纯 ASCII (IL_800_XXX_XXX.BIN)，以下最小实现足够：
 *
 *   ff_wtoupper : ASCII a-z -> A-Z（大小写不敏感匹配用）
 *   ff_uni2oem  : Unicode < 128 直接截断为 OEM 字节
 *   ff_oem2uni  : OEM 字节零扩展为 Unicode
 *
 * 如需支持非 ASCII 文件名，请用官方 ffunicode.c 替换本文件。
 */
#include "ff.h"

#if FF_USE_LFN

/* Unicode -> OEM (code page 转换; ASCII 直接截断) */
WCHAR ff_uni2oem (DWORD uni, WORD cp)
{
    (void)cp;
    if (uni < 128u) {
        return (WCHAR)uni;          /* ASCII 直接映射 */
    }
    return 0u;                       /* 非 ASCII: 无法转换 */
}

/* OEM -> Unicode (零扩展) — 注意: 官方 ff.h 返回 WCHAR */
WCHAR ff_oem2uni (WCHAR oem, WORD cp)
{
    (void)cp;
    if (oem < 128u) {
        return oem;                 /* ASCII 直接映射 */
    }
    return 0u;                       /* 非 ASCII: 无法转换 */
}

/* Unicode 大写转换 (ASCII a-z -> A-Z) — 注意: 官方 ff.h 参数和返回值都是 DWORD */
DWORD ff_wtoupper (DWORD c)
{
    if (c >= 'a' && c <= 'z') {
        return c - ('a' - 'A');
    }
    return c;
}

#endif /* FF_USE_LFN */
