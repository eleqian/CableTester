/********************************** (C) COPYRIGHT  *******************************
 * File Name          : debug.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2025/10/29
 * Description        : This file contains all the functions prototypes for UART
 *                      Printf , Delay functions.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "debug.h"

#define DEBUG_DATA0_ADDRESS  ((volatile uint32_t*)0xE0000380)
#define DEBUG_DATA1_ADDRESS  ((volatile uint32_t*)0xE0000384)

/*********************************************************************
 * @fn      Delay_Init
 *
 * @brief   Delay 模块初始化（保留以兼容既有初始化顺序）
 *
 * @return  none
 *
 * @note    旧实现用 p_us/p_ms 做软件循环计数，现已改为读 SysTick->CNT，
 *          不再需要任何预计算；本函数留空以免改动调用点。
 */
void Delay_Init(void)
{
}

/*********************************************************************
 * @fn      Delay_Cycles_Per_Us
 *
 * @brief   取得「1us 对应的 SysTick 计数周期数」
 *
 * @return  每微秒的周期数（至少为 1）
 *
 * @note    优先用 SysTick 的重装载值反推（CMP+1 就是 1ms 的周期数），
 *          这样不依赖 SystemCoreClock 是否已 Update；SysTick 还没配置时
 *          才退回主频计算。
 */
static uint32_t Delay_Cycles_Per_Us(void)
{
    uint32_t reload = SysTick->CMP + 1u;

    if(reload > 1000u) {
        return reload / 1000u;
    }

    {
        uint32_t clk = SystemCoreClock / 1000000u;
        return (clk != 0u) ? clk : 1u;
    }
}

/*********************************************************************
 * @fn      Delay_Us
 *
 * @brief   Microsecond Delay Time (使用SysTick硬件计数器实现高精度延时).
 *
 * @param   n - Microsecond number.
 *
 * @return  None
 *
 * @note    SysTick 的 CNT 从 0 递增到 CMP 后自动归零并产生 1ms 中断。
 *          旧实现用 while(CNT >= start) 等回绕，有两个坑：
 *            1) start 恰好为 0 时条件恒真，直接返回，延时严重不足；
 *            2) 跨回绕时先白等接近 1ms，PD 时序窗口会被拖爆。
 *          现改为「读 CNT 差值并累加」，既不依赖相位、也不会白等：
 *          CNT 只增不减（归零由硬件完成），每圈把实际经过的周期数累加，
 *          累加到目标周期数即返回，跨回绕时补上 (CMP+1-last)+now。
 *          spin 上限仅用于防止 SysTick 停摆（例如调试器 halt）时死等。
 */
void Delay_Us(uint32_t n)
{
    uint32_t cycles, last, acc, spin;

    if(n == 0u) {
        return;
    }

    cycles = Delay_Cycles_Per_Us() * n;
    last   = SysTick->CNT;
    acc    = 0u;
    spin   = cycles + 1024u;   /* 每轮至少前进 1 个周期，正常远用不到 */

    while((acc < cycles) && (spin-- != 0u)) {
        uint32_t now = SysTick->CNT;

        if(now >= last) {
            acc += now - last;
        } else {
            /* 跨过 1ms 回绕：先补到 CMP，再补归零后的 now */
            acc += (SysTick->CMP + 1u - last) + now;
        }
        last = now;
    }
}

/*********************************************************************
 * @fn      Delay_Ms
 *
 * @brief   Millisecond Delay Time (使用SysTick硬件计数器实现高精度延时).
 *
 * @param   n - Millisecond number.
 *
 * @return  None
 *
 * @note    按 1ms 分段调用 Delay_Us(1000)，不再依赖 "CNT>=start 等回绕"，
 *          避免 start 为 0 时延时为 0 的问题。
 */
void Delay_Ms(uint32_t n)
{
    while(n-- != 0u) {
        Delay_Us(1000u);
    }
}

/*********************************************************************
 * @fn      USART_Printf_Init
 *
 * @brief   Initializes the USARTx peripheral.
 *
 * @param   baudrate - USART communication baud rate.
 *
 * @return  None
 */
void USART_Printf_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;

#if(DEBUG == DEBUG_UART1)
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1 | RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

#elif(DEBUG == DEBUG_UART2)
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

#elif(DEBUG == DEBUG_UART3)
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

#endif

    USART_InitStructure.USART_BaudRate = baudrate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx;

#if(DEBUG == DEBUG_UART1)
    USART_Init(USART1, &USART_InitStructure);
    USART_Cmd(USART1, ENABLE);

#elif(DEBUG == DEBUG_UART2)
    USART_Init(USART2, &USART_InitStructure);
    USART_Cmd(USART2, ENABLE);

#elif(DEBUG == DEBUG_UART3)
    USART_Init(USART3, &USART_InitStructure);
    USART_Cmd(USART3, ENABLE);

#endif
}

/*********************************************************************
 * @fn      SDI_Printf_Enable
 *
 * @brief   Initializes the SDI printf Function.
 *
 * @param   None
 *
 * @return  None
 */
void SDI_Printf_Enable(void)
{
    *(DEBUG_DATA0_ADDRESS) = 0;
    Delay_Init();
    Delay_Ms(1);
}

/*********************************************************************
 * @fn      _write
 *
 * @brief   Support Printf Function
 *
 * @param   *buf - UART send Data.
 *          size - Data length
 *
 * @return  size - Data length
 */
__attribute__((used))
int _write(int fd, char *buf, int size)
{
    int i = 0;

#if (SDI_PRINT == SDI_PR_OPEN)
    int writeSize = size;

    do
    {

        /**
         * data0  data1 8 byte
         * data0 The storage length of the lowest byte, with a maximum of 7 bytes.
         */

        while( (*(DEBUG_DATA0_ADDRESS) != 0u))
        {

        }

        if(writeSize>7)
        {
            *(DEBUG_DATA1_ADDRESS) = (*(buf+i+3)) | (*(buf+i+4)<<8) | (*(buf+i+5)<<16) | (*(buf+i+6)<<24);
            *(DEBUG_DATA0_ADDRESS) = (7u) | (*(buf+i)<<8) | (*(buf+i+1)<<16) | (*(buf+i+2)<<24);

            i += 7;
            writeSize -= 7;
        }
        else
        {
            *(DEBUG_DATA1_ADDRESS) = (*(buf+i+3)) | (*(buf+i+4)<<8) | (*(buf+i+5)<<16) | (*(buf+i+6)<<24);
            *(DEBUG_DATA0_ADDRESS) = (writeSize) | (*(buf+i)<<8) | (*(buf+i+1)<<16) | (*(buf+i+2)<<24);

            writeSize = 0;
        }

    } while (writeSize);


#else
    for(i = 0; i < size; i++){
#if(DEBUG == DEBUG_UART1)
        while(USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
        USART_SendData(USART1, *buf++);
#elif(DEBUG == DEBUG_UART2)
        while(USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
        USART_SendData(USART2, *buf++);
#elif(DEBUG == DEBUG_UART3)
        while(USART_GetFlagStatus(USART3, USART_FLAG_TC) == RESET);
        USART_SendData(USART3, *buf++);
#endif
    }
#endif
    return size;
}

/*********************************************************************
 * @fn      _sbrk
 *
 * @brief   Change the spatial position of data segment.
 *
 * @return  size - Data length
 */
__attribute__((used))
void *_sbrk(ptrdiff_t incr)
{
    extern char _end[];
    extern char _heap_end[];
    static char *curbrk = _end;

    if ((curbrk + incr < _end) || (curbrk + incr > _heap_end)) {
        return NULL - 1;
    }

    curbrk += incr;
    return curbrk - incr;
}
