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

/*
 * ADC通道定义
 * 双通道电阻测量：V_PWR_L（101倍放大）用于<900mΩ高精度，
 *                V_PWR_H（未放大）用于900mΩ~9.99kΩ。
 * CC_E2通道用于华为快充检测（PA1上分压测量）。
 */
#define ADC_CHANNEL_V_PWR_L   ADC_Channel_8  /* PB0 - 运放放大通道（0~900mΩ）    */
#define ADC_CHANNEL_V_PWR_H   ADC_Channel_9  /* PB1 - 未放大通道（900mΩ~9.99kΩ） */
#define ADC_CHANNEL_CC_E2     ADC_Channel_1  /* PA1 - CC_E2电压（华为快充检测）  */

#define ADC_RESOLUTION 4095  /* 12位ADC满量程 */

/*
 * 挡位切换阈值（基于ADC值而非电阻值，避免在线计算）
 * ADC_SWITCH_THRESHOLD = 3686  → 对应约900mΩ（V_PWR_L有效线性上限）
 * ADC_SWITCH_HYST_LOW  = 33    → 对应约800mΩ（回差，防止临界区频繁跳变）
 */
#define ADC_SWITCH_THRESHOLD  3686   /* V_PWR_L adc > 此值 → 切至V_PWR_H   */
#define ADC_SWITCH_HYST_LOW   33     /* V_PWR_H adc < 此值 → 回切V_PWR_L   */

void ADC_Configuration(void);
uint16_t ADC_Read_Channel(uint8_t channel);
uint16_t ADC_Average_Read(uint8_t channel, uint8_t times);

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_ADC_H */
