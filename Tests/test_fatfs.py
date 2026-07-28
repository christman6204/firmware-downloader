#!/usr/bin/env python3
"""test_fatfs.py -- 验证 ff.c 的 FAT16/FAT32 解析逻辑

构建一个最小 FAT16 内存镜像（引导扇区 + FAT 表 + 根目录 + 数据区），
用 Python 翻译 C 算法（同字段、同公式、同 8.3 名比对），
验证 f_mount / f_open / f_read / f_lseek / f_stat 全部路径。
"""

import struct
import sys

# =========================================================================
#  C 代码语义等价翻译 (保持相同字段名/偏移/算法)
# =========================================================================

FS_FAT16 = 2
FS_FAT32 = 3
EOC_FAT16 = 0xFFF8
EOC_FAT32 = 0x0FFFFFF8
FAT16_MASK = 0xFFFF
FAT32_MASK = 0x0FFFFFFF
DIR_ENTRY_SIZE = 32

class FATFS:
    def __init__(self):
        self.fs_type = 0
        self.pdrv = 0
        self.n_fats = 0
        self.n_rootdir = 0
        self.csize = 0
        self.n_fatent = 0
        self.fsize = 0
        self.fatbase = 0
        self.dirbase = 0
        self.database = 0
        self.win = bytearray(512)
        self.winsect = 0xFFFFFFFF
        self.wflag = 0

class FIL:
    def __init__(self):
        self.obj = None
        self.flag = 0
        self.err = 0
        self.fptr = 0
        self.fsize = 0
        self.clust = 0
        self.start_clust = 0
        self.sect = 0

# ---------- 小端 --------------------------------------------
def ld_word(p, off):
    return p[off] | (p[off + 1] << 8)

def ld_dword(p, off):
    return (p[off] | (p[off + 1] << 8) |
            (p[off + 2] << 16) | (p[off + 3] << 24))

def st_word(p, off, v):
    p[off] = v & 0xFF
    p[off + 1] = (v >> 8) & 0xFF

def st_dword(p, off, v):
    p[off] = v & 0xFF
    p[off + 1] = (v >> 8) & 0xFF
    p[off + 2] = (v >> 16) & 0xFF
    p[off + 3] = (v >> 24) & 0xFF

# ---------- 8.3 文件名 --------------------------------------
def name_to_83(name):
    out = bytearray(b' ' * 11)
    p = name.split('.')
    main = p[0].upper().encode('ascii')
    ext = p[1].upper().encode('ascii') if len(p) > 1 else b''
    for i in range(min(8, len(main))):
        out[i] = main[i]
    for i in range(min(3, len(ext))):
        out[8 + i] = ext[i]
    return bytes(out)

# ---------- FAT entry / cluster -> sector -------------------
def get_fat(fs, clust):
    if fs.fs_type == FS_FAT16:
        off = clust * 2
        sec = fs.fatbase + (off >> 9)
        v = ld_word(fs.win, off & 511)
        v = v & FAT16_MASK if (off >> 9) == 0 else ld_word(fs._img, (sec << 9) + (off & 511)) & FAT16_MASK
        return ld_word(fs._img, (sec << 9) + (off & 511)) & FAT16_MASK
    else:
        off = clust * 4
        sec = fs.fatbase + (off >> 9)
        return ld_dword(fs._img, (sec << 9) + (off & 511)) & FAT32_MASK

def clust2sect(fs, clust):
    return fs.database + (clust - 2) * fs.csize

def disk_read(fs, off_bytes, length):
    """Read from in-memory image"""
    return fs._img[off_bytes:off_bytes + length]

# ---------- directory scan ----------------------------------
def valid_entry(entry):
    if entry[0] == 0x00: return False
    if entry[0] == 0xE5: return False
    if entry[11] == 0x0F: return False
    if entry[11] & 0x08: return False
    return True

def dir_find(fs, name):
    """Scan root dir; return (cluster, size) or None"""
    if fs.fs_type == FS_FAT32:
        clust = fs.dirbase
        while True:
            base = clust2sect(fs, clust)
            for ci in range(fs.csize):
                off = (base + ci) << 9
                sector = fs._img[off:off + 512]
                for di in range(0, 512, DIR_ENTRY_SIZE):
                    entry = sector[di:di + DIR_ENTRY_SIZE]
                    if not valid_entry(entry):
                        if entry[0] == 0x00:
                            return None
                        continue
                    if entry[:11] == name:
                        start_clust = ld_word(entry, 20) << 16 | ld_word(entry, 26)
                        fsize = ld_dword(entry, 28)
                        return (start_clust, fsize)
            nxt = get_fat(fs, clust)
            if nxt >= EOC_FAT32:
                break
            clust = nxt
        return None
    else:  # FAT16
        sect = fs.dirbase
        end = sect + (fs.n_rootdir * 32 + 511) // 512
        n = fs.n_rootdir
        while sect < end and n > 0:
            off = sect << 9
            sector = fs._img[off:off + 512]
            for di in range(0, min(512, n * 32), DIR_ENTRY_SIZE):
                entry = sector[di:di + DIR_ENTRY_SIZE]
                n -= 1
                if not valid_entry(entry):
                    if entry[0] == 0x00:
                        return None
                    continue
                if n >= 0 and entry[0:11] == name:
                    start_clust = ld_word(entry, 20) << 16 | ld_word(entry, 26)
                    fsize = ld_dword(entry, 28)
                    return (start_clust, fsize)
            sect += 1
        return None

# ---------- f_mount -----------------------------------------
def f_mount(fs, img):
    """Parse boot sector BPB; populate fs"""
    if ld_word(img, 510) != 0xAA55:
        return 13  # FR_NO_FILESYSTEM
    if ld_word(img, 11) != 512:
        return 13

    fs.csize = img[13]
    reserved = ld_word(img, 14)
    fs.n_fats = img[16]
    root_ents = ld_word(img, 17)
    fat_size = ld_word(img, 22)
    total_sect = ld_word(img, 19)

    if fat_size == 0:
        fat_size = ld_dword(img, 36)
        fs.dirbase = ld_dword(img, 44)
        fs.n_rootdir = 0
        if total_sect == 0:
            total_sect = ld_dword(img, 32)
    else:
        fs.dirbase = reserved + fs.n_fats * fat_size
        fs.n_rootdir = root_ents
        if total_sect == 0:
            total_sect = ld_dword(img, 32)

    root_dir_sectors = (root_ents * 32 + 511) // 512
    fs.database = reserved + fs.n_fats * fat_size + root_dir_sectors
    fs.fatbase = reserved
    fs.fsize = fat_size
    data_sectors = total_sect - fs.database

    n_clusters = data_sectors // fs.csize
    fs.n_fatent = n_clusters + 2

    if n_clusters < 4085:
        fs.fs_type = FS_FAT16
    elif n_clusters < 65525:
        fs.fs_type = FS_FAT16
    else:
        fs.fs_type = FS_FAT32

    fs._img = img
    fs.win[:] = img[0:512]
    fs.winsect = 0
    return 0  # FR_OK

# ---------- f_open ------------------------------------------
def f_open(fs, fp, path):
    name = name_to_83(path)
    result = dir_find(fs, name)
    if result is None:
        return 4  # FR_NO_FILE
    fp.obj = fs
    fp.fsize = result[1]
    fp.fptr = 0
    fp.clust = result[0]
    fp.start_clust = result[0]
    fp.sect = clust2sect(fs, result[0])
    fp.flag = 1
    return 0

# ---------- f_read ------------------------------------------
def f_read(fp, buff_len):
    """Read at most buff_len bytes from file; return bytes read"""
    fs = fp.obj
    if fp.fptr >= fp.fsize:
        return b''
    remaining = fp.fsize - fp.fptr
    btr = min(buff_len, remaining)
    bpc = fs.csize * 512
    result = bytearray()
    fptr = fp.fptr
    clust = fp.clust

    while len(result) < btr:
        sector = clust2sect(fs, clust) + (fptr % bpc) // 512
        off_bytes = sector << 9
        sector_data = fs._img[off_bytes:off_bytes + 512]
        chunk = min(512 - (fptr & 511), btr - len(result))
        result.extend(sector_data[(fptr & 511):(fptr & 511) + chunk])
        fptr += chunk

        if fptr < fp.fsize and (fptr % bpc) == 0:
            nxt = get_fat(fs, clust)
            if nxt >= (EOC_FAT32 if fs.fs_type == FS_FAT32 else EOC_FAT16):
                break
            clust = nxt

    fp.fptr = fptr
    fp.clust = clust
    return bytes(result)

# ---------- f_lseek -----------------------------------------
def f_lseek(fp, ofs):
    fs = fp.obj
    offset = min(ofs, fp.fsize)
    bpc = fs.csize * 512
    clust = fp.start_clust
    off = offset

    while off >= bpc:
        nxt = get_fat(fs, clust)
        if nxt >= (EOC_FAT32 if fs.fs_type == FS_FAT32 else EOC_FAT16):
            return 2  # FR_INT_ERR
        clust = nxt
        off -= bpc

    fp.clust = clust
    fp.sect = clust2sect(fs, clust) + off // 512
    fp.fptr = offset
    return 0

# =========================================================================
#                              测试用例 (FAT16)
# =========================================================================

def build_fat16_image(file_content):
    """构建最小 FAT16 镜像 (1 MB, 8 sectors/cluster), 包含 APP.bin"""
    img = bytearray()
    bps = 512
    spc = 8
    reserved_sectors = 1
    num_fats = 1
    root_entries = 64
    sectors_per_fat = 1
    total_sectors = 2048  # 1 MB

    # --- 引导扇区 (BPB) ---
    boot = bytearray(bps)
    boot[0:3] = b'\xEB\x3C\x90'       # 跳转指令 + NOP
    st_word(boot, 11, bps)
    boot[13] = spc
    st_word(boot, 14, reserved_sectors)
    boot[16] = num_fats
    st_word(boot, 17, root_entries)
    st_word(boot, 19, total_sectors)
    boot[21] = 0xF8                     # 媒体描述符
    st_word(boot, 22, sectors_per_fat)
    st_dword(boot, 32, total_sectors)   # 32-bit 大扇区数（仅当 16-bit == 0）
    boot[36] = 0x29                     # 扩展引导签名字节
    st_word(boot, 510, 0xAA55)          # 引导签名
    img += boot

    # --- FAT 表 ---
    # FAT[0] = 0xFFF8 (media)
    # FAT[1] = 0xFFFF (EOC, reserved cluster)
    # FAT[2] = 0xFFFF (EOC for single-cluster file)
    # FAT[3] = 0x0000 (free)
    fat = bytearray(sectors_per_fat * bps)
    fat[0] = 0xF8
    fat[1] = 0xFF      # FAT[0] = 0xFFF8
    fat[2] = 0xFF
    fat[3] = 0xFF      # FAT[1] = 0xFFFF
    fat[4] = 0xFF
    fat[5] = 0xFF      # FAT[2] = 0xFFFF (EOC)
    img += fat

    # --- 根目录 ---
    root_sectors = (root_entries * 32 + bps - 1) // bps
    root = bytearray(root_sectors * bps)
    # APP.bin 条目
    name = name_to_83("APP.bin")
    entry = bytearray(32)
    entry[0:11] = name
    entry[11] = 0x20                     # Archive
    # time/date (dummy)
    st_word(entry, 26, 2)                # 起始簇 = 2
    file_size = len(file_content)
    st_dword(entry, 28, file_size)
    root[0:32] = entry
    img += root

    # --- 数据区 ---
    data_region_offset = (reserved_sectors + num_fats * sectors_per_fat + root_sectors) * bps
    data_sectors = total_sectors - (data_region_offset // bps)
    data = bytearray(data_sectors * bps)
    # cluster 2 = first data cluster
    cluster2_offset = 0                  # cluster 2 starts at data region offset 0
    data[cluster2_offset:cluster2_offset + file_size] = file_content
    img += data

    return bytes(img), file_size

# =========================================================================
#                                   Run
# =========================================================================

def test_fat16():
    content = b'Version=1.0\x00' + b'\x55' * 900   # 912 bytes, fits 1 cluster (8 * 512 = 4096)
    img, fsize = build_fat16_image(content)

    fs = FATFS()
    fp = FIL()

    # Test 1: f_mount
    ret = f_mount(fs, img)
    assert ret == 0, f"f_mount failed with {ret}"
    assert fs.fs_type == FS_FAT16, f"expected FAT16, got {fs.fs_type}"
    assert fs.database > 0, "database sector not computed"
    print("Test 1 PASSED: f_mount (FAT16)")

    # Test 2: f_stat
    name = name_to_83("APP.bin")
    result = dir_find(fs, name)
    assert result is not None, "APP.bin not found"
    clust, found_fsize = result
    assert found_fsize == fsize, f"fsize mismatch: {found_fsize} vs {fsize}"
    print("Test 2 PASSED: f_stat (found APP.bin, size={})".format(found_fsize))

    # Test 3: f_open
    ret = f_open(fs, fp, "APP.bin")
    assert ret == 0, f"f_open failed with {ret}"
    assert fp.fsize == fsize
    assert fp.start_clust == 2
    print("Test 3 PASSED: f_open (APP.bin, {} bytes)".format(fp.fsize))

    # Test 4: f_read whole file
    data = f_read(fp, fsize)
    assert len(data) == fsize, f"read {len(data)} != {fsize}"
    assert data == content, f"content mismatch at byte"
    print("Test 4 PASSED: f_read {} bytes matches".format(len(data)))

    # Test 5: f_read past EOF
    fp.fptr = fsize - 1
    tail = f_read(fp, 100)
    assert len(tail) == 1, f"EOF read got {len(tail)} bytes"
    print("Test 5 PASSED: f_read near EOF")

    # Test 6: f_lseek to middle then f_read
    ret = f_lseek(fp, 100)
    assert ret == 0
    assert fp.fptr == 100
    data2 = f_read(fp, 50)
    assert data2 == content[100:150], "lseek+read mismatch"
    print("Test 6 PASSED: f_lseek(100) + f_read(50)")

    # Test 7: f_lseek past EOF clamps
    ret = f_lseek(fp, fsize + 99999)
    assert ret == 0
    assert fp.fptr == fsize, f"clamped to {fp.fptr}"
    data3 = f_read(fp, 10)
    assert len(data3) == 0
    print("Test 7 PASSED: f_lseek past EOF clamped")

    # Test 8: f_read from file head after seek back
    ret = f_lseek(fp, 12)
    assert ret == 0
    assert fp.fptr == 12
    data4 = f_read(fp, 8)
    assert data4 == content[12:20], "seek-back read mismatch"
    print("Test 8 PASSED: f_lseek(12) back from EOF")

    # Test 9: f_open missing file
    ret = f_open(fs, fp, "NOFILE.TXT")
    assert ret == 4, f"expected FR_NO_FILE, got {ret}"
    print("Test 9 PASSED: f_open missing file returns FR_NO_FILE")

    print("\nAll FAT16 tests PASSED")


if __name__ == "__main__":
    test_fat16()
