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

#ifdef __cplusplus
extern "C" {
#endif

/* 测试结果结构 */
typedef struct {
    uint8_t power_ok;          // VBUS和GND检测
    uint8_t cc1_ok;            // CC1检测（合并测试，与cc2_ok同值）
    uint8_t cc2_ok;            // CC2检测
    uint8_t dp_dm_ok;          // D+和D-合并
    uint8_t tx1_ok;            // TX1
    uint8_t rx1_ok;            // RX1
    uint8_t tx2_ok;            // TX2
    uint8_t rx2_ok;            // RX2
    uint8_t sbu_ok;            // SBU
    uint8_t shield_ok;         // 屏蔽层(EH)
    uint32_t resistance_mohm;  // 电阻值(mΩ)
    uint8_t has_emark;
    uint8_t fast_charge;       // 0:无, 1:Huawei单CC, 2:Huawei双CC

    // E-Mark详细信息
    uint8_t emarker_valid;
    uint8_t cable_type;
    uint8_t usb_speed;
    uint8_t current_capability;
    uint8_t epr_capable;
    uint8_t usb_version;          // USB版本 (Cable VDO1 Bit23-21)
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t id_header;           // ID Header原始值
    uint32_t cert_stat;           // Cert Stat VDO
    uint32_t product_vdo;         // Product VDO
    uint32_t cable_vdo1;          // Cable VDO1原始值
    uint32_t cable_vdo2;          // Cable VDO2原始值
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
