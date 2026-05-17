/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_adc.c
 * Author             : CableTester Team
 * Version            : V2.0
 * Date               : 2026/05/01
 * Description        : ADC底层驱动实现（仅包含初始化和基础读取）
 *
 * 模块说明：
 * - ADC1初始化配置
 * - ADC通道读取
 * - 多次采样取平均
 *********************************************************************************/

#include "cable_adc.h"
#include "debug.h"

/**
 * @brief  ADC初始化配置
 * @return none
 */
void ADC_Configuration(void)
{
    ADC_InitTypeDef ADC_InitStructure = {0};

    // 使能ADC时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    // ADC去初始化
    ADC_DeInit(ADC1);

    // 配置ADC时钟分频
    ADC_CLKConfig(ADC1, ADC_CLK_Div16);

    // ADC参数配置
    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode = DISABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel = 1;
    ADC_Init(ADC1, &ADC_InitStructure);

    // 使能ADC
    ADC_Cmd(ADC1, ENABLE);
}

/**
 * @brief  读取指定ADC通道值
 * @param  channel: ADC通道
 * @return ADC转换结果
 */
uint16_t ADC_Read_Channel(uint8_t channel)
{
    uint16_t val;

    // 配置规则通道 (使用最大采样时间11个周期)
    ADC_RegularChannelConfig(ADC1, channel, 1, ADC_SampleTime_11Cycles);

    // 启动软件转换
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);

    // 等待转换完成
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC));

    // 读取转换结果
    val = ADC_GetConversionValue(ADC1);

    return val;
}

/**
 * @brief  ADC多次采样取平均
 * @param  channel: ADC通道
 * @param  times: 采样次数
 * @return 平均ADC值
 */
uint16_t ADC_Average_Read(uint8_t channel, uint8_t times)
{
    uint32_t sum = 0;
    uint8_t i;

    for (i = 0; i < times; i++) {
        sum += ADC_Read_Channel(channel);
        Delay_Us(1000);
    }

    return (uint16_t)(sum / times);
}
