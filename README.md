# 固件下载器 (Firmware Downloader)

基于 STM32F103VET6 + UCOS-III 的固件下载设备，支持 SD 卡和内部 Flash 双数据源，通过串口协议向目标设备下发固件。

## 硬件

| 项目 | 规格 |
|------|------|
| MCU | STM32F103VET6 (Cortex-M3, 512KB Flash, 64KB RAM, 72MHz) |
| RTOS | UCOS-III (Micrium) |
| 外设库 | STM32F10x SPL |
| IDE | Keil MDK-ARM V5 |

## 功能

- **双数据源**：SD 卡（APP.bin） / MCU 内部 Flash（0x08020000）
- **双固件类型**：普通固件 / 出厂固件
- **双按键操作**：切换数据源 / 启动下载+切换固件类型
- **声光指示**：4 路 LED + 蜂鸣器（5种鸣响模式）
- **传输锁定**：下载期间禁止误操作
- **看门狗**：IWDG 4s 超时自动复位
- **FatFs R0.15**：SD 卡 FAT16/FAT32 只读文件系统
- **断点续传**：地址错误/写入失败自动从期望地址重发

## 目录结构

```
├── app/         应用层（任务、状态机、协议、CRC32、SD封装）
├── bsp/         板级支持包（GPIO/USART/SPI/IWDG）
├── Libraries/
│   ├── CMSIS/   Cortex-M3 核心
│   ├── FWlib/   STM32F10x 标准外设库
│   ├── FatFs/   FAT 文件系统 + UCOS-III 重入保护
│   └── mbedtls/ 加密库
├── ucos-iii/    UCOS-III 内核 + Cortex-M3 移植
├── uc-cpu/      μC/CPU 抽象层
├── uc-lib/      μC/LIB 工具库
├── RVMDK/       Keil 工程文件
├── scatter/     链接脚本
├── Tests/       Python 单元测试（CRC32 / 协议 / FAT）
└── docs/        文档
```

## 快速开始

1. 用 Keil MDK V5 打开 `RVMDK/downloader.uvprojx`
2. 编译（ARM Compiler 5）
3. 烧录到 STM32F103VET6 目标板
4. USART2 (PD5/PD6, 115200-8N1) 输出调试信息

## 文档

- [开发者文档](docs/开发者文档.md)
- [设计文档](docs/下载器设计文档_合并版.docx)

## 测试

Python 单元测试（纯逻辑模块）：

```bash
cd Tests
python test_crc32.py     # CRC32 算法
python test_protocol.py  # 协议帧编解码
```
