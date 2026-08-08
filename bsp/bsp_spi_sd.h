/* bsp/bsp_spi_sd.h */
#ifndef __BSP_SPI_SD_H
#define __BSP_SPI_SD_H

#include "stm32f10x.h"

#define SD_BLOCK_SIZE  512u

void    BSP_SPI2_Init(void);
void    SD_SPI_SetHighSpeed(void);
uint8_t SD_SPI_Init(void);
uint8_t SD_SPI_ReadBlock(uint32_t addr, uint8_t *buf);
uint8_t SD_SPI_WriteBlock(uint32_t addr, const uint8_t *buf);

/* 读 CSD 寄存器 (CMD9)，返回 16 字节 CSD 数据到 csd[16]。
   成功返回 0。用于查询卡容量等。 */
uint8_t SD_SPI_ReadCSD(uint8_t csd[16]);

#endif
