/********************************** (C) COPYRIGHT *******************************
 * File Name          : emarker_pd.c
 * Author             : CableTester Team
 * Version            : V1.0
 * Date               : 2026/05/05
 * Description        : E-Mark芯片PD协议通信实现
 *
 * 功能说明：
 * - 实现USB PD协议与E-Mark芯片的通信
 * - 发送Discover Identity VDM命令
 * - 接收并解析E-Mark响应数据
 * - 提取线缆详细信息（类型、速度、电流能力等）
 *********************************************************************************/

#include "emarker_pd.h"
#include "cable_gpio.h"
#include "debug.h"
#include <string.h>

/* PD通信缓冲区 */
__attribute__((aligned(4))) static uint8_t pd_tx_buf[34] = {0};
__attribute__((aligned(4))) static uint8_t pd_rx_buf[34] = {0};

/* PD控制结构 */
static PD_CONTROL pd_ctl;

/* 超时定义 */
#define PD_COMM_TIMEOUT_MS    100  // PD通信超时时间（毫秒）
#define PD_RETRY_COUNT        3    // 重试次数

/**
 * @brief  构建VDM Header
 * @param  cmd: VDM命令
 * @param  obj_pos: 对象位置（1-7）
 * @return VDM Header值
 */
static uint32_t Build_VDM_Header(uint8_t cmd, uint8_t obj_pos)
{
    uint32_t header = 0;

    // Bit 31-16: SVID (Standard or Vendor ID) - USB-IF Standard ID
    header |= (VDM_SID_USB_PD << 16);

    // Bit 15: VDM Type - 1 = Structured VDM
    header |= VDM_HDR_STD_VDM;

    // Bit 14-13: Structured VDM Version - 01 = VDM Version 2.1
    header |= VDM_HDR_VERSION_21;

    // Bit 12-11: Structured VDM Version (Minor)

    // Bit 10-8: Object Position (3 bits, range 1-7)
    header |= ((obj_pos & 0x07) << 8);

    // Bit 7-6: Command Type - Initiator
    header |= VDM_HDR_CMD_INITIATOR;

    // Bit 5: Reserved (0 for Structured VDM)

    // Bit 4-0: Command (5 bits)
    header |= (cmd & 0x1F);

    return header;
}

/**
 * @brief  构建消息Header
 * @param  msg_type: 消息类型（Control/Data）
 * @param  num_do: 数据对象数量
 * @param  msg_id: 消息ID
 * @return Message Header值（16位）
 */
static uint16_t Build_Message_Header(uint8_t msg_type, uint8_t num_do, uint8_t msg_id)
{
    uint16_t header = 0;

    // Extended bit (bit 15)
    header |= 0 << 15;

    // Number of Data Objects (bits 14:12)
    header |= ((num_do & 0x07) << 12);

    // Message ID (bits 11:9)
    header |= ((msg_id & 0x07) << 9);

    // Port Power Role / Cable Plug (bit 8) - 对于E-Mark读取，设置为Port
    header |= (0 << 8);

    // Specification Revision (bits 7:6) - PD 3.0
    header |= (DEF_PD_REVISION_30 << 6);

    // Port Data Role (bit 5)
    header |= (1 << 5);

    // Message Type (bits 4:0) - 直接使用传入的msg_type
    header |= (msg_type & 0x1F);

    return header;
}

/**
 * @brief  发送PD数据包
 * @param  data: 数据指针
 * @param  len: 数据长度（字节）
 * @param  sop: SOP类型
 * @return 0-成功, 1-失败
 */
static uint8_t PD_Send_Packet(uint8_t *data, uint8_t len, uint8_t sop, uint8_t ack)
{
    uint8_t retry = PD_RETRY_COUNT;

    while (retry--) {
        // 启用CC_LVE，切换到推挽输出模式用于发送
        // 根据官方示例，需要启用被选中的通信通道的LVE
        if (USBPD->CONFIG & CC_SEL) {
            // CC2被选中用于通信，启用CC2的LVE
            USBPD->PORT_CC2 |= CC_LVE;
        } else {
            // CC1被选中用于通信，启用CC1的LVE
            USBPD->PORT_CC1 |= CC_LVE;
        }

        // 配置DMA
        USBPD->DMA = (uint32_t)(uint8_t *)data;

        // 设置SOP类型
        USBPD->TX_SEL = sop;

        // 设置数据长度
        USBPD->BMC_TX_SZ = len;

        // 设置BMC时钟计数器（使用发送定时器值，确保300kHz速率）
        USBPD->BMC_CLK_CNT = UPD_TMR_TX_48M;

        // 使能发送
        USBPD->CONTROL |= PD_TX_EN;
        USBPD->STATUS &= ~IF_TX_END;
        USBPD->CONTROL |= BMC_START;

        // 等待发送完成（带超时）
        uint32_t timeout = 10000;
        while (!(USBPD->STATUS & IF_TX_END) && timeout--) {
            Delay_Us(10);
        }

        if (timeout == 0) {
            // 发送超时，禁用LVE后重试
            USBPD->PORT_CC1 &= ~CC_LVE;
            USBPD->PORT_CC2 &= ~CC_LVE;
            continue;
        }

        USBPD->STATUS |= IF_TX_END;

        // 禁用LVE，恢复恒流源上拉模式
        USBPD->PORT_CC1 &= ~CC_LVE;
        USBPD->PORT_CC2 &= ~CC_LVE;

        if (!ack) {
            return 0;
        }

        // 切换到接收模式，等待GoodCRC
        USBPD->CONFIG |= PD_ALL_CLR;
        USBPD->CONFIG &= ~PD_ALL_CLR;
        USBPD->CONFIG |= IE_RX_ACT | IE_RX_RESET | PD_DMA_EN;  // 使能接收中断和DMA
        USBPD->DMA = (uint32_t)(uint8_t *)pd_rx_buf;
        USBPD->CONTROL &= ~PD_TX_EN;
        USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
        USBPD->CONTROL |= BMC_START;

        // 等待GoodCRC（约750us）
        timeout = 7500;
        while (timeout--) {
            if (USBPD->STATUS & IF_RX_ACT) {
                USBPD->STATUS |= IF_RX_ACT;

                // 检查是否为GoodCRC
                if (/*USBPD->BMC_BYTE_CNT == 6) &&*/
                    ((pd_rx_buf[0] & 0x1F) == DEF_TYPE_GOODCRC)) {
                    // 发送成功
                    return 0;
                }
            }
             Delay_Us(10);
        }
    }

    return 1;  // 发送失败
}

/**
 * @brief  发送GoodCRC确认
 * @param  msg_id: Message ID from received message
 * @return 0-成功, 1-失败
 * @note   收到任何PD消息后必须在规定时间内回复GoodCRC
 *         GoodCRC是Control Message，Message Type = 0x01 (DEF_TYPE_GOODCRC)
 */
static uint8_t PD_Send_GoodCRC(uint8_t msg_id)
{
    uint16_t msg_header;

    // 构建GoodCRC Message Header
    // GoodCRC是Control Message，Message Type = 0x01 (DEF_TYPE_GOODCRC)
    // 没有Data Objects，所以num_do = 0
    msg_header = Build_Message_Header(DEF_TYPE_GOODCRC, 0, msg_id);

    // GoodCRC数据（小端序）
    pd_tx_buf[0] = msg_header & 0xFF;
    pd_tx_buf[1] = (msg_header >> 8) & 0xFF;

    // 发送GoodCRC（SOP'用于Cable Plug通信）
    return PD_Send_Packet(pd_tx_buf, 2, UPD_SOP1, 0);
}

/**
 * @brief  进入接收模式
 * @return none
 */
static void PD_Enter_Rx_Mode(void)
{
    USBPD->CONFIG |= PD_ALL_CLR;
    USBPD->CONFIG &= ~PD_ALL_CLR;
    USBPD->CONFIG |= IE_RX_ACT | IE_RX_RESET | PD_DMA_EN;
    USBPD->DMA = (uint32_t)(uint8_t *)pd_rx_buf;
    USBPD->CONTROL &= ~PD_TX_EN;
    USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
    USBPD->CONTROL |= BMC_START;
}

/**
 * @brief  等待PD响应
 * @param  timeout_ms: 超时时间（毫秒）
 * @return 0-收到响应, 1-超时
 * @note   在调用此函数前，必须已经配置好接收模式（DMA、BMC等）
 */
static uint8_t PD_Wait_Response(uint32_t timeout_ms)
{
    uint32_t start_time = system_tick_ms;

    // 清除之前的接收状态
    // USBPD->STATUS |= IF_RX_ACT | IF_RX_BIT | IF_RX_BYTE | IF_RX_RESET;

    // // 确保DMA缓冲区已设置
    // USBPD->DMA = (uint32_t)(uint8_t *)pd_rx_buf;

    // // 启动BMC接收
    // USBPD->CONTROL |= BMC_START;

    while ((system_tick_ms - start_time) < timeout_ms) {
        if (USBPD->STATUS & IF_RX_ACT) {
            USBPD->STATUS |= IF_RX_ACT;  // 清除中断标志

            // 检查是否收到有效数据
            if (USBPD->BMC_BYTE_CNT >= 6) {
                return 0;  // 收到响应
            }
        }
        Delay_Ms(1);
    }

    return 1;  // 超时
}

/******************************************************************************
 *                    API函数实现
 ******************************************************************************/

/**
 * @brief  初始化E-Mark PD通信模块
 * @param  vconn_cc: VCONN所在的CC通道 (1=CC1, 2=CC2)
 * @param  pd_comm_cc: PD通信使用的CC通道 (1=CC1, 2=CC2)
 * @note   根据VCONN和PD通信通道正确配置CC端口
 */
void EMarker_PD_Init(uint8_t vconn_cc, uint8_t pd_comm_cc)
{
    // GPIO_InitTypeDef GPIO_InitStructure = {0};

    // 使能时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);

    // 配置高阈值输入和V33模式
    // USBPD_IN_HVT: 高阈值输入模式，降低PD通信时的I/O功耗
    // USBPD_PHY_V33: VDD>4V时启用LDO降压，限制到约3.3V
    AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;

    // 初始化USBPD寄存器
    USBPD->CONTROL = 0;                  // 清除CONTROL（停止BMC/TX）
    USBPD->CONFIG = PD_DMA_EN;
    USBPD->STATUS = BUF_ERR | IF_RX_BIT | IF_RX_BYTE | IF_RX_ACT | IF_RX_RESET | IF_TX_END;

    // 清除控制结构
    memset(&pd_ctl, 0, sizeof(PD_CONTROL));

    // 初始化为Sink模式（读取E-Mark时作为UFP/Cable Plug）
    pd_ctl.Flag.Bit.PR_Role = 0;  // Sink角色
    pd_ctl.Flag.Bit.Auto_Ack_PRRole = 0;

    // 配置CC1/CC2端口
    // VCONN供电的CC线：不需要配置（由VCONN供电，E-Mark已上电）
    // PD通信的CC线：作为SRC等待E-Mark响应
    if (pd_comm_cc == 1) {
        // PD通信使用CC1
        USBPD->PORT_CC1 = CC_CMP_66 | CC_PU_330;  // Rp上拉
        // USBPD->PORT_CC1 = CC_CMP_66 | CC_LVE;
        USBPD->PORT_CC2 = CC_CMP_66;              // CC2不配置（VCONN供电）
    } else {
        // PD通信使用CC2
        USBPD->PORT_CC1 = CC_CMP_66;              // CC1不配置（VCONN供电）
        USBPD->PORT_CC2 = CC_CMP_66 | CC_PU_330;  // Rp上拉
        // USBPD->PORT_CC2 = CC_CMP_66 | CC_LVE;
    }

    // USBPD硬件通信通道(CC_SEL)设为PD通信通道(pd_comm_cc)。
    EMarker_Set_CC_Channel(pd_comm_cc);

    // 进入接收模式
    PD_Enter_Rx_Mode();
}

/**
 * @brief  检测并读取E-Mark芯片信息
 * @param  vconn_cc: VCONN所在的CC通道 (1=CC1, 2=CC2)
 * @param  pd_comm_cc: PD通信使用的CC通道 (1=CC1, 2=CC2)
 * @param  info: E-Mark信息结构指针
 * @return 0-成功, 1-失败
 * @note   调用此函数前，应已使能VCONN供电
 */
uint8_t EMarker_Read_Info(uint8_t vconn_cc, uint8_t pd_comm_cc, emarker_info_t *info)
{
    uint8_t ret;

    if (info == NULL) {
        return 1;
    }

    // 清空信息结构
    memset(info, 0, sizeof(emarker_info_t));

    // 注意：VCONN供电应由调用者完成
    // 这里只负责PD通信

    // 步骤1: 初始化PD通信（传入VCONN和PD通信通道）
    EMarker_PD_Init(vconn_cc, pd_comm_cc);

    // 步骤2: 发送Discover Identity命令
    ret = EMarker_Send_Discover_Identity();
    if (ret != 0) {
        return 1;  // 发送失败
    }

    // 步骤3: 重新进入接收模式，等待E-Mark的VDM响应
    PD_Enter_Rx_Mode();

    // 步骤4: 等待并接收响应
    ret = PD_Wait_Response(PD_COMM_TIMEOUT_MS);
    if (ret != 0) {
        return 1;  // 超时
    }

    // 步骤5: 提取Message ID并立即发送GoodCRC
    // Message Header在pd_rx_buf[0:1]，小端序
    uint16_t rx_msg_header = pd_rx_buf[0] | (pd_rx_buf[1] << 8);
    uint8_t rx_msg_id = (rx_msg_header >> 9) & 0x07;

    // 步骤6: 解析响应
    ret = EMarker_Parse_Response(info);

    // 立即发送GoodCRC（必须在tReceive时间内，最大750μs）
    PD_Send_GoodCRC(rx_msg_id);

    return ret;
}

/**
 * @brief  发送Discover Identity命令
 */
uint8_t EMarker_Send_Discover_Identity(void)
{
    uint16_t msg_header;
    uint32_t vdm_header;
    uint8_t tx_len;

    // 构建Message Header（Vendor Defined Message，1个数据对象）
    msg_header = Build_Message_Header(DEF_TYPE_VENDOR_DEFINED, 1, pd_ctl.Msg_ID);

    // 构建VDM Header（Discover Identity命令）
    vdm_header = Build_VDM_Header(VDM_CMD_DISCOVER_IDENTITY, 0);

    // 组装发送数据
    pd_tx_buf[0] = msg_header & 0xFF;
    pd_tx_buf[1] = (msg_header >> 8) & 0xFF;

    // VDM数据（小端格式）
    pd_tx_buf[2] = vdm_header & 0xFF;
    pd_tx_buf[3] = (vdm_header >> 8) & 0xFF;
    pd_tx_buf[4] = (vdm_header >> 16) & 0xFF;
    pd_tx_buf[5] = (vdm_header >> 24) & 0xFF;

    tx_len = 6;  // 2字节Header + 4字节VDM

    // 发送数据包（SOP'用于Cable Plug通信）
    return PD_Send_Packet(pd_tx_buf, tx_len, UPD_SOP1, 1);
}

/**
 * @brief  接收并解析VDM响应
 */
uint8_t EMarker_Parse_Response(emarker_info_t *info)
{
    uint16_t msg_header;
    uint8_t num_do;
    uint8_t msg_type;

    if (info == NULL) {
        return 1;
    }

    // 解析Message Header
    msg_header = pd_rx_buf[0] | (pd_rx_buf[1] << 8);
    msg_type = msg_header & 0x1F;
    num_do = (msg_header >> 12) & 0x07;

    // 检查是否为Vendor Defined Message
    if (msg_type != DEF_TYPE_VENDOR_DEFINED) {
        return 1;
    }

    // 检查是否有足够的数据对象
    if (num_do < 2) {
        return 1;  // 至少需要VDM Header和ID Header
    }

    uint32_t *ptr = (uint32_t *)&pd_rx_buf[2];

    // 解析VDM数据
    info->vdm_header = *ptr++;
    num_do--;

    // 检查VDM Header是否为ACK响应
    if ((info->vdm_header & VDM_HDR_CMD_TYPE_MASK) != VDM_HDR_CMD_ACK) {
        return 1;
    }

    // 解析ID Header
    if (num_do > 0) {
        info->id_header = *ptr++;
        num_do--;
        EMarker_Parse_ID_Header(info->id_header, info);
        info->valid = 1;
        info->id_header_present = 1;
    }

    // 解析Cert Stat VDO（如果存在）
    if (num_do > 0) {
        info->cert_stat = *ptr++;
        num_do--;
    }

    // 解析Product VDO（如果存在）
    if (num_do > 0) {
        info->product_vdo = *ptr++;
        num_do--;
        // USB Product ID位于Product VDO Bit31-16（Bit15-0为bcdDevice）
        info->product_id = (uint16_t)(info->product_vdo >> 16);
    }

    // 解析Cable VDO1（如果存在）
    if (num_do > 0) {
        info->cable_vdo1 = *ptr++;
        num_do--;
        EMarker_Parse_Cable_VDO1(info->cable_vdo1, info);
    }

    // 解析Cable VDO2（如果存在）
    if (num_do > 0) {
        info->cable_vdo2 = *ptr++;
        num_do--;
    }

    return 0;
}

/**
 * @brief  解析VDM Header
 */
void EMarker_Parse_VDM_Header(uint32_t vdm_header, emarker_info_t *info)
{
    if (info == NULL) {
        return;
    }

    info->vdm_header = vdm_header;
}

/**
 * @brief  解析ID Header
 */
void EMarker_Parse_ID_Header(uint32_t id_header, emarker_info_t *info)
{
    uint8_t product_type;

    if (info == NULL) {
        return;
    }

    info->id_header = id_header;

    // 提取Product Type
    product_type = (id_header & ID_HDR_PRODUCT_TYPE_MASK) >> 27;

    // 判断线缆类型
    switch (product_type) {
        case PRODUCT_TYPE_PASSIVE_CABLE:
            info->cable_type = CABLE_TYPE_PASSIVE;  // 被动线缆
            break;
        case PRODUCT_TYPE_ACTIVE_CABLE:
            info->cable_type = CABLE_TYPE_ACTIVE;  // 主动线缆
            break;
        default:
            info->cable_type = CABLE_TYPE_UNKNOWN;  // 未知
            break;
    }

    // 提取Vendor ID
    info->vendor_id = id_header & ID_HDR_VENDOR_ID_MASK;
}

/**
 * @brief  解析Cable VDO1
 */
void EMarker_Parse_Cable_VDO1(uint32_t cable_vdo1, emarker_info_t *info)
{
    uint8_t usb_speed;
    uint8_t current_cap;

    if (info == NULL) {
        return;
    }

    info->cable_vdo1 = cable_vdo1;

    // 提取USB速度等级 (Bit 2-0)
    usb_speed = cable_vdo1 & CABLE_VDO1_USB_SPEED_MASK;  // 不需要移位，已经在最低位
    info->usb_speed = usb_speed;

    // 提取电流能力 (Bit 6-5)
    current_cap = (cable_vdo1 & CABLE_VDO1_CURRENT_MASK) >> 5;
    info->current_capability = current_cap;

    // 提取EPR支持 (Bit 17)
    info->epr_capable = (cable_vdo1 & CABLE_VDO1_EPR_CAPABLE) ? 1 : 0;

    // 提取VDO版本 (Bit 23-21)
    info->vdo_version = (cable_vdo1 & CABLE_VDO1_VDO_VERSION_MASK) >> 21;
}

/******************************************************************************
 *                    E-Mark检测和控制函数
 ******************************************************************************/

/**
 * @brief  检测E-Mark芯片是否存在
 */
uint8_t EMarker_Detect_Chip(void)
{
    uint8_t vconn_cc, pd_comm_cc;
    return EMarker_Detect_CC_Channel(&vconn_cc, &pd_comm_cc);
}

/**
 * @brief  检测E-Mark芯片所在的CC通道（统一检测入口）
 */
uint8_t EMarker_Detect_CC_Channel(uint8_t *vconn_cc, uint8_t *pd_comm_cc)
{
    // GPIO_InitTypeDef GPIO_InitStructure = {0};
    uint8_t has_emark = 0;

    if (vconn_cc == NULL || pd_comm_cc == NULL) {
        return 0;
    }

    // 初始化默认值
    *vconn_cc = 1;   // 默认VCONN在CC1
    *pd_comm_cc = 2; // 默认PD通信使用CC2（与VCONN逆关系）

    // 临时初始化USBPD模块用于检测
    // RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);

    // 配置PC14/PC15为浮空输入
    // GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    // GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    // GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    // GPIO_Init(GPIOC, &GPIO_InitStructure);

    // 配置高阈值输入和V33模式
    AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;

    // 初始化USBPD寄存器
    USBPD->CONTROL = 0;                  // 清除CONTROL（停止BMC/TX）
    USBPD->CONFIG = PD_DMA_EN;
    USBPD->STATUS = BUF_ERR | IF_RX_BIT | IF_RX_BYTE | IF_RX_ACT | IF_RX_RESET | IF_TX_END;

    // 配置CC1/CC2端口为上拉模式（SRC），等待检测Rd下拉
    USBPD->PORT_CC1 = CC_CMP_66 | CC_PU_330;
    USBPD->PORT_CC2 = CC_CMP_66 | CC_PU_330;

    Delay_Ms(5);  // 等待稳定

    // 读取CC比较器状态
    uint16_t cc1_status = USBPD->PORT_CC1;
    uint16_t cc2_status = USBPD->PORT_CC2;

    // 检查PA_CC_AI位（比较器模拟输入状态）
    uint8_t cc1_has_rd = !(cc1_status & PA_CC_AI);  // CC1有Rd下拉
    uint8_t cc2_has_rd = !(cc2_status & PA_CC_AI);  // CC2有Rd下拉

    // 根据检测结果确定VCONN和PD通信通道：
    //   E-Mark在CC1 → VCONN=CC1、PD通信=CC2
    //   E-Mark在CC2 → VCONN=CC2、PD通信=CC1
    // EMarker_PD_Init内部将CC_SEL设为pd_comm_cc
    if (cc1_has_rd && !cc2_has_rd) {
        // E-Mark在CC1上
        has_emark = 1;
        *vconn_cc = 1;   // VCONN给CC1供电
        *pd_comm_cc = 2; // PD通信使用CC2
    } else if (!cc1_has_rd && cc2_has_rd) {
        // E-Mark在CC2上
        has_emark = 1;
        *vconn_cc = 2;   // VCONN给CC2供电
        *pd_comm_cc = 1; // PD通信使用CC1
    } else if (cc1_has_rd && cc2_has_rd) {
        // 两个CC都有Rd（特殊情况），默认使用CC1作为VCONN
        has_emark = 1;
        *vconn_cc = 1;
        *pd_comm_cc = 2;
    }
    // 否则 has_emark = 0，无E-Mark

    // 关闭USBPD模块，恢复引脚为浮空输入
    // USBPD->PORT_CC1 = CC_CMP_66;
    // USBPD->PORT_CC2 = CC_CMP_66;

    // GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    // GPIO_Init(GPIOC, &GPIO_InitStructure);

    return has_emark;
}

/**
 * @brief  使能VCONN供电
 * @param  cc_channel: CC通道 (1=CC1/PC3, 2=CC2/PC0)
 * @note   VCONN供电前先将GPIO置高，避免电压毛刺
 */
void EMarker_Enable_VCONN(uint8_t cc_channel)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    // 首先关闭所有VCONN输出，恢复为浮空输入
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_AUX_PIN | USB3_CC2_AUX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    Delay_Us(10);  // 短暂延迟，确保完全关闭

    // 根据CC通道选择使能对应的引脚
    if (cc_channel == 1) {
        // E-Mark在CC1上，使能PC3输出VCONN

        // 步骤1: 先置高电平（在配置为输出之前）
        GPIO_SetBits(GPIOC, USB3_CC1_AUX_PIN);

        // 步骤2: 配置为推挽输出（会立即输出高电平）
        GPIO_InitStructure.GPIO_Pin = USB3_CC1_AUX_PIN;  // PC3
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(GPIOC, &GPIO_InitStructure);

    } else if (cc_channel == 2) {
        // E-Mark在CC2上，使能PC0输出VCONN

        // 步骤1: 先置高电平（在配置为输出之前）
        GPIO_SetBits(GPIOC, USB3_CC2_AUX_PIN);

        // 步骤2: 配置为推挽输出（会立即输出高电平）
        GPIO_InitStructure.GPIO_Pin = USB3_CC2_AUX_PIN;  // PC0
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(GPIOC, &GPIO_InitStructure);
    }
}

/**
 * @brief  关闭VCONN供电
 */
void EMarker_Disable_VCONN(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    // 关闭所有VCONN输出，恢复为浮空输入
    GPIO_InitStructure.GPIO_Pin = USB3_CC1_AUX_PIN | USB3_CC2_AUX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
}

/**
 * @brief  设置USBPD通信的CC通道
 */
void EMarker_Set_CC_Channel(uint8_t cc_channel)
{
    if (cc_channel == 1) {
        USBPD->CONFIG &= ~CC_SEL;  // 选择CC1
    } else {
        USBPD->CONFIG |= CC_SEL;   // 选择CC2
    }
}

/**
 * @brief  使能右侧Type-C口CC通信线的Rd下拉电阻
 * @param  gnd_pin: 接地引脚 (1=PA1/CC_E2, 2=PA2/CC_E1)
 * @note   在左侧口使能VCONN供电后调用。E-Marker的CC通信线经线缆延伸至右口
 *         CC脚（CC1↔PA2, CC2↔PA1），通信线落在哪个脚由线缆物理插入方向
 *         决定（线缆翻面后交换），与pd_comm_cc无关。若通信线上没有E-Mark
 *         芯片自身的Rd，通信线悬空会使USBPD上拉无泄放回路，BMC直流偏置
 *         错乱、PD通信超时（表现为"检测到E-Marker但读取失败"）。
 *         电路上只有PA1↔PA2间1颗5.1kΩ电阻，采用"接地脚推挽输出低"复用：
 *         - gnd_pin=1 → PA1接地，PA2（若为通信线）经5.1kΩ对GND形成Rd
 *         - gnd_pin=2 → PA2接地，PA1（若为通信线）经5.1kΩ对GND形成Rd
 *         与USBPD上拉分压后静态约1.68V（330μA×5.1k），满足PD BMC接收
 *         电平（设计文档3.4.2节）。另一脚保持浮空，不干扰BMC信号。
 * @note   方向由EMarker_Full_Detect自动尝试两种并保留成功的一方，无需手动指定。
 */
void EMarker_Enable_Rd_Pulldown(uint8_t gnd_pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    /* 先恢复PA1/PA2为浮空输入，避免引脚模式切换产生毛刺 */
    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN | USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* 接地脚推挽输出低 → 另一脚（若为通信线）经唯一5.1kΩ电阻对GND形成Rd */
    if (gnd_pin == 1) {
        GPIO_InitStructure.GPIO_Pin = USB4_CC_E2_PIN;  /* PA1接地 */
    } else {
        GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN;  /* PA2接地 */
    }
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOA, GPIO_InitStructure.GPIO_Pin);
}

/**
 * @brief  关闭右侧Type-C口Rd下拉电阻
 * @note   恢复PA1/PA2为浮空输入，避免5.1kΩ影响后续CC通断等其他测试
 */
void EMarker_Disable_Rd_Pulldown(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    GPIO_InitStructure.GPIO_Pin = USB4_CC_E1_PIN | USB4_CC_E2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

/**
 * @brief  使用指定方向的右口Rd下拉，完成一次完整E-Mark PD通信
 * @param  gnd_pin: 接地引脚 (1=PA1, 2=PA2)，另一脚作为通信线Rd的接地点
 * @param  info: E-Mark信息结构指针
 * @return EMARK_OK-成功, EMARK_PD_FAIL-失败
 * @note   返回前PA1/PA2均恢复浮空；失败时调用者可交换方向重试。
 *         GoodCRC在解析后、关闭Rd前发送，保证应答时序正确。
 */
static uint8_t EMarker_Try_Comm(uint8_t gnd_pin, emarker_info_t *info)
{
    uint8_t ret;

    EMarker_Enable_Rd_Pulldown(gnd_pin);

    /* 发送Discover Identity命令 */
    ret = EMarker_Send_Discover_Identity();
    if (ret != 0) {
        EMarker_Disable_Rd_Pulldown();
        return EMARK_PD_FAIL;  /* 发送失败：线路偏置错误（方向不对） */
    }

    /* 重新进入接收模式，等待E-Mark的VDM响应 */
    PD_Enter_Rx_Mode();
    ret = PD_Wait_Response(PD_COMM_TIMEOUT_MS);
    if (ret != 0) {
        EMarker_Disable_Rd_Pulldown();
        return EMARK_PD_FAIL;  /* 超时 */
    }

    /* 提取Message ID并立即发送GoodCRC */
    uint16_t rx_msg_header = pd_rx_buf[0] | (pd_rx_buf[1] << 8);
    uint8_t rx_msg_id = (rx_msg_header >> 9) & 0x07;

    /* 解析响应 */
    ret = EMarker_Parse_Response(info);

    /* 立即发送GoodCRC（必须在tReceive时间内，最大750μs） */
    PD_Send_GoodCRC(rx_msg_id);

    EMarker_Disable_Rd_Pulldown();

    return (ret == 0) ? EMARK_OK : EMARK_PD_FAIL;
}

/**
 * @brief  完整的E-Mark检测流程（高层API）
 */
uint8_t EMarker_Full_Detect(emarker_info_t *info)
{
    uint8_t vconn_cc, pd_comm_cc;
    uint8_t ret;

    if (info == NULL) {
        return EMARK_NOT_FOUND;
    }

    // 步骤1: 检测E-Mark并确定CC通道（一次性完成）
    if (!EMarker_Detect_CC_Channel(&vconn_cc, &pd_comm_cc)) {
        return EMARK_NOT_FOUND;  // 无E-Mark
    }

    // 步骤2: 初始化PD通信（在VCONN供电前完成，避免引脚状态变化影响E-Mark）
    EMarker_PD_Init(vconn_cc, pd_comm_cc);

    // 步骤3: 使能VCONN供电
    EMarker_Enable_VCONN(vconn_cc);
    Delay_Ms(20);  // 等待VCONN稳定和E-Mark上电

    // 步骤4: 自动检测右口Rd方向并完成PD通信
    // 通信线落在右口PA1还是PA2由线缆物理插入方向决定（与pd_comm_cc无关），
    // 依次尝试两种方向，以是否收到E-Mark响应作为判据，自动保留正确方向。
    ret = EMarker_Try_Comm(1, info);  // 尝试PA1接地
    if (ret != EMARK_OK) {
        ret = EMarker_Try_Comm(2, info);  // 失败则交换：PA2接地
    }

    // 步骤5: 关闭VCONN（先撤右口Rd下拉，再关VCONN）
    EMarker_Disable_Rd_Pulldown();
    EMarker_Disable_VCONN();

    return ret;
}
