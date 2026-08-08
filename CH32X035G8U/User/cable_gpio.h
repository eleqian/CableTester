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

/*
 * GPIO引脚定义（Pin Definitions）
 * 按功能分组：左口信号线 → 右口信号线 → TFT → 电源 → 按键
 */

/* === Type-C接口1 (USB3 - 左侧测试口) 信号线 === */
#define USB3_CC1_PIN      GPIO_Pin_14   /* PC14 - USBPD硬件模块CC1引脚（PD通信）      */
#define USB3_CC1_AUX_PIN  GPIO_Pin_3    /* PC3  - VCONN辅助供电（非PD通信时使用）      */
#define USB3_CC2_PIN      GPIO_Pin_15   /* PC15 - USBPD硬件模块CC2引脚（PD通信）      */
#define USB3_CC2_AUX_PIN  GPIO_Pin_0    /* PC0  - VCONN辅助供电（非PD通信时使用）      */
#define USB3_DP_PIN       GPIO_Pin_10   /* PB10 - T_D网络，D+信号，右侧D+/D-短接环回  */
#define USB3_TX1_PIN      GPIO_Pin_12   /* PB12 - TX1发送正，负端接地                  */
#define USB3_TX2_PIN      GPIO_Pin_9    /* PB9  - TX2发送正，负端接地                  */
#define USB3_RX1_PIN      GPIO_Pin_11   /* PB11 - RX1接收正，负端接地                  */
#define USB3_RX2_PIN      GPIO_Pin_8    /* PB8  - RX2接收正，负端接地                  */
#define USB3_SBU_PIN      GPIO_Pin_7    /* PB7  - SBU（Sideband Use），右侧SBU短接     */

/* === Type-C接口2 (USB4 - 右侧测试口) 信号线 === */
/*
 * CC_E1 / CC_E2（右口CC引脚）：
 * PA2(CC_E1) 与 PA1(CC_E2) 之间经 5.1kΩ 电阻耦合。
 * E-Marker PD通信期间：通信侧脚保持浮空，另一脚推挽输出低，
 * 经5.1kΩ对GND形成Rd（通信线直流偏置，见emarker_pd.c EMarker_Enable_Rd_Pulldown）。
 * 通断测试时推挽输出低（合并检测）；华为快充检测时PA2=高、PA1=ADC。
 */
#define USB4_CC_E1_PIN    GPIO_Pin_2    /* PA2 - 右口CC1          */
#define USB4_CC_E2_PIN    GPIO_Pin_1    /* PA1 - 右口CC2          */
#define USB4_EH_PIN       GPIO_Pin_0    /* PA0 - 屏蔽层(EH)检测   */

/* === TFT显示屏控制线（硬件SPI1）=== */
#define TFT_CS_PIN        GPIO_Pin_4    /* PA4 - 片选（软件控制）         */
#define TFT_SCK_PIN       GPIO_Pin_5    /* PA5 - SPI1时钟（硬件SCK）      */
#define TFT_RS_PIN        GPIO_Pin_6    /* PA6 - 数据/命令选择（软件控制）*/
#define TFT_MOSI_PIN      GPIO_Pin_7    /* PA7 - SPI1数据（硬件MOSI）     */
/* RST：硬件RC电路复位，无软件控制引脚 */

/* === 电源控制 === */
/*
 * V_PWR_EN (PB6)：PMOS栅极控制，低电平使能测试电压。
 * V_PWR_L (PB0)：经101倍运放同相放大后输入ADC（0~900mΩ高精度量程）。
 * V_PWR_H (PB1)：未经放大的VBUS直接ADC输入（900mΩ~9.99kΩ量程）。
 * 两通道自动切换，阈值3686(≈900mΩ)，带回差33(≈800mΩ)。
 */
#define V_PWR_EN_PIN      GPIO_Pin_6    /* PB6 - PMOS使能                     */
#define V_PWR_L_PIN       GPIO_Pin_0    /* PB0 - 放大通道ADC输入（ADC_Ch8）   */
#define V_PWR_H_PIN       GPIO_Pin_1    /* PB1 - 未放大通道ADC输入（ADC_Ch9） */

/* === 按键 === */
#define KEY_PIN           GPIO_Pin_3    /* PA3 - 显示模式切换（上拉输入，低电平有效） */

/* TFT控制宏 */
#define TFT_CS_LOW()      GPIO_ResetBits(GPIOA, TFT_CS_PIN)
#define TFT_CS_HIGH()     GPIO_SetBits(GPIOA, TFT_CS_PIN)
#define TFT_RS_LOW()      GPIO_ResetBits(GPIOA, TFT_RS_PIN)
#define TFT_RS_HIGH()     GPIO_SetBits(GPIOA, TFT_RS_PIN)

/* 信号线测试状态枚举 */
typedef enum {
    SIGNAL_OPEN = 0,      /* 开路：上拉输入读到高电平 */
    SIGNAL_CONNECTED = 1, /* 连通：上拉输入被拉到低电平 */
    SIGNAL_SHORT = 2      /* 短接（预留） */
} signal_status_t;

/* 函数声明 */
void GPIO_Configuration(void);
void Set_Test_Voltage_Enable(uint8_t enable);
uint8_t Read_Key_State(void);
signal_status_t Test_Signal_Connection(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin);

/* CC_E1/CC_E2控制函数 */
void CC_E_Set_Output_Low(void);     /* PA1/PA2推挽输出低 → CC通断合并检测 */
void CC_E_Set_Floating(void);       /* PA1/PA2恢复浮空 → 测试间释放总线 */
void CC_E_FastCharge_Start(void);   /* PA2推挽高 + PA1 ADC → 5.1k自然上拉测华为分压 */
void CC_E_FastCharge_Stop(void);    /* 快充检测后恢复浮空 */

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_GPIO_H */
