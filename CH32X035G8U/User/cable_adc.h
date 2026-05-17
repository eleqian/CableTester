/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_adc.h
 * Author             : CableTester Team
 * Version            : V2.0
 * Date               : 2026/05/01
 * Description        : ADC底层驱动头文件（仅包含初始化和基础读取）
 *********************************************************************************/

#ifndef __CABLE_ADC_H
#define __CABLE_ADC_H

#include "ch32x035.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ADC通道定义 */
#define ADC_CHANNEL_V_PWR ADC_Channel_8  // PB0 - 电压检测

/* ADC分辨率 */
#define ADC_RESOLUTION 4095  // 12位ADC

/* 函数声明 - 底层驱动接口 */
void ADC_Configuration(void);                               // ADC初始化
uint16_t ADC_Read_Channel(uint8_t channel);                 // 读取单通道ADC值
uint16_t ADC_Average_Read(uint8_t channel, uint8_t times);  // 多次采样取平均

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_ADC_H */
