/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_test.c
 * Author             : CableTester Team
 * Version            : V3.1
 * Date               : 2026/05/17
 * Description        : 线缆测试核心功能实现
 *
 * 模块说明：
 * - 系统初始化
 * - 按键处理（非阻塞消抖）
 * - 状态机管理（测试流程 + 电阻测量）
 * - 通断测试（合并CC检测）
 * - 电阻测量（双通道自动切换）
 * - E-mark检测（USB PD协议）
 * - 快充识别（华为56k电阻检测）
 *********************************************************************************/

#include "cable_test.h"
#include "cable_gpio.h"
#include "cable_adc.h"
#include "cable_tft.h"
#include "emarker_pd.h"
#include "debug.h"

#define VREF_MV 3300

#define OPAMP_GAIN 101
#define SERIES_RESISTOR 100000

#define RES_CABLE_MAX         100000     // 有效测量上限(mΩ)，即100Ω
#define RES_SATURATED         (RES_CABLE_MAX + 1)  // 饱和返回值，触发power_ok=0 → OL

static uint8_t use_v_pwr_l_channel;      // 0=V_PWR_H, 1=V_PWR_L

volatile uint32_t system_tick_ms = 0;
test_result_t test_result = {0};
display_mode_t display_mode = MODE_CONTINUITY;

typedef enum {
    TEST_STATE_IDLE = 0,
    TEST_STATE_START,
    TEST_STATE_RUNNING,
    TEST_STATE_COMPLETE
} test_state_t;

typedef enum {
    RESISTANCE_STATE_IDLE = 0,
    RESISTANCE_STATE_MOS_ON,
    RESISTANCE_STATE_STABILIZING,
    RESISTANCE_STATE_SAMPLING,
    RESISTANCE_STATE_MOS_OFF,
    RESISTANCE_STATE_COMPLETE
} resistance_state_t;

static test_state_t test_state = TEST_STATE_IDLE;
static resistance_state_t resistance_state = RESISTANCE_STATE_IDLE;

static uint32_t last_test_time = 0;
static uint32_t state_start_time = 0;
static uint32_t last_key_time = 0;
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

void Switch_Display_Mode(void)
{
    display_mode++;
    if (display_mode >= MODE_MAX) {
        display_mode = MODE_CONTINUITY;
    }
    test_state = TEST_STATE_START;
}

/**
 * @brief  通断测试
 * @note   CC测试：PA1/PA2推挽输出低，左口CC读电平（合并检测）
 *         其他信号：环回法，上拉输入读电平
 */
void Test_Cable_Continuity(test_result_t *result)
{
    result->power_ok = 0;

    /* CC信号合并检测：PA1/PA2推挽输出低 */
    CC_E_Set_Output_Low();
    Delay_Ms(1);

    uint8_t cc1_level = GPIO_ReadInputDataBit(GPIOC, USB3_CC1_PIN);
    uint8_t cc2_level = GPIO_ReadInputDataBit(GPIOC, USB3_CC2_PIN);

    result->cc1_ok = (cc1_level == Bit_RESET) ? 1 : 0;
    result->cc2_ok = (cc2_level == Bit_RESET) ? 1 : 0;

    CC_E_Set_Floating();

    /* D+/D- 合并检测 */
    result->dp_dm_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_DP_PIN) == Bit_RESET) ? 1 : 0;

    /* TX/RX/SBU */
    result->tx1_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_TX1_PIN) == Bit_RESET) ? 1 : 0;
    result->rx1_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_RX1_PIN) == Bit_RESET) ? 1 : 0;
    result->tx2_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_TX2_PIN) == Bit_RESET) ? 1 : 0;
    result->rx2_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_RX2_PIN) == Bit_RESET) ? 1 : 0;
    result->sbu_ok = (GPIO_ReadInputDataBit(GPIOB, USB3_SBU_PIN) == Bit_RESET) ? 1 : 0;

    /* 屏蔽层检测 */
    result->shield_ok = (GPIO_ReadInputDataBit(GPIOA, USB4_EH_PIN) == Bit_RESET) ? 1 : 0;
}

/**
 * @brief  单次电阻采样（双通道自动切换）
 * @return 电阻值(mΩ)
 */
static uint32_t Calculate_Cable_Resistance_Single_Internal(void)
{
    uint16_t adc_value;
    uint32_t resistance_mohm = RES_SATURATED;

    if (use_v_pwr_l_channel) {
        adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_L);

        // V_PWR_L饱和（≥满量程）或超出有效线性范围（>3686）→ 切换到V_PWR_H
        // V_PWR_L饱和：换V_PWR_H重测
        if (adc_value >= ADC_RESOLUTION - 1) {
            use_v_pwr_l_channel = 0;
            adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_H);
            // V_PWR_H也饱和 → 路真正开路
            if (adc_value >= ADC_RESOLUTION - 1) {
                return RES_SATURATED;
            }
            goto calc_v_pwr_h;
        }

        if (adc_value > ADC_SWITCH_THRESHOLD) {
            use_v_pwr_l_channel = 0;
            adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_H);
            goto calc_v_pwr_h;
        }

        uint32_t denom = (uint32_t)OPAMP_GAIN * ADC_RESOLUTION - adc_value;
        if (denom == 0) return RES_SATURATED;

        resistance_mohm = ((uint32_t)adc_value * SERIES_RESISTOR) / denom;
    } else {
        adc_value = ADC_Read_Channel(ADC_CHANNEL_V_PWR_H);

calc_v_pwr_h:
        if (adc_value >= ADC_RESOLUTION - 1) {
            return RES_SATURATED;
        }

        uint32_t denom = ADC_RESOLUTION - adc_value;
        if (denom == 0) return RES_SATURATED;

        resistance_mohm = ((uint32_t)adc_value * SERIES_RESISTOR) / denom;

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

/**
 * @brief  华为快充检测
 * @note   PA2推挽高→5.1k→PA1(ADC)→线缆CC→56k→GND，测分压
 */
static uint8_t Detect_Fast_Charge_Type_Internal(void)
{
    uint16_t adc_value;
    uint32_t voltage_mv;

    // 步骤1: PA2=高, PA1=ADC
    CC_E_FastCharge_Start();
    Delay_Ms(5);

    // 步骤2: 读PA1分压
    adc_value = ADC_Read_Channel(ADC_CHANNEL_CC_E2);

    // 步骤3: 恢复浮空
    CC_E_FastCharge_Stop();

    voltage_mv = (uint32_t)adc_value * VREF_MV / ADC_RESOLUTION;

    // 单CC脚56k: V≈3.0V; 双CC并联28k: V≈2.8V
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

void Test_State_Machine(void)
{
    uint32_t current_time = system_tick_ms;

    switch (test_state) {
    case TEST_STATE_IDLE:
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
            use_v_pwr_l_channel = 1;  // 每周期从V_PWR_L开始
            resistance_state = RESISTANCE_STATE_MOS_ON;
            test_state = TEST_STATE_RUNNING;
            break;

        case MODE_EMARK:
            CC_E_Set_Floating();  // 确保PA1/PA2浮空
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
        // 电阻值（大字体）
        TFT_DrawString_Font(5, 5, "R=", COLOR_WHITE, COLOR_BLACK, FONT_12x24);
        if (result->power_ok == 0) {
            TFT_DrawString_Font(30, 5, " OL   ", COLOR_RED, COLOR_BLACK, FONT_12x24);
        } else {
            sprintf(buf, "%lu", (unsigned long)result->resistance_mohm);
            TFT_DrawString_Font(30, 5, buf, COLOR_YELLOW, COLOR_BLACK, FONT_12x24);
            TFT_DrawString_Font(81, 5, "m", COLOR_WHITE, COLOR_BLACK, FONT_12x24);
        }

        // 信号状态
        uint16_t x_pos = 5;
        const uint16_t y_line1 = 38;
        const uint16_t y_line2 = 48;

        TFT_DrawString_Font(x_pos, y_line1, "D+-",
                             result->dp_dm_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
        x_pos += 22;

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

        // 快充类型
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

                TFT_DrawString_Font(5, y_pos, "EPR:", COLOR_WHITE, COLOR_BLACK, FONT_5x7);
                TFT_DrawString_Font(35, y_pos, result->epr_capable ? "Yes" : "No",
                                     result->epr_capable ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK, FONT_5x7);
                y_pos += 10;

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
