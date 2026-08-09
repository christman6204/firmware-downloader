#!/usr/bin/env python3
"""
STM32 OTA 合并工具

功能: 将 BT HEX 转 BIN + 与 APP BIN 合并 → IL800_FD.BIN

合并布局:
  0x00000  BT 数据 (HEX 转 BIN, 从 0 偏移)
  ...      间隙 0xFF
  0x20000  APP 数据 (BIN 原样)

运行:  python merge_tool.py
"""

import os, sys, threading, json
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hex_utils import hex_to_binary

# ── Apple-style palette (与 factory_tool 一致) ────────
C_BG        = "#f2f2f7"
C_CARD      = "#ffffff"
C_PRIMARY   = "#0071e3"
C_PRIMARY_H = "#0077ed"
C_TEXT      = "#1d1d1f"
C_SUBTEXT   = "#86868b"
C_BORDER    = "#d2d2d7"
C_LOG_BG    = "#1d1d1f"
C_LOG_FG    = "#e5e5e7"
C_LOG_OK    = "#30d158"
C_LOG_WARN  = "#ffd60a"
C_LOG_ERR   = "#ff453a"
C_LOG_DIM   = "#6e6e73"

FONT        = ("Microsoft YaHei UI", 11, "bold")
FONT_SM     = ("Microsoft YaHei UI", 10)
FONT_BOLD   = ("Microsoft YaHei UI", 12, "bold")
FONT_TITLE  = ("Microsoft YaHei UI", 15, "bold")
FONT_MONO   = ("Consolas", 12)

PAD_CARD_X  = 20
PAD_CARD_Y  = 16
GAP         = 10

# ── 合并参数 ─────────────────────────────────────────
APP_OFFSET   = 0x20000       # APP 起始偏移 128KB
APP_SIZE_OFF = 0x20002       # APP 文件大小字段偏移 (4字节小端)
FILL_BYTE    = 0xFF          # 间隙填充
OUTPUT_NAME  = "IL800_FD.BIN"  # 输出固定名

def fmt_size(n):
    if n < 1024*1024: return f"{n/1024:.1f} KB"
    return f"{n/1024/1024:.2f} MB"

# ── helpers (复用 factory_tool 样式) ────────────────
def _card(parent):
    f = tk.Frame(parent, bg=C_CARD, highlightthickness=1, highlightbackground=C_BORDER)
    f.pack(fill=tk.X, pady=(0, GAP))
    b = tk.Frame(f, bg=C_CARD)
    b.pack(fill=tk.X, padx=PAD_CARD_X, pady=PAD_CARD_Y)
    return b

def _row(parent, label, w=9):
    r = tk.Frame(parent, bg=C_CARD)
    r.pack(fill=tk.X, pady=(0, 5))
    tk.Label(r, text=label, width=w, anchor=tk.W, font=FONT, bg=C_CARD, fg=C_TEXT).pack(side=tk.LEFT)
    e = tk.Entry(r, font=FONT, bg="#f5f5f7", fg=C_TEXT, relief=tk.FLAT, bd=0,
                 highlightthickness=1, highlightbackground=C_BORDER,
                 highlightcolor=C_PRIMARY, insertbackground=C_TEXT)
    e.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=3)
    return e

def _btn_row(parent):
    r = tk.Frame(parent, bg=C_CARD); r.pack(fill=tk.X, pady=(6,0))
    return r

def _small_btn(parent, text, cmd):
    b = tk.Button(parent, text=text, font=("Microsoft YaHei UI", 12, "bold"),
                  bg="#e8e8ed", fg=C_TEXT, activebackground="#d2d2d7",
                  activeforeground=C_TEXT, relief=tk.RAISED, bd=1,
                  padx=28, pady=6, cursor="hand2", command=cmd)
    b.pack(side=tk.RIGHT, padx=(4,0))
    return b

def _log(parent, h):
    f = _card(parent)
    log = tk.Text(f, height=h, state=tk.DISABLED, font=FONT_MONO,
                  bg=C_LOG_BG, fg=C_LOG_FG, insertbackground="white",
                  relief=tk.FLAT, padx=14, pady=10, borderwidth=0)
    log.pack(fill=tk.BOTH, expand=True)
    log.tag_config("ok", foreground=C_LOG_OK)
    log.tag_config("warn", foreground=C_LOG_WARN)
    log.tag_config("err", foreground=C_LOG_ERR)
    log.tag_config("dim", foreground=C_LOG_DIM)
    log.tag_config("info", foreground=C_LOG_FG)
    return log

def _append(w, msg, tag=None):
    w.configure(state=tk.NORMAL); w.insert(tk.END, msg+"\n", tag)
    w.see(tk.END); w.configure(state=tk.DISABLED); w.update_idletasks()

def _clear(w):
    w.configure(state=tk.NORMAL); w.delete("1.0",tk.END); w.configure(state=tk.DISABLED)

def parse_app_version(filename):
    """从 IL_800_XXX.XXX.bin 解析主/子版本号"""
    base = os.path.splitext(os.path.basename(filename))[0]
    parts = base.split("_")
    # IL_800_1.21 → parts = ["IL","800","1.21"]
    for p in parts:
        if "." in p:
            try:
                major, minor = p.split(".")
                return int(major), int(minor)
            except ValueError:
                pass
    return None, None

# ================================================================
class App:
    def __init__(self, root):
        self.root = root
        root.title("STM32 OTA 合并工具")
        root.geometry("680x600")
        root.minsize(620, 540)
        root.configure(bg=C_BG)

        # 配置文件路径
        tool_dir = os.path.dirname(os.path.abspath(__file__))
        self._config_path = os.path.join(tool_dir, ".merge_tool_config.json")

        s = ttk.Style()
        s.theme_use("clam")
        s.configure("TNotebook", background=C_BG, borderwidth=0, tabmargins=(0,0,0,0))
        s.configure("TNotebook.Tab", font=("Microsoft YaHei UI", 12, "bold"), padding=(28,12),
                    borderwidth=0, background=C_BG, foreground=C_TEXT)
        s.map("TNotebook.Tab", background=[("selected",C_CARD)], foreground=[("selected",C_SUBTEXT)])

        self._build()
        self._auto_load_config()

    def _build(self):
        # ── header ──
        h = tk.Frame(self.root, bg=C_CARD, height=60, highlightthickness=1, highlightbackground=C_BORDER)
        h.pack(fill=tk.X)
        h.pack_propagate(False)
        tk.Label(h, text="  STM32 OTA 合并工具", font=FONT_TITLE, bg=C_CARD, fg=C_TEXT).pack(side=tk.LEFT, padx=20)

        # ── body ──
        body = tk.Frame(self.root, bg=C_BG, padx=GAP+2, pady=GAP)
        body.pack(fill=tk.BOTH, expand=True)

        # 输入文件卡片
        c = _card(body)
        tk.Label(c, text="输入文件", font=FONT_BOLD, bg=C_CARD, fg=C_TEXT).pack(anchor=tk.W, pady=(0,4))

        r1 = tk.Frame(c, bg=C_CARD); r1.pack(fill=tk.X, pady=(0,5))
        self.t_bt = _row(c, "BT HEX")
        self.t_bt.bind("<FocusOut>", lambda e: self._auto_pair_app())

        def browse_bt():
            p = filedialog.askopenfilename(title="选择 BT HEX (IL800_FD.HEX)",
                filetypes=[("HEX files","*.hex"),("All files","*.*")])
            if p:
                self.t_bt.delete(0, tk.END)
                self.t_bt.insert(0, p)
                self._auto_pair_app()
                self._save_config()

        _small_btn(r1, "浏览", browse_bt)

        r2 = tk.Frame(c, bg=C_CARD); r2.pack(fill=tk.X, pady=(0,5))
        self.t_app = _row(c, "APP BIN")

        def browse_app():
            p = filedialog.askopenfilename(title="选择 APP BIN (IL_800_XXX.XXX.bin)",
                filetypes=[("BIN files","*.bin"),("All files","*.*")])
            if p:
                self.t_app.delete(0, tk.END)
                self.t_app.insert(0, p)
                self._show_version()
                self._save_config()

        _small_btn(r2, "浏览", browse_app)

        r3 = tk.Frame(c, bg=C_CARD); r3.pack(fill=tk.X)
        self.t_outdir = _row(c, "输出目录")

        def browse_dir():
            p = filedialog.askdirectory(title="选择输出目录")
            if p:
                self.t_outdir.delete(0, tk.END)
                self.t_outdir.insert(0, p)
                self._save_config()

        _small_btn(r3, "选择", browse_dir)

        # 版本提示
        self.version_label = tk.Label(c, text="", font=FONT_SM, bg=C_CARD, fg=C_SUBTEXT)
        self.version_label.pack(anchor=tk.W, pady=(4,0))

        # 合并按钮
        r4 = _btn_row(c)
        self.btn_merge = tk.Button(r4, text="合并生成 IL800_FD.BIN", font=("Microsoft YaHei UI", 14, "bold"),
            bg=C_PRIMARY, fg="white", activebackground=C_PRIMARY_H,
            activeforeground="white", relief=tk.FLAT, padx=30, pady=9,
            cursor="hand2", bd=0, command=self._run_merge)
        self.btn_merge.pack(side=tk.RIGHT)

        # 日志
        self.log = _log(body, 16)

    def _auto_pair_app(self):
        """自动查找同目录下的 APP BIN"""
        bt = self.t_bt.get().strip()
        if not bt or not os.path.exists(bt):
            return
        d = os.path.dirname(bt)
        matches = [f for f in os.listdir(d)
                   if f.lower().startswith("il_800") and f.lower().endswith(".bin")]
        if len(matches) == 1:
            self.t_app.delete(0, tk.END)
            self.t_app.insert(0, os.path.join(d, matches[0]))
            self._show_version()

    def _show_version(self):
        """从 APP BIN 文件名解析版本号"""
        ap = self.t_app.get().strip()
        if not ap or not os.path.exists(ap):
            self.version_label.configure(text="")
            return
        major, minor = parse_app_version(ap)
        if major is not None:
            self.version_label.configure(
                text=f"APP 版本: v{major}.{minor}", fg=C_PRIMARY)
        else:
            self.version_label.configure(
                text="文件名未包含版本号 (IL_800_XXX.XXX.bin)", fg=C_LOG_WARN)

    def _save_config(self):
        """保存文件路径到本地配置，下次启动自动加载"""
        try:
            cfg = {
                "bt_hex": self.t_bt.get().strip(),
                "app_bin": self.t_app.get().strip(),
                "outdir": self.t_outdir.get().strip(),
            }
            with open(self._config_path, "w", encoding="utf-8") as f:
                json.dump(cfg, f)
        except Exception:
            pass

    def _auto_load_config(self):
        """启动时自动恢复上次选择的文件路径"""
        try:
            if not os.path.exists(self._config_path):
                return
            with open(self._config_path, encoding="utf-8") as f:
                cfg = json.load(f)

            for attr, key in [(self.t_bt, "bt_hex"),
                              (self.t_app, "app_bin"),
                              (self.t_outdir, "outdir")]:
                val = cfg.get(key, "")
                if val:
                    attr.delete(0, tk.END)
                    attr.insert(0, val)

            # 恢复版本号显示
            self._show_version()
        except Exception:
            pass

    def _verify_output(self, bt_data, app_data, output_path):
        """严格校验合并输出镜像。

        校验项:
          1. 文件大小 == 0x20000 + APP 大小
          2. offset 0        : BT 数据逐字节一致
          3. offset 0x20000  : APP 数据[0:2] + 大小字段(0x20002) + APP数据[6:] 一致
          4. 间隙 (BT末..0x20000): 全部 0xFF
          5. 无重叠 (BT 未超 0x20000)
        """
        try:
            with open(output_path, "rb") as f:
                out = f.read()

            # 1. 大小
            expect_size = APP_OFFSET + len(app_data)
            if len(out) != expect_size:
                _append(self.log, f"  [FAIL] 大小 {len(out)} != {expect_size}", "err")
                return False

            # 5. BT 不超 APP 偏移
            if len(bt_data) > APP_OFFSET:
                _append(self.log, f"  [FAIL] BT 数据超 0x{APP_OFFSET:05X}", "err")
                return False

            # 2. BT 数据一致
            if out[0:len(bt_data)] != bt_data:
                _append(self.log, "  [FAIL] BT 数据不一致", "err")
                return False
            _append(self.log, f"  [OK] BT 数据 ({len(bt_data):,} B) 一致", "ok")

            # 4. 间隙全 0xFF
            gap = out[len(bt_data):APP_OFFSET]
            if any(b != FILL_BYTE for b in gap):
                n_bad = sum(1 for b in gap if b != FILL_BYTE)
                _append(self.log, f"  [FAIL] 间隙 {len(gap):,} B 中有 {n_bad} 个非 0x{FILL_BYTE:02X}", "err")
                return False
            _append(self.log, f"  [OK] 间隙 ({len(gap):,} B) 全 0x{FILL_BYTE:02X}", "ok")

            # 3. APP 数据一致 (考虑 0x20002 处大小字段被补丁)
            #    布局: [0:2]=APP[0:2] [2:6]=size_LE [6:]=APP[6:]
            if len(app_data) >= 6:
                if out[APP_OFFSET:APP_OFFSET + 2] != app_data[0:2]:
                    _append(self.log, "  [FAIL] APP 头部[0:2] 不一致", "err")
                    return False
                size_le = out[APP_SIZE_OFF:APP_SIZE_OFF + 4]
                size_val = int.from_bytes(size_le, "little")
                if size_val != len(app_data):
                    _append(self.log, f"  [FAIL] APP 大小字段 {size_val} != {len(app_data)}", "err")
                    return False
                app_tail = out[APP_OFFSET + 6:APP_OFFSET + len(app_data)]
                if app_tail != app_data[6:]:
                    _append(self.log, "  [FAIL] APP 数据[6:] 不一致", "err")
                    return False
                _append(self.log, f"  [OK] APP 数据 ({len(app_data):,} B) 一致 (大小字段 {size_val:,} B)", "ok")
            else:
                if out[APP_OFFSET:APP_OFFSET + len(app_data)] != app_data:
                    _append(self.log, "  [FAIL] APP 数据不一致", "err")
                    return False
                _append(self.log, f"  [OK] APP 数据 ({len(app_data):,} B) 一致", "ok")

            _append(self.log, "  全部校验通过", "ok")
            return True
        except Exception as e:
            _append(self.log, f"  [FAIL] 校验异常: {e}", "err")
            return False

    def _run_merge(self):
        bt = self.t_bt.get().strip()
        ap = self.t_app.get().strip()
        outdir = self.t_outdir.get().strip()

        if not bt or not ap:
            messagebox.showwarning("缺少输入", "请选择 BT HEX 和 APP BIN 文件")
            return
        if not os.path.exists(bt):
            messagebox.showerror("文件不存在", bt)
            return
        if not os.path.exists(ap):
            messagebox.showerror("文件不存在", ap)
            return
        if not outdir:
            outdir = os.path.dirname(bt)
            self.t_outdir.delete(0, tk.END)
            self.t_outdir.insert(0, outdir)
            self._save_config()
        if not os.path.isdir(outdir):
            messagebox.showerror("目录不存在", outdir)
            return

        self.btn_merge.configure(state=tk.DISABLED, text="处理中...")
        _clear(self.log)
        threading.Thread(target=self._do_merge, args=(bt, ap, outdir), daemon=True).start()

    def _do_merge(self, bt, ap, outdir):
        try:
            # ---- Step 1: BT HEX → BIN ----
            _append(self.log, f"BT HEX: {bt}", "dim")
            start_addr, bt_data = hex_to_binary(bt)
            _append(self.log, f"  HEX 起始地址: 0x{start_addr:08X}", "dim")
            _append(self.log, f"  BT 数据大小: {len(bt_data):,} B ({fmt_size(len(bt_data))})", "ok")

            # BT 不能超过 APP 偏移
            if len(bt_data) > APP_OFFSET:
                _append(self.log, f"[ERROR] BT 数据超过 0x{APP_OFFSET:05X} (128KB), 无法合并", "err")
                return

            # ---- Step 2: 读取 APP BIN ----
            _append(self.log, f"APP BIN: {ap}", "dim")
            with open(ap, "rb") as f:
                app_data = f.read()
            _append(self.log, f"  APP 数据大小: {len(app_data):,} B ({fmt_size(len(app_data))})", "ok")

            # 解析版本
            major, minor = parse_app_version(ap)
            if major is not None:
                _append(self.log, f"  APP 版本: v{major}.{minor}", "ok")

            # ---- Step 3: 合并 ----
            _append(self.log, f"合并布局:", "dim")
            _append(self.log, f"  0x00000  BT 数据 ({len(bt_data):,} B)", "dim")
            _append(self.log, f"  0x{APP_OFFSET:05X}  APP 数据 ({len(app_data):,} B)", "dim")
            _append(self.log, f"  间隙填充: 0x{FILL_BYTE:02X}", "dim")

            total_size = APP_OFFSET + len(app_data)
            buffer = bytearray([FILL_BYTE] * total_size)

            # BT 数据从 0 偏移
            buffer[0:len(bt_data)] = bt_data

            # APP 数据从 0x20000 偏移
            buffer[APP_OFFSET:APP_OFFSET + len(app_data)] = app_data

            # 在 0x20002 写入 APP 文件大小 (4字节小端)
            if len(app_data) >= 6:
                size_le = len(app_data).to_bytes(4, "little")
                buffer[APP_SIZE_OFF:APP_SIZE_OFF + 4] = size_le
                _append(self.log,
                        f"  0x{APP_SIZE_OFF:05X}  APP 大小字段: {len(app_data):,} B (LE)",
                        "info")
            else:
                _append(self.log,
                        f"  [WARN] APP 文件 < 6 字节, 无法写入大小字段", "warn")

            # ---- Step 4: 输出 ----
            output = os.path.join(outdir, OUTPUT_NAME)
            with open(output, "wb") as f:
                f.write(buffer)

            _append(self.log, f"  -> {output}", "ok")
            _append(self.log, f"  文件大小: {len(buffer):,} B ({fmt_size(len(buffer))})", "info")
            _append(self.log, "", "dim")

            # ---- Step 5: 严格校验 ----
            _append(self.log, "校验镜像...", "dim")
            ok = self._verify_output(bt_data, app_data, output)
            if ok:
                _append(self.log, f"合并完成: {OUTPUT_NAME} (校验通过)", "ok")
            else:
                _append(self.log, "校验失败: 输出镜像与源数据不一致!", "err")

        except Exception as e:
            _append(self.log, f"[ERROR] {e}", "err")
        finally:
            self.root.after(0, lambda: self.btn_merge.configure(
                state=tk.NORMAL, text="合并生成 IL800_FD.BIN"))

def main():
    root = tk.Tk()
    App(root)
    root.mainloop()

if __name__ == "__main__":
    main()
