/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_test.c
 * Author             : CableTester Team
 * Version            : V3.0
 * Date               : 2026/05/01
 * Description        : 线缆测试核心功能实现（包含状态机、按键处理和所有测试逻辑）
 *
 * 模块说明：
 * - 系统初始化：测试模块初始化
 * - 按键处理：带消抖的按键扫描和模式切换
 * - 状态机管理：测试流程控制和电阻测量状态机
 * - 通断测试：检测所有信号线的连通性
 * - 电阻测量：计算VBUS/GND回路电阻
 * - E-mark检测：识别E-mark芯片并控制VCONN
 * - 快充识别：检测VOOC/Huawei快充线缆
 * - 结果显示：TFT显示测试结果
 *********************************************************************************/

#include "cable_test.h"
#include "cable_gpio.h"
#include "cable_adc.h"
#include "cable_tft.h"
#include "emarker_pd.h"
#include "debug.h"

/* 参考电压 (mV) */
#define VREF_VOLTAGE_MV 3300  // 3.3V = 3300mV

/* INA199增益系数 (放大100倍) */
#define INA199_GAIN 100

/* 串联电阻值(毫欧) */
#define SERIES_RESISTOR 100000

/* 电阻测量最大值(毫欧) */
#define RES_CABLE_MAX (SERIES_RESISTOR / INA199_GAIN)

/******************************************************************************
 *                    全局变量
 ******************************************************************************/

/* 系统时间戳（ms）- 由SysTick中断更新 */
volatile uint32_t system_tick_ms = 0;

/* 测试结果和显示模式 */
test_result_t test_result = {0};
display_mode_t display_mode = MODE_CONTINUITY;

/******************************************************************************
 *                    内部状态机变量
 ******************************************************************************/

/* 测试状态机枚举 */
typedef enum {
    TEST_STATE_IDLE = 0,
    TEST_STATE_START,
    TEST_STATE_RUNNING,
    TEST_STATE_COMPLETE
} test_state_t;

/* 电阻测量状态机 */
typedef enum {
    RESISTANCE_STATE_IDLE = 0,
    RESISTANCE_STATE_MOS_ON,       // MOS开启，等待RC稳定
    RESISTANCE_STATE_STABILIZING,  // RC稳定中
    RESISTANCE_STATE_SAMPLING,     // ADC采样中
    RESISTANCE_STATE_MOS_OFF,      // MOS关闭
    RESISTANCE_STATE_COMPLETE      // 测量完成
} resistance_state_t;

/* 状态机变量 */
static test_state_t test_state = TEST_STATE_IDLE;
static resistance_state_t resistance_state = RESISTANCE_STATE_IDLE;

/* 时间戳变量 */
static uint32_t last_test_time = 0;    // 上次测试时间
static uint32_t state_start_time = 0;  // 状态开始时间
static uint32_t last_key_time = 0;     // 上次按键时间

/* 帧率计算变量 */
static uint32_t frame_count = 0;    // 帧计数器
static uint32_t last_fps_time = 0;  // 上次FPS计算时间
static uint16_t current_fps = 0;    // 当前帧率（FPS）

/* 计数器 */
static uint8_t resistance_sample_count = 0;  // 电阻采样计数
static uint32_t resistance_sum = 0;          // 电阻累加和

/******************************************************************************
 *                    内部辅助函数声明
 ******************************************************************************/
static uint8_t Detect_Emark_Chip_Internal(void);
static uint8_t Detect_Fast_Charge_Type_Internal(void);
static uint32_t Calculate_Cable_Resistance_Single_Internal(void);
static void Resistance_Test_State_Machine(void);

/******************************************************************************
 *                    初始化函数
 ******************************************************************************/

/**
 * @brief  测试模块初始化
 * @return none
 * @note   在System_Init中调用，初始化测试相关变量
 */
void Cable_Test_Init(void)
{
    // 初始化状态机变量
    test_state = TEST_STATE_IDLE;
    resistance_state = RESISTANCE_STATE_IDLE;

    last_test_time = 0;

    state_start_time = 0;
    last_key_time = 0;
    resistance_sample_count = 0;
    resistance_sum = 0;

    // 初始化测试结果
    test_result.power_ok = 0;
    test_result.cc1_ok = 0;
    test_result.cc2_ok = 0;
    test_result.dp_dm_ok = 0;
    test_result.tx1_ok = 0;
    test_result.rx1_ok = 0;
    test_result.tx2_ok = 0;
    test_result.rx2_ok = 0;
    test_result.sbu_ok = 0;
    test_result.shield_ok = 0;
    test_result.resistance_mohm = 0;
    test_result.has_emark = 0;
    test_result.fast_charge = 0;

    // 初始化E-Mark详细信息
    test_result.emarker_valid = 0;
    test_result.cable_type = 2;  // 未知
    test_result.usb_speed = 0;
    test_result.current_capability = 0;
    test_result.epr_capable = 0;
    test_result.vendor_id = 0;
    test_result.product_id = 0;

    // 初始化显示模式
    display_mode = MODE_CONTINUITY;
}

/******************************************************************************
 *                    按键处理功能
 ******************************************************************************/

/**
 * @brief  按键扫描（带消抖，非阻塞）
 * @return 1:按键按下, 0:无按键
 */
uint8_t Key_Scan_With_Debounce(void)
{
    static uint8_t key_state = 0;        // 0:空闲, 1:等待释放
    uint8_t key_raw = Read_Key_State();  // 1=按下, 0=未按下
    uint32_t current_time = system_tick_ms;

    if (key_state == 0) {
        // 空闲状态，检测按键按下
        if (key_raw == 1) {  // 按键按下（Read_Key_State返回1表示按下）
            // 检查消抖时间（20ms）
            if (current_time - last_key_time >= 20) {
                key_state = 1;
                last_key_time = current_time;
                return 1;  // 返回按键事件
            }
        } else {
            last_key_time = current_time;  // 更新最后按键时间
        }
    } else {
        // 等待释放状态
        if (key_raw == 0) {  // 按键已释放
            key_state = 0;
            last_key_time = current_time;
        }
    }

    return 0;
}

/**
 * @brief  切换显示模式
 * @return none
 */
void Switch_Display_Mode(void)
{
    display_mode++;
    if (display_mode >= MODE_MAX) {
        display_mode = MODE_CONTINUITY;
    }

    // 立即触发测试状态机，更新显示
    test_state = TEST_STATE_START;
}

/******************************************************************************
 *                    通断测试功能
 ******************************************************************************/

/**
 * @brief  测试线缆通断性
 * @param  result: 测试结果结构指针
 * @return none
 *
 * 测试原理（环回测试）：
 * - 所有信号线在初始化时已配置为上拉输入（IPU），始终保持该状态
 * - CC_E (PA1) 配置为输出低电平，作为CC信号的回路
 * - 右侧接口：每组信号的正负端在内部短接
 * - 检测方法：如果线缆连通，上拉输入会被拉到GND（低电平）
 *            如果线缆断开，上拉输入保持高电平
 *
 * CC信号检测特殊处理：
 * - CC_E (PA1) 输出低电平
 * - CC1 (PC3) 和 CC2 (PC0) 保持上拉输入
 * - 如果CC1连通，则PC3被拉低 → CC1 OK
 * - 如果CC2连通，则PC0被拉低 → CC2 OK
 * - Type-C正反插决定哪个CC脚连通
 *
 * 测试组别：
 * - Power: VBUS和GND（通过电阻测量模式判断）
 * - CC1/CC2: 分别检测（与Type-C正反插有关）
 * - D+/D-: PB10(上拉输入) + D-(接地)，右侧D+/D-短接
 * - TX1/RX1: PB12/PB11分别测试
 * - TX2/RX2: PB9/PB8分别测试
 * - SBU: PB7(上拉输入) + SBU-(接地)
 *
 * 注意：所有GPIO保持上拉输入状态，无需动态切换，直接读取即可
 */
void Test_Cable_Continuity(test_result_t *result)
{
    // 初始化结果为未知（Power需要电阻测量后确定）
    result->power_ok = 0;  // 将在电阻测量完成后更新

    /* CC信号检测（CC1和CC2分别检测） */
    // 使用CC_R提供5.1k下拉电阻作为CC信号的回路
    CC_R_Set_Pull_Mode(CC_R_PULL_DOWN);
    Delay_Ms(1);  // 等待稳定

    // 直接读取CC1和CC2电平（已在初始化时配置为上拉输入）
    uint8_t cc1_level = GPIO_ReadInputDataBit(GPIOC, USB3_CC1_PIN);
    uint8_t cc2_level = GPIO_ReadInputDataBit(GPIOC, USB3_CC2_PIN);

    // CC1低电平表示连通，CC2低电平表示连通
    result->cc1_ok = (cc1_level == Bit_RESET) ? 1 : 0;
    result->cc2_ok = (cc2_level == Bit_RESET) ? 1 : 0;

    // 恢复CC_R为浮空（避免影响其他测试）
    CC_R_Set_Pull_Mode(CC_R_FLOATING);

    /* D+/D- 信号检测（合并测试） */
    // D+ (PB10) 已配置为上拉输入，D-接地，右侧D+/D-短接
    // 读取D+电平：低电平表示连通（D+→右侧短接→D-→GND）
    result->dp_dm_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_DP_PIN) == Bit_RESET) ? 1 : 0;

    /* TX1/RX1信号检测 */
    result->tx1_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_TX1_PIN) == Bit_RESET) ? 1 : 0;
    result->rx1_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_RX1_PIN) == Bit_RESET) ? 1 : 0;

    /* TX2/RX2信号检测 */
    result->tx2_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_TX2_PIN) == Bit_RESET) ? 1 : 0;
    result->rx2_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_RX2_PIN) == Bit_RESET) ? 1 : 0;

    /* SBU信号检测 */
    result->sbu_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_SBU_PIN) == Bit_RESET) ? 1 : 0;

    /* 屏蔽层(EH)检测 */
    // EH (PA0) 已配置为上拉输入，直接读取电平
    // 低电平表示屏蔽层连通（被拉到GND），高电平表示断开
    result->shield_ok = (GPIO_ReadInputDataBit(GPIOA, USB4_EH_PIN) == Bit_RESET) ? 1 : 0;
}

/******************************************************************************
 *                    电阻测量功能
 ******************************************************************************/

/**
 * @brief  计算线缆电阻值（单次采样）
 * @return 电阻值(毫欧 mΩ)
 *
 * 硬件原理：
 * - VREF电源通过SERIES_RESISTOR限流电阻连接到VBUS
 * - VBUS通过线缆电阻R_cable连接到GND（右侧接口短接）
 * - INA199测量VBUS节点电压，放大INA199_GAIN倍
 * - 经过10k+0.1uF低通滤波后输入ADC
 *
 * 计算公式推导（精确分压原理，单位：VREF=mV, SERIES_RESISTOR=mΩ, R_cable=mΩ）：
 * VBUS电压 V_vbus(mV) = VREF × R_cable / (SERIES_RESISTOR + R_cable)
 *
 * INA199放大后：
 * V_amp(mV) = V_vbus(mV) × INA199_GAIN
 *           = VREF × R_cable × INA199_GAIN / (SERIES_RESISTOR + R_cable)
 *
 * ADC转换：
 * adc_value = V_amp(mV) / VREF(mV) × ADC_RESOLUTION
 *           = R_cable × INA199_GAIN × ADC_RESOLUTION / (SERIES_RESISTOR + R_cable)
 *
 * 反推电阻（精确公式）：
 * adc_value × (SERIES_RESISTOR + R_cable) = R_cable × INA199_GAIN × ADC_RESOLUTION
 * adc_value × SERIES_RESISTOR = R_cable × (INA199_GAIN × ADC_RESOLUTION - adc_value)
 * R_cable(mΩ) = adc_value × SERIES_RESISTOR / (INA199_GAIN × ADC_RESOLUTION - adc_value)
 *
 */
static uint32_t Calculate_Cable_Resistance_Single_Internal(void)
{
    uint16_t adc_value;
    uint32_t resistance_mohm;
    uint32_t denominator;

    // 读取ADC值 (单次采样，由状态机控制多次采样取平均)
    adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR);

    // 返回最大值表示开路
    if (adc_value >= ADC_RESOLUTION - 1) {
        return 9999;
    }

    // 计算分母: INA199_GAIN × ADC_RESOLUTION - adc_value
    denominator = (uint32_t)INA199_GAIN * ADC_RESOLUTION - adc_value;

    // 如果分母为0或负数，说明ADC值异常大，返回最大值
    if (denominator == 0) {
        return 9999;
    }

    // 使用整数运算计算电阻 (毫欧)
    // R_mohm = adc_value × SERIES_RESISTOR / (INA199_GAIN × ADC_RESOLUTION - adc_value)
    resistance_mohm = ((uint32_t)adc_value * SERIES_RESISTOR) / denominator;

    return resistance_mohm;
}

/**
 * @brief  获取单次电阻测量值（供外部调用）
 * @return 电阻值(毫欧 mΩ)
 */
uint32_t Get_Cable_Resistance_Single(void)
{
    return Calculate_Cable_Resistance_Single_Internal();
}

/******************************************************************************
 *                    E-mark检测功能
 ******************************************************************************/

/**
 * @brief  检测E-mark芯片（内部实现）
 * @return 1-存在E-mark, 0-不存在
 * @note   调用EMarker模块的检测函数
 */
static uint8_t Detect_Emark_Chip_Internal(void)
{
    return EMarker_Detect_Chip();
}

/**
 * @brief  检测E-mark芯片信息
 * @param  result: 测试结果结构指针
 * @return none
 */
void Detect_Emark_Info(test_result_t *result)
{
    emarker_info_t emark_info;
    uint8_t ret;

    // 步骤1: 初步检测是否存在E-mark
    result->has_emark = Detect_Emark_Chip_Internal();

    if (!result->has_emark) {
        // 没有检测到E-mark，清空详细信息
        result->emarker_valid = 0;
        result->cable_type = 2;  // 未知
        result->usb_speed = 0;
        result->current_capability = 0;
        result->epr_capable = 0;
        result->vendor_id = 0;
        result->product_id = 0;
        return;
    }

    // 步骤2: 使用EMarker模块的完整检测流程
    ret = EMarker_Full_Detect(&emark_info);

    if (ret == 0 && emark_info.valid) {
        // 成功读取E-Mark信息
        result->emarker_valid = 1;
        result->cable_type = emark_info.cable_type;
        result->usb_speed = emark_info.usb_speed;
        result->current_capability = emark_info.current_capability;
        result->epr_capable = emark_info.epr_capable;
        result->vendor_id = emark_info.vendor_id;
        result->product_id = emark_info.product_id;
    } else {
        // PD通信失败，只保留基本检测结果
        result->emarker_valid = 0;
        result->cable_type = 2;  // 未知
        result->usb_speed = 0;
        result->current_capability = 0;
        result->epr_capable = 0;
        result->vendor_id = 0;
        result->product_id = 0;
    }
}

/******************************************************************************
 *                    快充识别功能
 ******************************************************************************/

/**
 * @brief  检测快充线缆类型（内部实现）
 * @return 0-无快充, 1-Huawei单CC电阻, 2-Huawei双CC电阻
 */
static uint8_t Detect_Fast_Charge_Type_Internal(void)
{
    uint16_t adc_value;
    uint32_t voltage_mv;
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    // 步骤1: 配置PA1为模拟输入用于ADC测量
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // 步骤2: 设置CC_R为上拉模式（通过5.1k上拉CC_E）
    CC_R_Set_Pull_Mode(CC_R_PULL_UP);
    Delay_Ms(5);  // 等待稳定

    // 步骤3: 读取CC_E的ADC值
    adc_value = ADC_Read_Channel(ADC_CHANNEL_CC_E);

    // 步骤4: 恢复CC_R为浮空，PA1恢复为浮空输入
    CC_R_Set_Pull_Mode(CC_R_FLOATING);
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // 步骤5: 计算电压并判断
    // VREF=3.3V, 12位ADC
    voltage_mv = (uint32_t)adc_value * 3300 / 4095;

    // 华为充电线原理：
    // - USB-A转C线在CC引脚有56k电阻连接VBUS
    // - VBUS已短接到GND用于线阻测量，相当于56k下拉到GND
    // - CC_E通过5.1k上拉到3.3V，形成分压电路
    // - 单CC脚：V = 3.3V * 56k / (5.1k + 56k) ≈ 3.0V
    // - 双CC脚并联：V = 3.3V * 28k / (5.1k + 28k) ≈ 2.8V

    if (voltage_mv <= 3100 && voltage_mv >= 2900) {
        return 1;  // Huawei快充
    } else if (voltage_mv <= 2900 && voltage_mv >= 2700) {
        return 2;  // Huawei快充
    }

    return 0;  // 无快充
}

/**
 * @brief  检测快充类型
 * @param  result: 测试结果结构指针
 * @return none
 */
void Detect_Fast_Charge_Info(test_result_t *result)
{
    result->fast_charge = Detect_Fast_Charge_Type_Internal();
}

/******************************************************************************
 *                    电阻测量状态机（非阻塞）
 ******************************************************************************/

/**
 * @brief  电阻测量状态机（非阻塞）
 * @return none
 *
 * 测量流程：
 * 1. 打开MOS管，等待20ms让电路稳定
 * 2. ADC连续采样20ms，取平均值计算电阻
 * 3. 关闭MOS管，完成测量
 */
static void Resistance_Test_State_Machine(void)
{
    uint32_t current_time = system_tick_ms;
    uint32_t elapsed_time = current_time - state_start_time;

    switch (resistance_state) {
    case RESISTANCE_STATE_IDLE:
        // 空闲状态，等待启动
        break;

    case RESISTANCE_STATE_MOS_ON:
        // 开启MOS管
        Set_Test_Voltage_Enable(1);
        state_start_time = current_time;
        resistance_state = RESISTANCE_STATE_STABILIZING;
        break;

    case RESISTANCE_STATE_STABILIZING:
        // 等待电路稳定（20ms）
        if (elapsed_time >= 20) {
            // 初始化采样变量
            resistance_sample_count = 0;
            resistance_sum = 0;
            state_start_time = current_time;
            resistance_state = RESISTANCE_STATE_SAMPLING;
        }
        break;

    case RESISTANCE_STATE_SAMPLING:
        // ADC连续采样阶段（持续20ms）
        // 每次采样间隔约1ms，共采样20次
        if (elapsed_time >= 1) {
            // 执行单次采样
            uint32_t single_resistance = Calculate_Cable_Resistance_Single_Internal();

            resistance_sum += single_resistance;
            resistance_sample_count++;

            // 更新采样时间基准
            state_start_time = current_time;

            // 采样满20次后关闭MOS
            if (resistance_sample_count >= 20) {
                Set_Test_Voltage_Enable(0);
                state_start_time = current_time;
                resistance_state = RESISTANCE_STATE_MOS_OFF;
            }
        }
        break;

    case RESISTANCE_STATE_MOS_OFF:
        // MOS已关闭，计算平均值
        if (resistance_sample_count > 0) {
            test_result.resistance_mohm = resistance_sum / resistance_sample_count;
        } else {
            test_result.resistance_mohm = 0;
        }
        // 根据电阻值判断Power通断状态
        test_result.power_ok = (test_result.resistance_mohm <= RES_CABLE_MAX) ? 1 : 0;
        resistance_state = RESISTANCE_STATE_COMPLETE;
        break;

    case RESISTANCE_STATE_COMPLETE:
        // 测量完成，返回空闲
        resistance_state = RESISTANCE_STATE_IDLE;
        test_state = TEST_STATE_COMPLETE;
        break;

    default:
        resistance_state = RESISTANCE_STATE_IDLE;
        break;
    }
}

/******************************************************************************
 *                    测试状态机主函数
 ******************************************************************************/

/**
 * @brief  测试状态机主函数
 * @return none
 * @note   在主循环中调用，处理测试流程和状态转换
 */
void Test_State_Machine(void)
{
    uint32_t current_time = system_tick_ms;

    switch (test_state) {
    case TEST_STATE_IDLE:
        // 检查是否到达测试周期（200ms = 每秒5次）
        if (current_time - last_test_time >= 200) {
            last_test_time = current_time;
            test_state = TEST_STATE_START;
        }
        break;

    case TEST_STATE_START:
        // 根据当前模式启动相应测试
        switch (display_mode) {
        case MODE_CONTINUITY:
            // 合并模式：通断测试 + 电阻测量 + 快充识别
            Test_Cable_Continuity(&test_result);
            Detect_Fast_Charge_Info(&test_result);  // 同时检测快充类型
            // 然后启动电阻测量状态机
            resistance_state = RESISTANCE_STATE_MOS_ON;
            test_state = TEST_STATE_RUNNING;
            break;

        case MODE_EMARK:
            Detect_Emark_Info(&test_result);
            test_state = TEST_STATE_COMPLETE;
            break;

        default:
            test_state = TEST_STATE_COMPLETE;
            break;
        }
        break;

    case TEST_STATE_RUNNING:
        // 运行中，执行状态机
        Resistance_Test_State_Machine();
        break;

    case TEST_STATE_COMPLETE:
        // 测试完成，更新显示
        TFT_ShowTestResult(&test_result, display_mode);
        test_state = TEST_STATE_IDLE;
        break;

    default:
        test_state = TEST_STATE_IDLE;
        break;
    }
}

/******************************************************************************
 *                    TFT显示功能
 ******************************************************************************/

/**
 * @brief  显示测试结果（优化版，合并通断和内阻）
 * @param  result: 测试结果结构
 * @param  mode: 显示模式
 * @return none
 */
void TFT_ShowTestResult(test_result_t *result, display_mode_t mode)
{
    char buf[20];
    static display_mode_t last_mode = MODE_CONTINUITY;
    uint32_t current_time = system_tick_ms;

    // 帧率计算：每1000ms更新一次FPS
    frame_count++;
    if (current_time - last_fps_time >= 1000) {
        current_fps = frame_count;
        frame_count = 0;
        last_fps_time = current_time;
    }

    // 如果模式改变，清屏；否则只刷新内容区域
    if (mode != last_mode) {
        TFT_Clear(COLOR_BLACK);
        last_mode = mode;
    }

    switch (mode) {
    case MODE_CONTINUITY:
        // ========== 合并模式：内阻 + 通断 + 快充识别 ==========

        // 内阻测量结果（大字体显示）
        TFT_DrawString_Font(5, 5, "R=", COLOR_WHITE, COLOR_BLACK, FONT_12x24);

        if (result->power_ok == 0) {
            TFT_DrawString_Font(30, 5, " OL   ", COLOR_RED, COLOR_BLACK, FONT_12x24);
        } else {
            sprintf(buf, "%3u", result->resistance_mohm);
            TFT_DrawString_Font(30, 5, buf, COLOR_YELLOW, COLOR_BLACK, FONT_12x24);
            TFT_DrawString_Font(69, 5, " m", COLOR_WHITE, COLOR_BLACK, FONT_12x24);
        }

        // 通断测试（紧凑显示，用颜色直接表示状态）
        // 第一行：D+- CC1/CC2 SBU SHLD
        // 第二行：TX1 RX1 TX2 RX2 SBU
        uint16_t x_pos = 5;
        const uint16_t y_line1 = 38;
        const uint16_t y_line2 = 48;

        TFT_DrawString_Font(x_pos, y_line1, "D+-",
                             result->dp_dm_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;

        // CC1和CC2分别显示，只显示连通的那个
        if (result->cc1_ok || result->cc2_ok) {
            // 优先显示连通的CC脚
            if (result->cc1_ok) {
                TFT_DrawString_Font(x_pos, y_line1, "CC1", COLOR_GREEN, COLOR_BLACK, FONT_5x7);
            } else if (result->cc2_ok) {
                TFT_DrawString_Font(x_pos, y_line1, "CC2", COLOR_GREEN, COLOR_BLACK, FONT_5x7);
            }
        } else {
            // 都不连通，显示红色CC
            TFT_DrawString_Font(x_pos, y_line1, "CC ", COLOR_RED, COLOR_BLACK, FONT_5x7);
        }
        x_pos += 22;
        TFT_DrawString_Font(x_pos, y_line1, "SBU",
                             result->sbu_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;
        TFT_DrawString_Font(x_pos, y_line1, "SHLD",
                             result->shield_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;

        x_pos = 5;

        TFT_DrawString_Font(x_pos, y_line2, "TX1",
                             result->tx1_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;
        TFT_DrawString_Font(x_pos, y_line2, "RX1",
                             result->rx1_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;
        TFT_DrawString_Font(x_pos, y_line2, "TX2",
                             result->tx2_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;
        TFT_DrawString_Font(x_pos, y_line2, "RX2",
                             result->rx2_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;

        // 快充类型（左下角小字显示）
        const char *charge_type = "          ";
        uint16_t charge_color = COLOR_GRAY;
        switch (result->fast_charge) {
        case 1:
            charge_type = "Huawei 5A";
            charge_color = COLOR_YELLOW;
            break;
        case 2:
            charge_type = "Huawei 5A";
            charge_color = COLOR_MAGENTA;
            break;
        }
        TFT_DrawString_Font(0, 65, charge_type, charge_color, COLOR_BLACK, FONT_5x7);

        // 显示帧率（右下角）
        sprintf(buf, "%dFPS", current_fps);
        TFT_DrawString_Font(125, 65, buf, COLOR_GRAY, COLOR_BLACK, FONT_5x7);

        break;

    case MODE_EMARK:
        // 显示E-mark信息（使用批量绘制）
        // E-Mark存在性
        TFT_DrawString_Font(5, 0, "E-Mark:", COLOR_WHITE, COLOR_BLACK, FONT_5x7);
        TFT_DrawString_Font(60, 0, result->has_emark ? "YES" : "NO ",
                             result->has_emark ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);

        if (result->has_emark) {
            uint16_t y_pos = 10;

            // 数据有效性
            if (result->emarker_valid) {
                // 线缆类型
                const char *type_str;
                uint16_t type_color;
                switch (result->cable_type) {
                    case 0:
                        type_str = "Passive Cable";
                        type_color = COLOR_YELLOW;
                        break;
                    case 1:
                        type_str = "Active Cable";
                        type_color = COLOR_MAGENTA;
                        break;
                    default:
                        type_str = "Unknown Type";
                        type_color = COLOR_GRAY;
                        break;
                }
                TFT_DrawString_Font(5, y_pos, type_str, type_color, COLOR_BLACK, FONT_5x7);
                y_pos += 10;

                // USB速度等级
                const char *speed_str;
                switch (result->usb_speed) {
                    case 0x00:
                        speed_str = "USB 2.0";
                        break;
                    case 0x01:
                        speed_str = "USB 3.2 Gen1";
                        break;
                    case 0x02:
                        speed_str = "USB 3.2 Gen2";
                        break;
                    case 0x03:
                        speed_str = "USB4 Gen3";
                        break;
                    case 0x04:
                        speed_str = "USB4 Gen4";
                        break;
                    default:
                        speed_str = "Unknown";
                        break;
                }
                TFT_DrawString_Font(5, y_pos, speed_str, COLOR_CYAN, COLOR_BLACK, FONT_5x7);
                y_pos += 10;

                // 电流能力
                const char *current_str;
                uint16_t current_color;
                switch (result->current_capability) {
                    case 0x00:
                    case 0x01:
                        current_str = "3A";
                        current_color = COLOR_YELLOW;
                        break;
                    case 0x02:
                        current_str = "5A";
                        current_color = COLOR_GREEN;
                        break;
                    default:
                        current_str = "3A";
                        current_color = COLOR_RED;
                        break;
                }
                TFT_DrawString_Font(5, y_pos, "Current:", COLOR_WHITE, COLOR_BLACK, FONT_5x7);
                TFT_DrawString_Font(55, y_pos, current_str, current_color, COLOR_BLACK, FONT_5x7);
                y_pos += 10;

                // EPR支持
                TFT_DrawString_Font(5, y_pos, "EPR:", COLOR_WHITE, COLOR_BLACK, FONT_5x7);
                TFT_DrawString_Font(35, y_pos, result->epr_capable ? "Yes" : "No",
                                     result->epr_capable ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK, FONT_5x7);
                y_pos += 10;

                // Vendor ID
                sprintf(buf, "VID:0x%04X", result->vendor_id);
                TFT_DrawString_Font(5, y_pos, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x7);

            } else {
                // PD通信失败
                TFT_DrawString_Font(5, 10, "PD Comm Failed", COLOR_RED, COLOR_BLACK, FONT_5x7);
                TFT_DrawString_Font(5, 20, "Basic Cable", COLOR_GRAY, COLOR_BLACK, FONT_5x7);
            }
        } else {
            TFT_DrawString_Font(5, 10, "No E-Mark Chip", COLOR_GRAY, COLOR_BLACK, FONT_5x7);
        }

        // 显示帧率（右下角）
        sprintf(buf, "%dFPS", current_fps);
        TFT_DrawString_Font(125, 65, buf, COLOR_GRAY, COLOR_BLACK, FONT_5x7);

        break;

    default:
        break;
    }
}

/**
 * @brief  显示欢迎界面
 * @return none
 */
void TFT_ShowWelcome(void)
{
    TFT_Clear(COLOR_BLACK);

    TFT_DrawString(20, 20, "CableTester", COLOR_RED, COLOR_BLACK);
    TFT_DrawString(15, 35, "USB Cable Test", COLOR_WHITE, COLOR_BLACK);
    TFT_DrawString(25, 50, "V3.0 2026", COLOR_GRAY, COLOR_BLACK);
    Delay_Ms(1000);
    TFT_Clear(COLOR_BLACK);
}
