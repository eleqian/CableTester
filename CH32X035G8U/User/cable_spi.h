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

/* SPI引脚定义 (使用硬件SPI1) */
// TFT_CS   - PA4 (软件控制片选)
// TFT_SCK  - PA5 (SPI1 SCK)
// TFT_MOSI - PA7 (SPI1 MOSI)
// TFT_RS   - PA6 (软件控制RS/DC)
// TFT_RST  - PA3 (软件控制复位)

/* 注意：TFT控制宏已在cable_gpio.h中定义 */

/* 函数声明 */
void SPI_Configuration(void);
void SPI_WriteByte(uint8_t data);
void SPI_Write_Buffer(const uint8_t *data, uint32_t len);  // 批量写入
void TFT_Write_Command(uint8_t cmd);
void TFT_Write_Data(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_SPI_H */
