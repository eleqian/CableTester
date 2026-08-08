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
 *   V_PWR_H (PB1) = 未放大直接ADC    → 900mΩ~10kΩ（近10kΩ处分辨率~240mΩ/LSB）
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

#define RES_CABLE_MAX         10000000    /* 有效测量上限(mΩ) = 10kΩ             */
#define RES_SATURATED         (RES_CABLE_MAX + 1)  /* 双通道饱和返回值 → OL     */
#define RES_DISPLAY_MAX_MOHM  RES_CABLE_MAX  /* 显示上限(mΩ): >=10kΩ时显示"----" */

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

/* E-Mark信息复位: 全部清零, 线缆类型置未知(2) */
static void Reset_Emark_Info(emarker_info_t *info)
{
    memset(info, 0, sizeof(emarker_info_t));
    info->cable_type = CABLE_TYPE_UNKNOWN;  /* 未知 */
}

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
    test_result.fast_charge = FAST_CHARGE_NONE;
    Reset_Emark_Info(&test_result.emark);

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
 * CC信号：合并检测（无CC_R，PA1/PA2直接驱动）。
 *   PA1↔PA2间5.1kΩ耦合导致无法独立分辨CC1/CC2，
 *   Type-C正反插本就只通一根CC，合并检测即可。
 *   双向驱动互读：右口拉低→读左口 + 左口拉低→读右口，
 *   兼容E-Mark电缆（芯片Rd会持续拉低其所在CC线，旧"释放恢复高"
 *   判定会被两根CC线同时拉低而误报不通），且能拒绝单端E-Mark假连通
 *   （无线缆时右口两根CC脚均为高阻，读不到左口拉低）。
 *
 * 其他信号：环回法，引脚配置为上拉输入。
 *   若线缆连通，上拉输入被右侧接口短接GND拉低；
 *   若开路，保持高电平。
 */
void Test_Cable_Continuity(test_result_t *result)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    result->power_ok = 0;

    /* PC14/PC15配置为上拉输入（左口读方向） */
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_PIN | USB3_CC2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    Delay_Ms(1);

    /* ① 右口拉低 → 读左口（有线缆则左口被拉低） */
    CC_E_Set_Output_Low();
    Delay_Ms(1);
    uint8_t cc1_r_low = GPIO_ReadInputDataBit(GPIOC, USB3_CC1_PIN);
    uint8_t cc2_r_low = GPIO_ReadInputDataBit(GPIOC, USB3_CC2_PIN);

    /* ② 右口释放；左口PC3/PC0推挽输出低 → 读右口（反向验证，防单端E-Mark假连通）
     * 注意：PC3/PC0与PC14/PC15同属左口CC1/CC2网络，但PC14/PC15受USBPD PHY
     * 控制（残留CC_PU_330恒流源/CC_SEL通道输出级会顶住GPIO推挽拉低），
     * 故必须用PC3/PC0（普通GPIO）拉低网络 */
    CC_E_Set_Floating();
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_AUX_PIN | USB3_CC2_AUX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOC, USB3_CC1_AUX_PIN | USB3_CC2_AUX_PIN);
    Delay_Ms(1);

    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN | USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    Delay_Ms(1);
    uint8_t cc1_l_low = GPIO_ReadInputDataBit(GPIOA, USB4_CC_E1_PIN); /* 右口CC1(PA2) */
    uint8_t cc2_l_low = GPIO_ReadInputDataBit(GPIOA, USB4_CC_E2_PIN); /* 右口CC2(PA1) */

    /* ③ 双端连通判定：右拉低→左低 且 左拉低→右低 = 真正双端连通
     *    注意：单端E-Mark（左口仅有芯片Rd、无线缆）在②中右口读不到
     *    左口拉低（高阻），自动判为不通，替代旧"释放恢复高"防假逻辑 */
    result->cc1_ok = (cc1_r_low == Bit_RESET && cc1_l_low == Bit_RESET) ? 1 : 0;
    result->cc2_ok = (cc2_r_low == Bit_RESET && cc2_l_low == Bit_RESET) ? 1 : 0;

    /* 恢复PC14/PC15、PC3/PC0、PA1/PA2为浮空（USBPD模块/其他测试需要） */
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_PIN | USB3_CC2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_AUX_PIN | USB3_CC2_AUX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN | USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

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
        Reset_Emark_Info(&result->emark);
        return;
    }

    /* 通过USB PD协议发送Discover Identity，读取完整E-Mark信息 */
    ret = EMarker_Full_Detect(&emark_info);

    if (ret == EMARK_OK && emark_info.valid) {
        /* 嵌套结构整体拷贝, 一次赋值完成全部字段 */
        result->emark = emark_info;
    } else {
        Reset_Emark_Info(&result->emark);
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
        return FAST_CHARGE_HUAWEI_1CC;
    } else if (voltage_mv <= 2900 && voltage_mv >= 2700) {
        return FAST_CHARGE_HUAWEI_2CC;
    }
    return FAST_CHARGE_NONE;
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
        /* 超过10kΩ量程判power_ok=0 → 显示"----" */
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
                if (!test_result.emark.valid) {
                    Detect_Emark_Info(&test_result);
                }
                test_result.has_emark = 1;
            } else {
                if (test_result.has_emark) {
                    test_result.has_emark = 0;
                    Reset_Emark_Info(&test_result.emark);
                }
            }
            use_v_pwr_l_channel = 1;  /* 每周期从V_PWR_L高精度通道开始 */
            resistance_state = RESISTANCE_STATE_MOS_ON;
            test_state = TEST_STATE_RUNNING;
            break;

        case MODE_EMARK:
            CC_E_Set_Floating();  /* 检测前先释放右口CC总线；通信所需Rd由EMarker_Full_Detect内部使能 */
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
 *   <1000 mΩ  → "xxx"      (毫欧整数, 右对齐到4字符槽)
 *   1~9.99Ω   → "x.xx"     (欧姆，两位小数, 右对齐到5字符槽)
 *   10~99.9Ω  → "xx.x"     (欧姆，一位小数, 右对齐到5字符槽)
 *   100~999Ω  → "100"~"999"(欧姆整数, 右对齐到5字符槽)
 *   1k~9.99k  → "1.00"~"9.99" (千欧, 0.01kΩ单位, 恒4字符)
 *   >=10kΩ/OL → "----"     (超量程/开路, 右对齐到5字符槽)
 * 数值槽位+1空格+单位合计恒7字符(4+1+2 / 5+1+1 / 4+1+2), 保证各量程显示宽度对齐
 *
 * @param buf: 输出缓冲区（至少6字节）
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
    } else if (res_mohm < 100000) {
        uint16_t whole = (uint16_t)(res_mohm / 1000);
        uint16_t frac = (uint16_t)((res_mohm % 1000) / 100);
        sprintf(tmp, "%u.%u", whole, frac);
    } else if (res_mohm < 1000000) {
        /* 100~999Ω: 整数显示（保留欧姆单位） */
        sprintf(tmp, "%lu", res_mohm / 1000);
    } else if (res_mohm < RES_DISPLAY_MAX_MOHM) {
        /* 千欧: val = 阻值/0.01kΩ (100..999) → "1.00"~"9.99" */
        uint16_t val = (uint16_t)(res_mohm / 10000);
        sprintf(tmp, "%u.%02u", val / 100, val % 100);
    } else {
        strcpy(tmp, "----");
    }
    /* 各档右对齐槽位: 毫欧/千欧=4字符, 欧姆/OL=5字符; 加单位后合计恒7字符,
       每次重绘覆盖全部7个字符位置, 无残留 */
    uint8_t len = (uint8_t)strlen(tmp);
    uint8_t slot;
    if (!power_ok) {
        slot = 5;
    } else if (res_mohm < 1000) {
        slot = 4;
    } else if (res_mohm < 1000000) {
        slot = 5;
    } else {
        slot = 4;  /* 千欧 "1.00"~"9.99" */
    }
    uint8_t pad = slot - len;
    uint8_t i;
    for (i = 0; i < pad; i++) buf[i] = ' ';
    for (i = 0; i < len; i++) buf[pad + i] = tmp[i];
    buf[slot] = '\0';
}

/*
 * 电阻单位格式化（恒白色显示, 前缀固定1空格与数值分隔）
 *   <1000 mΩ  → " mΩ"   (毫欧, 3字符)
 *   1Ω~999Ω   → " Ω"    (欧姆, 2字符)
 *   1k~9.99k  → " kΩ"   (千欧, 3字符)
 *   OL        → "  "    (空白, 2字符)
 * 数值槽位+1空格+单位合计恒7字符(4+1+2 / 5+1+1 / 4+1+2), 保证各量程宽度对齐
 *
 * @param buf: 输出缓冲区（至少4字节）
 * @param res_mohm: 电阻值(mΩ)
 * @param power_ok: 电源状态
 */
static void Format_Resistance_Unit(char *buf, uint32_t res_mohm, uint8_t power_ok)
{
    if (!power_ok) {
        buf[0] = ' '; buf[1] = ' '; buf[2] = '\0';  /* 空格间隙+空槽, 合计7字符防残留 */
        return;
    }
    if (res_mohm < 1000) {
        buf[0] = ' '; buf[1] = 'm'; buf[2] = (char)CHAR_OMEGA; buf[3] = '\0';
    } else if (res_mohm < 1000000) {
        buf[0] = ' '; buf[1] = (char)CHAR_OMEGA; buf[2] = '\0';
    } else {
        buf[0] = ' '; buf[1] = 'k'; buf[2] = (char)CHAR_OMEGA; buf[3] = '\0';
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
    TFT_DrawString_Font(x + 8, y, label, COLOR_WHITE, COLOR_BLACK, FONT_5x8);
}

/*
 * 通断模式显示 (160x80 横屏)
 * ┌─────────────────────────────────────┐ y=0
 * │ ●USB 3.2             Huawei 5A      │ ← 状态栏 (5x8)
 * ├─────────────────────────────────────┤ y=9
 * │      R: 15.6mΩ                      │ ← 电阻值 (12x24 居中, 数值4档色/单位白色)
 * ├─────────────────────────────────────┤ y=40
 * │ ■D+-  ■CC   ■SBU  ■SHLD             │ ← 信号线 (5x8)
 * │ ■TX1  ■RX1  ■TX2  ■RX2              │
 * ├─────────────────────────────────────┤ y=64
 * │ E-Marker: 5A       USB3.2G2         │ ← 底部信息 (5x8)
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
        sprintf(class_buf, "%-11s", "POWER ONLY");
        class_color = COLOR_ORANGE; indicator_color = COLOR_ORANGE;
    } else {
        uint8_t usb3_lanes = 0;
        if (result->tx1_ok && result->rx1_ok) usb3_lanes++;
        if (result->tx2_ok && result->rx2_ok) usb3_lanes++;
        if ((result->cc1_ok || result->cc2_ok) && result->sbu_ok && result->shield_ok && usb3_lanes == 2) {
            sprintf(class_buf, "%-11s", "TYPE-C FULL");
            class_color = COLOR_GREEN; indicator_color = COLOR_GREEN;
        } else if (usb3_lanes == 2) {
            sprintf(class_buf, "%-11s", "USB 3.2");
            class_color = COLOR_CYAN; indicator_color = COLOR_CYAN;
        } else if (usb3_lanes == 1) {
            sprintf(class_buf, "%-11s", "USB 3.0");
            class_color = COLOR_CYAN; indicator_color = COLOR_CYAN;
        } else {
            sprintf(class_buf, "%-11s", "USB 2.0");
            class_color = COLOR_YELLOW; indicator_color = COLOR_YELLOW;
        }
    }
    TFT_FillRect(3, 1, 5, 5, indicator_color);
    TFT_DrawString_Font(12, 0, class_buf, class_color, COLOR_BLACK, FONT_5x8);

    char charge_buf[10];
    uint16_t charge_color = COLOR_GRAY;
    if (result->fast_charge != FAST_CHARGE_NONE) {
        sprintf(charge_buf, "%-9s", "HUAWEI 5A");
        charge_color = (result->fast_charge == FAST_CHARGE_HUAWEI_1CC) ? COLOR_PURPLE : COLOR_ORANGE;  /* 紫色=华为5A线缆插头色 */
    } else {
        sprintf(charge_buf, "%-9s", "");
        charge_color = COLOR_BLACK;
    }
    TFT_DrawString_Font(106, 0, charge_buf, charge_color, COLOR_BLACK, FONT_5x8);

    TFT_FillRect(0, 10, 160, 1, COLOR_DARK_GRAY);

    /* --- 分区2：电阻值 (y=13, 12x24 居中) --- */
    char rval[8], runit[4];
    Format_Resistance_Value(rval, result->resistance_mohm, result->power_ok);
    Format_Resistance_Unit(runit, result->resistance_mohm, result->power_ok);
    uint16_t r_color = Get_Resistance_Color(result->resistance_mohm, result->power_ok);
    TFT_DrawString_Font(15, 13, "R:", COLOR_WHITE, COLOR_BLACK, FONT_12x24);
    /* 数值+1空格+单位合计恒7字符(13px/字符): 毫欧"123 mΩ"与千欧"9.99 kΩ"总宽对齐, 起点同为x=54 */
    TFT_DrawString_Font(54, 13, rval, r_color, COLOR_BLACK, FONT_12x24);
    TFT_DrawString_Font((uint16_t)(54 + strlen(rval) * 13), 13, runit,
                        COLOR_WHITE, COLOR_BLACK, FONT_12x24);

    TFT_FillRect(0, 40, 160, 1, COLOR_DARK_GRAY);

    /* --- 分区3：信号线 (y=45/54, 5x8) --- */
    Draw_Signal_Indicator(4,  45, "D+-", result->dp_dm_ok);
    Draw_Signal_Indicator(42, 45, "CC",  result->cc1_ok || result->cc2_ok);
    Draw_Signal_Indicator(80, 45, "SBU", result->sbu_ok);
    Draw_Signal_Indicator(118,45, "SHLD",result->shield_ok);
    Draw_Signal_Indicator(4,  54, "TX1", result->tx1_ok);
    Draw_Signal_Indicator(42, 54, "RX1", result->rx1_ok);
    Draw_Signal_Indicator(80, 54, "TX2", result->tx2_ok);
    Draw_Signal_Indicator(118,54, "RX2", result->rx2_ok);

    TFT_FillRect(0, 64, 160, 1, COLOR_DARK_GRAY);

    /* --- 分区4：底部信息栏 (y=68, 5x8) --- */
    char em_left[16];
    uint16_t em_color;
    if (result->has_emark && result->emark.valid) {
        const char *cur_str = (result->emark.current_capability == CABLE_CURRENT_VBUS) ? "5A" : "3A";
        sprintf(em_left, "E-Mark: %-2s ", cur_str);  /* 固定11字符宽度 */
        em_color = COLOR_GREEN;
    } else if (result->has_emark) {
        sprintf(em_left, "%-11s", "E-Mark: YES");
        em_color = COLOR_ORANGE;  /* 有E-Mark但读取失败 → 橙色警告 */
    } else {
        sprintf(em_left, "%-11s", "E-Mark: NO");
        em_color = COLOR_GRAY;
    }
    TFT_DrawString_Font(4, 68, em_left, em_color, COLOR_BLACK, FONT_5x8);
    if (result->has_emark && result->emark.valid) {
        const char *speed_str;
        uint16_t speed_color = COLOR_CYAN;
        switch (result->emark.usb_speed) {
            case USB_SPEED_USB20_ONLY: speed_str = "USB2.0   "; speed_color = COLOR_YELLOW; break;
            case USB_SPEED_USB32_GEN1: speed_str = "USB3.2 G1"; speed_color = COLOR_CYAN;  break;
            case USB_SPEED_USB32_GEN2: speed_str = "USB3.2 G2"; speed_color = COLOR_CYAN;  break;
            case USB_SPEED_USB4_GEN3:  speed_str = "USB4 G3  "; speed_color = COLOR_GREEN; break;
            case USB_SPEED_USB4_GEN4:  speed_str = "USB4 G4  "; speed_color = COLOR_GREEN; break;
            default:   speed_str = "         "; speed_color = COLOR_BLACK; break;
        }
        /* 与E-Mark模式同一速度色阶: 2.0=黄 / G1G2=青 / G3G4=绿 */
        TFT_DrawString_Font(84, 68, speed_str, speed_color, COLOR_BLACK, FONT_5x8);
    } else {
        TFT_DrawString_Font(84, 68, "         ", COLOR_BLACK, COLOR_BLACK, FONT_5x8);
    }
}

/*
 * 线缆长度近似值（由Cable VDO1 Latency位 16:13 换算，见USB PD spec）
 * 信号延迟近似对应线缆长度：
 *   0000b无效 / 0001b~0111b = 1m~7m / 1000b~1010b = 100m~300m / 1011b~1111b无效
 */
static const char *const lat_len_str[16] = {
    "?",     /* 0000: 无效 */
    "~1m",   /* <10ns     */
    "~2m",   /* 10-20ns   */
    "~3m",   /* 20-30ns   */
    "~4m",   /* 30-40ns   */
    "~5m",   /* 40-50ns   */
    "~6m",   /* 50-60ns   */
    "~7m",   /* 60-70ns   */
    "~100m", /* 1000ns    */
    "~200m", /* 2000ns    */
    "~300m", /* 3000ns    */
    "?", "?", "?", "?", "?"  /* 1011-1111: 无效 */
};

/*
 * E-Mark模式显示 (160x80 横屏, 详尽模式)
 * ┌─────────────────────────────────────┐ y=0
 * │ E-Mark: YES             Passive     │ ← 标题 + 线缆类型 (5x8)
 * ├─────────────────────────────────────┤ y=9
 * │ USB 3.2 Gen2              5A        │ ← 速度+电流 (8x16)
 * ├─────────────────────────────────────┤ y=30
 * │ EPR:Yes   MaxV:20V                  │ ← 2列详细字段(左x=4/右x=82)
 * │ SBU:Yes   Len:~1m                   │
 * │ VID:0xFFFF PID:0xFFFF               │
 * │ HW:0x1    FW:0x1                    │
 * │ VDO:12345678 87654321               │ ← 原始VDO合并一行
 * └─────────────────────────────────────┘ y=79
 * 配色统一规则: 标签白色 / 能力值绿(支持)或灰(不支持) / 中性数据白 / 原始数据灰
 */
void TFT_ShowEmark(test_result_t *result)
{
    char buf[24];
    static uint8_t last_has_emark = 0;
    if (result->has_emark != last_has_emark) {
        TFT_FillRect(0, 13, 160, 66, COLOR_BLACK);
        last_has_emark = result->has_emark;
    }

    /* --- 标题栏 (y=0): E-Mark状态 + 线缆类型(右侧) --- */
    TFT_DrawString_Font(4, 0, "E-Mark:", COLOR_WHITE, COLOR_BLACK, FONT_5x8);
    TFT_DrawString_Font(56, 0, result->has_emark ? "YES" : "NO ",
                         result->has_emark ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, FONT_5x8);
    /* 类型固定8字符槽(%-8s)防残留; 无效时画空槽 */
    const char *type_short = "";
    if (result->has_emark && result->emark.valid) {
        switch (result->emark.cable_type) {
            case CABLE_TYPE_PASSIVE: type_short = "Passive"; break;
            case CABLE_TYPE_ACTIVE:  type_short = "Active";  break;
            default:type_short = "Unknown"; break;
        }
    }
    sprintf(buf, "%-8s", type_short);
    TFT_DrawString_Font(106, 0, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);
    TFT_FillRect(0, 10, 160, 1, COLOR_DARK_GRAY);

    if (result->has_emark && result->emark.valid) {
        /* --- 速度 + 电流 (y=13, 8x16) --- */
        const char *speed_str;
        uint16_t speed_color = COLOR_CYAN;
        switch (result->emark.usb_speed) {
            case USB_SPEED_USB20_ONLY: speed_str = "USB2.0";      speed_color = COLOR_YELLOW; break;
            case USB_SPEED_USB32_GEN1: speed_str = "USB3.2 Gen1"; speed_color = COLOR_CYAN;  break;
            case USB_SPEED_USB32_GEN2: speed_str = "USB3.2 Gen2"; speed_color = COLOR_CYAN;  break;
            case USB_SPEED_USB4_GEN3:  speed_str = "USB4 Gen3";   speed_color = COLOR_GREEN; break;
            case USB_SPEED_USB4_GEN4:  speed_str = "USB4 Gen4";   speed_color = COLOR_GREEN; break;
            default:   speed_str = "Unknown";     speed_color = COLOR_GRAY;  break;
        }
        sprintf(buf, "%-14s", speed_str);
        TFT_DrawString_Font(5, 13, buf, speed_color, COLOR_BLACK, FONT_8x16);

        const char *cur_str = (result->emark.current_capability == CABLE_CURRENT_VBUS) ? "5A" : "3A";
        uint16_t cur_color = (result->emark.current_capability == CABLE_CURRENT_VBUS) ? COLOR_GREEN : COLOR_YELLOW;
        TFT_DrawString_Font(130, 13, cur_str, cur_color, COLOR_BLACK, FONT_8x16);

        TFT_FillRect(0, 30, 160, 1, COLOR_DARK_GRAY);

        /* --- 2列详细字段 (y=34/43/52/61): 左列x=4, 右列x=82对齐 --- */
        uint8_t maxv = (uint8_t)((result->emark.cable_vdo1 & CABLE_VDO1_MAX_VBUS_MASK) >> 9);
        uint8_t latency = (uint8_t)((result->emark.cable_vdo1 & CABLE_VDO1_LATENCY_MASK) >> 13);
        uint8_t sbu = (result->emark.cable_vdo1 & CABLE_VDO1_SBU_SUPPORTED) ? 1 : 0;
        uint8_t hw_ver = (uint8_t)((result->emark.cable_vdo1 & CABLE_VDO1_HW_VERSION_MASK) >> 28);
        uint8_t fw_ver = (uint8_t)((result->emark.cable_vdo1 & CABLE_VDO1_FW_VERSION_MASK) >> 24);
        const char *maxv_str;
        switch (maxv) {
            case CABLE_MAXV_20V: maxv_str = "20V"; break;
            case CABLE_MAXV_50V: maxv_str = "50V"; break;
            default:maxv_str = "20V"; break;
        }

        /* EPR + MaxV */
        sprintf(buf, "EPR:");
        TFT_DrawString_Font(4, 34, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);
        sprintf(buf, "%-3s", result->emark.epr_capable ? "Yes" : "No");
        TFT_DrawString_Font(28, 34, buf, result->emark.epr_capable ? COLOR_GREEN : COLOR_GRAY,
                             COLOR_BLACK, FONT_5x8);
        sprintf(buf, "MaxV:%-3s", maxv_str);
        TFT_DrawString_Font(82, 34, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);

        /* SBU + Len */
        sprintf(buf, "SBU:");
        TFT_DrawString_Font(4, 43, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);
        sprintf(buf, "%-3s", sbu ? "Yes" : "No");
        TFT_DrawString_Font(28, 43, buf, sbu ? COLOR_GREEN : COLOR_GRAY,
                             COLOR_BLACK, FONT_5x8);
        sprintf(buf, "Len:%-5s", lat_len_str[latency]);
        TFT_DrawString_Font(82, 43, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);

        /* VID + PID */
        sprintf(buf, "VID:0x%04X", result->emark.vendor_id);
        TFT_DrawString_Font(4, 52, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);
        sprintf(buf, "PID:0x%04X", result->emark.product_id);
        TFT_DrawString_Font(82, 52, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);

        /* HW + FW */
        sprintf(buf, "HW :0x%X", hw_ver);
        TFT_DrawString_Font(4, 61, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);
        sprintf(buf, "FW :0x%X", fw_ver);
        TFT_DrawString_Font(82, 61, buf, COLOR_WHITE, COLOR_BLACK, FONT_5x8);

        /* --- 原始VDO合并一行 (y=70) --- */
        /* VDO:%08X(13字符) + %08X(8字符) = 21字符; VDO2缺失时补8空格固定宽度 */
        if (result->emark.cable_vdo2 != 0) {
            sprintf(buf, "VDO:%08X %08X", result->emark.cable_vdo1, result->emark.cable_vdo2);
        } else {
            sprintf(buf, "VDO:%08X %8s", result->emark.cable_vdo1, "");
        }
        TFT_DrawString_Font(4, 70, buf, COLOR_GRAY, COLOR_BLACK, FONT_5x8);
    } else if (result->has_emark && !result->emark.valid) {
        TFT_DrawString_Font(5, 13, "PD Comm Failed", COLOR_RED, COLOR_BLACK, FONT_8x16);
    } else {
        TFT_DrawString_Font(5, 13, "No E-Mark Chip", COLOR_GRAY, COLOR_BLACK, FONT_8x16);
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
    TFT_DrawBorder(COLOR_CYAN);                          /* 青色边框(唯一青色强调) */
    /* 标题 */
    TFT_DrawString_Font(8, 14, "USB ", COLOR_CYAN, COLOR_BLACK, FONT_8x16);
    TFT_DrawString_Font(44, 14, "Cable Tester", COLOR_CYAN, COLOR_BLACK, FONT_8x16);
    /* 分隔线 */
    TFT_FillRect(3, 40, 154, 1, COLOR_DARK_GRAY);
    /* 版本+作者 */
    TFT_DrawString_Font(32, 50, "V2.2", COLOR_CYAN, COLOR_BLACK, FONT_5x8);
    TFT_DrawString_Font(56, 50, "  by eleqian", COLOR_WHITE, COLOR_BLACK, FONT_5x8);
    /* 模拟进度条: 深灰底 + 青色填充动画 (20步×50ms = 1s) */
    TFT_FillRect(10, 68, 140, 6, COLOR_DARK_GRAY);
    for (uint8_t i = 1; i <= 20; i++) {
        TFT_FillRect(11, 69, (uint16_t)(i * 138) / 20, 4, COLOR_CYAN);
        Delay_Ms(50);
    }
    TFT_Clear(COLOR_BLACK);
}
