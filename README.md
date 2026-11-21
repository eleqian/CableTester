# USB线缆测试仪 (CableTester)

基于CH32X035G8U的USB线缆测试仪。插入线缆即可一键检测：**所有信号线通断、电源线电阻、E-Mark芯片信息、华为快充线**，结果在TFT彩屏上实时显示。

**功能特性**：

- **主控**：CH32X035G8U（RISC-V RV32IMAC，48MHz）
- **屏幕**：ST7735S 160×80 彩色 TFT
- **交互**：1 个按键切换显示模式
- **两组测试接口**：Type-A + Type-C（左侧）、Micro-B + Type-C（右侧）
- **Type-C 全信号通断测试**：屏蔽层(EH)、VBUS/GND、CC、SBU、D+/D-、TX1/2、RX1/2
- **电源线电阻测量**：两挡位自动切换，0\~900mΩ 高精度 + 900mΩ\~9.99kΩ 量程，纯整数运算
- **E-Mark 芯片信息**：USB PD 各种信息读取，支持主动和被动线缆，支持VCONN和VBUS供电芯片（3.3V，不支持需要5V供电的芯片）；
- **华为快充线识别**：56kΩ 电阻检测

---

## 快速上手

1. **供电**：将 USB-C 供电口接入 5V 电源（C-C线或A-C线均可）
2. **插线**：将双头线缆两端分别插入左侧/右侧测试口
3. **切换模式**：短按 `KEY` 键在「通断模式」与「E-Mark 模式」之间切换
4. **观察屏幕**：见下方示例

---

## 界面示例

### 通断模式（默认）

```
┌───────────────────────────────────┐  160×80
│ ● USB 2.0          HUAWEI 5A      │  ← 线缆等级 / 快充状态
├───────────────────────────────────┤
│        R: 123 mΩ                  │  ← 电源线电阻（12×24 大字体）
├───────────────────────────────────┤
│ ■ D+- ■ CC  ■ SBU ■ SHLD          │  ← 信号通断（绿=通 / 红=断）
│ ■ TX1 ■ RX1 ■ TX2 ■ RX2           │
├───────────────────────────────────┤
│ E-Mark: 5A        USB3.2 G2       │  ← E-Mark 电流档 / 速率
└───────────────────────────────────┘
```

- 等级颜色：`TYPE-C FULL`=绿（全功能） / `USB 3.2`/`USB 3.0`=青 / `USB 2.0`=黄 / `POWER ONLY`=橙 / `OPEN`=红
- 电阻颜色：<100mΩ 绿 / 100~150 黄 / 150~200 橙 / 200~500 红 / ≥500mΩ 或开路（`----`）白
- 快充：`HUAWEI 5A` 紫色显示（与华为 5A 线缆插头同色）

### E-Mark 模式

被动线缆（如常见 5A 充电线）：

```
┌───────────────────────────────────┐  160×80
│ E-Mark: YES          Passive      │  ← 存在性 / 线缆类型
├───────────────────────────────────┤
│ USB 3.2 Gen2            5A        │  ← 速率 / 电流（8×16 大字体）
├───────────────────────────────────┤
│ EPR: Yes    MaxV: 20V             │  ← EPR 支持 / 最高 VBUS
│ VCON: No    Len: ~2m              │  ← 是否需 VCONN / 线缆长度
│ VID: 0x8254 PID: 0x0000           │  ← Vendor ID / Product ID
│ HW: 0x0     FW: 0x0               │  ← 硬件 / 固件版本
│ VDO: 00084040                     │  ← Cable VDO 原始值
└───────────────────────────────────┘
```

主动线缆（带 Re-driver/Re-timer 或有源光纤线）会换成另一组字段：

```
┌───────────────────────────────────┐  160×80
│ E-Mark: YES           Active      │
├───────────────────────────────────┤
│ USB4 Gen3               --        │  ← 无直通 VBUS 时电流显示 --
├───────────────────────────────────┤
│ EPR: Yes    MaxV: 50V             │
│ SBU: Act    Len: ~100m            │  ← SBU 支持 / 长度（主动线编码不同）
│ VID: 0x2109 PID: 0x8888           │
│ T: 90/110   Opt/RTmr              │  ← 最高/关断温度、光纤、Re-timer
│ VDO: 126A5E5A 5A6E264B            │  ← VDO1 + VDO2
└───────────────────────────────────┘
```

- `E-Mark: NO`：线缆无 E-Mark 芯片；`Reading...`：正在分步读取；`PD Comm Failed`：一轮候选全部失败（之后会自动重试）
- 电流档：只有 `01b=3A` / `10b=5A` 两种有效编码（主动线无直通 VBUS 时显示 `--`）
- `VCON:` 仅被动线缆显示（是否需要持续 VCONN）；`SBU:` 仅主动线缆显示
- 速率色阶：USB2.0=黄 / USB3.2 Gen1-Gen2=青 / USB4 Gen3-Gen4=绿

---

## 故障排查

| 现象 | 可能原因 | 解决 |
|------|---------|------|
| 屏幕无显示 | TFT 连接/供电异常 | 检查 3.3V 供电与 SPI 接线（CS→PA4, SCK→PA5, SDA→PA7, RS→PA6） |
| 电阻显示 `----` | 线缆未正常连接 | 确保两端都插到位 |
| 电阻测量偏差大 | 接触不良 | 清洁接口触点 |
| 显示 `E-Mark: NO` | 线缆不支持 E-Mark | 换全功能 USB3/4 线缆测试 |
| 显示 `PD Comm Failed` | VCONN 时序 / 右口 Rd 方向 / 芯片非标数据 | 右口 Rd 方向已由固件自动试错（V2.2）；确认 VCONN 与 PD 通道逆关系正确；部分线缆仅 VBUS 供电，读取期间需开 `V_PWR_EN`（固件已自动）；一直显示 `Reading...` 说明还在分步读取，属正常 |
| E-Mark 电流档与线缆标称不符 | 早期误按 PD3.0 编码解析 | V2.4 起按 R3.2 `01b=3A / 10b=5A` 解析；若仍不符，把 `VDO` 原始值反馈核对 |
| 快充不识别 | 非华为线/接触不良 | 使用华为 5A 线缆并确保插紧 |
| 按键无响应 | 引脚配置错误 | 确认 KEY 连接至 PA3(KEY_M) |
| 下载失败 | WCH-Link 连接 | 检查 SWD 线：DIO→PC18, DCK→PC19 |

---

## 开发者指南

### 目录结构

```
CH32X035G8U/
├── Startup/     # 启动文件（向量表、复位处理）
├── Core/        # RISC-V 核心初始化（时钟、延时）
├── Peripheral/  # WCH HAL 库（GPIO/ADC/SPI/USBPD 等）
├── Debug/       # printf 串口调试
├── User/        # 应用代码
│   ├── main.c           # SysTick(1ms)、初始化、主循环
│   ├── cable_gpio.c/h   # 引脚配置、电源控制、CC_E 驱动
│   ├── cable_adc.c/h    # 双通道 ADC、电阻计算、自动量程
│   ├── cable_spi.c/h    # 硬件 SPI1 驱动 TFT
│   ├── cable_tft.c/h    # ST7735S 驱动、多字体渲染
│   ├── cable_test.c/h   # 测试状态机、按键、双模式显示
│   ├── emarker_pd.c/h   # USB PD 协议、E-Mark 通信
│   └── cable_font.c/h   # 点阵字库（5×8/8×16/12×24）
└── Ld/Link.ld           # 链接脚本（62K Flash / 20K RAM）
```

### 构建

在 MounRiver Studio 中，打开 `CH32X035G8U.wvproj` 工程 → 点击`编译`。注意 `obj/` 下 Makefile 由 IDE 自动生成，勿手工修改。

或使用命令行方式：

```bash
cd CH32X035G8U/obj
make -j16 all
```

产物：`obj/CH32X035G8U.elf`（另含 `.hex`/`.lst`/`.map`）。工具链：`riscv-none-embed-gcc`（`-march=rv32imacxw -mabi=ilp32`，`-Os`）。

### 固件下载

#### 使用WCH-Link

- SWD引出到了供电USB-C口的SBU引脚，使用转接板连接到WCH-Link/WCH-LinkE
- 使用WCH-LinkUtility软件下载：[WCH-LinkUtility](https://www.wch.cn/downloads/WCH-LinkUtility_ZIP.html)
- 或在MounRiver Studio内点击 `下载`（F8）下载

#### 直接使用USB

- 按住ISP按键，再插入供电USB-C口连接电脑
- 使用WCH ISP软件下载：[WCH-ISP](https://www.wch.cn/downloads/WCHISPTool_Setup_exe.html)

---

## 文档

- **详细设计文档**: [CableTester_Design.md](doc/CableTester_Design.md)（含 USB PD 位域定义附录）
- **CH32X035 数据手册**: [CH32X035DS0.PDF](doc/CH32X035DS0.PDF)
- **CH32X035 参考手册**: [CH32X035RM.PDF](doc/CH32X035RM.PDF)
- **电路图**: [sch.png](doc/sch.png)
- **立创开源项目（实物演示）**：[oshwhub](https://oshwhub.com/eleqian/project_nbpzkxcx)

---

## 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| V1.0 | 2026-04-26 | 初始版本，完成固件代码开发 |
| V1.1 | 2026-05-05 | 支持 E-Mark PD 通信功能，支持读取详细信息 |
| V2.0 | 2026-07-26 | 新硬件版本适配 |
| V2.1 | 2026-08-02 | 界面美化 |
| V2.2 | 2026-08-08 | E-Mark 通信可靠性：右口 Rd 下拉自动检测与交换（复用 PA1↔PA2 间 5.1kΩ，方向随线缆插入方向自动试错切换） |
| V2.3 | 2026-11-14 | 支持VBUS供电的 E-Mark，优化信息显示和按键响应 |
| V2.4 | 2026-11-14 | E-Mark 区分主动/被动线缆解析、修正电流档编码、改为非阻塞分步读取（重试期间按键可响应）、界面区分 Reading/失败 |

## 许可证

本项目的硬件设计和固件代码遵循MIT开源协议。
