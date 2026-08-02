/********************************** (C) COPYRIGHT *******************************
 * File Name          : emarker_pd.h
 * Author             : CableTester Team
 * Version            : V1.0
 * Date               : 2026/05/05
 * Description        : E-Mark芯片PD协议通信头文件
 *
 * 功能说明：
 * - 使用CH32X035内置USBPD模块与E-Mark芯片通信
 * - 发送Discover Identity命令读取E-Mark信息
 * - 解析VDM（Vendor Defined Message）响应数据
 * - 提取线缆类型、电流能力、USB版本等信息
 *********************************************************************************/

#ifndef __EMARKER_PD_H
#define __EMARKER_PD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ch32x035.h"
#include "ch32x035_usbpd.h"

/* E-Mark信息结构 */
typedef struct {
    uint8_t  valid;              // 数据有效性标志
    uint8_t  id_header_present;  // ID Header是否存在

    // Discover Identity响应数据（5个DWORD）
    uint32_t vdm_header;         // VDM Header
    uint32_t id_header;          // ID Header
    uint32_t cert_stat;          // Cert Stat VDO (可选)
    uint32_t product_vdo;        // Product VDO
    uint32_t cable_vdo1;         // Cable VDO 1
    uint32_t cable_vdo2;         // Cable VDO 2 (可选)

    // 解析后的信息
    uint8_t  cable_type;         // 线缆类型: 0-被动, 1-主动, 2-未知
    uint8_t  epr_capable;        // EPR支持: 0-不支持, 1-支持
    uint8_t  usb_speed;          // USB速度等级
    uint8_t  current_capability; // 电流能力: 3A或5A
    uint8_t  vdo_version;        // VDO版本
    uint16_t vendor_id;          // Vendor ID
    uint16_t product_id;         // USB Product ID (Product VDO Bit31-16)

} emarker_info_t;

/* USB PD消息类型定义 */
#define PD_MSG_TYPE_CONTROL       0x00
#define PD_MSG_TYPE_DATA          0x01

/* VDM相关定义 */
#define VDM_CMD_DISCOVER_IDENTITY 0x01
#define VDM_SID_USB_PD            0xFF00  // USB-IF Standard SID (16 bits, Bit 31-16)

/* VDM Header位定义 - According to USB PD Spec v3.2 Section 6.4.12.2 */
// Structured VDM Header format:
// Bit 31-16: SVID (Standard or Vendor ID)
// Bit 15:    VDM Type (1=Structured, 0=Unstructured)
// Bit 14-13: Structured VDM Version (01=Version 2.0)
// Bit 12-11: Reserved (0)
// Bit 10-8:  Object Position (3 bits, range 1-7)
// Bit 7-6:   Command Type (01=Initiator, 10=Responder)
// Bit 5:     Reserved (0 for Structured VDM)
// Bit 4-0:   Command (5 bits)

#define VDM_HDR_STD_VDM           (0x01 << 15)  // Structured VDM at Bit 15
#define VDM_HDR_VERSION_21        (0x01 << 13)  // VDM Version 2.1 at Bit 14-13
#define VDM_HDR_OBJ_POS_MASK      (0x07 << 8)   // Object Position mask (3 bits at Bit 10-8)
#define VDM_HDR_CMD_TYPE_MASK     (0x03 << 6)   // Command Type mask (Bit 7-6)
#define VDM_HDR_CMD_INITIATOR     (0x00 << 6)   // Initiator at Bit 7-6
/* Command Type Response values (for Responder, Bit Bit 7-6) */
#define VDM_HDR_CMD_ACK           (0x01 << 6)  // ACK
#define VDM_HDR_CMD_NAK           (0x02 << 6)  // NAK
#define VDM_HDR_CMD_BUSY          (0x03 << 6)  // BUSY

/* ID Header位定义 */
#define ID_HDR_PRODUCT_TYPE_MASK  (0x07 << 27)  // Product Type (Responder)
#define ID_HDR_MODAL_SUPPORTED    (0x01 << 26)  // Modal Operation Supported
#define ID_HDR_VENDOR_ID_MASK     (0xFFFF << 0) // Vendor ID

/* Product Type Values */
#define PRODUCT_TYPE_NOT_CABLE     0x00
#define PRODUCT_TYPE_PASSIVE_CABLE 0x03
#define PRODUCT_TYPE_ACTIVE_CABLE  0x04
#define PRODUCT_TYPE_VPD           0x06

/* Active Cable VDO1位定义 - According to USB PD 3.0 Spec */
// Note: This is for Active Cable, differs from Passive Cable
#define CABLE_VDO1_HW_VERSION_MASK   (0x0F << 28)  // Bit 31-28: Hardware Version
#define CABLE_VDO1_FW_VERSION_MASK   (0x0F << 24)  // Bit 27-24: Firmware Version
#define CABLE_VDO1_VDO_VERSION_MASK  (0x07 << 21)  // Bit 23-21: VDO Version
// Bit 20: Reserved
#define CABLE_VDO1_PLUG_TYPE_MASK    (0x03 << 18)  // Bit 19-18: USB Type-C plug to USB Type-C/Captive
#define CABLE_VDO1_EPR_CAPABLE       (0x01 << 17)  // Bit 17: EPR Capable
#define CABLE_VDO1_LATENCY_MASK      (0x0F << 13)  // Bit 16-13: Cable Latency (4 bits)
#define CABLE_VDO1_TERMINATION_TYPE  (0x03 << 11)  // Bit 12-11: Cable Termination Type (2 bits)
#define CABLE_VDO1_MAX_VBUS_MASK     (0x03 << 9)   // Bit 10-9: Maximum VBUS Voltage (2 bits)
#define CABLE_VDO1_SBU_SUPPORTED     (0x01 << 8)   // Bit 8: SBU Supported
#define CABLE_VDO1_SBU_TYPE          (0x01 << 7)   // Bit 7: SBU Type
#define CABLE_VDO1_CURRENT_MASK      (0x03 << 5)   // Bit 6-5: VBUS Current Handling (2 bits)
#define CABLE_VDO1_VBUS_THROUGH      (0x01 << 4)   // Bit 4: VBUS Through Cable
#define CABLE_VDO1_SOP2_PRESENT      (0x01 << 3)   // Bit 3: SOP'' Controller Present
#define CABLE_VDO1_USB_SPEED_MASK    (0x07 << 0)   // Bit 2-0: USB Highest Speed (3 bits)

/* USB Speed Values */
#define USB_SPEED_USB20_ONLY    0x00
#define USB_SPEED_USB32_GEN1    0x01
#define USB_SPEED_USB32_GEN2    0x02
#define USB_SPEED_USB4_GEN3     0x03
#define USB_SPEED_USB4_GEN4     0x04

/* Current Capability Values */
#define CABLE_CURRENT_3A      0x00  // 3A
#define CABLE_CURRENT_5A      0x01  // 5A
#define CABLE_CURRENT_VBUS    0x02  // VBUS only

/******************************************************************************
 *                    函数声明
 ******************************************************************************/

/**
 * @brief  初始化E-Mark PD通信模块
 * @param  vconn_cc: VCONN所在的CC通道 (1=CC1, 2=CC2)
 * @param  pd_comm_cc: PD通信使用的CC通道 (1=CC1, 2=CC2)
 * @return none
 * @note   根据VCONN和PD通信通道正确配置CC端口，并将CC_SEL设为pd_comm_cc
 */
void EMarker_PD_Init(uint8_t vconn_cc, uint8_t pd_comm_cc);

/**
 * @brief  检测E-Mark芯片是否存在
 * @return 1-存在E-mark, 0-不存在
 * @note   使用USBPD硬件比较器检测CC线上的Rd电阻
 */
uint8_t EMarker_Detect_Chip(void);

/**
 * @brief  检测E-Mark芯片所在的CC通道
 * @param  vconn_cc: 输出参数，VCONN应该在哪个CC (1=CC1, 2=CC2)
 * @param  pd_comm_cc: 输出参数，PD通信应该使用哪个CC (1=CC1, 2=CC2)
 * @return 1-存在E-mark, 0-不存在
 * @note   根据USB Type-C规范（本设计实测为逆关系）：
 *         - E-Mark在CC1上 → VCONN给CC1供电，PD通信使用CC2
 *         - E-Mark在CC2上 → VCONN给CC2供电，PD通信使用CC1
 *         注意：EMarker_PD_Init内部必须将CC_SEL设为pd_comm_cc，
 *         否则翻面后命令发往错误通道，表现为"检测到E-Marker但读取失败"（必现）
 */
uint8_t EMarker_Detect_CC_Channel(uint8_t *vconn_cc, uint8_t *pd_comm_cc);

/**
 * @brief  使能VCONN供电
 * @param  cc_channel: CC通道 (1=CC1/PC3, 2=CC2/PC0)
 * @note   根据CC通道选择对应的辅助引脚输出VCONN
 */
void EMarker_Enable_VCONN(uint8_t cc_channel);

/**
 * @brief  关闭VCONN供电
 * @return none
 */
void EMarker_Disable_VCONN(void);

/**
 * @brief  设置USBPD通信的CC通道
 * @param  cc_channel: CC通道 (1=CC1, 2=CC2)
 * @return none
 */
void EMarker_Set_CC_Channel(uint8_t cc_channel);

/**
 * @brief  检测并读取E-Mark芯片信息（底层API）
 * @param  vconn_cc: VCONN所在的CC通道 (1=CC1, 2=CC2)
 * @param  pd_comm_cc: PD通信使用的CC通道 (1=CC1, 2=CC2)
 * @param  info: E-Mark信息结构指针
 * @return 0-成功, 1-失败
 * @note   调用此函数前，应已使能VCONN供电
 * @note   通常使用EMarker_Full_Detect()高层API，此函数用于需要精细控制的场景
 */
uint8_t EMarker_Read_Info(uint8_t vconn_cc, uint8_t pd_comm_cc, emarker_info_t *info);

/**
 * @brief  完整的E-Mark检测流程（高层API）
 * @param  info: E-Mark信息结构指针
 * @return 0-成功, 1-无E-Mark, 2-PD通信失败
 * @note   自动完成检测、VCONN控制、PD通信全流程
 */
uint8_t EMarker_Full_Detect(emarker_info_t *info);

/**
 * @brief  发送Discover Identity命令
 * @return 0-成功, 1-失败
 */
uint8_t EMarker_Send_Discover_Identity(void);

/**
 * @brief  接收并解析VDM响应
 * @param  info: E-Mark信息结构指针
 * @return 0-成功, 1-失败
 */
uint8_t EMarker_Parse_Response(emarker_info_t *info);

/**
 * @brief  解析VDM Header
 * @param  vdm_header: VDM Header值
 * @param  info: E-Mark信息结构指针
 * @return none
 */
void EMarker_Parse_VDM_Header(uint32_t vdm_header, emarker_info_t *info);

/**
 * @brief  解析ID Header
 * @param  id_header: ID Header值
 * @param  info: E-Mark信息结构指针
 * @return none
 */
void EMarker_Parse_ID_Header(uint32_t id_header, emarker_info_t *info);

/**
 * @brief  解析Cable VDO1
 * @param  cable_vdo1: Cable VDO1值
 * @param  info: E-Mark信息结构指针
 * @return none
 */
void EMarker_Parse_Cable_VDO1(uint32_t cable_vdo1, emarker_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* __EMARKER_PD_H */
