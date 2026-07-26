/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_gpio.c
 * Author             : CableTester Team
 * Version            : V2.0
 * Date               : 2026/05/01
 * Description        : GPIO底层驱动实现（仅包含初始化和基础控制）
 *
 * 模块说明：
 * - GPIO初始化配置
 * - 测试电压使能控制
 * - 按键状态读取
 * - 信号线连通性测试（底层接口）
 *********************************************************************************/

#include "cable_gpio.h"
#include "debug.h"

/**
 * @brief  GPIO初始化配置
 * @return none
 *
 * 引脚配置说明：
 * - CC1, CC2: 上拉输入（用于通断检测和E-mark检测）
 * - D+, TX, RX, SBU: 上拉输入（用于通断检测）
 * - CC辅助引脚: 浮空输入（用于VCONN供电时切换为输出）
 * - AUX_G: 浮空输入（VOOC快充检测）
 * - TFT控制线: 推挽输出
 * - V_PWR_EN: 推挽输出（PMOS控制，低电平使能）
 * - V_PWR_SENSE: 模拟输入（ADC采样）
 * - KEY: 上拉输入（低电平有效）
 */
void GPIO_Configuration(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    // 使能GPIO时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);

    /* Type-C接口1 (USB3) 信号线配置 */
    // CC1, CC2 (PC14/PC15) - USBPD硬件模块引脚，由USBPD模块自动控制
    // 配置PC14/PC15为浮空输入（USBPD硬件模块的CC1/CC2引脚）
    // 这些引脚由USBPD硬件模块自动控制，不需要软件切换方向
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // CC辅助引脚 (PC3/PC0) - 初始为浮空输入
    // 用途1: VCONN辅助供电时切换为输出高电平（并联加强驱动能力）
    // 用途2: ADC测量时配置为模拟输入
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_AUX_PIN | USB3_CC2_AUX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // D+, TX, RX, SBU - 上拉输入，检测信号线通断
    GPIO_InitStructure.GPIO_Pin = USB3_DP_PIN | USB3_TX1_PIN | USB3_TX2_PIN |
                                  USB3_RX1_PIN | USB3_RX2_PIN | USB3_SBU_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* USB-A接口 (USB2) 信号线配置 */
    // CC_R - 初始为浮空输入，后续可通过CC_R_Set_Pull_Mode()动态控制
    GPIO_InitStructure.GPIO_Pin = CC_R_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* Type-C接口2 (USB4) 信号线配置 */
    // CC_E - 浮空输入，不使用内置上下拉电阻，由外部5.1k电阻通过CC_R控制
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // EH - 上拉输入，用于屏蔽层检测（始终保持上拉输入模式）
    GPIO_InitStructure.GPIO_Pin = USB4_EH_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* TFT显示屏控制线配置 */
    // CS, RS, RST - 推挽输出 (SCK和MOSI由硬件SPI控制)
    GPIO_InitStructure.GPIO_Pin = TFT_CS_PIN | TFT_RS_PIN | TFT_RST_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_SetBits(GPIOA, TFT_CS_PIN | TFT_RS_PIN | TFT_RST_PIN);

    // 注意：PA5(SCK)和PA7(MOSI)在SPI_Configuration中配置为复用功能

    /* 电源控制配置 */
    // V_PWR_EN - 推挽输出，初始关闭（PMOS高电平关闭）
    GPIO_InitStructure.GPIO_Pin = V_PWR_EN_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    GPIO_SetBits(GPIOB, V_PWR_EN_PIN);  // PMOS初始关闭

    // V_PWR_SENSE - 模拟输入，用于ADC
    GPIO_InitStructure.GPIO_Pin = V_PWR_SENSE_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* 按键配置 */
    // KEY - 上拉输入（低电平有效）
    GPIO_InitStructure.GPIO_Pin = KEY_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

/**
 * @brief  使能/关闭测试电压
 * @param  enable: 1-使能, 0-关闭
 * @note   PMOS低电平使能，高电平关闭
 * @return none
 */
void Set_Test_Voltage_Enable(uint8_t enable)
{
    if (enable) {
        GPIO_ResetBits(GPIOB, V_PWR_EN_PIN);  // PMOS导通
    } else {
        GPIO_SetBits(GPIOB, V_PWR_EN_PIN);    // PMOS关闭
    }
}

/**
 * @brief  读取按键状态
 * @return 1-按下, 0-未按下
 */
uint8_t Read_Key_State(void)
{
    return GPIO_ReadInputDataBit(GPIOA, KEY_PIN) == Bit_RESET ? 1 : 0;
}

/**
 * @brief  测试信号线连通性（底层接口）
 * @param  GPIOx: GPIO端口
 * @param  GPIO_Pin: GPIO引脚
 * @return 信号状态（SIGNAL_CONNECTED或SIGNAL_OPEN）
 * @note   假设引脚已配置为上拉输入
 */
signal_status_t Test_Signal_Connection(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    uint8_t level;

    // 读取引脚电平
    level = GPIO_ReadInputDataBit(GPIOx, GPIO_Pin);

    // 低电平表示连通（被拉到GND），高电平表示开路
    if (level == Bit_RESET) {
        return SIGNAL_CONNECTED;
    } else {
        return SIGNAL_OPEN;
    }
}

/**
 * @brief  设置CC_R输出状态以控制CC_E外部电阻
 * @param  mode: CC_R控制模式 (CC_R_PULL_UP/CC_R_PULL_DOWN/CC_R_FLOATING)
 * @note   CC_R通过5.1k电阻连接到CC_E，控制CC_E的上拉/下拉
 * @return none
 */
void CC_R_Set_Pull_Mode(uint8_t mode)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    if (mode == CC_R_FLOATING) {
        // CC_R浮空输入，CC_E无外部电阻
        GPIO_InitStructure.GPIO_Pin = CC_R_PIN;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
        GPIO_Init(GPIOB, &GPIO_InitStructure);
    } else {
        // CC_R推挽输出，控制外部5.1k电阻
        GPIO_InitStructure.GPIO_Pin = CC_R_PIN;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(GPIOB, &GPIO_InitStructure);

        if (mode == CC_R_PULL_UP) {
            GPIO_SetBits(GPIOB, CC_R_PIN);   // 输出高电平，上拉CC_E
        } else {
            GPIO_ResetBits(GPIOB, CC_R_PIN); // 输出低电平，下拉CC_E
        }
    }
}
