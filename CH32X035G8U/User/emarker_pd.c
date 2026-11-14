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
#include "cable_adc.h"
#include "debug.h"
#include <string.h>

/******************************************************************************
 *                    WCH-Link 调试诊断：结果固化在 RAM，GDB 直接回读
 ******************************************************************************/
/* g_emark_diag 必须是全局非 static 符号，否则 -g + --gc-sections 后 GDB 找不到 */
emark_diag_t g_emark_diag;

/* 当前事务的运行时配置（扫描/复用成功配置时由调用者设置） */
static uint8_t s_tx_sop    = UPD_SOP1;             /* 发送使用的 SOP' / SOP'' */
static uint8_t s_spec_rev  = DEF_PD_REVISION_30;   /* 请求使用的 SpecRev */
static uint8_t s_diag_mode = 0;                    /* 1=扫描模式，缩短超时加快遍历 */
/* 1=分步重试路径（EMarker_Task_Step）：用短超时尽快让步，
 * 保证读取失败时主循环仍能在 50ms 量级内回来扫按键 */
static uint8_t s_fast_fail = 0;
/* 调试：事务内部分段最大耗时（建立/发送/等响应，ms） */
volatile uint32_t g_t_setup = 0, g_t_tx = 0, g_t_rx = 0;
/* 1=采集 VCONN 上升/下降曲线（仅换线扫描时用；单次约 40ms，会拖慢主循环） */
static uint8_t s_deep_diag = 0;

/* 兼容维度：部分旧 E-Mark 只认 bit5=1 的原固件头；Rp 电流档影响 CC 偏置 */
static uint8_t s_hdr_bit5 = 0;
static uint8_t s_rp_mode  = CC_PU_330;
static uint32_t s_last_retry_ms = 0;  /* 全失败时抑制重复事务的节流时间戳 */

/* 物理层维度：USBPD_IN_HVT 把 PC14/PC15 输入阈值抬到约 2.2V。
 * 本设计通信线静态仅约 1.68V(330uA × 5.1k Rd)，若该阈值参与 BMC 接收，
 * 芯片将永远看不到线缆发来的波形 —— 这是「别的线能读、这根线读不到」
 * 的强候选。PD_FILT_EN 为输入滤波，同样可能影响弱信号解码。 */
static uint8_t s_hvt_mode  = 1;
static uint8_t s_filt_mode = 0;

/* VBUS 供电维度：规范允许 E-Marked 线缆用 VBUS 代替 VCONN 取电
 * （Type-C R2.0 §2.5：Electronically marked cables may use VBUS instead of
 * VCONN as VBUS is available across the cable）。这类芯片的 VCONN 引脚可能
 * 根本不存在，只给 VCONN 时芯片始终不上电 —— 与「Ra 检测得到、CC 线零应答、
 * VCONN 无 RC 充电特征」的实测完全吻合。读取期间打开 V_PWR_EN（3.3V 经
 * 100Ω 限流到 VBUS），读完立即关闭，避免影响电阻/通断测试。 */
static uint8_t  s_vbus_mode = 0;         /* 1=读取期间打开 V_PWR_EN */
static uint16_t s_vbus_settle_ms = 0;    /* 上电后等待芯片建立的时间 */
static uint8_t  s_vconn_off = 0;         /* 1=VBUS 供电型，全程不给 VCONN */

/* 上电建立时间：VBUS 与 VCONN 同时供电，合并为一次等待。
 * 原来「VBUS 等 settle + VCONN 再等 60ms」两段串行，单次事务固定多花 60ms；
 * 合并后总时长就等于这里的值。实测 50Ω/3.23V 下芯片启动很快，
 * 用 EMARK_PWR_SETTLE_MS 作为固定值，必要时按实测下调。 */
#define EMARK_PWR_SETTLE_MS       5u     /* 实测 5ms 即可稳定读通（40→20→10→5 逐级验证） */
#define EMARK_PWR_SETTLE_RETRY_MS 40u    /* 快读失败后的加时重试，覆盖上电偏慢的芯片 */
/* 兼容档：VCONN-only 的 E-Marker 需要 VCONN 建立后再通信 */
#define EMARK_VCONN_SETTLE_MS     60u
/* 回退档：纯 VBUS 供电（历史已实测可用） */
#define EMARK_VBUS_SETTLE_SHORT   30u
#define EMARK_VBUS_SETTLE_LONG    100u

/* 发送阶段的细粒度诊断（PD_Send_Packet 填充） */
typedef struct {
    uint8_t  goodcrc_seen;
    uint8_t  goodcrc_aux;
    uint8_t  tx_timeout;
    uint8_t  retry_left;
    uint8_t  last_rx0;
    uint16_t goodcrc_cnt;
    /* PHY 状态机 / CC 线真实波形 */
    uint8_t  tx_done;        /* IF_TX_END 是否真的置位 */
    uint8_t  rx_state_bm;    /* CONTROL[4:2] 出现过的状态位图 */
    uint8_t  rx_flags_bm;    /* IF_RX_BIT/BYTE/RESET/ACT 出现过的位图 */
    uint8_t  cc_tog_tx;      /* 发送期间 CC 比较器输出翻转次数 */
    uint8_t  cc_tog_rx;      /* 等GoodCRC期间 CC 比较器输出翻转次数 */
    uint8_t  max_byte_cnt;   /* 轮询期间 BMC_BYTE_CNT 最大值 */
} emark_tx_diag_t;
static emark_tx_diag_t s_tx_diag;

/* ---------------- 调试采集：默认关闭（见 emarker_pd.h 的 EMARK_DEBUG_DIAG） --------- */
#if EMARK_DEBUG_DIAG
/* 读取当前被 CC_SEL 选中的通信线比较器输出，并累计翻转次数 */
static void Diag_Sample_CC_Toggle(uint8_t *toggles)
{
    volatile uint16_t *port = (USBPD->CONFIG & CC_SEL) ?
                              &USBPD->PORT_CC2 : &USBPD->PORT_CC1;
    static uint8_t last;
    uint8_t now = (uint8_t)((*port & PA_CC_AI) ? 1 : 0);

    if (now != last) {
        if (*toggles < 255) {
            (*toggles)++;
        }
        last = now;
    }
}

/* 记录 PHY 接收状态机与中断标志轨迹（即使 CRC 失败也会留下痕迹） */
static void Diag_Sample_RX_State(void)
{
    uint8_t st = (uint8_t)((USBPD->CONTROL >> 2) & 0x07);
    uint8_t fl = 0;
    uint16_t status = USBPD->STATUS;

    if (st) {
        s_tx_diag.rx_state_bm |= (uint8_t)(1u << st);
    }
    if (status & IF_RX_BIT)   { fl |= 0x01; }
    if (status & IF_RX_BYTE)  { fl |= 0x02; }
    if (status & IF_RX_RESET) { fl |= 0x04; }
    if (status & IF_RX_ACT)   { fl |= 0x08; }
    s_tx_diag.rx_flags_bm |= fl;
    if (USBPD->BMC_BYTE_CNT > s_tx_diag.max_byte_cnt) {
        s_tx_diag.max_byte_cnt = (uint8_t)USBPD->BMC_BYTE_CNT;
    }
}
#else
/* 关闭诊断时编译为空操作，调用点无需改动、也不产生代码 */
#define Diag_Sample_CC_Toggle(t)   do { (void)(t); } while (0)
#define Diag_Sample_RX_State()     do { } while (0)
#endif

/**
 * @brief  从可能非 4 字节对齐的缓冲区读取小端 32 位 VDO
 * @note   RV32IMAC 不保证支持非对齐 lw；原实现 (uint32_t*)&pd_rx_buf[2] 有隐患
 */
static uint32_t Read_VDO_LE(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#if EMARK_DEBUG_DIAG
/**
 * @brief  用 USBPD 内置比较器的多档阈值粗测 CC 线电压
 * @param  cc: 1=CC1(PC14), 2=CC2(PC15)
 * @return 0=<0.22V, 1=>=0.22V, 2=>=0.45V, 3=>=0.55V,
 *         4=>=0.66V, 5=>=0.95V, 6=>=1.23V
 * @note   仅调试用；采样后恢复原 PORT_CCx 配置，不影响随后的 BMC 收发
 */
static uint8_t Diag_Sense_CC_Voltage(uint8_t cc)
{
    static const uint16_t th[6] = {
        CC_CMP_22, CC_CMP_45, CC_CMP_55, CC_CMP_66, CC_CMP_95, CC_CMP_123
    };
    volatile uint16_t *port = (cc == 1) ? &USBPD->PORT_CC1 : &USBPD->PORT_CC2;
    uint16_t saved = *port;
    uint8_t level = 0, i;

    for (i = 0; i < 6; i++) {
        *port = (uint16_t)((*port & ~CC_CMP_Mask) | th[i]);
        Delay_Us(20);
        if (*port & PA_CC_AI) {
            level = i + 1;
        } else {
            break;
        }
    }
    *port = saved;
    return level;
}

/**
 * @brief  VCONN 电气指纹：重新上电，测「加电到 CC 越过 1.23V」的爬升时间
 * @note   纯 Ra(约1k) 负载会瞬间到位；若插头内 E-Marker 带去耦/涌流电容
 *         (规范允许等效 <=10uF)，上电会呈现可观测的 RC 爬升。据此可判断
 *         「Ra 是真的、但芯片有没有在上电」。函数内完成一次完整上电。
 */
static void Diag_VCONN_Profile(uint8_t cc, uint16_t *rise)
{
    volatile uint16_t *port = (cc == 1) ? &USBPD->PORT_CC1 : &USBPD->PORT_CC2;
    uint16_t saved = *port;
    uint32_t t;

    *port = (uint16_t)((*port & ~CC_CMP_Mask) | CC_CMP_123);

    EMarker_Disable_VCONN();                       /* 先断电放电 */
    t = 0;
    while ((*port & PA_CC_AI) && t < 5000u) {
        Delay_Us(1);
        t++;
    }

    EMarker_Enable_VCONN(cc);                     /* 重新上电并计时 */
    t = 0;
    while (!(*port & PA_CC_AI) && t < 5000u) {
        Delay_Us(1);
        t++;
    }
    *rise = (uint16_t)t;

    *port = saved;
}

/** @brief  断电后测 CC 跌破 1.23V 的时间（有电容则明显变慢） */
static uint16_t Diag_VCONN_Fall(uint8_t cc)
{
    volatile uint16_t *port = (cc == 1) ? &USBPD->PORT_CC1 : &USBPD->PORT_CC2;
    uint16_t saved = *port;
    uint32_t t = 0;

    *port = (uint16_t)((*port & ~CC_CMP_Mask) | CC_CMP_123);
    EMarker_Disable_VCONN();
    while ((*port & PA_CC_AI) && t < 5000u) {
        Delay_Us(1);
        t++;
    }
    *port = saved;
    return (uint16_t)t;
}
#else
/* 关闭诊断：给出等价的空实现，调用点保持原样 */
#define Diag_Sense_CC_Voltage(cc)        (0u)
#define Diag_VCONN_Profile(cc, rise)     do { *(rise) = 0u; } while (0)
#define Diag_VCONN_Fall(cc)              (0u)
#endif

/* PD通信缓冲区 */
__attribute__((aligned(4))) static uint8_t pd_tx_buf[34] = {0};
__attribute__((aligned(4))) static uint8_t pd_rx_buf[34] = {0};

/* PD控制结构 */
static PD_CONTROL pd_ctl;

/* 超时定义 */
#define PD_COMM_TIMEOUT_MS    100  // PD通信超时时间（毫秒）
/* 分步重试时的响应窗口：规范 tSenderResponse 为 24~30ms，取 35ms 留余量 */
#define EMARK_FAST_RSP_TIMEOUT_MS  35u
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
 * @param  spec_rev: Specification Revision（可降级到 PD2.0 兼容老 E-Mark）
 * @return Message Header值（16位）
 */
static uint16_t Build_Message_Header(uint8_t msg_type, uint8_t num_do,
                                     uint8_t msg_id, uint8_t spec_rev)
{
    uint16_t header = 0;

    // Extended bit (bit 15)
    header |= 0 << 15;

    // Number of Data Objects (bits 14:12)
    header |= ((num_do & 0x07) << 12);

    // Message ID (bits 11:9)
    header |= ((msg_id & 0x07) << 9);

    // bit8: SOP'/SOP'' 下是 Cable Plug 位，0 = 消息来自 DFP/UFP（请求方）
    header |= (0 << 8);

    // Specification Revision (bits 7:6)
    header |= ((spec_rev & 0x03) << 6);

    // bit5: 对 SOP'/SOP'' 是 Reserved，规范要求 0；但原固件为 1 时
    // 部分线缆反而能读，故作为扫描维度自动适配（s_hdr_bit5）。
    header |= ((s_hdr_bit5 & 0x01) << 5);

    // Message Type (bits 4:0)
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
    /* 6 字节包的 BMC 发送只需约 0.2ms，原 100ms 超时在 CC 线被短路时会
     * 让每次重试白等 100ms；GoodCRC 规范要求 1ms 内返回，7.5ms 足够。
     * 扫描模式进一步缩短窗口，避免 16 组组合把启动拖到几十秒。 */
    /* 重试次数保持 3 次：上电后的第一包常因 CC 线未稳定而损坏，
     * 前几次成功读通其实靠的就是重发。分步路径只缩短单个窗口，不减少重试。 */
    uint8_t  retry  = s_diag_mode ? 1 : PD_RETRY_COUNT;
    /* 用 system_tick_ms 截止而不是「迭代次数 × 估算耗时」：
     * 迭代计时既不准也没法表达真实时间，失败时单次事务容易被拖到 200ms+ */
    uint32_t tx_win_ms  = s_diag_mode ? 2u  : 3u;
    uint32_t crc_win_ms = s_diag_mode ? 2u  : 3u;
    memset(&s_tx_diag, 0, sizeof(s_tx_diag));

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
        /* IF_TX_END 是 RW1C（写1清0，写0无效）。原写法 &= ~IF_TX_END 清不掉
         * 残留标志，会导致下面的等待立即退出、BMC 包被截断、E-Mark 不回 GoodCRC */
        USBPD->STATUS |= IF_TX_END;
        USBPD->CONTROL |= BMC_START;

        // 等待发送完成（tick 截止），同时采样 CC 线波形证明 BMC 真的上线
        uint32_t t_tx0 = system_tick_ms;
        while (!(USBPD->STATUS & IF_TX_END) &&
               (system_tick_ms - t_tx0) < tx_win_ms) {
            Diag_Sample_CC_Toggle(&s_tx_diag.cc_tog_tx);
            Delay_Us(2);   /* 轮询粒度保持细，截止时间由 tick 保证 */
        }

        if (!(USBPD->STATUS & IF_TX_END)) {
            // 发送超时（CC线被短路/偏置错误时常见），禁用LVE后重试
            s_tx_diag.tx_timeout = 1;
            s_tx_diag.retry_left = retry;
            USBPD->PORT_CC1 &= ~CC_LVE;
            USBPD->PORT_CC2 &= ~CC_LVE;
            continue;
        }

        s_tx_diag.tx_done = 1;
        USBPD->STATUS |= IF_TX_END;

        // 禁用LVE，恢复恒流源上拉模式
        USBPD->PORT_CC1 &= ~CC_LVE;
        USBPD->PORT_CC2 &= ~CC_LVE;

        if (!ack) {
            s_tx_diag.retry_left = retry;
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

        // 等待GoodCRC（tick 截止窗口 3ms，规范 tReceive ≤ 750us）
        uint32_t t_crc0 = system_tick_ms;
        while ((system_tick_ms - t_crc0) < crc_win_ms) {
            /* 关键判据：即使 CRC 解码失败，线缆发出的 BMC 波形仍会让 CC
             * 比较器输出翻转、让 RX_STATE 离开 000。据此可区分
             * 「线缆根本没发」与「发了但本芯片没解出」。 */
            Diag_Sample_CC_Toggle(&s_tx_diag.cc_tog_rx);
            Diag_Sample_RX_State();

            if (USBPD->STATUS & IF_RX_ACT) {
                /* 先记录原始状态，再清标志：BMC_AUX 指示本次收到的是 SOP/SOP'/SOP'' */
                s_tx_diag.goodcrc_cnt = USBPD->BMC_BYTE_CNT;
                s_tx_diag.goodcrc_aux = USBPD->STATUS & 0x03;
                s_tx_diag.last_rx0   = pd_rx_buf[0];
                USBPD->STATUS |= IF_RX_ACT;

                // 检查是否为GoodCRC（保留原判定条件，避免改变已能成功的线缆）
                if ((pd_rx_buf[0] & 0x1F) == DEF_TYPE_GOODCRC) {
                    s_tx_diag.goodcrc_seen = 1;
                    s_tx_diag.retry_left   = retry;
                    return 0;
                }
            }
            Delay_Us(2);
        }
        s_tx_diag.retry_left = retry;
    }

    return 1;  // 发送失败
}

/**
 * @brief  发送GoodCRC确认
 * @param  msg_id: Message ID from received message
 * @param  spec_rev: 被应答消息的 Specification Revision（GoodCRC 必须沿用）
 * @return 0-成功, 1-失败
 * @note   收到任何PD消息后必须在规定时间内回复GoodCRC
 *         GoodCRC是Control Message，Message Type = 0x01 (DEF_TYPE_GOODCRC)
 */
static uint8_t PD_Send_GoodCRC(uint8_t msg_id, uint8_t spec_rev)
{
    uint16_t msg_header;

    // 构建GoodCRC Message Header（num_do = 0）
    msg_header = Build_Message_Header(DEF_TYPE_GOODCRC, 0, msg_id, spec_rev);

    // GoodCRC数据（小端序）
    pd_tx_buf[0] = msg_header & 0xFF;
    pd_tx_buf[1] = (msg_header >> 8) & 0xFF;

    // SOP 类型必须与请求一致：SOP' 或 SOP''
    return PD_Send_Packet(pd_tx_buf, 2, s_tx_sop, 0);
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
        /* 原来是 Delay_Ms(1) / Delay_Us()：前者 1ms 粒度会把 GoodCRC 应答
         * 推迟到 tReceiverResponse 之外，后者有 SysTick 回绕盲区。
         * 改用 20us 紧凑忙等，超时仍由 system_tick_ms 判定 */
        Delay_Us(20);
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

    // USBPD_PHY_V33: 3.3V VDD 应用直接用 VDD（必须置 1）
    // USBPD_IN_HVT: 高阈值(~2.2V)输入；本设计通信线静态仅约 1.68V，
    // 若该阈值作用于 BMC 接收会完全收不到波形，故作为扫描维度可关闭。
    AFIO->CTLR |= USBPD_PHY_V33;
    if (s_hvt_mode) {
        AFIO->CTLR |= USBPD_IN_HVT;
    } else {
        AFIO->CTLR &= ~USBPD_IN_HVT;
    }

    // 初始化USBPD寄存器
    USBPD->CONTROL = 0;                  // 清除CONTROL（停止BMC/TX）
    USBPD->CONFIG = PD_DMA_EN;
    if (s_filt_mode) {
        USBPD->CONFIG |= (1u << 0);      /* PD_FILT_EN: 输入滤波 */
    }
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
        USBPD->PORT_CC1 = CC_CMP_66 | s_rp_mode;  // Rp上拉（电流档可扫描）
        USBPD->PORT_CC2 = CC_CMP_66;              // CC2不配置（VCONN供电）
    } else {
        // PD通信使用CC2
        USBPD->PORT_CC1 = CC_CMP_66;              // CC1不配置（VCONN供电）
        USBPD->PORT_CC2 = CC_CMP_66 | s_rp_mode;  // Rp上拉（电流档可扫描）
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

    // 步骤5: 提取Message ID / SpecRev 并立即发送GoodCRC
    // Message Header在pd_rx_buf[0:1]，小端序
    uint16_t rx_msg_header = pd_rx_buf[0] | ((uint16_t)pd_rx_buf[1] << 8);
    uint8_t  rx_msg_id     = (rx_msg_header >> 9) & 0x07;
    uint8_t  rx_spec_rev   = (rx_msg_header >> 6) & 0x03;

    /* 先应答再解析：GoodCRC 必须在 tReceiverResponse 内发出，解析会白白耗掉时间 */
    PD_Send_GoodCRC(rx_msg_id, rx_spec_rev);

    // 步骤6: 解析响应
    ret = EMarker_Parse_Response(info);

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
    msg_header = Build_Message_Header(DEF_TYPE_VENDOR_DEFINED, 1,
                                      pd_ctl.Msg_ID, s_spec_rev);

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

    // 发送数据包（SOP' 或 SOP''，由当前组合决定）
    return PD_Send_Packet(pd_tx_buf, tx_len, s_tx_sop, 1);
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

    /* pd_rx_buf[2] 只有 2 字节对齐，RV32 上不能用 uint32_t* 解引用；
     * 逐字节拼接，规避非对齐加载异常 */
    const uint8_t *ptr = (const uint8_t *)&pd_rx_buf[2];

    // 解析VDM数据
    info->vdm_header = Read_VDO_LE(ptr);
    ptr += 4;
    num_do--;

    // 检查VDM Header是否为ACK响应
    if ((info->vdm_header & VDM_HDR_CMD_TYPE_MASK) != VDM_HDR_CMD_ACK) {
        return 1;
    }

    // 解析ID Header
    if (num_do > 0) {
        info->id_header = Read_VDO_LE(ptr);
        ptr += 4;
        num_do--;
        EMarker_Parse_ID_Header(info->id_header, info);
        info->valid = 1;
        info->id_header_present = 1;
    }

    // 解析Cert Stat VDO（如果存在）
    if (num_do > 0) {
        info->cert_stat = Read_VDO_LE(ptr);
        ptr += 4;
        num_do--;
    }

    // 解析Product VDO（如果存在）
    if (num_do > 0) {
        info->product_vdo = Read_VDO_LE(ptr);
        ptr += 4;
        num_do--;
        // USB Product ID位于Product VDO Bit31-16（Bit15-0为bcdDevice）
        info->product_id = (uint16_t)(info->product_vdo >> 16);
    }

    // 解析Cable VDO1（如果存在）—— 内部按 cable_type 分派 Passive/Active
    if (num_do > 0) {
        info->cable_vdo1 = Read_VDO_LE(ptr);
        ptr += 4;
        num_do--;
        EMarker_Parse_Cable_VDO1(info->cable_vdo1, info);
    }

    // 解析Cable VDO2（仅 Active Cable 有定义）
    if (num_do > 0) {
        info->cable_vdo2 = Read_VDO_LE(ptr);
        ptr += 4;
        num_do--;
        EMarker_Parse_Cable_VDO2(info->cable_vdo2, info);
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
 * @brief  解析Cable VDO1（按线缆类型分派）
 */
void EMarker_Parse_Cable_VDO1(uint32_t cable_vdo1, emarker_info_t *info)
{
    if (info == NULL) {
        return;
    }

    info->cable_vdo1 = cable_vdo1;

    /* ---- 两种线缆共用的位（位置相同） ---- */
    // USB速度等级 (Bit 2-0)
    info->usb_speed = (uint8_t)(cable_vdo1 & CABLE_VDO1_USB_SPEED_MASK);
    // EPR支持 (Bit 17)
    info->epr_capable = (cable_vdo1 & CABLE_VDO1_EPR_CAPABLE) ? 1 : 0;
    // VDO版本 (Bit 23-21)
    info->vdo_version = (uint8_t)((cable_vdo1 & CABLE_VDO1_VDO_VERSION_MASK) >> 21);
    // 插头形态 (Bit 19-18)
    info->plug_type = (uint8_t)((cable_vdo1 & CABLE_VDO1_PLUG_TYPE_MASK) >> 18);
    // 最高 VBUS 电压 (Bit 10-9)：仅 11b=50V，01b/10b 已废弃按 20V
    info->max_vbus_50v = (((cable_vdo1 & CABLE_VDO1_MAX_VBUS_MASK) >> 9) == CABLE_MAXV_50V) ? 1 : 0;
    // 终端类型 (Bit 12-11)：原值保留，含义按类型解释
    info->termination = (uint8_t)((cable_vdo1 & CABLE_VDO1_TERMINATION_TYPE) >> 11);

    if (info->cable_type == CABLE_TYPE_ACTIVE) {
        /* ================= Active Cable VDO1（Table 6.43） ================= */
        uint8_t current_cap;

        // Bit8/7：SBU（反逻辑，0 才表示支持）
        info->sbu_supported = (cable_vdo1 & ACTIVE_VDO1_SBU_SUPPORTED) ? 0 : 1;
        info->sbu_active    = (cable_vdo1 & ACTIVE_VDO1_SBU_TYPE) ? 1 : 0;
        // Bit4/3：直通VBUS / SOP''控制器（仅 Active 有定义）
        info->vbus_through  = (cable_vdo1 & ACTIVE_VDO1_VBUS_THROUGH) ? 1 : 0;
        info->sop2_present  = (cable_vdo1 & ACTIVE_VDO1_SOP2_PRESENT) ? 1 : 0;

        /* Bit6:5 电流能力：00b/11b 为 Invalid（Shall Not be used），按 3A 保守处理；
         * 且规范规定 VBUS Through Cable = 0 时该字段 Shall be Ignored —— 此时线内
         * 没有直通 VBUS 线，标称电流没有意义，置为 0xFF 供显示层显示 "N/A"。 */
        current_cap = (uint8_t)((cable_vdo1 & CABLE_VDO1_CURRENT_MASK) >> 5);
        if (!info->vbus_through) {
            info->current_capability = 0xFF;
        } else {
            info->current_capability = (current_cap == 2u) ? CABLE_CURRENT_5A
                                                           : CABLE_CURRENT_3A;
        }
    } else {
        /* ================= Passive Cable VDO1（Table 6.42） ================= */
        uint8_t current_cap;

        // Bit8/7、Bit4/3 在 Passive 中是 Reserved，保持 0
        info->sbu_supported = 0;
        info->sbu_active    = 0;
        info->vbus_through  = 1;   // 被动线必然含 VBUS 线
        info->sop2_present  = 0;

        /* Bit6:5（Table 6.42）：
         *   00b Invalid→按 01b(3A)；01b=3A；10b=5A；11b Invalid→按 01b(3A)
         * 注意与早期 PD3.0 草案（00b=3A/01b=5A）相反，只有 10b 才是 5A */
        current_cap = (uint8_t)((cable_vdo1 & CABLE_VDO1_CURRENT_MASK) >> 5);
        info->current_capability = (current_cap == 2u) ? CABLE_CURRENT_5A
                                                       : CABLE_CURRENT_3A;
    }
}

/**
 * @brief  解析 Active Cable VDO2（Table 6.44）
 * @note   仅 Active Cable 会返回该 VDO；Passive 线缆解析时保持全 0
 */
void EMarker_Parse_Cable_VDO2(uint32_t cable_vdo2, emarker_info_t *info)
{
    if (info == NULL) {
        return;
    }

    info->cable_vdo2 = cable_vdo2;

    if (info->cable_type != CABLE_TYPE_ACTIVE) {
        return;   // Passive 没有 VDO2 定义
    }

    info->max_temp      = (uint8_t)((cable_vdo2 & ACTIVE_VDO2_MAX_TEMP_MASK) >> 24);
    info->shutdown_temp = (uint8_t)((cable_vdo2 & ACTIVE_VDO2_SHUTDOWN_MASK) >> 16);
    info->optical       = (cable_vdo2 & ACTIVE_VDO2_OPTICAL) ? 1 : 0;
    info->retimer       = (cable_vdo2 & ACTIVE_VDO2_RETIMER) ? 1 : 0;
    // 以下三项为反逻辑位：0 表示"支持"
    info->usb4_supported = (cable_vdo2 & ACTIVE_VDO2_USB4_SUPPORTED) ? 0 : 1;
    info->usb2_supported = (cable_vdo2 & ACTIVE_VDO2_USB2_SUPPORTED) ? 0 : 1;
    info->usb3_supported = (cable_vdo2 & ACTIVE_VDO2_USB3_SUPPORTED) ? 0 : 1;
    info->lanes          = (cable_vdo2 & ACTIVE_VDO2_TWO_LANES) ? 2 : 1;
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
 * @brief  用一组完整配置执行一次 E-Mark PD 事务（扫描与正式流程共用）
 * @param  vconn:    VCONN供电通道 1/2
 * @param  pd_cc:    USBPD通信通道 1/2
 * @param  gnd:      右口接地脚 0=浮空(不外加Rd), 1=PA1, 2=PA2
 * @param  sop:      UPD_SOP1(SOP') 或 UPD_SOP2(SOP'')
 * @param  spec_rev: DEF_PD_REVISION_30 / DEF_PD_REVISION_20
 * @param  info:     E-Mark信息输出
 * @param  diag:     诊断记录指针，正式流程传 NULL
 * @return EMARK_OK / EMARK_PD_FAIL
 * @note   每个组合独立做 PD_Init → VCONN → 20ms稳定 → Rd → 发送 → 接收 →
 *         GoodCRC → 解析 → 清理，避免上一组合的 PHY 状态污染下一组合。
 */
static uint8_t EMarker_Do_Transaction(uint8_t vconn, uint8_t pd_cc, uint8_t gnd,
                                      uint8_t sop, uint8_t spec_rev,
                                      emarker_info_t *info,
                                      emark_diag_item_t *diag)
{
    uint8_t  ret;
    uint16_t rx_msg_header;
    uint8_t  rx_msg_id, rx_spec_rev;

    if (diag != NULL) {
        memset(diag, 0, sizeof(emark_diag_item_t));
        diag->vconn_cc   = vconn;
        diag->pd_comm_cc = pd_cc;
        diag->gnd_pin    = gnd;
        diag->sop        = (sop == UPD_SOP2) ? 1 : 0;
        diag->spec_rev   = spec_rev;
    }

    s_tx_sop   = sop;
    s_spec_rev = spec_rev;

    if (diag != NULL) {
        diag->hdr_bit5 = s_hdr_bit5;
        diag->rp_mode  = s_rp_mode;
        diag->hvt      = s_hvt_mode;
        diag->filt_en  = s_filt_mode;
        /* 供电形态：0=VCONN-only 1=VBUS+VCONN 同时 2=VBUS-only */
        if (!s_vbus_mode) {
            diag->vbus_on  = 0;
            diag->pwr_mode = 0;
        } else if (s_vconn_off) {
            diag->vbus_on  = 2;
            diag->pwr_mode = 2;
        } else {
            diag->vbus_on  = 1;
            diag->pwr_mode = 1;
        }
        diag->pwr_settle_ms = s_vbus_mode ? s_vbus_settle_ms : EMARK_VCONN_SETTLE_MS;
    }

    /* 电源建立：VBUS 与 VCONN 同时上电，只等一次建立时间。
     * 原来先给 VBUS 等 settle、再配 PD 上 VCONN 又等 60ms，两段串行；
     * 合并后单次事务至少省 60ms，且两种供电同时存在对双供电芯片更稳。 */
    uint32_t t_seg = system_tick_ms;

    if (s_vbus_mode) {
        Set_Test_Voltage_Enable(1);
    }

    EMarker_PD_Init(vconn, pd_cc);
    if (s_vconn_off) {
        /* 典型 VBUS 供电型线缆不需要 VCONN：VCONN 脚只是 Ra，
         * 强行加 3.3V 反而让该 CC 长期高电平，可能干扰芯片判断 */
        EMarker_Disable_VCONN();
    } else if (diag != NULL && s_deep_diag) {
        /* 内部完成一次断电→重新上电，顺便采 VCONN 爬升指纹
         * （约 40ms，只在调试扫描时开启，避免拖慢按键响应） */
        Diag_VCONN_Profile(vconn, &diag->vconn_rise_us);
    } else {
        EMarker_Enable_VCONN(vconn);
    }
    /* 一次等待同时覆盖 VBUS 上电与 VCONN 建立（含 Ra≈1k 上电时间） */
    Delay_Ms(s_vbus_settle_ms ? s_vbus_settle_ms : EMARK_VCONN_SETTLE_MS);
    if (gnd != 0) {
        EMarker_Enable_Rd_Pulldown(gnd);
    } else {
        /* 右口完全浮空：覆盖芯片自带 Rd、不需要外部终端的线缆 */
        EMarker_Disable_Rd_Pulldown();
    }

    if (EMARK_DEBUG_DIAG && diag != NULL) {
        Delay_Ms(2);                /* 等 CC 线 RC 稳定 */
        diag->cc1_v   = Diag_Sense_CC_Voltage(1);
        diag->cc2_v   = Diag_Sense_CC_Voltage(2);
        diag->gpio_cc = (uint8_t)((GPIOC->INDR >> 14) & 0x03);
        diag->gpio_pa = (uint8_t)((GPIOA->INDR >> 1) & 0x03);
        if (s_vbus_mode) {
            /* VBUS 经 100Ω 限流：若插头内芯片真在取电，压降会明显；
             * 3.3V 无压降说明没有任何负载，即插头内只有 Ra 电阻 */
            uint16_t adc = ADC_Average_Read(ADC_CHANNEL_V_PWR_H, 4);
            diag->vbus_adc8 = (uint8_t)(adc >> 4);
        }
    }

    if (EMARK_DEBUG_DIAG && diag != NULL) {
        uint32_t d = system_tick_ms - t_seg;
        if (d > g_t_setup) g_t_setup = d;
        t_seg = system_tick_ms;
    }

    ret = EMarker_Send_Discover_Identity();

    if (EMARK_DEBUG_DIAG && diag != NULL) {
        uint32_t d = system_tick_ms - t_seg;
        if (d > g_t_tx) g_t_tx = d;
        t_seg = system_tick_ms;
    }

    if (EMARK_DEBUG_DIAG && diag != NULL) {
        diag->goodcrc_cnt  = s_tx_diag.goodcrc_cnt;
        diag->goodcrc_aux  = s_tx_diag.goodcrc_aux;
        diag->tx_retry     = s_tx_diag.retry_left;
        diag->tx_done      = s_tx_diag.tx_done;
        diag->rx_state_bm  = s_tx_diag.rx_state_bm;
        diag->rx_flags_bm  = s_tx_diag.rx_flags_bm;
        diag->cc_tog_tx    = s_tx_diag.cc_tog_tx;
        diag->cc_tog_rx    = s_tx_diag.cc_tog_rx;
        diag->max_byte_cnt = s_tx_diag.max_byte_cnt;
    }

    if (ret != 0) {
        if (diag != NULL) {
            diag->stage = 1;
            diag->fail  = s_tx_diag.tx_timeout ? 1 : 2;
        }
        goto cleanup;
    }

    PD_Enter_Rx_Mode();
    /* 分步重试路径：规范 tSenderResponse 最大 30ms，留 5ms 余量后放弃本次候选 */
    ret = PD_Wait_Response(s_fast_fail ? EMARK_FAST_RSP_TIMEOUT_MS
                                       : (s_diag_mode ? 60u : PD_COMM_TIMEOUT_MS));
    if (ret != 0) {
        if (diag != NULL) { diag->stage = 2; diag->fail = 3; }
        goto cleanup;
    }

    rx_msg_header = pd_rx_buf[0] | ((uint16_t)pd_rx_buf[1] << 8);
    rx_msg_id     = (rx_msg_header >> 9) & 0x07;
    rx_spec_rev   = (rx_msg_header >> 6) & 0x03;

    if (diag != NULL) {
        diag->bmc_aux      = USBPD->STATUS & 0x03;
        diag->byte_cnt     = USBPD->BMC_BYTE_CNT;
        diag->rx_head[0]   = pd_rx_buf[0];
        diag->rx_head[1]   = pd_rx_buf[1];
        memcpy(diag->rx_bytes, pd_rx_buf, 8);
        diag->vdm_header   = Read_VDO_LE(&pd_rx_buf[2]);
    }

    /* 先发 GoodCRC 再解析，保证应答在 tReceiverResponse 内 */
    PD_Send_GoodCRC(rx_msg_id, rx_spec_rev);

    /* 解析前清零：解析按 num_do 逐段填字段，少了某个 VDO（例如没有 Cable VDO2）
     * 就会残留上一根线的旧值（曾出现 cable_vdo2=0xAAAA 被当真实数据显示） */
    memset(info, 0, sizeof(emarker_info_t));

    ret = EMarker_Parse_Response(info);
    if (ret == 0 && info->valid) {
        if (diag != NULL) diag->stage = 4;
        ret = EMARK_OK;
    } else {
        if (diag != NULL) { diag->stage = 3; diag->fail = 4; }
        ret = EMARK_PD_FAIL;
    }

cleanup:
    EMarker_Disable_Rd_Pulldown();
    if (s_vconn_off) {
        EMarker_Disable_VCONN();
    } else if (diag != NULL && s_deep_diag) {
        diag->vconn_fall_us = Diag_VCONN_Fall(vconn);  /* 内部断电并计时 */
    } else {
        EMarker_Disable_VCONN();
    }
    /* VBUS 必须在每组结束时立即撤掉：否则会干扰后续通断/电阻测试 */
    if (s_vbus_mode) {
        Set_Test_Voltage_Enable(0);
    }
    return ret;
}

/* 右口 Rd 尝试顺序：0=完全浮空（芯片自带终端），2=PA2 接地，1=PA1 接地。
 * 已实测 PA1 接地会把通信线硬短路（togTX≈0），故放最后。 */
static const uint8_t k_gnd_order[3] = {0, 2, 1};

/**
 * @brief  固定供电配置读取：VBUS 与 VCONN 同时供电
 * @param  info:   E-Mark 信息输出
 * @param  record: 1=写入 g_emark_diag 供 GDB 回读
 * @param  settle_ms: 上电后等待芯片建立的时间(ms)
 * @return EMARK_OK / EMARK_PD_FAIL / EMARK_NOT_FOUND
 * @note   最多 6 次事务（SOP'×3 + SOP''×3），命中即停；每次只有一个建立
 *         等待（VBUS 与 VCONN 同时上电），最坏约 6×(settle+12)ms。
 */
static uint8_t EMarker_Fixed_Ladder(emarker_info_t *info, uint8_t record,
                                    uint16_t settle_ms)
{
    uint8_t vconn_cc, pd_cc, si, gi, ret;
    emark_diag_item_t *it;

    if (!EMarker_Detect_CC_Channel(&vconn_cc, &pd_cc)) {
        return EMARK_NOT_FOUND;
    }

    if (record) {
        /* 记录检测阶段的通道判定，便于回读对比 */
        g_emark_diag.detect_cc1_ai   = (USBPD->PORT_CC1 & PA_CC_AI) ? 1 : 0;
        g_emark_diag.detect_cc2_ai   = (USBPD->PORT_CC2 & PA_CC_AI) ? 1 : 0;
        g_emark_diag.detect_vconn_cc = vconn_cc;
        g_emark_diag.detect_pd_cc    = pd_cc;
    }

    s_hdr_bit5   = 0;
    s_rp_mode    = CC_PU_330;
    s_hvt_mode   = 1;
    s_filt_mode  = 0;
    s_vbus_mode  = 1;                        /* 读取期间打开 V_PWR_EN */
    s_vconn_off  = 0;                        /* 同时给 VCONN */
    s_vbus_settle_ms = settle_ms;

    for (si = 0; si <= 1; si++) {
        for (gi = 0; gi < 3; gi++) {
            it = NULL;
            if (record) {
                if (g_emark_diag.count >= EMARK_DIAG_MAX) {
                    return EMARK_PD_FAIL;
                }
                it = &g_emark_diag.items[g_emark_diag.count];
            }

            ret = EMarker_Do_Transaction(vconn_cc, pd_cc, k_gnd_order[gi],
                                         si ? UPD_SOP2 : UPD_SOP1,
                                         DEF_PD_REVISION_30, info, it);
            if (record) {
                if (ret == EMARK_OK) {
                    g_emark_diag.best_index    = g_emark_diag.count;
                    g_emark_diag.best_vconn_cc = vconn_cc;
                    g_emark_diag.best_pd_cc    = pd_cc;
                    g_emark_diag.best_gnd_pin  = k_gnd_order[gi];
                    g_emark_diag.best_sop      = si;
                    g_emark_diag.best_spec_rev = DEF_PD_REVISION_30;
                }
                g_emark_diag.count++;
            }
            if (ret == EMARK_OK) {
                return EMARK_OK;
            }
        }
    }
    return EMARK_PD_FAIL;
}

/**
 * @brief  兼容回退：其它供电方式的线缆（VCONN-only / VBUS-only）
 * @note   固定 VBUS+VCONN 同时供电失败时才走这里，最多 6 次事务。
 */
static uint8_t EMarker_Compat_Ladder(emarker_info_t *info)
{
    uint8_t vconn_cc, pd_cc, gi, ret;

    if (!EMarker_Detect_CC_Channel(&vconn_cc, &pd_cc)) {
        return EMARK_NOT_FOUND;
    }

    s_hdr_bit5  = 0;
    s_rp_mode   = CC_PU_330;
    s_hvt_mode  = 1;
    s_filt_mode = 0;

    /* ① 纯 VCONN（规范默认，绝大多数线缆） */
    s_vbus_mode      = 0;
    s_vconn_off      = 0;
    s_vbus_settle_ms = 0;
    for (gi = 0; gi < 3; gi++) {
        ret = EMarker_Do_Transaction(vconn_cc, pd_cc, k_gnd_order[gi],
                                     UPD_SOP1, DEF_PD_REVISION_30, info, NULL);
        if (ret == EMARK_OK) {
            return EMARK_OK;
        }
    }

    /* ② 纯 VBUS（历史实测可用的备用路径） */
    s_vbus_mode      = 1;
    s_vconn_off      = 1;
    s_vbus_settle_ms = EMARK_VBUS_SETTLE_LONG;
    for (gi = 0; gi < 3; gi++) {
        ret = EMarker_Do_Transaction(vconn_cc, pd_cc, k_gnd_order[gi],
                                     UPD_SOP1, DEF_PD_REVISION_30, info, NULL);
        if (ret == EMARK_OK) {
            return EMARK_OK;
        }
    }

    s_vbus_mode      = 0;
    s_vconn_off      = 0;
    s_vbus_settle_ms = 0;
    return EMARK_PD_FAIL;
}

/******************************************************************************
 *  分步重试状态机：每次主循环只做一次 PD 事务，读取失败时也能响应按键
 *
 *  原实现把 18 次候选一口气跑完：单次失败事务 = 发送重试3×(20+15)ms
 *  + 等响应 100ms ≈ 205ms，整轮 3s 以上，期间主循环完全无法扫描按键。
 *  改为单步后，每步最多阻塞约 60ms（5ms 建立 + 3/5ms 发送 + 35ms 响应窗口），
 *  步与步之间回到主循环，按键与插拔检测都能及时执行。
 ******************************************************************************/
#define EMARK_TASK_CAND_MAX     18u   /* 6(快读) + 6(加时) + 3(VCONN) + 3(VBUS) */
#define EMARK_TASK_GAP_MS       20u   /* 两步之间至少让出 20ms 给主循环 */

/* 单步事务的最大耗时（ms），调试用：定位阻塞到底在哪个阶段 */
volatile uint32_t g_task_max_ms = 0;

static uint8_t  s_task_active = 0;
static uint8_t  s_task_idx;
static uint8_t  s_task_vconn, s_task_pd;
static uint32_t s_task_last_ms;

/* 按候选序号配置供电方式（与 Fixed_Ladder / Compat_Ladder 等价） */
static void EMarker_Task_Apply(uint8_t idx)
{
    s_hdr_bit5  = 0;
    s_rp_mode   = CC_PU_330;
    s_hvt_mode  = 1;
    s_filt_mode = 0;

    if (idx < 6) {                 /* 快读：VBUS+VCONN 同时供电，5ms */
        s_vbus_mode      = 1;
        s_vconn_off      = 0;
        s_vbus_settle_ms = EMARK_PWR_SETTLE_MS;
    } else if (idx < 12) {         /* 加时：同一配置，40ms */
        s_vbus_mode      = 1;
        s_vconn_off      = 0;
        s_vbus_settle_ms = EMARK_PWR_SETTLE_RETRY_MS;
    } else if (idx < 15) {         /* 纯 VCONN（规范默认） */
        s_vbus_mode      = 0;
        s_vconn_off      = 0;
        s_vbus_settle_ms = 0;
    } else {                       /* 纯 VBUS（历史可用备用路径）
                                    * 换 50Ω 后芯片 ~5ms 就起，不再等 100ms，
                                    * 把最后这一组的单步阻塞压到 160ms 以内 */
        s_vbus_mode      = 1;
        s_vconn_off      = 1;
        s_vbus_settle_ms = EMARK_PWR_SETTLE_RETRY_MS;
    }
}

uint8_t EMarker_Task_Busy(void)
{
    return s_task_active;
}

void EMarker_Task_Abort(void)
{
    s_task_active = 0;
    s_fast_fail   = 0;
    EMarker_Disable_Rd_Pulldown();
    EMarker_Disable_VCONN();
    Set_Test_Voltage_Enable(0);
}

/**
 * @brief  推进一步 E-Mark 读取（非阻塞）
 * @param  info: 成功时填充
 * @return EMARK_OK 成功 / EMARK_TASK_BUSY 进行中 / EMARK_PD_FAIL 全部失败 /
 *         EMARK_NOT_FOUND 无 E-Mark
 * @note   首次调用只做初始化并返回 BUSY；之后每一步执行一次事务。
 */
uint8_t EMarker_Task_Step(emarker_info_t *info)
{
    uint8_t  ret, sop, gnd;
    emark_diag_item_t *it;

    if (info == NULL) {
        return EMARK_NOT_FOUND;
    }

    s_diag_mode = 0;

    /* --- 启动一轮新序列 --- */
    if (!s_task_active) {
        if (!EMarker_Detect_CC_Channel(&s_task_vconn, &s_task_pd)) {
            return EMARK_NOT_FOUND;
        }
        memset(&g_emark_diag, 0, sizeof(emark_diag_t));
        g_emark_diag.magic       = EMARK_DIAG_MAGIC;
        g_emark_diag.best_index  = 0xFF;
        g_emark_diag.detect_cc1_ai   = (USBPD->PORT_CC1 & PA_CC_AI) ? 1 : 0;
        g_emark_diag.detect_cc2_ai   = (USBPD->PORT_CC2 & PA_CC_AI) ? 1 : 0;
        g_emark_diag.detect_vconn_cc = s_task_vconn;
        g_emark_diag.detect_pd_cc    = s_task_pd;

        s_task_idx    = 0;
        s_task_active = 1;
        s_fast_fail   = 1;          /* 失败路径用短超时，尽快让步 */
        s_task_last_ms = system_tick_ms;
        return EMARK_TASK_BUSY;
    }

    /* --- 步间让出，保证按键/显示有执行机会 --- */
    if ((system_tick_ms - s_task_last_ms) < EMARK_TASK_GAP_MS) {
        return EMARK_TASK_BUSY;
    }
    s_task_last_ms = system_tick_ms;

    EMarker_Task_Apply(s_task_idx);
    if (s_task_idx < 12) {
        sop = ((s_task_idx % 6u) >= 3u) ? UPD_SOP2 : UPD_SOP1;
        gnd = k_gnd_order[s_task_idx % 3u];
    } else if (s_task_idx < 15) {
        sop = UPD_SOP1;
        gnd = k_gnd_order[s_task_idx - 12u];
    } else {
        sop = UPD_SOP1;
        gnd = k_gnd_order[s_task_idx - 15u];
    }

    it = NULL;
    if (g_emark_diag.count < EMARK_DIAG_MAX) {
        it = &g_emark_diag.items[g_emark_diag.count];
    }

    {
        uint32_t t_step = system_tick_ms;
        ret = EMarker_Do_Transaction(s_task_vconn, s_task_pd, gnd, sop,
                                     DEF_PD_REVISION_30, info, it);
        t_step = system_tick_ms - t_step;
#if EMARK_DEBUG_DIAG
        if (t_step > g_task_max_ms) {
            g_task_max_ms = t_step;
        }
#else
        (void)t_step;
#endif
    }
    if (it != NULL) {
        if (ret == EMARK_OK) {
            g_emark_diag.best_index    = g_emark_diag.count;
            g_emark_diag.best_vconn_cc = s_task_vconn;
            g_emark_diag.best_pd_cc    = s_task_pd;
            g_emark_diag.best_gnd_pin  = gnd;
            g_emark_diag.best_sop      = (sop == UPD_SOP2) ? 1 : 0;
            g_emark_diag.best_spec_rev = DEF_PD_REVISION_30;
        }
        g_emark_diag.count++;
    }

    if (ret == EMARK_OK) {
        s_task_active  = 0;
        s_fast_fail    = 0;
        g_emark_diag.done = 1;
        return EMARK_OK;
    }

    if (++s_task_idx >= EMARK_TASK_CAND_MAX) {
        s_task_active  = 0;
        s_fast_fail    = 0;
        g_emark_diag.done = 1;
        return EMARK_PD_FAIL;
    }
    return EMARK_TASK_BUSY;
}

/**
 * @brief  完整的E-Mark检测流程（高层API）
 * @note   固定配置：VBUS 与 VCONN 同时供电，一次事务成功即返回；
 *         失败才走兼容回退（VCONN-only / VBUS-only）。
 */
uint8_t EMarker_Full_Detect(emarker_info_t *info)
{
    uint8_t ret;

    if (info == NULL) {
        return EMARK_NOT_FOUND;
    }

    s_diag_mode = 0;

    /* 已确认无芯片时抑制 200ms 周期的重复事务（长事务会占住主循环、
     * 使按键无响应）；每 5 秒重试一次，便于换线后自动恢复 */
    if (g_emark_diag.done && g_emark_diag.best_index == 0xFF) {
        if ((system_tick_ms - s_last_retry_ms) < 5000u) {
            return EMARK_PD_FAIL;
        }
        s_last_retry_ms = system_tick_ms;
    }

    /* 每次读取都刷新诊断快照：固定配置成功时通常只有 1 组数据 */
    memset(&g_emark_diag, 0, sizeof(emark_diag_t));
    g_emark_diag.magic      = EMARK_DIAG_MAGIC;
    g_emark_diag.best_index = 0xFF;

    ret = EMarker_Fixed_Ladder(info, 1, EMARK_PWR_SETTLE_MS);
    if (ret != EMARK_OK) {
        /* 边缘线缆：芯片上电偏慢时用较长建立时间再试一轮（只在失败时付出代价） */
        ret = EMarker_Fixed_Ladder(info, 1, EMARK_PWR_SETTLE_RETRY_MS);
    }
    if (ret != EMARK_OK) {
        ret = EMarker_Compat_Ladder(info);
    }
    g_emark_diag.done = 1;
    return ret;
}

/**
 * @brief  重新扫描（换线后适配），结果固化到 g_emark_diag 供 GDB 回读
 * @note   固定配置：VBUS + VCONN 同时供电，SOP'/SOP'' × Rd{浮空,PA2,PA1}
 *         最多 6 组；命中即停。相比原 36 组扫描，最坏耗时从约 4s 降到约 0.1s。
 */
uint8_t EMarker_Debug_Scan(void)
{
#if EMARK_DEBUG_DIAG
    emarker_info_t info;
    uint8_t ret;

    s_diag_mode = 1;
    s_deep_diag = 1;
    memset(&g_emark_diag, 0, sizeof(emark_diag_t));
    g_emark_diag.magic      = EMARK_DIAG_MAGIC;
    g_emark_diag.best_index = 0xFF;

    ret = EMarker_Fixed_Ladder(&info, 1, EMARK_PWR_SETTLE_MS);

    s_diag_mode = 0;
    s_deep_diag = 0;
    s_tx_sop    = UPD_SOP1;
    s_spec_rev  = DEF_PD_REVISION_30;
    s_hdr_bit5  = 0;
    s_rp_mode   = CC_PU_330;
    s_hvt_mode  = 1;
    s_filt_mode = 0;
    s_vbus_mode = 0;
    s_vbus_settle_ms = 0;
    s_vconn_off = 0;
    EMarker_Disable_Rd_Pulldown();
    EMarker_Disable_VCONN();
    Set_Test_Voltage_Enable(0);   /* 兜底：确保 VBUS 不被留在开启状态 */
    g_emark_diag.done = 1;
    return ret;
#else
    /* 诊断关闭：本接口保留符号以便上层条件编译，调用时不做任何扫描 */
    return EMARK_NOT_FOUND;
#endif
}
