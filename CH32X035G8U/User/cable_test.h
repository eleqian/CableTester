/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_test.h
 * Author             : CableTester Team
 * Version            : V3.1
 * Date               : 2026/05/17
 * Description        : 线缆测试核心功能头文件（包含状态机和按键处理）
 *********************************************************************************/

#ifndef __CABLE_TEST_H
#define __CABLE_TEST_H

#include "ch32x035.h"
#include "cable_tft.h"
#include "emarker_pd.h"  // emarker_info_t: 嵌套进test_result_t, 整体拷贝赋值

#ifdef __cplusplus
extern "C" {
#endif

/* 快充检测类型 */
typedef enum {
    FAST_CHARGE_NONE = 0,    // 无快充
    FAST_CHARGE_HUAWEI_1CC,  // Huawei单CC检测(2.9~3.1V)
    FAST_CHARGE_HUAWEI_2CC   // Huawei双CC检测(2.7~2.9V)
} fast_charge_type_t;

/* E-Mark 读取状态
 * 用于界面区分「正在读取」和「真的失败」：旧实现只看 emark.valid，
 * 切到 E-Mark 页的第一帧 valid 还是 0，会先闪一下 "PD Comm Failed"。 */
#define EMARK_STATE_IDLE     0u  // 未开始（刚插入/刚切页，尚未发起事务）
#define EMARK_STATE_READING  1u  // 读取中（分步重试推进中）
#define EMARK_STATE_OK       2u  // 读取成功
#define EMARK_STATE_FAIL     3u  // 一轮候选全部失败（之后会按间隔自动重试）

/* 测试结果结构 */
typedef struct {
    uint8_t power_ok;          // VBUS和GND检测: 1=回路通(测量有效), 0=开路
    uint8_t cc1_ok;            // CC1检测
    uint8_t cc2_ok;            // CC2检测
    uint8_t dp_dm_ok;          // D+和D-合并检测
    uint8_t tx1_ok;            // TX1检测
    uint8_t rx1_ok;            // RX1检测
    uint8_t tx2_ok;            // TX2检测
    uint8_t rx2_ok;            // RX2检测
    uint8_t sbu_ok;            // SBU检测
    uint8_t shield_ok;         // 屏蔽层(EH)检测
    uint32_t resistance_mohm;  // 电阻值(mΩ), 开路时为RES_SATURATED
    uint8_t has_emark;         // 是否检测到E-Mark芯片: 1=有, 0=无
    uint8_t fast_charge;       // 快充检测: 见fast_charge_type_t枚举
    uint8_t emark_state;       // E-Mark读取状态: 见 EMARK_STATE_*

    // E-Mark详细信息（仅has_emark && emark.valid时有效）
    emarker_info_t emark;      // E-Mark芯片信息（嵌套结构, 直接整体拷贝, 避免逐字段冗余赋值）
} test_result_t;

/* 显示模式枚举（V3.0：仅两种模式） */
typedef enum {
    MODE_CONTINUITY = 0,  // 通断 + 电阻 + 快充（合并显示）
    MODE_EMARK,           // E-mark芯片信息
    MODE_MAX
} display_mode_t;

extern volatile uint32_t system_tick_ms;
extern test_result_t test_result;
extern display_mode_t display_mode;

/* 时序调试计数开关：1=统计主循环最大间隔 / 检测 / 绘制耗时（GDB 回读），0=关闭（默认）
 * 关闭时这些统计变量不分配、更新点编译为空，不影响时序。 */
#ifndef CABLE_DEBUG_TIMING
#define CABLE_DEBUG_TIMING   0
#endif

#if CABLE_DEBUG_TIMING
extern volatile uint32_t g_loop_max_gap;       /* 主循环最大间隔(ms)，即按键最坏延迟 */
extern volatile uint32_t g_loop_max_gap_mark;  /* 出现最大间隔时的 tick */
extern volatile uint32_t g_det_max_ms;         /* 在线检测最坏耗时 */
extern volatile uint32_t g_draw_max_ms;        /* 单帧绘制最坏耗时 */
#define TIMING_MAX(var, val)   do { uint32_t _tv = (val); if (_tv > (var)) (var) = _tv; } while (0)
#else
#define TIMING_MAX(var, val)   do { (void)(val); } while (0)
#endif

void Cable_Test_Init(void);
void Test_Cable_Continuity(test_result_t *result);
void Test_Cable_Resistance(test_result_t *result);
uint32_t Get_Cable_Resistance_Single(void);
void Detect_Emark_Info(test_result_t *result);
void Detect_Fast_Charge_Info(test_result_t *result);
uint8_t Key_Scan_With_Debounce(void);
void Switch_Display_Mode(void);
void Test_State_Machine(void);
void TFT_ShowTestResult(test_result_t *result, display_mode_t mode);
void TFT_ShowContinuity(test_result_t *result);
void TFT_ShowEmark(test_result_t *result);
void TFT_ShowWelcome(void);

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_TEST_H */
