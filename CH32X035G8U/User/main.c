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
#include "cable_tft.h"
#include "emarker_pd.h"

/* 主循环最大间隔(ms)：衡量最坏情况下主循环被事务占住多久，
 * 直接对应按键/插拔检测的最大延迟。WCH-Link + GDB 可直接回读。
 * 由 CABLE_DEBUG_TIMING 控制，默认关闭（不占 RAM、不做统计）。 */
#if CABLE_DEBUG_TIMING
volatile uint32_t g_loop_max_gap = 0;
volatile uint32_t g_loop_max_gap_mark = 0;
#endif

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

    /* 上电后不再做全量 E-Mark 扫描：正式流程由状态机按需读取。
     * 需要诊断时把 emarker_pd.h 的 EMARK_DEBUG_DIAG 置 1，这里会自动执行一次扫描，
     * 并把结果固化到 g_emark_diag 供 GDB 回读。 */
#if EMARK_DEBUG_DIAG
    TFT_DrawString_Font(4, 20, "E-Mark scan...", COLOR_CYAN, COLOR_BLACK, FONT_8x16);
    EMarker_Debug_Scan();
    TFT_Clear(COLOR_BLACK);
#endif

    // 主循环（无阻塞）
    {
#if CABLE_DEBUG_TIMING
        uint32_t prev_tick = system_tick_ms;
#endif
        while (1) {
            // 按键扫描（非阻塞，带消抖）
            if (Key_Scan_With_Debounce()) {
                // 切换显示模式
                Switch_Display_Mode();
            }

            // 执行测试状态机（非阻塞）
            Test_State_Machine();

#if CABLE_DEBUG_TIMING
            /* 记录本轮循环耗时（含被 PD 事务占住的时间） */
            {
                uint32_t now = system_tick_ms;
                uint32_t gap = now - prev_tick;
                if (gap > g_loop_max_gap) {
                    g_loop_max_gap = gap;
                    g_loop_max_gap_mark = now;
                }
                prev_tick = now;
            }
#endif

            // 短暂延时，降低CPU占用
            Delay_Ms(1);
        }
    }
}
