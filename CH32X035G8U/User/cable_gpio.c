/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_gpio.c
 * Author             : CableTester Team
 * Version            : V3.0
 * Date               : 2026/05/17
 * Description        : GPIO底层驱动实现（仅包含初始化和基础控制）
 *
 * 模块说明：
 * - GPIO初始化配置
 * - 测试电压使能控制
 * - 按键状态读取
 * - CC_E1/CC_E2控制（通断测试、华为快充检测）
 *********************************************************************************/

#include "cable_gpio.h"
#include "debug.h"

/**
 * @brief  GPIO初始化配置
 * @return none
 */
void GPIO_Configuration(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);

    /* Type-C接口1 (USB3) 信号线配置 */
    // PC14/PC15 - USBPD硬件模块引脚，由USBPD模块自动控制，配置为浮空输入
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // PC3/PC0 - VCONN辅助供电，初始为浮空输入
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_AUX_PIN | USB3_CC2_AUX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // D+, TX, RX, SBU - 上拉输入，检测信号线通断
    GPIO_InitStructure.GPIO_Pin = USB3_DP_PIN | USB3_TX1_PIN | USB3_TX2_PIN |
                                  USB3_RX1_PIN | USB3_RX2_PIN | USB3_SBU_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* Type-C接口2 (USB4) 信号线配置 */
    // CC_E1 (PA2) 和 CC_E2 (PA1) - 浮空输入，5.1kΩ耦合
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN | USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // EH (PA0) - 上拉输入，用于屏蔽层检测
    GPIO_InitStructure.GPIO_Pin = USB4_EH_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* TFT显示屏控制线配置 */
    // CS, RS - 推挽输出（SCK和MOSI由硬件SPI控制）
    GPIO_InitStructure.GPIO_Pin = TFT_CS_PIN | TFT_RS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_SetBits(GPIOA, TFT_CS_PIN | TFT_RS_PIN);

    // PA5(SCK)和PA7(MOSI)在SPI_Configuration中配置为复用功能

    /* 电源控制配置 */
    // V_PWR_EN (PB6) - 推挽输出，初始关闭（PMOS高电平关闭）
    GPIO_InitStructure.GPIO_Pin = V_PWR_EN_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    GPIO_SetBits(GPIOB, V_PWR_EN_PIN);

    // V_PWR_L (PB0) 和 V_PWR_H (PB1) - 模拟输入，用于ADC
    GPIO_InitStructure.GPIO_Pin = V_PWR_L_PIN | V_PWR_H_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* 按键配置 */
    // KEY (PA3) - 上拉输入（低电平有效）
    GPIO_InitStructure.GPIO_Pin = KEY_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

void Set_Test_Voltage_Enable(uint8_t enable)
{
    if (enable) {
        GPIO_ResetBits(GPIOB, V_PWR_EN_PIN);
    } else {
        GPIO_SetBits(GPIOB, V_PWR_EN_PIN);
    }
}

uint8_t Read_Key_State(void)
{
    return GPIO_ReadInputDataBit(GPIOA, KEY_PIN) == Bit_RESET ? 1 : 0;
}

signal_status_t Test_Signal_Connection(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    if (GPIO_ReadInputDataBit(GPIOx, GPIO_Pin) == Bit_RESET) {
        return SIGNAL_CONNECTED;
    }
    return SIGNAL_OPEN;
}

/**
 * @brief  CC_E1/CC_E2推挽输出低（CC通断测试用）
 * @note   CC1(PA2)/CC2(PA1)均拉到GND，左口CC读电平判断连通性
 */
void CC_E_Set_Output_Low(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    // PA1/PA2推挽输出低
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN | USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOA, USB4_CC_E1_PIN | USB4_CC_E2_PIN);
}

/**
 * @brief  CC_E1/CC_E2恢复浮空输入
 */
void CC_E_Set_Floating(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN | USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

/**
 * @brief  华为快充检测：PA2推挽输出高，PA1配置为ADC输入
 * @note   利用PA1↔PA2间5.1kΩ自然形成上拉，分压测华为56k
 */
void CC_E_FastCharge_Start(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    // PA2推挽输出高（上拉源）
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_SetBits(GPIOA, USB4_CC_E1_PIN);

    // PA1模拟输入（ADC测分压）
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

/**
 * @brief  华为快充检测后恢复浮空
 */
void CC_E_FastCharge_Stop(void)
{
    CC_E_Set_Floating();
}
