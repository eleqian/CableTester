/********************************** (C) COPYRIGHT *******************************
 * File Name          : cable_test.h
 * Author             : CableTester Team
 * Version            : V3.0
 * Date               : 2026/05/01
 * Description        : 线缆测试核心功能头文件（包含状态机和按键处理）
 *
 * 模块说明：
 * - 通断测试：检测所有信号线的连通性
 * - 电阻测量：计算VBUS/GND回路电阻（状态机实现）
 * - E-mark检测：识别E-mark芯片并控制VCONN
 * - 快充识别：检测VOOC/Huawei快充线缆
 * - 结果显示：TFT显示测试结果
 * - 状态机管理：测试流程控制和按键处理
 *********************************************************************************/

#ifndef __CABLE_TEST_H
#define __CABLE_TEST_H

#include "ch32x035.h"
#include "cable_tft.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 测试结果结构 */
typedef struct {
    uint8_t power_ok;        // VBUS和GND检测，超量程时也为0
    uint8_t cc1_ok;          // CC1单独检测
    uint8_t cc2_ok;          // CC2单独检测
    uint8_t dp_dm_ok;        // D+和D-合并
    uint8_t tx1_ok;          // TX1单独
    uint8_t rx1_ok;          // RX1单独
    uint8_t tx2_ok;          // TX2单独
    uint8_t rx2_ok;          // RX2单独
    uint8_t sbu_ok;          // SBU
    uint8_t shield_ok;       // 屏蔽层(EH)连通性
    uint32_t resistance_mohm;  // 电阻值(毫欧 mΩ)，在power_ok=1时有效
    uint8_t has_emark;
    uint8_t fast_charge;  // 0:无, 1:VOOC, 2:Huawei

    // E-Mark详细信息（新增）
    uint8_t emarker_valid;      // E-Mark数据有效性
    uint8_t cable_type;         // 线缆类型: 0-被动, 1-主动, 2-未知
    uint8_t usb_speed;          // USB速度等级
    uint8_t current_capability; // 电流能力: 0-3A, 1-5A, 2-VBUS only
    uint8_t epr_capable;        // EPR支持
    uint16_t vendor_id;         // Vendor ID
    uint16_t product_id;        // Product ID
} test_result_t;

/* 显示模式枚举 */
typedef enum {
    MODE_CONTINUITY = 0,  // 通断测试 + 电阻测量 + 快充识别（合并模式）
    MODE_EMARK,           // E-mark芯片检测
    MODE_MAX
} display_mode_t;

/******************************************************************************
 *                    全局变量声明（由cable_test.c定义）
 ******************************************************************************/

/* 系统时间戳（ms）- 由SysTick中断更新 */
extern volatile uint32_t system_tick_ms;

/* 测试结果和显示模式 */
extern test_result_t test_result;
extern display_mode_t display_mode;

/******************************************************************************
 *                    初始化函数
 ******************************************************************************/

/* 测试模块初始化（在System_Init中调用） */
void Cable_Test_Init(void);

/******************************************************************************
 *                    测试函数声明
 ******************************************************************************/

/* 通断测试 */
void Test_Cable_Continuity(test_result_t *result);

/* 电阻测量（兼容接口，实际由状态机处理） */
void Test_Cable_Resistance(test_result_t *result);

/* 获取单次电阻测量值 */
uint32_t Get_Cable_Resistance_Single(void);

/* E-mark检测 */
void Detect_Emark_Info(test_result_t *result);

/* 快充识别 */
void Detect_Fast_Charge_Info(test_result_t *result);

/******************************************************************************
 *                    状态机和按键处理
 ******************************************************************************/

/* 按键扫描（带消抖，非阻塞） */
uint8_t Key_Scan_With_Debounce(void);

/* 切换显示模式 */
void Switch_Display_Mode(void);

/* 测试状态机主函数（在主循环中调用） */
void Test_State_Machine(void);

/******************************************************************************
 *                    显示函数声明
 ******************************************************************************/

/* 显示测试结果 */
void TFT_ShowTestResult(test_result_t *result, display_mode_t mode);

/* 显示欢迎界面 */
void TFT_ShowWelcome(void);

#ifdef __cplusplus
}
#endif

#endif /* __CABLE_TEST_H */
