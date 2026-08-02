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
#include <string.h>

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
    test_result.usb_version = 0;
    test_result.vendor_id = 0;
    test_result.product_id = 0;
    test_result.id_header = 0;
    test_result.cert_stat = 0;
    test_result.product_vdo = 0;
    test_result.cable_vdo1 = 0;
    test_result.cable_vdo2 = 0;

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
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    result->power_ok = 0;

    /* CC信号双向检测：PC14/PC15带上拉，防单端E-Mark假连通 */
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_PIN | USB3_CC2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    Delay_Ms(1);

    /* ① 右口拉低 → 读左口 */
    CC_E_Set_Output_Low();
    Delay_Ms(1);
    uint8_t cc1_r_low = GPIO_ReadInputDataBit(GPIOC, USB3_CC1_PIN);
    uint8_t cc2_r_low = GPIO_ReadInputDataBit(GPIOC, USB3_CC2_PIN);

    /* ② 右口释放 → 读左口（上拉恢复高，E-Mark持续拉低则不变） */
    CC_E_Set_Floating();
    Delay_Ms(1);
    uint8_t cc1_r_float = GPIO_ReadInputDataBit(GPIOC, USB3_CC1_PIN);
    uint8_t cc2_r_float = GPIO_ReadInputDataBit(GPIOC, USB3_CC2_PIN);

    /* ③ 双端连通判定：右拉低时左低 + 右释放后左恢复高 = 真正双端连通 */
    result->cc1_ok = (cc1_r_low == Bit_RESET && cc1_r_float != Bit_RESET) ? 1 : 0;
    result->cc2_ok = (cc2_r_low == Bit_RESET && cc2_r_float != Bit_RESET) ? 1 : 0;

    /* 恢复PC14/PC15为浮空（USBPD模块需要） */
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

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
        result->usb_version = 0;
        result->vendor_id = 0;
        result->product_id = 0;
        result->id_header = 0;
        result->cert_stat = 0;
        result->product_vdo = 0;
        result->cable_vdo1 = 0;
        result->cable_vdo2 = 0;
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
        result->usb_version = emark_info.usb_version;
        result->vendor_id = emark_info.vendor_id;
        result->product_id = emark_info.product_id;
        result->id_header = emark_info.id_header;
        result->cert_stat = emark_info.cert_stat;
        result->product_vdo = emark_info.product_vdo;
        result->cable_vdo1 = emark_info.cable_vdo1;
        result->cable_vdo2 = emark_info.cable_vdo2;
    } else {
        result->emarker_valid = 0;
        result->cable_type = 2;
        result->usb_speed = 0;
        result->current_capability = 0;
        result->epr_capable = 0;
        result->usb_version = 0;
        result->vendor_id = 0;
        result->product_id = 0;
        result->id_header = 0;
        result->cert_stat = 0;
        result->product_vdo = 0;
        result->cable_vdo1 = 0;
        result->cable_vdo2 = 0;
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
            /* E-Mark检测：首次探测到时读取基本信息 */
            CC_E_Set_Floating();
            if (EMarker_Detect_Chip()) {
                if (!test_result.emarker_valid) {
                    Detect_Emark_Info(&test_result);
                }
                test_result.has_emark = 1;
            } else {
                if (test_result.has_emark) {
                    test_result.has_emark = 0;
                    test_result.emarker_valid = 0;
                    test_result.cable_type = 2;
                    test_result.usb_speed = 0;
                    test_result.current_capability = 0;
                    test_result.epr_capable = 0;
                    test_result.usb_version = 0;
                    test_result.vendor_id = 0;
                    test_result.product_id = 0;
                    test_result.id_header = 0;
                    test_result.cert_stat = 0;
                    test_result.product_vdo = 0;
                    test_result.cable_vdo1 = 0;
                    test_result.cable_vdo2 = 0;
                }
            }
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
 * 自适应电阻格式化
 *
 * 电阻数值格式化（不含单位，供独立着色）
 * 根据阻值自动选择最佳显示格式：
 *   <1000 mΩ  → "xxx"     (毫欧整数)
 *   >=1000 mΩ → "x.xx"    (欧姆，两位小数)
 *   >=10000 mΩ→ "xx.x"    (欧姆，一位小数)
 *   OL        → "----"    (开路)
 *
 * @param buf: 输出缓冲区（至少5字节），右对齐填充到4字符槽位
 * @param res_mohm: 电阻值(mΩ)
 * @param power_ok: 电源状态
 */
static void Format_Resistance_Value(char *buf, uint32_t res_mohm, uint8_t power_ok)
{
    char tmp[8];
    if (!power_ok) {
        strcpy(tmp, "----");
    } else if (res_mohm < 1000) {
        sprintf(tmp, "%lu", res_mohm);
    } else if (res_mohm < 10000) {
        uint16_t whole = (uint16_t)(res_mohm / 1000);
        uint16_t frac = (uint16_t)((res_mohm % 1000) / 10);
        sprintf(tmp, "%u.%02u", whole, frac);
    } else {
        uint16_t whole = (uint16_t)(res_mohm / 1000);
        uint16_t frac = (uint16_t)((res_mohm % 1000) / 100);
        if (whole >= 99) {
            strcpy(tmp, "----");
        } else {
            sprintf(tmp, "%u.%u", whole, frac);
        }
    }
    /* 右对齐到4字符槽位：数值+单位总宽恒定，避免残留像素 */
    uint8_t len = (uint8_t)strlen(tmp);
    uint8_t pad = 4 - len;
    uint8_t i;
    for (i = 0; i < pad; i++) buf[i] = ' ';
    for (i = 0; i < len; i++) buf[pad + i] = tmp[i];
    buf[4] = '\0';
}

/*
 * 电阻单位格式化（恒白色显示）
 *   <1000 mΩ → "mΩ"   (毫欧)
 *   >=1000 mΩ→ "Ω "   (欧姆)
 *   OL        → "  "   (空白)
 *
 * @param buf: 输出缓冲区（至少3字节），固定2字符宽度
 * @param res_mohm: 电阻值(mΩ)
 * @param power_ok: 电源状态
 */
static void Format_Resistance_Unit(char *buf, uint32_t res_mohm, uint8_t power_ok)
{
    if (!power_ok) {
        buf[0] = ' '; buf[1] = ' '; buf[2] = '\0';
        return;
    }
    if (res_mohm < 1000) {
        buf[0] = 'm'; buf[1] = (char)CHAR_OMEGA; buf[2] = '\0';
    } else {
        buf[0] = (char)CHAR_OMEGA; buf[1] = ' '; buf[2] = '\0';
    }
}

/*
 * 电阻数值颜色选择（5档）
 *   <100 mΩ  绿色  → 优秀
 *   100-150  黄色  → 良好
 *   150-200  橙色  → 中等
 *   200-500  红色  → 差
 *   >=500    白色  → 超差/OL
 */
static uint16_t Get_Resistance_Color(uint32_t res_mohm, uint8_t power_ok)
{
    if (!power_ok) return COLOR_WHITE;  /* OL 显示为白色 */
    if (res_mohm < 100)  return COLOR_GREEN;
    if (res_mohm <= 150) return COLOR_YELLOW;
    if (res_mohm <= 200) return COLOR_ORANGE;
    if (res_mohm < 500)  return COLOR_RED;
    return COLOR_WHITE;
}

/*
 * 绘制信号指示灯 + 标签
 *
 * @param x, y: 左上角坐标
 * @param label: 信号标签字符串
 * @param connected: 连通状态(1=连通, 0=开路)
 */
static void Draw_Signal_Indicator(uint16_t x, uint16_t y, const char *label, uint8_t connected)
{
    uint16_t color = connected ? COLOR_GREEN : COLOR_RED;
    TFT_FillRect(x, y + 1, 5, 5, color);
    TFT_DrawString_Font(x + 8, y, label, COLOR_WHITE, COLOR_BLACK, FONT_5x7);
}

/*
 * 通断模式显示 (160x80 横屏)
 * ┌─────────────────────────────────────┐ y=0
 * │ ●FULL FEAT          Huawei 5A      │ ← 状态栏 (5x7)
 * ├─────────────────────────────────────┤ y=9
 * │      R: 15.6mΩ                    │ ← 电阻值 (12x24 居中, 数值4档色/单位白色)
 * ├─────────────────────────────────────┤ y=39
 * │ ■D+-  ■CC   ■SBU  ■SHLD            │ ← 信号线 (5x7)
 * │ ■TX1  ■RX1  ■TX2  ■RX2            │
 * ├─────────────────────────────────────┤ y=61
 * │ E-Marker: 5A       USB3.2G2         │ ← 底部信息 (5x7)
 * └─────────────────────────────────────┘ y=79
 */
void TFT_ShowContinuity(test_result_t *result)
{
    /* --- 分区1：状态栏 (y=0) --- */
    char class_buf[14];
    uint16_t class_color, indicator_color;

    if (result->power_ok == 0) {
        sprintf(class_buf, "%-11s", "OPEN");
        class_color = COLOR_RED; indicator_color = COLOR_RED;
    } else if (result->dp_dm_ok == 0) {
        sprintf(class_buf, "%-11s", "Power ONLY");
        class_color = COLOR_ORANGE; indicator_color = COLOR_ORANGE;
    } else {
        uint8_t usb3_lanes = 0;
        if (result->tx1_ok && result->rx1_ok) usb3_lanes++;
        if (result->tx2_ok && result->rx2_ok) usb3_lanes++;
        if ((result->cc1_ok || result->cc2_ok) && result->sbu_ok && result->shield_ok && usb3_lanes == 2) {
            sprintf(class_buf, "%-11s", "Type-C Full");
            class_color = COLOR_GREEN; indicator_color = COLOR_GREEN;
        } else if (usb3_lanes == 2) {
            sprintf(class_buf, "%-11s", "USB 3.2");
            class_color = COLOR_CYAN; indicator_color = COLOR_GREEN;
        } else if (usb3_lanes == 1) {
            sprintf(class_buf, "%-11s", "USB 3.0");
            class_color = COLOR_CYAN; indicator_color = COLOR_GREEN;
        } else {
            sprintf(class_buf, "%-11s", "USB 2.0");
            class_color = COLOR_YELLOW; indicator_color = COLOR_YELLOW;
        }
    }
    TFT_FillRect(3, 1, 5, 5, indicator_color);
    TFT_DrawString_Font(12, 0, class_buf, class_color, COLOR_BLACK, FONT_5x7);

    char charge_buf[10];
    uint16_t charge_color = COLOR_GRAY;
    if (result->fast_charge == 1) {
        sprintf(charge_buf, "%-9s", "Huawei 5A"); charge_color = COLOR_YELLOW;
    } else if (result->fast_charge == 2) {
        sprintf(charge_buf, "%-9s", "Huawei 5A"); charge_color = COLOR_MAGENTA;
    } else {
        sprintf(charge_buf, "%-9s", ""); charge_color = COLOR_BLACK;
    }
    TFT_DrawString_Font(106, 0, charge_buf, charge_color, COLOR_BLACK, FONT_5x7);

    TFT_FillRect(0, 9, 160, 1, COLOR_DARK_GRAY);

    /* --- 分区2：电阻值 (y=13, 12x24 居中) --- */
    char rval[8], runit[4];
    Format_Resistance_Value(rval, result->resistance_mohm, result->power_ok);
    Format_Resistance_Unit(runit, result->resistance_mohm, result->power_ok);
    uint16_t r_color = Get_Resistance_Color(result->resistance_mohm, result->power_ok);
    TFT_DrawString_Font(15, 13, "R:", COLOR_WHITE, COLOR_BLACK, FONT_12x24);
    TFT_DrawString_Font(54, 13, rval, r_color, COLOR_BLACK, FONT_12x24);  /* 数值: 4档颜色, 4字符槽, 与:间隔1字符 */
    TFT_DrawString_Font(119, 13, runit, COLOR_WHITE, COLOR_BLACK, FONT_12x24);  /* 单位: 恒白色, 与数值间隔1字符 */

    TFT_FillRect(0, 39, 160, 1, COLOR_DARK_GRAY);

    /* --- 分区3：信号线 (y=43/52, 5x7) --- */
    Draw_Signal_Indicator(4,  43, "D+-", result->dp_dm_ok);
    Draw_Signal_Indicator(42, 43, "CC",  result->cc1_ok || result->cc2_ok);
    Draw_Signal_Indicator(80, 43, "SBU", result->sbu_ok);
    Draw_Signal_Indicator(118,43, "SHLD",result->shield_ok);
    Draw_Signal_Indicator(4,  52, "TX1", result->tx1_ok);
    Draw_Signal_Indicator(42, 52, "RX1", result->rx1_ok);
    Draw_Signal_Indicator(80, 52, "TX2", result->tx2_ok);
    Draw_Signal_Indicator(118,52, "RX2", result->rx2_ok);

    TFT_FillRect(0, 61, 160, 1, COLOR_DARK_GRAY);

    /* --- 分区4：底部信息栏 (y=65, 5x7) --- */
    char em_left[16];
    uint16_t em_color;
    if (result->has_emark && result->emarker_valid) {
        const char *cur_str = (result->current_capability == 2) ? "5A" : "3A";
        sprintf(em_left, "E-Marker: %-2s ", cur_str);  /* 固定13字符宽度 */
        em_color = COLOR_GREEN;
    } else if (result->has_emark) {
        sprintf(em_left, "%-13s", "E-Marker: YES");
        em_color = COLOR_YELLOW;
    } else {
        sprintf(em_left, "%-13s", "E-Marker: NO");
        em_color = COLOR_GRAY;
    }
    TFT_DrawString_Font(4, 65, em_left, em_color, COLOR_BLACK, FONT_5x7);
    if (result->has_emark && result->emarker_valid) {
        const char *speed_str;
        switch (result->usb_speed) {
            case 0x00: speed_str = "USB2.0   "; break;
            case 0x01: speed_str = "USB3.2G1 "; break;
            case 0x02: speed_str = "USB3.2G2 "; break;
            case 0x03: speed_str = "USB4G3  "; break;
            case 0x04: speed_str = "USB4G4  "; break;
            default:   speed_str = "         "; break;
        }
        TFT_DrawString_Font(84, 65, speed_str, COLOR_CYAN, COLOR_BLACK, FONT_5x7);
    } else {
        TFT_DrawString_Font(84, 65, "         ", COLOR_BLACK, COLOR_BLACK, FONT_5x7);
    }
}

/*
 * E-Mark模式显示 (160x80 横屏, 详尽模式)
 * ┌─────────────────────────────────────┐ y=0
 * │ E-Mark: YES             EPR:Yes     │ ← 标题 + EPR (5x7)
 * ├─────────────────────────────────────┤ y=9
 * │ USB 3.2 Gen2              5A        │ ← 速度+电流 (8x16)
 * ├─────────────────────────────────────┤ y=30
 * │ Passive  USB v1.3                   │ ← 类型 + VDO版本 (5x7)
 * │ VID:0xFFFF  PID:0xFFFF              │ ← VID/PID
 * │ SBU:Y  Lat:2  MaxV:20V              │ ← 线缆特性
 * │ VDO1:0x12345678                     │ ← 原始VDO
 * │ VDO2:0x87654321                     │ ← 原始VDO2
 * └─────────────────────────────────────┘ y=79
 */
void TFT_ShowEmark(test_result_t *result)
{
    char buf[24];
    static uint8_t last_has_emark = 0;
    if (result->has_emark != last_has_emark) {
        TFT_FillRect(0, 13, 160, 66, COLOR_BLACK);
        last_has_emark = result->has_emark;
    }

    /* --- 标题栏 (y=0) --- */
    TFT_DrawString_Font(4, 0, "E-Mark:", COLOR_WHITE, COLOR_BLACK, FONT_5x7);
    TFT_DrawString_Font(56, 0, result->has_emark ? "YES" : "NO",
                         result->has_emark ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x7);
    if (result->has_emark && result->emarker_valid) {
        sprintf(buf, "EPR:%-3s", result->epr_capable ? "Yes" : "No");
        TFT_DrawString_Font(106, 0, buf, result->epr_capable ? COLOR_GREEN : COLOR_GRAY,
                             COLOR_BLACK, FONT_5x7);
    }
    TFT_FillRect(0, 9, 160, 1, COLOR_DARK_GRAY);

    if (result->has_emark && result->emarker_valid) {
        /* --- 速度 + 电流 (y=13, 8x16) --- */
        const char *speed_str;
        uint16_t speed_color = COLOR_CYAN;
        switch (result->usb_speed) {
            case 0x00: speed_str = "USB 2.0";      speed_color = COLOR_GRAY;  break;
            case 0x01: speed_str = "USB 3.2 Gen1"; speed_color = COLOR_CYAN;  break;
            case 0x02: speed_str = "USB 3.2 Gen2"; speed_color = COLOR_CYAN;  break;
            case 0x03: speed_str = "USB4 Gen3";    speed_color = COLOR_GREEN; break;
            case 0x04: speed_str = "USB4 Gen4";    speed_color = COLOR_GREEN; break;
            default:   speed_str = "Unknown";      speed_color = COLOR_GRAY;  break;
        }
        TFT_DrawString_Font(5, 13, speed_str, speed_color, COLOR_BLACK, FONT_8x16);

        const char *cur_str = (result->current_capability == 2) ? "5A" : "3A";
        uint16_t cur_color = (result->current_capability == 2) ? COLOR_GREEN : COLOR_YELLOW;
        TFT_DrawString_Font(130, 13, cur_str, cur_color, COLOR_BLACK, FONT_8x16);

        TFT_FillRect(0, 30, 160, 1, COLOR_DARK_GRAY);

        /* --- 类型 + VDO版本 (y=34) --- */
        const char *type_short;
        switch (result->cable_type) {
            case 0: type_short = "Passive"; break;
            case 1: type_short = "Active";  break;
            default:type_short = "Unknown"; break;
        }
        const char *ver_str;
        switch (result->usb_version) {
            case 0: ver_str = "v1.0"; break;
            case 1: ver_str = "v1.3"; break;
            case 2: ver_str = "v2.0"; break;
            case 3: ver_str = "v2.1"; break;
            case 4: ver_str = "v3.0"; break;
            case 5: ver_str = "v3.1"; break;
            case 6: ver_str = "v3.2"; break;
            default:ver_str = "v?";   break;
        }
        sprintf(buf, "%-8s USB %s", type_short, ver_str);
        TFT_DrawString_Font(4, 34, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x7);

        /* --- VID/PID (y=43) --- */
        sprintf(buf, "VID:0x%04X  PID:0x%04X", result->vendor_id, result->product_id);
        TFT_DrawString_Font(4, 43, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x7);

        /* --- 线缆特性 (y=52): SBU/延迟/最大VBUS --- */
        uint8_t latency = (uint8_t)((result->cable_vdo1 & CABLE_VDO1_LATENCY_MASK) >> 13);
        uint8_t maxv = (uint8_t)((result->cable_vdo1 & CABLE_VDO1_MAX_VBUS_MASK) >> 9);
        uint8_t sbu = (result->cable_vdo1 & CABLE_VDO1_SBU_SUPPORTED) ? 1 : 0;
        const char *maxv_str;
        switch (maxv) {
            case 0: maxv_str = "20V"; break;
            case 1: maxv_str = "30V"; break;
            case 2: maxv_str = "40V"; break;
            case 3: maxv_str = "50V"; break;
            default:maxv_str = "?";   break;
        }
        sprintf(buf, "SBU:%s  Lat:%u  MaxV:%s", sbu ? "Y" : "N", latency, maxv_str);
        TFT_DrawString_Font(4, 52, buf, COLOR_CYAN, COLOR_BLACK, FONT_5x7);

        /* --- 原始VDO (y=61, 70) --- */
        sprintf(buf, "VDO1:0x%08X", result->cable_vdo1);
        TFT_DrawString_Font(4, 61, buf, COLOR_GRAY, COLOR_BLACK, FONT_5x7);

        if (result->cable_vdo2 != 0) {
            sprintf(buf, "VDO2:0x%08X", result->cable_vdo2);
        } else {
            sprintf(buf, "%-16s", "");
        }
        TFT_DrawString_Font(4, 70, buf, COLOR_GRAY, COLOR_BLACK, FONT_5x7);

    } else if (result->has_emark && !result->emarker_valid) {
        TFT_DrawString_Font(5, 15, "PD Comm Failed", COLOR_RED, COLOR_BLACK, FONT_12x24);
        TFT_DrawString_Font(4, 43, "Basic Cable Info     ", COLOR_GRAY, COLOR_BLACK, FONT_5x7);
    } else {
        TFT_DrawString_Font(5, 15, "No E-Mark Chip", COLOR_GRAY, COLOR_BLACK, FONT_12x24);
        TFT_DrawString_Font(4, 43, "Basic USB Cable      ", COLOR_GRAY, COLOR_BLACK, FONT_5x7);
    }
}

/*
 * TFT显示测试结果分发器
 */
void TFT_ShowTestResult(test_result_t *result, display_mode_t mode)
{
    static display_mode_t last_mode = MODE_CONTINUITY;

    if (mode != last_mode) {
        TFT_Clear(COLOR_BLACK);
        last_mode = mode;
    }

    switch (mode) {
    case MODE_CONTINUITY:
        TFT_ShowContinuity(result);
        break;
    case MODE_EMARK:
        TFT_ShowEmark(result);
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
