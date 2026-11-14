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

    // 解析后的信息（Passive / Active 共用部分）
    uint8_t  cable_type;         // 线缆类型: 0-被动, 1-主动, 2-未知
    uint8_t  epr_capable;        // EPR支持: 0-不支持, 1-支持
    uint8_t  usb_speed;          // USB速度等级
    uint8_t  current_capability; // 电流能力: 3A或5A
    uint8_t  vdo_version;        // VDO版本
    uint16_t vendor_id;          // Vendor ID
    uint16_t product_id;         // USB Product ID (Product VDO Bit31-16)

    /* --- 以下字段 Passive / Active 语义不同，解析时按类型分别填充 --- */
    uint8_t  plug_type;          // Bit19:18: 2=Type-C, 3=Captive
    uint8_t  termination;        // Bit12:11 原值（Passive: 0=不需VCONN,1=需; Active: 2/3=需VCONN）
    uint8_t  max_vbus_50v;       // 1=最高 50V, 0=20V
    uint8_t  sbu_supported;      // 仅 Active: 1=支持 SBU（注意规范是反逻辑位）
    uint8_t  sbu_active;         // 仅 Active: 1=SBU 为有源
    uint8_t  vbus_through;       // 仅 Active: Bit4 线缆内含直通 VBUS
    uint8_t  sop2_present;       // 仅 Active: Bit3 支持 SOP''

    /* --- 仅 Active Cable VDO2 有意义的字段 --- */
    uint8_t  max_temp;           // 最高工作温度 (℃)
    uint8_t  shutdown_temp;      // 关断温度 (℃)
    uint8_t  optical;            // 1=光纤, 0=铜
    uint8_t  retimer;            // 1=Re-timer, 0=Re-driver
    uint8_t  usb4_supported;     // 1=支持 USB4（规范为反逻辑位）
    uint8_t  usb2_supported;     // 1=支持 USB2（规范为反逻辑位）
    uint8_t  usb3_supported;     // 1=支持 USB3（规范为反逻辑位）
    uint8_t  lanes;              // 1 或 2 条 USB3 lane

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

/* cable_type取值（由ID Header Product Type归一化后的线缆类型） */
#define CABLE_TYPE_PASSIVE  0
#define CABLE_TYPE_ACTIVE   1
#define CABLE_TYPE_UNKNOWN  2

/* Cable VDO1 位定义 —— 依据 USB PD R3.2 v1.2 (2026-03)
 *   Passive Cable : Table 6.42 (p.161)
 *   Active Cable  : Table 6.43 (p.164)
 * 两者位**位置**大部分相同，但同一位置的含义/取值不同，必须按线缆类型分别解析：
 *   Bit 8   Passive=Reserved           | Active=SBU Supported（0=支持，反逻辑）
 *   Bit 7   Passive=Reserved           | Active=SBU Type（0=无源，1=有源）
 *   Bit 4   Passive=Reserved           | Active=VBUS Through Cable
 *   Bit 3   Passive=Reserved           | Active=SOP'' Controller Present
 *   Bit12:11 Passive 0=不需VCONN/1=需   | Active 2=一端有源一端无源/3=两端有源（均需VCONN）
 *   Bit16:13 Passive 1000=>7m          | Active 1000~1010 = 100m/200m/300m
 *   Bit 6:5  Passive 00/11=Invalid→3A  | Active 00/11=Invalid；且 VBUSThrough=0 时整字段忽略
 */
#define CABLE_VDO1_HW_VERSION_MASK   (0x0F << 28)  // Bit 31-28: Hardware Version
#define CABLE_VDO1_FW_VERSION_MASK   (0x0F << 24)  // Bit 27-24: Firmware Version
#define CABLE_VDO1_VDO_VERSION_MASK  (0x07 << 21)  // Bit 23-21: VDO Version
// Bit 20: Reserved
#define CABLE_VDO1_PLUG_TYPE_MASK    (0x03 << 18)  // Bit 19-18: 10b=Type-C, 11b=Captive
#define CABLE_VDO1_EPR_CAPABLE       (0x01 << 17)  // Bit 17: EPR Capable
#define CABLE_VDO1_LATENCY_MASK      (0x0F << 13)  // Bit 16-13: Cable Latency (4 bits)
#define CABLE_VDO1_TERMINATION_TYPE  (0x03 << 11)  // Bit 12-11: 见上方差异说明
#define CABLE_VDO1_MAX_VBUS_MASK     (0x03 << 9)   // Bit 10-9: Maximum VBUS Voltage (2 bits)

/* --- 仅 Active Cable VDO1 有效的位（Passive 中为 Reserved） --- */
#define ACTIVE_VDO1_SBU_SUPPORTED    (0x01 << 8)   // 0=支持SBU, 1=不支持（注意反逻辑）
#define ACTIVE_VDO1_SBU_TYPE         (0x01 << 7)   // 0=无源SBU, 1=有源SBU
#define ACTIVE_VDO1_VBUS_THROUGH     (0x01 << 4)   // 0=无直通VBUS, 1=有
#define ACTIVE_VDO1_SOP2_PRESENT     (0x01 << 3)   // 0=无SOP''控制器, 1=有

#define CABLE_VDO1_CURRENT_MASK      (0x03 << 5)   // Bit 6-5: VBUS Current Handling (2 bits)
#define CABLE_VDO1_USB_SPEED_MASK    (0x07 << 0)   // Bit 2-0: USB Highest Speed (3 bits)

/* Active Cable VDO2 位定义（Table 6.44, p.166） */
#define ACTIVE_VDO2_MAX_TEMP_MASK    (0xFFu << 24) // Bit 31-24: 最高工作温度 (℃)
#define ACTIVE_VDO2_SHUTDOWN_MASK    (0xFFu << 16) // Bit 23-16: 关断温度 (℃)
// Bit 15: Reserved
#define ACTIVE_VDO2_U3_POWER_MASK    (0x07 << 12)  // Bit 14-12: U3/CLd 功耗
#define ACTIVE_VDO2_U3_MODE          (0x01 << 11)  // Bit 11: U3→U0 模式
#define ACTIVE_VDO2_OPTICAL          (0x01 << 10)  // Bit 10: 0=铜, 1=光纤
#define ACTIVE_VDO2_RETIMER          (0x01 << 9)   // Bit 9: 0=Re-driver, 1=Re-timer
#define ACTIVE_VDO2_USB4_SUPPORTED   (0x01 << 8)   // 0=支持USB4, 1=不支持（反逻辑）
#define ACTIVE_VDO2_USB2_HOPS_MASK   (0x03 << 6)   // Bit 7-6: USB2 Hub Hops
#define ACTIVE_VDO2_USB2_SUPPORTED   (0x01 << 5)   // 0=支持USB2, 1=不支持（反逻辑）
#define ACTIVE_VDO2_USB3_SUPPORTED   (0x01 << 4)   // 0=支持USB3, 1=不支持（反逻辑）
#define ACTIVE_VDO2_TWO_LANES        (0x01 << 3)   // 0=1 lane, 1=2 lanes
#define ACTIVE_VDO2_OPT_ISOLATED     (0x01 << 2)   // Bit 2: 光电隔离
#define ACTIVE_VDO2_ASYM_SUPPORTED   (0x01 << 1)   // Bit 1: USB4 非对称模式
#define ACTIVE_VDO2_USB_GEN2         (0x01 << 0)   // Bit 0: 0=Gen1, 1=Gen2+

/* Cable VDO1 字段取值 */
#define CABLE_PLUG_TYPE_TYPEC   0x02  // 10b: USB Type-C
#define CABLE_PLUG_TYPE_CAPTIVE 0x03  // 11b: Captive
#define CABLE_TERM_VCONN_NOT_REQ 0x00 // Passive 00b: 仅 Discover Identity 时需要 VCONN
#define CABLE_TERM_VCONN_REQ     0x01 // Passive 01b: 需要持续 VCONN
#define ACTIVE_TERM_ONE_ACTIVE   0x02 // Active 10b: 一端有源/一端无源，需 VCONN
#define ACTIVE_TERM_BOTH_ACTIVE  0x03 // Active 11b: 两端有源，需 VCONN

/* Cable VDO1 MaxV (Bit10-9) 取值 */
#define CABLE_MAXV_20V   0x00  // 20V（01b/10b 已废弃，Receiver 按 20V 处理）
#define CABLE_MAXV_50V   0x03  // 50V

/* USB Speed Values */
#define USB_SPEED_USB20_ONLY    0x00
#define USB_SPEED_USB32_GEN1    0x01
#define USB_SPEED_USB32_GEN2    0x02
#define USB_SPEED_USB4_GEN3     0x03
#define USB_SPEED_USB4_GEN4     0x04

/* Current Capability Values（已归一化，不区分线缆类型） */
#define CABLE_CURRENT_3A      0x00  // 3A
#define CABLE_CURRENT_5A      0x01  // 5A

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
 * @brief  使能右侧Type-C口CC通信线的Rd下拉电阻
 * @param  gnd_pin: 接地引脚 (1=PA1/CC_E2, 2=PA2/CC_E1)
 * @return none
 * @note   在左侧口使能VCONN供电之后调用。E-Marker通信线经线缆延伸至右口，
 *         通信线落在PA1还是PA2由线缆物理插入方向决定（与pd_comm_cc无关）。
 *         右口PA1(CC_E2)↔PA2(CC_E1)间仅有1颗5.1kΩ电阻，复用方式：
 *         - gnd_pin=1 → PA1推挽输出低，PA2经5.1kΩ对GND形成Rd（通信线在PA2）
 *         - gnd_pin=2 → PA2推挽输出低，PA1经5.1kΩ对GND形成Rd（通信线在PA1）
 *         与USBPD上拉分压后通信线静态约1.68V，满足PD BMC电平要求。
 * @note   方向由EMarker_Full_Detect自动尝试两种并保留成功的一方（试错交换），
 *         无需按pd_comm_cc手动选择。
 */
void EMarker_Enable_Rd_Pulldown(uint8_t gnd_pin);

/**
 * @brief  关闭右侧Type-C口Rd下拉电阻
 * @return none
 * @note   恢复PA1/PA2为浮空输入，避免5.1kΩ影响后续CC通断等其他测试
 */
void EMarker_Disable_Rd_Pulldown(void);

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
 * @return EMARK_OK-成功, EMARK_NOT_FOUND-无E-Mark, EMARK_PD_FAIL-PD通信失败
 * @note   自动完成检测、VCONN控制、PD通信全流程
 */
#define EMARK_OK          0   /* 成功 */
#define EMARK_NOT_FOUND   1   /* 未检测到E-Mark */
#define EMARK_PD_FAIL     2   /* PD通信失败(发送失败/超时/解析失败) */
uint8_t EMarker_Full_Detect(emarker_info_t *info);

/* 分步（非阻塞）读取：每次调用最多做一次 PD 事务，失败时也不卡主循环 */
#define EMARK_TASK_BUSY   3   /* 进行中，下一步继续 */
uint8_t EMarker_Task_Step(emarker_info_t *info);
uint8_t EMarker_Task_Busy(void);
void    EMarker_Task_Abort(void);

/******************************************************************************
 *                    WCH-Link 调试诊断接口（不依赖串口，GDB 直接回读内存）
 ******************************************************************************/
/* 诊断开关：1=开启诊断采集（GDB 可回读 g_emark_diag / 计时器 / 扫描接口），
 *           0=关闭（默认）。
 * 关闭后：诊断条目从 16 组缩到 1 组（省约 900B RAM）、电气指纹与 PHY 波形
 *         采样函数不参与编译、启动时也不再做全量扫描。 */
#ifndef EMARK_DEBUG_DIAG
#define EMARK_DEBUG_DIAG    0
#endif

#define EMARK_DIAG_MAGIC    0x454D4447u  /* "EMDG"，用于确认读到的结构体有效 */
#if EMARK_DEBUG_DIAG
#define EMARK_DIAG_MAX      16           /* 快读 6 组 + 加时重试 6 组 */
#else
#define EMARK_DIAG_MAX      1            /* 关闭时只留 1 组占位，保证结构体可用 */
#endif

/**
 * @brief 单组组合的原始诊断数据
 * @note  stage: 1=发送失败(无GoodCRC) 2=等VDM响应超时 3=收到响应但解析失败 4=成功
 *        fail : 1=BMC发送超时 2=未收到GoodCRC 3=等响应超时 4=解析失败
 */
typedef struct {
    uint8_t  vconn_cc;       /* VCONN供电通道 1/2 */
    uint8_t  pd_comm_cc;     /* USBPD通信通道 1/2 */
    uint8_t  gnd_pin;        /* 右口接地脚 1=PA1, 2=PA2 */
    uint8_t  sop;            /* 0=SOP', 1=SOP'' */
    uint8_t  spec_rev;       /* 2=PD3.0, 1=PD2.0 */
    uint8_t  stage;          /* 见 @note */
    uint8_t  fail;           /* 见 @note */
    uint8_t  bmc_aux;        /* STATUS[1:0] 响应包的SOP类型 */
    uint8_t  tx_retry;       /* 发送阶段剩余重试次数 */
    uint8_t  goodcrc_aux;    /* 发送阶段收到的SOP类型 */
    uint8_t  rx_head[2];     /* 响应消息头原始2字节 */
    uint8_t  rx_bytes[8];    /* 响应缓冲区前8字节 */
    uint16_t byte_cnt;       /* 响应BMC_BYTE_CNT */
    uint16_t goodcrc_cnt;    /* 发送阶段BMC_BYTE_CNT */
    uint32_t vdm_header;     /* 响应VDM Header */
    /* 电气快照：VCONN稳定 + 右口Rd配置之后、发送之前采集 */
    uint8_t  cc1_v;          /* CC1 比较器电压档 0=<0.22V .. 6=>=1.23V */
    uint8_t  cc2_v;          /* CC2 比较器电压档 */
    uint8_t  gpio_cc;        /* PC14/PC15 数字输入电平 bit0/bit1 */
    uint8_t  gpio_pa;        /* PA1/PA2 数字输入电平 bit0/bit1 */
    uint8_t  hdr_bit5;       /* 本组消息头 bit5 取值（兼容旧固件行为） */
    uint8_t  rp_mode;        /* 本组 Rp 电流档：4=330uA, 12=80uA */
    uint8_t  vbus_on;        /* 读取期间是否打开 V_PWR_EN 给 VBUS 供电（VBUS 供电型 E-Marker） */
    uint8_t  vbus_adc8;      /* 开启VBUS后PB1实测电压，ADC>>4（255≈3.3V）；压降反映芯片取电 */
    /* --- 第二轮实测扩展：PHY 状态机与 CC 线真实波形 --- */
    uint8_t  hvt;            /* AFIO USBPD_IN_HVT：1=高阈值(~2.2V), 0=普通阈值 */
    uint8_t  filt_en;        /* CONFIG.PD_FILT_EN 输入滤波 */
    uint8_t  tx_done;        /* IF_TX_END 是否真的置位（1=发送完成, 0=超时） */
    uint8_t  rx_state_bm;    /* 等GoodCRC期间 CONTROL[4:2] 出现过的状态位图 */
    uint8_t  rx_flags_bm;    /* 期间出现过的 IF_RX_* 标志位图 bit0=BIT,1=BYTE,2=RESET,3=ACT */
    uint8_t  cc_tog_tx;      /* 发送期间 PA_CC_AI 翻转次数(饱和255)，证明BMC真的上线 */
    uint8_t  cc_tog_rx;      /* 等GoodCRC期间 PA_CC_AI 翻转次数，>0 说明线缆确实发了波形 */
    uint8_t  max_byte_cnt;   /* 轮询期间 BMC_BYTE_CNT 最大值 */
    uint16_t vconn_rise_us;  /* VCONN 上电到 CC 越过 1.23V 的时间(us)，电气指纹 */
    uint16_t vconn_fall_us;  /* VCONN 断电到 CC 跌破 1.23V 的时间(us) */
    uint16_t pwr_settle_ms;  /* 本组上电后等待芯片建立的时间(ms) */
    uint8_t  pwr_mode;       /* 0=VCONN-only 1=VBUS+VCONN 同时 2=VBUS-only */
    uint8_t  reserved2;
} emark_diag_item_t;

typedef struct {
    uint32_t magic;
    uint8_t  done;               /* 扫描完成标志，GDB轮询此位 */
    uint8_t  count;              /* 已尝试的组合数 */
    uint8_t  detect_cc1_ai;      /* 检测阶段CC1比较器原始值 */
    uint8_t  detect_cc2_ai;      /* 检测阶段CC2比较器原始值 */
    uint8_t  detect_vconn_cc;    /* 检测阶段判定的VCONN通道 */
    uint8_t  detect_pd_cc;       /* 检测阶段判定的PD通道 */
    uint8_t  best_index;         /* 第一个成功的组合索引，0xFF=全部失败 */
    uint8_t  best_vconn_cc;
    uint8_t  best_pd_cc;
    uint8_t  best_gnd_pin;
    uint8_t  best_sop;
    uint8_t  best_spec_rev;
    uint8_t  best_hdr_bit5;
    uint8_t  best_rp_mode;
    emark_diag_item_t items[EMARK_DIAG_MAX];
} emark_diag_t;

extern emark_diag_t g_emark_diag;

/**
 * @brief 一次性扫描 VCONN通道/SOP类型/右口Rd方向/SpecRev 的所有组合
 * @return EMARK_OK 找到可用组合；EMARK_PD_FAIL 全部失败
 * @note  结果写入 g_emark_diag 供 WCH-Link + GDB 回读；成功配置会被
 *        EMarker_Full_Detect() 自动复用。每次调用都会重新扫描，可用于换线后重新适配。
 */
uint8_t EMarker_Debug_Scan(void);

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

/**
 * @brief  解析Active Cable VDO2（Table 6.44）
 * @param  cable_vdo2: Cable VDO2值
 * @param  info: E-Mark信息结构指针
 * @return none
 * @note   仅 Active Cable 有该 VDO；Passive 调用时直接返回。
 */
void EMarker_Parse_Cable_VDO2(uint32_t cable_vdo2, emarker_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* __EMARKER_PD_H */
