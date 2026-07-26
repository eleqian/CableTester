/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_test.c
 * Author             : CableTester Team
 * Version            : V3.1
 * Date               : 2026/05/17
 * Description        : 线缆测试核心功能实现
 *
 * 模块说明：
 * - 系统初始化，按键处理（非阻塞消抖）
 * - 状态机管理（测试流程 + 电阻测量）
 * - 通断测试：CC合并检测（PA1/PA2推挽低），其余信号环回法
 * - 电阻测量：V_PWR_L/V_PWR_H双通道自动切换，非阻塞状态机
 * - E-mark检测：通过USB PD协议读取详细信息
 * - 快充识别：PA2推挽高 + PA1 ADC 测华为56k分压
 *
 * 电阻测量双通道原理：
 *   V_PWR_L (PB0) = 101倍运放同相放大 → 0~900mΩ 高精度（~0.24mΩ/LSB）
 *   V_PWR_H (PB1) = 未放大直接ADC    → 900mΩ~100Ω（~24mΩ/LSB）
 *   自动切换阈值：V_PWR_L adc > 3686 切至V_PWR_H
 *   回差：V_PWR_H adc < 33 切回V_PWR_L
 *   双通道均饱和（ADC=4095）→ 开路
 *********************************************************************************/

#include "cable_test.h"
#include "cable_gpio.h"
#include "cable_adc.h"
#include "cable_tft.h"
#include "emarker_pd.h"
#include "debug.h"

#define VREF_MV 3300

/*
 * 电阻测量关键参数（Key measurement parameters）
 * SERIES_RESISTOR = 100000 mΩ = 100Ω 串联参考电阻
 * OPAMP_GAIN = 101（由 1 + 10kΩ/100Ω = 101 决定）
 *
 * 放大通道公式：R(mΩ) = adc × 100000 / (101 × 4095 - adc)
 * 未放大通道公式：R(mΩ) = adc × 100000 / (4095 - adc)
 */
#define OPAMP_GAIN 101
#define SERIES_RESISTOR 100000

#define RES_CABLE_MAX         100000     /* 有效测量上限(mΩ) = 100Ω             */
#define RES_SATURATED         (RES_CABLE_MAX + 1)  /* 双通道饱和返回值 → OL     */

/* 当前使用的ADC通道：1=V_PWR_L(放大), 0=V_PWR_H(未放大)，每周期从V_PWR_L开始 */
static uint8_t use_v_pwr_l_channel;

volatile uint32_t system_tick_ms = 0;
test_result_t test_result = {0};
display_mode_t display_mode = MODE_CONTINUITY;

/* === 测试状态机 === */
typedef enum {
    TEST_STATE_IDLE,     /* 空闲，等待周期到达 */
    TEST_STATE_START,    /* 启动当前模式测试 */
    TEST_STATE_RUNNING,  /* 电阻状态机运行中（仅CONTINUITY模式） */
    TEST_STATE_COMPLETE  /* 完成，更新显示 */
} test_state_t;

/* === 电阻测量状态机（非阻塞，1ms时间片驱动）=== */
typedef enum {
    RESISTANCE_STATE_IDLE,          /* 空闲 */
    RESISTANCE_STATE_MOS_ON,        /* 开启MOS，启动测试电压 */
    RESISTANCE_STATE_STABILIZING,   /* 等待RC滤波稳定（20ms） */
    RESISTANCE_STATE_SAMPLING,      /* 20次ADC采样，每次间隔1ms */
    RESISTANCE_STATE_MOS_OFF,       /* 关闭MOS，计算平均值 */
    RESISTANCE_STATE_COMPLETE       /* 完成 */
} resistance_state_t;

static test_state_t test_state = TEST_STATE_IDLE;
static resistance_state_t resistance_state = RESISTANCE_STATE_IDLE;

static uint32_t last_test_time = 0;    /* 上次测试启动时间戳 */
static uint32_t state_start_time = 0;  /* 当前状态开始时间戳 */
static uint32_t last_key_time = 0;     /* 上次按键时间戳（消抖） */
static uint32_t frame_count = 0;
static uint32_t last_fps_time = 0;
static uint16_t current_fps = 0;
static uint8_t resistance_sample_count = 0;
static uint32_t resistance_sum = 0;

static uint8_t Detect_Emark_Chip_Internal(void);
static uint8_t Detect_Fast_Charge_Type_Internal(void);
static uint32_t Calculate_Cable_Resistance_Single_Internal(void);
static void Resistance_Test_State_Machine(void);

void Cable_Test_Init(void)
{
    test_state = TEST_STATE_IDLE;
    resistance_state = RESISTANCE_STATE_IDLE;
    last_test_time = 0;
    state_start_time = 0;
    last_key_time = 0;
    resistance_sample_count = 0;
    resistance_sum = 0;
    use_v_pwr_l_channel = 1;

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
    test_result.emarker_valid = 0;
    test_result.cable_type = 2;
    test_result.usb_speed = 0;
    test_result.current_capability = 0;
    test_result.epr_capable = 0;
    test_result.vendor_id = 0;
    test_result.product_id = 0;

    display_mode = MODE_CONTINUITY;
}

/*
 * 按键扫描（非阻塞消抖，20ms去抖时间）
 * key_state=0：空闲，检测到按下→消抖确认→返回1
 * key_state=1：等待释放，检测到释放→恢复空闲
 */
uint8_t Key_Scan_With_Debounce(void)
{
    static uint8_t key_state = 0;
    uint8_t key_raw = Read_Key_State();
    uint32_t current_time = system_tick_ms;

    if (key_state == 0) {
        if (key_raw == 1) {
            if (current_time - last_key_time >= 20) {
                key_state = 1;
                last_key_time = current_time;
                return 1;
            }
        } else {
            last_key_time = current_time;
        }
    } else {
        if (key_raw == 0) {
            key_state = 0;
            last_key_time = current_time;
        }
    }
    return 0;
}

/* 切换显示模式（两个模式循环：CONTINUITY ↔ EMARK） */
void Switch_Display_Mode(void)
{
    display_mode++;
    if (display_mode >= MODE_MAX) {
        display_mode = MODE_CONTINUITY;
    }
    test_state = TEST_STATE_START;
}

/*
 * 通断测试（Continuity test）
 *
 * CC信号：合并检测（无CC_R，PA1/PA2直接推挽输出低）。
 *   PA1↔PA2间5.1kΩ耦合导致无法独立分辨CC1/CC2，
 *   Type-C正反插本就只通一根CC，合并检测即可。
 *
 * 其他信号：环回法，引脚配置为上拉输入。
 *   若线缆连通，上拉输入被右侧接口短接GND拉低；
 *   若开路，保持高电平。
 */
void Test_Cable_Continuity(test_result_t *result)
{
    result->power_ok = 0;

    /* CC信号合并检测：PA1/PA2推挽输出低 → 左口CC读电平 */
    CC_E_Set_Output_Low();
    Delay_Ms(1);

    uint8_t cc1_level = GPIO_ReadInputDataBit(GPIOC, USB3_CC1_PIN);
    uint8_t cc2_level = GPIO_ReadInputDataBit(GPIOC, USB3_CC2_PIN);

    result->cc1_ok = (cc1_level == Bit_RESET) ? 1 : 0;
    result->cc2_ok = (cc2_level == Bit_RESET) ? 1 : 0;

    CC_E_Set_Floating();  /* 释放总线，E-Marker等后续测试需要PA1/PA2浮空 */

    /* D+/D- 合并检测（右侧D+短接D-→GND） */
    result->dp_dm_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_DP_PIN) == Bit_RESET) ? 1 : 0;

    /* TX/RX/SBU 独立检测 */
    result->tx1_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_TX1_PIN) == Bit_RESET) ? 1 : 0;
    result->rx1_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_RX1_PIN) == Bit_RESET) ? 1 : 0;
    result->tx2_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_TX2_PIN) == Bit_RESET) ? 1 : 0;
    result->rx2_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_RX2_PIN) == Bit_RESET) ? 1 : 0;
    result->sbu_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_SBU_PIN) == Bit_RESET) ? 1 : 0;

    /* 屏蔽层检测 */
    result->shield_ok = (GPIO_ReadInputDataBit(GPIOA, USB4_EH_PIN) == Bit_RESET) ? 1 : 0;
}

/*
 * 单次电阻采样（双通道自动切换）
 *
 * 工作流程：
 * 1. 优先使用 V_PWR_L（放大101倍，高精度），ADC读值
 * 2. 若 V_PWR_L 饱和（ADC≥4094）或超出线性区（ADC>3686）：
 *    → 丢弃V_PWR_L采样，立即切换到V_PWR_H（未放大）
 *    → V_PWR_H也饱和 → 开路（返回RES_SATURATED触发"OL"）
 * 3. 同一测量周期内所有采样来自同一通道，避免混用不同增益的读数
 * 4. 下个周期开始时（TEST_STATE_START）重置为V_PWR_L
 *
 * @return 电阻值(mΩ)，若开路返回 RES_SATURATED
 */
static uint32_t Calculate_Cable_Resistance_Single_Internal(void)
{
    uint16_t adc_value;
    uint32_t resistance_mohm = RES_SATURATED;

    if (use_v_pwr_l_channel) {
        adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_L);

        /* V_PWR_L饱和 → 切V_PWR_H重测；V_PWR_H也饱和 → 开路 */
        if (adc_value >= ADC_RESOLUTION - 1) {
            use_v_pwr_l_channel = 0;
            adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_H);
            if (adc_value >= ADC_RESOLUTION - 1) {
                return RES_SATURATED;
            }
            goto calc_v_pwr_h;
        }

        /* V_PWR_L超出线性区（>900mΩ对应点）→ 切V_PWR_H，丢弃本采样 */
        if (adc_value > ADC_SWITCH_THRESHOLD) {
            use_v_pwr_l_channel = 0;
            adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_H);
            goto calc_v_pwr_h;
        }

        /* V_PWR_L在有效量程内：R = adc × 100000 / (101 × 4095 - adc) */
        uint32_t denom = (uint32_t)OPAMP_GAIN * ADC_RESOLUTION - adc_value;
        if (denom == 0) return RES_SATURATED;

        resistance_mohm = ((uint32_t)adc_value * SERIES_RESISTOR) / denom;
    } else {
        adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_H);

calc_v_pwr_h:
        /* V_PWR_H饱和 → 开路 */
        if (adc_value >= ADC_RESOLUTION - 1) {
            return RES_SATURATED;
        }

        /* V_PWR_H公式（无增益）：R = adc × 100000 / (4095 - adc) */
        uint32_t denom = ADC_RESOLUTION - adc_value;
        if (denom == 0) return RES_SATURATED;

        resistance_mohm = ((uint32_t)adc_value * SERIES_RESISTOR) / denom;

        /* 回差：V_PWR_H adc < 33（≈800mΩ）时切回V_PWR_L高精度通道 */
        if (adc_value < ADC_SWITCH_HYST_LOW) {
            use_v_pwr_l_channel = 1;
        }
    }

    return resistance_mohm;
}

uint32_t Get_Cable_Resistance_Single(void)
{
    return Calculate_Cable_Resistance_Single_Internal();
}

static uint8_t Detect_Emark_Chip_Internal(void)
{
    return EMarker_Detect_Chip();
}

void Detect_Emark_Info(test_result_t *result)
{
    emarker_info_t emark_info;
    uint8_t ret;

    result->has_emark = Detect_Emark_Chip_Internal();

    if (!result->has_emark) {
        result->emarker_valid = 0;
        result->cable_type = 2;
        result->usb_speed = 0;
        result->current_capability = 0;
        result->epr_capable = 0;
        result->vendor_id = 0;
        result->product_id = 0;
        return;
    }

    /* 通过USB PD协议发送Discover Identity，读取完整E-Mark信息 */
    ret = EMarker_Full_Detect(&emark_info);

    if (ret == 0 && emark_info.valid) {
        result->emarker_valid = 1;
        result->cable_type = emark_info.cable_type;
        result->usb_speed = emark_info.usb_speed;
        result->current_capability = emark_info.current_capability;
        result->epr_capable = emark_info.epr_capable;
        result->vendor_id = emark_info.vendor_id;
        result->product_id = emark_info.product_id;
    } else {
        result->emarker_valid = 0;
        result->cable_type = 2;
        result->usb_speed = 0;
        result->current_capability = 0;
        result->epr_capable = 0;
        result->vendor_id = 0;
        result->product_id = 0;
    }
}

/*
 * 华为快充检测
 *
 * 原理（无CC_R，利用PA1↔PA2间5.1k自然上拉）：
 *   ① PA2 = 推挽输出高(3.3V)          → 上拉源
 *   ② PA1 = ADC输入                   → 经5.1k自然上拉
 *   ③ 华为线CC脚56k电阻接VBUS/GND     → 形成分压
 *   ④ 单CC脚56k：V ≈ 3.0V，双CC 28k：V ≈ 2.8V
 *
 * @return 0=无快充 1=华为单CC 2=华为双CC
 */
static uint8_t Detect_Fast_Charge_Type_Internal(void)
{
    uint16_t adc_value;
    uint32_t voltage_mv;

    CC_E_FastCharge_Start();
    Delay_Ms(5);

    adc_value = ADC_Read_Channel(ADC_CHANNEL_CC_E2);

    CC_E_FastCharge_Stop();

    voltage_mv = (uint32_t)adc_value * VREF_MV / ADC_RESOLUTION;

    if (voltage_mv <= 3100 && voltage_mv >= 2900) {
        return 1;
    } else if (voltage_mv <= 2900 && voltage_mv >= 2700) {
        return 2;
    }
    return 0;
}

void Detect_Fast_Charge_Info(test_result_t *result)
{
    result->fast_charge = Detect_Fast_Charge_Type_Internal();
}

/*
 * 电阻测量状态机（非阻塞）
 *
 * 时序（~55ms总开启时间，每1秒执行一次，占空比~5.5%）：
 *   MOS_ON        → 立即（开启PMOS）
 *   STABILIZING   → 20ms（RC滤波稳定，5τ=5ms但留足余量）
 *   SAMPLING      → 20次 × 1ms = 20ms（每次1间隔采样）
 *   MOS_OFF       → 立即（关闭PMOS，计算平均）
 *   COMPLETE      → 立即（返回IDLE）
 */
static void Resistance_Test_State_Machine(void)
{
    uint32_t current_time = system_tick_ms;
    uint32_t elapsed_time = current_time - state_start_time;

    switch (resistance_state) {
    case RESISTANCE_STATE_IDLE:
        break;

    case RESISTANCE_STATE_MOS_ON:
        Set_Test_Voltage_Enable(1);
        state_start_time = current_time;
        resistance_state = RESISTANCE_STATE_STABILIZING;
        break;

    case RESISTANCE_STATE_STABILIZING:
        if (elapsed_time >= 20) {
            resistance_sample_count = 0;
            resistance_sum = 0;
            state_start_time = current_time;
            resistance_state = RESISTANCE_STATE_SAMPLING;
        }
        break;

    case RESISTANCE_STATE_SAMPLING:
        if (elapsed_time >= 1) {
            uint32_t single_resistance = Calculate_Cable_Resistance_Single_Internal();
            resistance_sum += single_resistance;
            resistance_sample_count++;
            state_start_time = current_time;

            if (resistance_sample_count >= 20) {
                Set_Test_Voltage_Enable(0);
                state_start_time = current_time;
                resistance_state = RESISTANCE_STATE_MOS_OFF;
            }
        }
        break;

    case RESISTANCE_STATE_MOS_OFF:
        if (resistance_sample_count > 0) {
            test_result.resistance_mohm = resistance_sum / resistance_sample_count;
        } else {
            test_result.resistance_mohm = 0;
        }
        /* 超过100Ω量程判power_ok=0 → 显示"OL" */
        test_result.power_ok = (test_result.resistance_mohm <= RES_CABLE_MAX) ? 1 : 0;
        resistance_state = RESISTANCE_STATE_COMPLETE;
        break;

    case RESISTANCE_STATE_COMPLETE:
        resistance_state = RESISTANCE_STATE_IDLE;
        test_state = TEST_STATE_COMPLETE;
        break;

    default:
        resistance_state = RESISTANCE_STATE_IDLE;
        break;
    }
}

/*
 * 测试状态机主函数（主循环中每帧调用一次）
 *
 * MODE_CONTINUITY：
 *   通断测试(立即) → 快充检测(立即) → 电阻测量状态机(~55ms异步)
 * MODE_EMARK：
 *   确保PA1/PA2浮空 → E-Mark检测(USB PD协议)
 */
void Test_State_Machine(void)
{
    uint32_t current_time = system_tick_ms;

    switch (test_state) {
    case TEST_STATE_IDLE:
        /* 每200ms触发一次测试周期（≈5次/秒） */
        if (current_time - last_test_time >= 200) {
            last_test_time = current_time;
            test_state = TEST_STATE_START;
        }
        break;

    case TEST_STATE_START:
        switch (display_mode) {
        case MODE_CONTINUITY:
            Test_Cable_Continuity(&test_result);
            Detect_Fast_Charge_Info(&test_result);
            use_v_pwr_l_channel = 1;  /* 每周期从V_PWR_L高精度通道开始 */
            resistance_state = RESISTANCE_STATE_MOS_ON;
            test_state = TEST_STATE_RUNNING;
            break;

        case MODE_EMARK:
            CC_E_Set_Floating();  /* E-Marker PD通信期间PA1/PA2必须浮空 */
            Detect_Emark_Info(&test_result);
            test_state = TEST_STATE_COMPLETE;
            break;

        default:
            test_state = TEST_STATE_COMPLETE;
            break;
        }
        break;

    case TEST_STATE_RUNNING:
        Resistance_Test_State_Machine();
        break;

    case TEST_STATE_COMPLETE:
        TFT_ShowTestResult(&test_result, display_mode);
        test_state = TEST_STATE_IDLE;
        break;

    default:
        test_state = TEST_STATE_IDLE;
        break;
    }
}

/*
 * TFT显示测试结果
 *
 * MODE_CONTINUITY：合并显示
 *   上排：R=xxxx m（电阻值，使用%lu避免newlib-nano的%u位数截断）
 *   中排：D+- CC SBU SHLD / TX1 RX1 TX2 RX2（状态彩色指示）
 *   下排：快充类型 + FPS帧率
 *
 * MODE_EMARK：
 *   E-Mark存在性 + 详细Cable VDO信息（类型、速率、电流、EPR、VID）
 *   若PD通信失败显示"PD Comm Failed"
 */
void TFT_ShowTestResult(test_result_t *result, display_mode_t mode)
{
    char buf[20];
    static display_mode_t last_mode = MODE_CONTINUITY;
    uint32_t current_time = system_tick_ms;

    frame_count++;
    if (current_time - last_fps_time >= 1000) {
        current_fps = frame_count;
        frame_count = 0;
        last_fps_time = current_time;
    }

    if (mode != last_mode) {
        TFT_Clear(COLOR_BLACK);
        last_mode = mode;
    }

    switch (mode) {
    case MODE_CONTINUITY:
        /* 电阻值（大字体） */
        TFT_DrawString_Font(5, 5, "R=", COLOR_WHITE, COLOR_BLACK, FONT_12x24);
        if (result->power_ok == 0) {
            TFT_DrawString_Font(30, 5, " OL   ", COLOR_RED, COLOR_BLACK, FONT_12x24);
        } else {
            /* %lu：用unsigned long避免newlib-nano printf对%u的位数截断 */
            sprintf(buf, "%lu", (unsigned long)result->resistance_mohm);
            TFT_DrawString_Font(30, 5, buf, COLOR_YELLOW, COLOR_BLACK, FONT_12x24);
            TFT_DrawString_Font(69, 5, " m", COLOR_WHITE, COLOR_BLACK, FONT_12x24);
        }

        /* 信号线状态（两行紧凑排列） */
        uint16_t x_pos = 5;
        const uint16_t y_line1 = 38;
        const uint16_t y_line2 = 48;

        TFT_DrawString_Font(x_pos, y_line1, "D+-",
                             result->dp_dm_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;

        /* CC合并检测：任一CC连通即绿色 */
        if (result->cc1_ok || result->cc2_ok) {
            TFT_DrawString_Font(x_pos, y_line1, "CC", COLOR_GREEN, COLOR_BLACK, FONT_5x7);
        } else {
            TFT_DrawString_Font(x_pos, y_line1, "CC", COLOR_RED, COLOR_BLACK, FONT_5x7);
        }
        x_pos += 22;
        TFT_DrawString_Font(x_pos, y_line1, "SBU",
                             result->sbu_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;
        TFT_DrawString_Font(x_pos, y_line1, "SHLD",
                             result->shield_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);

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

        /* 快充类型（左下角） */
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

        sprintf(buf, "%dFPS", current_fps);
        TFT_DrawString_Font(125, 65, buf, COLOR_GRAY, COLOR_BLACK, FONT_5x7);
        break;

    case MODE_EMARK:
        TFT_DrawString_Font(5, 0, "E-Mark:", COLOR_WHITE, COLOR_BLACK, FONT_5x7);
        TFT_DrawString_Font(60, 0, result->has_emark ? "YES" : "NO ",
                             result->has_emark ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);

        if (result->has_emark) {
            uint16_t y_pos = 10;
            if (result->emarker_valid) {
                /* 线缆类型 */
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

                /* USB速度等级 */
                const char *speed_str;
                switch (result->usb_speed) {
                    case 0x00: speed_str = "USB 2.0"; break;
                    case 0x01: speed_str = "USB 3.2 Gen1"; break;
                    case 0x02: speed_str = "USB 3.2 Gen2"; break;
                    case 0x03: speed_str = "USB4 Gen3"; break;
                    case 0x04: speed_str = "USB4 Gen4"; break;
                    default:   speed_str = "Unknown"; break;
                }
                TFT_DrawString_Font(5, y_pos, speed_str, COLOR_CYAN, COLOR_BLACK, FONT_5x7);
                y_pos += 10;

                /* 电流能力 */
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

                /* EPR支持 */
                TFT_DrawString_Font(5, y_pos, "EPR:", COLOR_WHITE, COLOR_BLACK, FONT_5x7);
                TFT_DrawString_Font(35, y_pos, result->epr_capable ? "Yes" : "No",
                                     result->epr_capable ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK, FONT_5x7);
                y_pos += 10;

                /* 厂商ID */
                sprintf(buf, "VID:0x%04X", result->vendor_id);
                TFT_DrawString_Font(5, y_pos, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x7);
            } else {
                TFT_DrawString_Font(5, 10, "PD Comm Failed", COLOR_RED, COLOR_BLACK, FONT_5x7);
                TFT_DrawString_Font(5, 20, "Basic Cable", COLOR_GRAY, COLOR_BLACK, FONT_5x7);
            }
        } else {
            TFT_DrawString_Font(5, 10, "No E-Mark Chip", COLOR_GRAY, COLOR_BLACK, FONT_5x7);
        }

        sprintf(buf, "%dFPS", current_fps);
        TFT_DrawString_Font(125, 65, buf, COLOR_GRAY, COLOR_BLACK, FONT_5x7);
        break;

    default:
        break;
    }
}

void TFT_ShowWelcome(void)
{
    TFT_Clear(COLOR_BLACK);
    TFT_DrawString(20, 20, "CableTester", COLOR_RED, COLOR_BLACK);
    TFT_DrawString(15, 35, "USB Cable Test", COLOR_WHITE, COLOR_BLACK);
    TFT_DrawString(25, 50, "V3.0 2026", COLOR_GRAY, COLOR_BLACK);
    Delay_Ms(1000);
    TFT_Clear(COLOR_BLACK);
}
