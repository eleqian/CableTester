/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_spi.c
 * Author             : CableTester Team
 * Version            : V1.0.0
 * Date               : 2026/04/26
 * Description        : SPI configuration implementation for CableTester (TFT display using Hardware SPI)
*********************************************************************************/

#include "cable_spi.h"
#include "cable_gpio.h"
#include "debug.h"

/**
 * @brief  SPI初始化配置 (硬件SPI1)
 * @return none
 */
void SPI_Configuration(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    SPI_InitTypeDef  SPI_InitStructure = {0};

    // 使能SPI1和GPIOA时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1 | RCC_APB2Periph_GPIOA, ENABLE);

    /* 配置SPI1引脚: PA5(SCK), PA7(MOSI) */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;  // 复用推挽输出
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* CS, RS, RST引脚已在cable_gpio.c中初始化为普通GPIO */

    /* SPI1配置 */
    SPI_InitStructure.SPI_Direction = SPI_Direction_1Line_Tx;  // 单向发送模式（只写TFT）
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;              // 主机模式
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;          // 8位数据
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;                // 空闲时SCK为高电平
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;               // 第二个边沿采样
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;                  // 软件NSS
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;  // 4分频 (48MHz/4=12MHz)
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;         // MSB优先
    SPI_InitStructure.SPI_CRCPolynomial = 7;                   // CRC多项式（不使用CRC）
    SPI_Init(SPI1, &SPI_InitStructure);

    /* 使能SPI1 */
    SPI_Cmd(SPI1, ENABLE);

    /* 初始状态设置 */
    TFT_CS_HIGH();
    TFT_RS_HIGH();
}

/**
 * @brief  SPI写入一个字节 (硬件SPI)
 * @param  data: 要写入的数据
 * @return none
 */
void SPI_WriteByte(uint8_t data)
{
    // 等待发送缓冲区为空
    while(SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);

    // 发送数据
    SPI_I2S_SendData(SPI1, data);

    // 等待发送完成
    while(SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);
}

/**
 * @brief  SPI批量写入 (优化版本，减少等待时间)
 * @param  data: 数据缓冲区指针
 * @param  len: 数据长度
 * @return none
 * @note   连续发送时只在最后等待一次，提高效率
 */
void SPI_Write_Buffer(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    for(i = 0; i < len; i++) {
        // 等待发送缓冲区为空
        while(SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);

        // 发送数据（不等待完成，继续发送下一个）
        SPI_I2S_SendData(SPI1, data[i]);
    }

    // 最后等待所有数据发送完成
    while(SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);
}
