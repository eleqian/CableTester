/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_gpio.h
 * Author             : CableTester Team
 * Version            : V3.0
 * Date               : 2026/05/17
 * Description        : GPIO底层驱动头文件（仅包含初始化和基础控制）
 *********************************************************************************/

#ifndef __CABLE_GPIO_H
#define __CABLE_GPIO_H

#include "ch32x035.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO引脚定义 */
// Type-C接口1 (USB3) 信号线
#define USB3_CC1_PIN      GPIO_Pin_14   // PC14 - USBPD硬件模块CC1引脚（PD通信）
#define USB3_CC1_AUX_PIN  GPIO_Pin_3    // PC3  - VCONN辅助供电（非PD通信时使用）
#define USB3_CC2_PIN      GPIO_Pin_15   // PC15 - USBPD硬件模块CC2引脚（PD通信）
#define USB3_CC2_AUX_PIN  GPIO_Pin_0    // PC0  - VCONN辅助供电（非PD通信时使用）
#define USB3_DP_PIN       GPIO_Pin_10   // PB10 (T_D网络)
#define USB3_TX1_PIN      GPIO_Pin_12   // PB12
#define USB3_TX2_PIN      GPIO_Pin_9    // PB9
#define USB3_RX1_PIN      GPIO_Pin_11   // PB11
#define USB3_RX2_PIN      GPIO_Pin_8    // PB8
#define USB3_SBU_PIN      GPIO_Pin_7    // PB7

// Type-C接口2 (USB4) 信号线
#define USB4_CC_E1_PIN    GPIO_Pin_2    // PA2 - CC_E1（右侧接口CC1，经5.1kΩ连接CC_E2）
#define USB4_CC_E2_PIN    GPIO_Pin_1    // PA1 - CC_E2（右侧接口CC2，经5.1kΩ连接CC_E1）
#define USB4_EH_PIN       GPIO_Pin_0    // PA0

// TFT显示屏控制线
#define TFT_CS_PIN        GPIO_Pin_4    // PA4
#define TFT_SCK_PIN       GPIO_Pin_5    // PA5 (SPI1 SCK)
#define TFT_RS_PIN        GPIO_Pin_6    // PA6 (数据/命令选择)
#define TFT_MOSI_PIN      GPIO_Pin_7    // PA7 (SPI1 MOSI)

// 电源控制
#define V_PWR_EN_PIN      GPIO_Pin_6    // PB6 (测试电源使能，PMOS低电平使能)
#define V_PWR_L_PIN       GPIO_Pin_0    // PB0 (放大通道ADC输入)
#define V_PWR_H_PIN       GPIO_Pin_1    // PB1 (未放大通道ADC输入)

// 按键
#define KEY_PIN           GPIO_Pin_3    // PA3 (KEY_M按键，低电平有效)

/* TFT控制宏定义 */
#define TFT_CS_LOW()      GPIO_ResetBits(GPIOA, TFT_CS_PIN)
#define TFT_CS_HIGH()     GPIO_SetBits(GPIOA, TFT_CS_PIN)
#define TFT_RS_LOW()      GPIO_ResetBits(GPIOA, TFT_RS_PIN)
#define TFT_RS_HIGH()     GPIO_SetBits(GPIOA, TFT_RS_PIN)

/* 测试状态枚举 */
typedef enum {
    SIGNAL_OPEN = 0,
    SIGNAL_CONNECTED = 1,
    SIGNAL_SHORT = 2
} signal_status_t;

/* 函数声明 - 底层驱动接口 */
void GPIO_Configuration(void);
void Set_Test_Voltage_Enable(uint8_t enable);
uint8_t Read_Key_State(void);
signal_status_t Test_Signal_Connection(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin);

/* CC_E1/CC_E2 控制函数 */
void CC_E_Set_Output_Low(void);     // PA1/PA2推挽输出低（CC通断测试）
void CC_E_Set_Floating(void);       // PA1/PA2恢复浮空输入
void CC_E_FastCharge_Start(void);   // PA2推挽高、PA1 ADC（华为快充检测）
void CC_E_FastCharge_Stop(void);    // 快充检测后恢复浮空

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_GPIO_H */
