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

/* SPI 等待超时上限：约 10ms（48MHz 下每次循环约十余个周期）。
 * 原实现是无超时死等，一旦 SPI 状态异常（BSY 不清）整个主循环会被卡死：
 * 屏幕停在最后一帧、按键与插拔检测全部失效，但 SysTick 仍在走 —— 
 * 表现为「界面没反应、芯片却没死」。改成有界等待后最多丢字节，不会停摆。 */
#define SPI_WAIT_TIMEOUT   50000u

void SPI_Wait_TXE(void)
{
    uint32_t to = SPI_WAIT_TIMEOUT;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET && --to) {
    }
}

void SPI_Wait_Idle(void)
{
    uint32_t to = SPI_WAIT_TIMEOUT;
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET && --to) {
    }
}

/**
 * @brief  SPI写入一个字节 (硬件SPI)
 * @param  data: 要写入的数据
 * @return none
 */
void SPI_WriteByte(uint8_t data)
{
    SPI_Wait_TXE();            /* 有界等待，避免异常时死等 */

    // 发送数据
    SPI_I2S_SendData(SPI1, data);

    SPI_Wait_Idle();           /* 有界等待发送完成 */
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
        SPI_Wait_TXE();        /* 有界等待，避免异常时死等 */

        // 发送数据（不等待完成，继续发送下一个）
        SPI_I2S_SendData(SPI1, data[i]);
    }

    SPI_Wait_Idle();           /* 有界等待发送完成 */
}
