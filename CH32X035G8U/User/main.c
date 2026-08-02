/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : CableTester Team
 * Version            : V3.0
 * Date               : 2026/05/01
 * Description        : 主程序入口（V3.0 模块化版本）
 *
 * 系统工作流程：
 * - 主循环周期：1ms（通过SysTick中断维护时间戳）
 * - 测试触发周期：1秒（自动触发）
 * - 电阻测量：非阻塞状态机，分阶段执行
 * - RC滤波稳定时间：5ms（状态机等待）
 * - 占空比：约5.5%（55ms/1s），降低功耗
 * - 优势：无阻塞延时，按键响应及时，系统实时性好
 *********************************************************************************/

#include "ch32x035.h"
#include "debug.h"
#include "cable_gpio.h"
#include "cable_adc.h"
#include "cable_spi.h"
#include "cable_test.h"

/**
 * @brief  SysTick配置函数（RISC-V CH32X035）
 * @param  ticks: SysTick重装载值
 * @return 0:成功, 1:失败
 */
static uint32_t SysTick_Config(uint32_t ticks)
{
    if ((ticks - 1UL) > 0xFFFFFFFFUL) {
        return 1UL; /* Reload value impossible */
    }

    /* 清除状态寄存器 */
    SysTick->SR = 0;

    /* 清零计数器 */
    SysTick->CNT = 0;

    /* 设置比较值（重装载值） */
    SysTick->CMP = ticks - 1UL;

    /* 配置SysTick：使能中断、自动重装载、启动计数器 */
    /* CTLR位定义：
     * Bit 0: STE (SysTick Timer Enable)
     * Bit 1: STIE (SysTick Timer Interrupt Enable)
     * Bit 2: STCLK (计数器时钟源选择位)
     * Bit 3: STRE (SysTick Timer Auto Reload Enable)
     */
    SysTick->CTLR = 0xF; /* 使能STE + STIE + STRE, HCLK 做时基 */

    /* 使能SysTick中断 */
    NVIC_SetPriority(SysTicK_IRQn, 15);
    NVIC_EnableIRQ(SysTicK_IRQn);

    return 0UL; /* Success */
}

/**
 * @brief  系统初始化
 * @return none
 */
void System_Init(void)
{
    // 中断优先级配置
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    // 系统时钟更新
    SystemCoreClockUpdate();

    // 延时函数初始化
    Delay_Init();

    // 配置SysTick定时器（1ms中断）
    SysTick_Config(SystemCoreClock / 1000);

    // GPIO初始化
    GPIO_Configuration();

    // ADC初始化
    ADC_Configuration();

    // SPI初始化
    SPI_Configuration();

    // TFT初始化
    TFT_Init();

    // 测试模块初始化
    Cable_Test_Init();
}

/**
 * @brief  主函数
 * @return none
 */
int main(void)
{
    // 系统初始化
    System_Init();

    // 显示欢迎界面
    TFT_ShowWelcome();

    // 主循环（无阻塞）
    while (1) {
        // 按键扫描（非阻塞，带消抖）
        if (Key_Scan_With_Debounce()) {
            // 切换显示模式
            Switch_Display_Mode();
        }

        // 执行测试状态机（非阻塞）
        Test_State_Machine();

        // 短暂延时，降低CPU占用
        Delay_Ms(1);
    }
}
