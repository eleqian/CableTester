/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_adc.h
 * Author             : CableTester Team
 * Version            : V2.1
 * Date               : 2026/05/17
 * Description        : ADC底层驱动头文件（仅包含初始化和基础读取）
 *********************************************************************************/

#ifndef __CABLE_ADC_H
#define __CABLE_ADC_H

#include "ch32x035.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ADC通道定义 */
#define ADC_CHANNEL_V_PWR_L   ADC_Channel_8  // PB0 - 运放放大通道
#define ADC_CHANNEL_V_PWR_H   ADC_Channel_9  // PB1 - 未放大通道
#define ADC_CHANNEL_CC_E2     ADC_Channel_1  // PA1 - CC_E2电压（华为充电线检测）

/* ADC分辨率 */
#define ADC_RESOLUTION 4095

/* 挡位切换阈值 */
#define ADC_SWITCH_THRESHOLD  3686   // V_PWR_L adc > 此值切至V_PWR_H
#define ADC_SWITCH_HYST_LOW   33     // V_PWR_H adc < 此值切回V_PWR_L（对应~800mΩ）

void ADC_Configuration(void);
uint16_t ADC_Read_Channel(uint8_t channel);
uint16_t ADC_Average_Read(uint8_t channel, uint8_t times);

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_ADC_H */
