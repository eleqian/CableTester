/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_spi.h
 * Author             : CableTester Team
 * Version            : V1.0.0
 * Date               : 2026/04/26
 * Description        : SPI configuration header file for CableTester (TFT display)
*********************************************************************************/

#ifndef __CABLE_SPI_H
#define __CABLE_SPI_H

#include "ch32x035.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 函数声明 */
void SPI_Configuration(void);
void SPI_WriteByte(uint8_t data);
void SPI_Write_Buffer(const uint8_t *data, uint32_t len);  // 批量写入

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_SPI_H */
