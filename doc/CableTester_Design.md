# CableTester 详细设计文档

**版本**: V2.4
**更新日期**: 2026-11-14
**作者**: CableTester Team

---

## 版本历史

| 版本 | 日期 | 作者 | 说明 |
|------|------|------|------|
| V1.0 | 2026-04-26 | Lingma | 初始版本 |
| V1.1 | 2026-04-26 | Lingma | 修正引脚分配和关键电路（PMOS、E-mark检测） |
| V1.2 | 2026-04-26 | Lingma | 升级为硬件SPI1驱动TFT |
| V1.3 | 2026-04-26 | Lingma | 优化为纯整数运算，移除浮点库依赖 |
| V1.4 | 2026-04-26 | Lingma | RC滤波优化（5ms延时，10%占空比） |
| V1.5 | 2026-04-26 | Lingma | 修复MOS开启时间过短问题（ADC采样间隔优化） |
| V1.6 | 2026-04-26 | Lingma | 优化MOS控制策略（100ms内连续测量） |
| V1.7 | 2026-04-29 | Lingma | **重构为状态机架构，去除阻塞延时，修复SysTick配置** |
| V1.8 | 2026-05-17 | Lingma | **硬件改动适配：参考电阻改为100Ω，AUX_G重命名为CC_R并实现外部电阻控制** |
| V2.0 | 2026-07-26 | OpenCode | **硬件改动适配：参考电阻改为100Ω，运放增益101倍，取消CC_R，CC_E拆分为CC_E1/CC_E2** |
| V2.1 | 2026-08-02 | OpenCode | **显示界面优化：E-Mark界面重排（2列对齐布局、类型入标题栏、线缆长度/HW-FW版本/Product ID解析、VDO合并一行）、显示配色统一、电阻数值与单位宽度对齐、test_result_t嵌套emarker_info_t** |
| V2.2 | 2026-08-08 | OpenCode | **E-Mark通信可靠性修复：右口Rd下拉自动检测与交换（复用PA1↔PA2间5.1kΩ，方向与线缆物理插入方向相关，试错交换自动保留正确方向）** |
| V2.3 | 2026-11-14 | WorkBuddy | **支持VBUS供电的 E-Mark，优化信息显示和按键响应** |
| V2.4 | 2026-11-14 | WorkBuddy | **E-Mark 读取重做：主动/被动线缆 VDO 按 PD R3.2 分别解析、电流档修正为 01b=3A/10b=5A、读取改为非阻塞分步状态机（失败重试期间按键可响应）、界面区分 Reading/失败；修复 Delay_Us 回绕缺陷并删除 PD_Busy_Us/PD_Wait_Ms；诊断代码收进 EMARK_DEBUG_DIAG / CABLE_DEBUG_TIMING 编译宏（默认关闭）** |

---

## 1. 项目概述

### 1.1 项目背景
CableTester是一款基于CH32X035G8U微控制器的USB线缆测试仪，用于检测USB线缆的通断性、电阻值、E-mark芯片信息及快充协议支持情况。

### 1.2 功能需求
系统需要实现以下功能：

1. **供电管理**
   - 支持USB-C口供电

2. **线缆通断测试**
   - 支持Type-C全信号测试（VBUS、GND、CC、SBU、D+、D-、TX1/2、RX1/2）

3. **电源线阻测量**
   - 测量电源和地线总电阻
   - 量程：0-900mΩ/900mΩ-9.99kΩ
   - 分辨率：1mΩ/0.1Ω（近10kΩ端粗至约240mΩ/LSB）
   - 采用参考电阻分压环回法测量

4. **E-Mark 信息读取**
   - E-Mark 芯片检测（CC 引脚 1kΩ 下拉，Ra 检测）
   - 通过 USB PD `Discover Identity` 读取线缆信息
   - **按 PD R3.2 区分被动线缆（Table 6.42）与主动线缆（Table 6.43/6.44）解析**
   - 解析内容：线缆类型、USB 速率、电流能力（3A/5A）、EPR、最高 VBUS、线缆长度、
     VID/PID、HW/FW 版本；主动线另有 SBU 支持、温度、铜/光纤、Re-driver/Re-timer、lane 数
   - 支持 VCONN 供电与 VBUS 供电两类 E-Marker

5. **其他功能**
   - 快充识别电阻检测（华为线等）
   - 按键切换显示内容

## 2. 硬件设计

### 2.1 主控芯片
- **型号**：CH32X035G8U
- **架构**：RISC-V (无硬件FPU)
- **封装**：QFN-28 (4.0mm x 4.0mm)
- **主频**：48MHz
- **Flash**：64KB
- **SRAM**：20KB
- **特性**：集成ADC、USB、USBPD、SPI等外设

### 2.2 引脚分配

#### MCU引脚映射
| 引脚编号 | 引脚名 | ADC | SYS | SPI  | USB | OPA       | 外设网络名 |
|------|------|-----|-----|------|-----|-----------|----------|
| 5    | PA0  | A0  | -   | -    | -   | -         | EH       |
| 6    | PA1  | A1  | -   | -    | -   | O1N2/O2N2 | CC_E2    |
| 7    | PA2  | A2  | -   | -    | -   | O2O1      | CC_E1    |
| 8    | PA3  | A3  | -   | -    | -   | O1O0      | KEY_M    |
| 9    | PA4  | A4  | -   | CS   | -   | O2O0      | TFT_CS   |
| 10   | PA5  | A5  | -   | SCK  | -   | O2N0      | TFT_SCK  |
| 11   | PA6  | A6  | -   | MISO | -   | O1N0      | TFT_RS   |
| 12   | PA7  | A7  | -   | MOSI | -   | O2P0      | TFT_SDA  |
| 13   | PB0  | A8  | -   | -    | -   | O1P0      | V_PWR_L  |
| 16   | PB1  | A9  | -   | -    | -   | O2N1      | V_PWR_H  |
| 14   | PB3  | -   | -   | -    | -   | O2P1      | UART_TX  |
| 15   | PB4  | -   | -   | -    | -   | O1P2      | UART_RX  |
| 17   | PB6  | -   | -   | -    | -   | O1N1      | V_PWR_EN |
| 18   | PB7  | -   | RST | -    | -   | O2P2      | SBU      |
| 19   | PB8  | -   | -   | -    | -   | O1P1      | RX2      |
| 20   | PB9  | -   | MCO | -    | -   | -         | TX2      |
| 21   | PB10 | -   | -   | -    | -   | -         | T_D      |
| 22   | PB11 | -   | -   | -    | -   | -         | RX1      |
| 23   | PB12 | -   | -   | -    | -   | -         | TX1      |
| 3    | PC0  | A10 | -   | -    | -   | -         | CC2      |
| 4    | PC3  | A13 | RST | -    | -   | -         | CC1      |
| 28   | PC14 | -   | -   | CC1  | -   | -         | CC1      |
| 1    | PC15 | -   | -   | CC2  | -   | -         | CC2      |
| 26   | PC16 | -   | -   | -    | UDM | -         | UDM      |
| 27   | PC17 | -   | -   | -    | UDP | -         | UDP      |
| 25   | PC18 | -   | DIO | -    | -   | -         | DIO      |
| 24   | PC19 | -   | DCK | -    | -   | -         | DCK      |

#### Type-C接口1 (USB3 - 左侧测试口)

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| CC1 | PC3, PC14 | CC1 | PC3=VCONN供电，PC14=USBPD硬件通信（两端插时PA2经线缆直连此脚） |
| CC2 | PC0, PC15 | CC2 | PC0=VCONN供电，PC15=USBPD硬件通信（两端插时PA1经线缆直连此脚） |
| VBUS | PB1 | V_PWR_H | 测量电流输出，未经过放大的ADC测量信号 |
| D+ | PB10 | T_D | USB数据正，D-接地 |
| TX1 | PB12 | TX1 | USB3.0发送1，负端接地 |
| TX2 | PB9 | TX2 | USB3.0发送2，负端接地 |
| RX1 | PB11 | RX1 | USB3.0接收1，负端接地 |
| RX2 | PB8 | RX2 | USB3.0接收2，负端接地 |
| SBU | PB7 | SBU | Sideband Use |
| GND | - | GND | 线缆GND |

#### USB-A接口 (USB2 - 左侧)

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| D+ | PB10 | T_D | 与Type-C D+共用，D-接地 |
| TX1 | PB12 | TX1 | 与Type-C TX1共用，负端接地 |
| RX1 | PB11 | RX1 | 与Type-C RX1共用，负端接地 |
| VBUS | PB1 | V_PWR_H | 测量电流输出，未经过放大的ADC测量信号 |
| GND | - | GND | 线缆GND |

#### Type-C接口2 (USB4 - 右侧测试口)

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| CC1 | PA2 | CC_E1 | 与CC_E2经5.1kΩ耦合，E-Marker期间必须浮空；CC通断时推挽输出低；华为快充时推挽输出高作上拉 |
| CC2 | PA1 | CC_E2 | 与CC_E1经5.1kΩ耦合，E-Marker期间必须浮空；CC通断时推挽输出低；华为快充时ADC输入测分压 |
| EH | PA0 | EH | 屏蔽层检测 |
| SHORT | - | SHORT | 所有VBUS/GND短接，用于测试线阻 |
| - | - | - | TX1/TX2/RX1/RX2的正负信号分别短接，D+/D-短接，两个SBU短接 |

#### Micro-USB接口 (USB5 - 右侧)

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| SHORT | - | SHORT | 所有VBUS/GND短接，用于测试线阻 |
| - | - | - | D+/D-短接 |

**注意**：
支持测试3种线缆类型：
- 双端type-c线；
- USB-A + type-c线；
- USB-A + mirco-USB线；

#### USB通信和调试 (USB1)

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| D+ | PC17 | UDP | USB D+ |
| D- | PC16 | UDM | USB D- |
| DIO | PC18 | DIO | 调试DIO |
| DCK | PC19 | DCK | 调试DCK |
| VBUS | - | - | 5V电源输入 |
| GND | - | GND | GND |

#### UART通信接口

预留扩展串口，连接UART_TX/UART_RX；
| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| TX | PB3 | UART_TX | 串口发送 |
| RX | PB4 | UART_RX | 串口接收 |
| VCC | - | - | 3.3V电源输出 |
| GND | - | GND | GND |

#### TFT显示屏 (ST7735S - 硬件SPI1)

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| CS | PA4 | TFT_CS | 片选（软件控制） |
| RS/DC | PA6 | TFT_RS | 数据/命令选择（软件控制） |
| SCK | PA5 | TFT_SCK | SPI时钟（硬件SPI1_SCK） |
| MOSI | PA7 | TFT_SDA | SPI数据（硬件SPI1_MOSI） |
| RST | - | - | 复位（硬件RC电路） |

**注意**:
- SCK和MOSI使用硬件SPI1专用引脚，配置为复用功能
- CS、RS由普通GPIO控制
- RST为硬件RC电路复位，无软件控制

**字体系统**：
- 支持5×8、8×16和12×24三种点阵字体
- 5×8：紧凑显示，适合小屏幕和普通文本
- 8×16：清晰易读，适合标题和重要信息
- 12×24：大字体，适合电阻值等主要信息显示
- 字体数据以C数组形式存储在`cable_font.c/h`中，定义字形表（Font5x8/8x16/12x24）
- 渲染通过`TFT_DrawString_Font()`函数实现，支持指定位置、颜色、背景色和字体尺寸

#### 按键

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| KEY | PA3 | KEY_M | 功能切换按键（上拉输入） |

#### 电源控制

| 信号 | MCU引脚 | 网络名 | 说明 |
|------|---------|--------|------|
| V_PWR_EN | PB6 | V_PWR_EN | 测试电源使能（PMOS低电平使能），串联50Ω参考电阻，再经过USB座和线缆接地，分压法测量电阻 |
| V_PWR_L | PB0 | V_PWR_L | 经过101倍放大的分压测量（ADC_Channel_8），用于测量较小的电阻（小于1欧姆） |
| V_PWR_H | PB1 | V_PWR_H | 未经过放大的分压测量（ADC_Channel_9），用于测量较大的电阻（超过V_PWR_L量程时使用） |

### 2.3 关键电路

#### 2.3.1 电阻测量电路

采用通用运放同向放大，同时提供放大和未放大两路ADC测量通道，根据测量值自动切换挡位。

**放大通道（V_PWR_L, PB0, ADC_Channel_8）：**

```
V_TEST (3.3V) -> R5 (50Ω) -> 线缆VBUS -> 线缆GND -> 短接回路 -> GND
                              |
                              +-> 运放同相输入端 (+)
                                      |
                                      运放增益 = 1 + Rf / Rg = 1 + 10kΩ / 100Ω = 101
                                      |
                              +-> 运放输出 -> R (10kΩ) -> C (0.1μF) -> GND
                              |                              |
                              |                              +-> ADC (PB0, V_PWR_L)
                              |
                              +-> 直接连接 -> R (10kΩ) -> C (0.1μF) -> GND
                                                                            |
                                                                            +-> ADC (PB1, V_PWR_H)
```

**未放大通道（V_PWR_H, PB1, ADC_Channel_9）：**
- VBUS信号直接经RC滤波后进入ADC，用于测量较大电阻（超过V_PWR_L有效量程时使用）

**RC低通滤波器（两通道共用10kΩ+0.1μF）：**
- R = 10kΩ, C = 0.1μF
- 时间常数 τ = R × C = 1ms
- 稳定时间：5τ = 5ms（达到99.3%稳定度）
- 作用：滤除高频噪声，提高测量稳定性

**挡位切换策略**：

V_PWR_L（放大通道，增益101）有效线性范围上限约900mΩ，此时ADC读数约3686（满量程的90%）。超过此点后接近饱和，改用V_PWR_H（未放大通道）测量。

| 条件 | 使用通道 | 量程 |
|------|---------|------|
| R < 900 mΩ（V_PWR_L adc < 3686） | V_PWR_L（放大101倍） | 0 ~ 900 mΩ |
| R ≥ 900 mΩ（V_PWR_L adc ≥ 3686） | V_PWR_H（未放大） | 900 mΩ ~ 10 kΩ |

切换带回差：当V_PWR_L adc > 3686（≈900mΩ）时切至V_PWR_H；当V_PWR_H adc < 33（≈800mΩ）时切回V_PWR_L，避免临界区频繁跳变。

**计算公式（整数运算）**：

放大通道（V_PWR_L，R < 900mΩ）：
```
R_mohm = adc_value × SERIES_RESISTOR / (OPAMP_GAIN × ADC_RESOLUTION - adc_value)
其中：SERIES_RESISTOR = 50000 mΩ, OPAMP_GAIN = 101, ADC_RESOLUTION = 4095
```

未放大通道（V_PWR_H，R ≥ 900mΩ）：
```
R_mohm = adc_value × SERIES_RESISTOR / (ADC_RESOLUTION - adc_value)
其中：SERIES_RESISTOR = 50000 mΩ, ADC_RESOLUTION = 4095
```

**推导过程（放大通道）**：
- VBUS电压 V_vbus(mV) = VREF × R_cable / (SERIES_RESISTOR + R_cable)
- 运放放大后：V_amp(mV) = V_vbus(mV) × OPAMP_GAIN
- ADC转换：adc_value = V_amp(mV) / VREF(mV) × ADC_RESOLUTION
- 反推电阻：R_cable(mΩ) = adc_value × SERIES_RESISTOR / (OPAMP_GAIN × ADC_RESOLUTION - adc_value)

**技术参数**：
- V_PWR_L量程：0-900 mΩ（分辨率约0.24 mΩ）
- V_PWR_H量程：900 mΩ - 9.99 kΩ（分辨率约24 mΩ/LSB，近10kΩ处约240 mΩ/LSB）
- 挡位切换点：900 mΩ（带回差）
- 精度：< ±2%
- **测量周期：1秒（开启55ms，关闭945ms，占空比~5.5%）**
- **RC稳定时间：5ms**

#### 2.3.2 电源控制电路

```
V_PWR_EN (PB6) -> Q1栅极 (PMOS场效应管) -> V_TEST输出3.3V
```

**工作原理**：
- 采用PMOS作为开关（低电平使能，高电平关闭）
- 每1秒开启约55ms以降低功耗（占空比~5.5%）
- 初始状态：PB6输出高电平（PMOS关闭）
- 使能状态：PB6输出低电平（PMOS导通）

**优势**：
- PMOS导通电阻小，压降低
- 无需基极限流电阻
- 控制逻辑简单

#### 2.3.3 E-mark检测电路

```
CC1/CC2 -> Type-C CC引脚
              |
              +-> 1kΩ下拉电阻 (存在E-mark时)

检测到E-mark后：
CC1_AUX (PC3) + CC2_AUX (PC0) -> 并联输出3.3V -> VCONN供电
```

**工作流程**：
1. 配置CC1或CC2为上拉输入
2. 读取引脚电平
3. 如果为低电平，说明存在1k下拉电阻（E-mark芯片）
4. 检测到E-mark后，通过PC3和PC0并联输出VCONN供电（详见2.3.3.1）
5. 在另一个CC引脚进行PD协议通信（PC14/PC15 USBPD硬件模块）

**技术要点**：
- CC_E1 (PA2) 和 CC_E2 (PA1) 在E-Marker检测阶段必须保持浮空输入；进入PD通信后，由固件自动在PA1/PA2间选择一脚接地（经5.1kΩ形成通信线Rd，见3.4.4）
- VCONN使用2个GPIO（PC3/PC0）并联提供足够驱动能力（约40mA）
- E-mark芯片通常需要<5mA，足够使用
- **VBUS 供电型 E-Marker**：Type-C R2.0 §2.5 允许线缆用 VBUS 代替 VCONN 取电，
  这类芯片的 VCONN 引脚可能根本不存在。固件在读取期间打开 `V_PWR_EN`
  （3.3V 经 50Ω 到 VBUS，线端约 3.22V），读完立即撤掉，避免干扰后续电阻/通断测试

#### 2.3.3.1 CC_E1/CC_E2控制与测试方案

CC_E1 (PA2) 和 CC_E2 (PA1) 之间通过 5.1kΩ 电阻耦合，用于右口的CC信号回路和华为快充检测。**已取消独立的CC_R引脚**，通过PA1/PA2直接驱动控制。

**电路连接**：
```
PA2 (CC_E1) ──[5.1kΩ]── PA1 (CC_E2)
```

**各测试模式下的引脚状态**：

| 测试模式 | PA2(CC_E1) | PA1(CC_E2) | 说明 |
|---------|-----------|-----------|------|
| E-Marker检测（探测Rd） | 浮空输入 | 浮空输入 | 检测阶段两脚必须高阻，避免5.1k影响USBPD引脚电平 |
| E-Marker PD通信 | 一脚接地(推挽低) + 一脚浮空 | 一脚接地(推挽低) + 一脚浮空 | 通信线所在脚浮空，另一脚推挽输出低，经5.1kΩ对GND形成通信线Rd（方向由固件自动试错交换，见3.4.4） |
| CC通断测试 | 推挽输出低 | 推挽输出低 | 右口CC被拉到GND，左口CC读电平判断 |
| 华为快充检测 | 推挽输出高(3.3V) | ADC输入 | 5.1k自然形成上拉，分压测华为56k |
| 电阻测量 | 浮空输入 | 浮空输入 | 走PB0/PB1通道，与CC无关 |

**CC通断测试**（合并检测，双向驱动互读，V2.2更新）：
```
① 右口PA1=PA2=推挽输出低 → 读左口PC14/PC15（有线缆则被拉低）
② 右口释放（PA1/PA2浮空）；左口PC14/PC15推挽输出低 → 读右口PA2/PA1（反向验证）
③ 判定：右拉低时左口低 且 左拉低时右口低 = 真正双端连通
```
- 兼容E-Mark电缆：芯片Rd会持续拉低其所在CC线，旧"右口释放后左口恢复高"判定会被两根CC线同时拉低而误报不通，故改用双向互读
- 拒绝单端E-Mark假连通：左口仅有芯片Rd、无线缆时，②中右口两根CC脚均为高阻（读高），自动判为不通
- 注：PA1↔PA2间5.1k耦合导致无法独立分辨CC1/CC2，Type-C正反插本来就只通一根CC，合并检测即可

**华为充电线检测**：
```
① PA2=推挽输出高(3.3V)，PA1=ADC输入
② 5.1kΩ（PA2→PA1）自然形成上拉
③ 华为线CC脚56k→右口短接GND，形成分压
④ ADC读PA1电压判断
```
计算公式：
```
V_pa1 = 3.3V × 56k / (5.1k + 56k) ≈ 3.0V（单CC脚）
V_pa1 = 3.3V × 28k / (5.1k + 28k) ≈ 2.8V（双CC脚并联）
V_pa1 ≈ 3.3V（无华为线，开路）
```

#### 2.3.4 TFT显示电路（硬件SPI1）

```
MCU (CH32X035)           ST7735S TFT
PA5 (SPI1_SCK)    ->     SCK
PA7 (SPI1_MOSI)   ->     SDA/MOSI
PA4 (GPIO)        ->     CS
PA6 (GPIO)        ->     RS/DC
(硬件RC复位)      ->     RST
```

**SPI1配置参数**：
- 工作模式：主机模式 (Master)
- 数据方向：单向发送 (1Line_Tx)
- 数据位宽：8位
- 时钟极性 (CPOL)：High (空闲时SCK为高)
- 时钟相位 (CPHA)：2Edge (第二个边沿采样)
- 波特率分频：4分频 (48MHz / 4 = **12MHz**)
- 字节顺序：MSB优先
- NSS管理：软件NSS

**性能指标**：
- SPI时钟频率：12MHz
- ST7735S最大支持：15MHz
- 安全裕量：3MHz (20%)
- 帧刷新时间：约15ms (相比软件SPI提升6-7倍)

## 3. 软件设计

### 3.1 系统架构（V1.7 状态机版本）

```
┌─────────────────────────────────────┐
│         应用层 (Application)        │
│  ┌──────────┬──────────┬──────────┐ │
│  │ UI显示   │ 测试逻辑 │ 按键处理 │ │
│  └──────────┴──────────┴──────────┘ │
├─────────────────────────────────────┤
│     状态机层 (State Machine)        │
│  ┌───────────────────────────────┐  │
│  │ 测试状态机 + 电阻测量状态机   │  │
│  │ - 非阻塞执行                  │  │
│  │ - 时间戳驱动                  │  │
│  │ - 事件响应                    │  │
│  └───────────────────────────────┘  │
├─────────────────────────────────────┤
│       驱动层 (Driver Layer)         │
│  ┌──────┬──────┬──────┬───────────┐ │
│  │GPIO  │ ADC  │ SPI1 │ SysTick   │ │
│  └──────┴──────┴──────┴───────────┘ │
├─────────────────────────────────────┤
│      硬件抽象层 (HAL)               │
│     CH32X035标准外设库              │
└─────────────────────────────────────┘
```

**V1.7架构特点：**
- **非阻塞设计**：所有延时改为时间戳判断，无Delay_Ms()阻塞
- **状态机驱动**：通过状态机管理复杂时序（如电阻测量）
- **事件响应**：按键等事件立即响应，不受测试阻塞
- **SysTick中断**：1ms中断维护全局时间戳（仅用于时间基准）
- **硬件计时延时**：Delay_Us/Delay_Ms 基于 SysTick->CNT 差值累加，µs 级精度且不白等
- **高实时性**：主循环可快速响应外部事件

### 3.2 模块设计

#### 3.2.1 GPIO模块 (cable_gpio.c/h)

**功能**：
- 初始化所有GPIO引脚
- 提供信号线控制接口
- 提供通断测试功能
- E-mark检测和VCONN供电控制

**主要函数**：
```c
void GPIO_Configuration(void);
void Set_Test_Voltage_Enable(uint8_t enable);  // PMOS控制(PB6)
uint8_t Read_Key_State(void);
signal_status_t Test_Signal_Connection(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin);
void Enable_VCONN_Power(uint8_t enable);       // PC3/PC0并联
void CC_E_Set_Output_Low(void);                // PA1/PA2推挽输出低（CC通断测试）
void CC_E_Set_Floating(void);                  // PA1/PA2恢复浮空输入
```

**关键技术**：
- PMOS控制（PB6）：低电平使能，高电平关闭
- VCONN供电（PC3/PC0）：并联输出，提供E-mark芯片电源
- CC通断：PA1/PA2推挽输出低，合并检测CC1/CC2连通性
- 华为快充：PA2推挽高+PA1 ADC，利用5.1k自然上拉测分压
- **E-Marker期间PA1/PA2必须浮空**，避免5.1k影响USBPD引脚

#### 3.2.2 ADC模块 (cable_adc.c/h)

**功能**：
- 初始化ADC1
- 双通道ADC采样：V_PWR_L（PB0, ADC_Channel_8, 101倍运放放大）和V_PWR_H（PB1, ADC_Channel_9, 未放大）
- 自动挡位切换（V_PWR_L adc > 3686时切换到V_PWR_H）
- 计算线缆电阻值（纯整数运算）

**主要函数**：
```c
void ADC_Configuration(void);
uint16_t ADC_Read_Channel(uint8_t channel);
uint16_t ADC_Average_Read(uint8_t channel, uint8_t times);
uint32_t Calculate_Cable_Resistance(uint8_t channel);  // 指定通道计算电阻(mΩ)
uint16_t Read_Input_Voltage_mV(void);                   // 返回毫伏(mV)
```

**整数运算优化**：
所有计算使用32位整数运算，无浮点依赖，代码体积减少约4KB。

**关键参数**：
- 运放增益：101倍（同相放大，1+10kΩ/100Ω）
- 串联参考电阻：**50Ω**（V2.3 起由 100Ω 改为 50Ω：VBUS 供电型 E-Marker 读取时
  3.3V 经 50Ω 到线端约 3.22V，可越过芯片 UVLO；100Ω 时仅约 2.97V 起不来。
  固件里 `SERIES_RESISTOR` 必须与硬件值一致，否则电阻读数按比例偏差）
- ADC分辨率：12位 (4095)
- 参考电压：3.3V
- V_PWR_L量程：0~900mΩ（分辨率~0.24mΩ）
- V_PWR_H量程：900mΩ~9.99kΩ（分辨率~24mΩ/LSB，近10kΩ处~240mΩ/LSB）

#### 3.2.3 SPI模块 (cable_spi.c/h)

**功能**：
- 初始化硬件SPI1用于TFT通信
- 提供SPI读写接口

**主要函数**：
```c
void SPI_Configuration(void);
void SPI_WriteByte(uint8_t data);
void TFT_Write_Command(uint8_t cmd);
void TFT_Write_Data(uint8_t data);
```

**硬件SPI1配置**：
- 引脚：PA5 (SCK), PA7 (MOSI)
- 模式：主机模式，单向发送
- 时钟：12MHz (4分频)
- CPOL：High, CPHA：2Edge

**异常保护**：`SPI_Wait_TXE()` / `SPI_Wait_Idle()` 带 `SPI_WAIT_TIMEOUT = 50000u`
次轮询上界（约 10ms），SPI 状态异常时只丢字节、不会卡死主循环。

#### 3.2.4 TFT显示模块 (cable_tft.c/h)

**功能**：
- 初始化ST7735S显示屏
- 提供图形绘制函数
- 显示测试结果（两种模式）

**主要函数**：
```c
void TFT_Init(void);
void TFT_Clear(uint16_t color);
void TFT_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg);
void TFT_ShowTestResult(test_result_t *result, display_mode_t mode);
void TFT_ShowWelcome(void);
```

**显示模式（V3.0）**：
1. MODE_CONTINUITY - 通断测试+电阻值+快充识别，合并显示在同一页面
2. MODE_EMARK - E-mark信息（通过USB PD协议读取详细信息）

**显示布局（V2.1）**：

通断模式（160×80 横屏）：
```
y=0   ●USB 3.2            HUAWEI 5A     ← 状态栏: 指示灯+等级(5x8)
y=13  R: 123 mΩ                          ← 电阻值 (12x24): 数值5档色/单位白色
y=45  ■D+-  ■CC   ■SBU  ■SHLD           ← 信号线 (5x8): 绿=通/红=断
y=54  ■TX1  ■RX1  ■TX2  ■RX2
y=68  E-Mark: 5A         USB3.2 G2       ← 底部信息 (5x8)
```

E-Mark 模式（被动线缆）：
```
y=0   E-Mark: YES             Passive     ← 标题 + 线缆类型
y=13  USB 3.2 Gen2              5A        ← 速度+电流 (8x16)
y=34  EPR:Yes   MaxV:20V                  ← 2列详细字段(左x=4/右x=82)
y=43  VCON:No   Len:~1m                   ← 是否需要 VCONN / 长度
y=52  VID:0x8254 PID:0x0000
y=61  HW:0x1    FW:0x0
y=70  VDO:00084040                        ← 原始 VDO（主动线再附 VDO2）
```

E-Mark 模式（主动线缆）：
```
y=0   E-Mark: YES             Active
y=13  USB4 Gen3                --         ← 无直通 VBUS 时电流显示 "--"
y=34  EPR:Yes   MaxV:50V
y=43  SBU:Act   Len:~100m                 ← SBU 支持(Pas/Act/No) / 长度
y=52  VID:0x2109 PID:0x8888
y=61  T:90/110  Opt/RTmr                  ← 温度(最高/关断) / 介质与元件
y=70  VDO:126A5E5A 5A6E264B
```

**读取状态提示**：
- 检测到芯片但一轮候选还没跑完 → `Reading...`（黄），不会先闪失败
- 一轮 18 个候选全部失败 → `PD Comm Failed`（红）；之后每 5s 后台重试且不再切回 Reading
- 无芯片 → `No E-Mark Chip`（灰）

**配色统一规则（V2.1）**：
- 绿=优秀/通过/支持，黄=中等，橙=警告，红=错误/断路，紫=华为5A快充，青=USB3.x速率标识，灰=中性/原始数据，白=标签/中性数值
- 电阻数值5档色：<100mΩ绿 / 100-150黄 / 150-200橙 / 200-500红 / ≥500及OL白
- 速度色阶全屏统一：USB2.0黄 / Gen1-Gen2青 / USB4 Gen3-Gen4绿
- 电阻数值+单位总宽恒定对齐：毫欧"123 mΩ"(3+1+2字符)与欧姆"1.23 Ω"(4+1+1字符)均为6字符宽

**数据结构**（整数类型）：
```c
typedef struct {
    uint8_t power_ok;          // VBUS和GND检测: 1=回路通, 0=开路(显示"----")
    uint8_t cc1_ok, cc2_ok;    // CC检测（合并测试）
    uint8_t dp_dm_ok;          // D+和D-合并检测
    uint8_t tx1_ok, rx1_ok, tx2_ok, rx2_ok;
    uint8_t sbu_ok, shield_ok;
    uint32_t resistance_mohm;  // 电阻值(mΩ), 开路时为RES_SATURATED
    uint8_t has_emark;         // 是否检测到E-Mark芯片
    uint8_t fast_charge;       // 快充: 0无, 1华为单CC, 2华为双CC
    emarker_info_t emark;      // E-Mark芯片信息（嵌套结构, 整体拷贝赋值）
} test_result_t;
```

#### 3.2.5 测试模块 (cable_test.c/h)

**功能**：
- 执行通断测试
- 执行电阻测量（整数运算）
- 检测E-mark芯片
- 识别快充线缆

**主要函数**：
```c
void Cable_Test_Init(void);
void Test_State_Machine(void);                 // 非阻塞测试状态机（含 E-Mark 分步读取）
void Test_Cable_Continuity(test_result_t *result);
uint32_t Get_Cable_Resistance_Single(void);
void Detect_Emark_Info(test_result_t *result); // 阻塞式兼容接口（正式流程走 EMarker_Task_Step）
void Detect_Fast_Charge_Info(test_result_t *result);
void Switch_Display_Mode(void);
void TFT_ShowTestResult(test_result_t *result, display_mode_t mode);
```

#### 3.2.6 按键模块（非阻塞实现）

**功能**：
- 按键扫描
- 消抖处理（时间戳方式）
- 模式切换

**实现方式（V1.7）**：
- 使用 `system_tick_ms` 时间戳判断消抖时间
- 状态机管理按键状态（空闲/等待释放）
- **无阻塞延时**，立即响应
- 消抖时间：20ms（通过时间戳差值判断）

#### 3.2.7 E-Mark PD通信模块 (emarker_pd.c/h)

**功能**：
- 自动检测 E-Mark 芯片存在性（Ra≈1kΩ）
- 通过 USB PD `Discover Identity` 读取并解析 VDM 响应
- VCONN / VBUS 双供电支持（部分 E-Marker 只从 VBUS 取电，读取期间需开 `V_PWR_EN`）
- 多 CC 通道自动选择、右口 Rd 下拉方向自动交换（见 3.4.4）
- **非阻塞分步读取**：`EMarker_Task_Step()` 每次只做一次 PD 事务，失败重试期间
  主循环仍能扫按键、刷新显示（实测主循环最大间隔 50~87ms）
- **按线缆类型分别解析 Cable VDO**（被动 / 主动，位定义不同）

**数据结构**（`emarker_info_t`）：
```c
typedef struct {
    uint8_t  valid;              // 数据有效性标志
    uint8_t  id_header_present;  // ID Header 是否存在
    uint32_t vdm_header;
    uint32_t id_header;
    uint32_t cert_stat;
    uint32_t product_vdo;
    uint32_t cable_vdo1;         // Cable VDO1（被动/主动位定义不同）
    uint32_t cable_vdo2;         // Cable VDO2（仅主动线缆返回）
    /* ---- 两种线缆共用 ---- */
    uint8_t  cable_type;         // 0=被动, 1=主动, 2=未知
    uint8_t  epr_capable;
    uint8_t  usb_speed;          // 0=USB2 / 1=Gen1 / 2=Gen2 / 3=USB4 G3 / 4=USB4 G4
    uint8_t  current_capability; // 3A / 5A；主动线无直通 VBUS 时为 0xFF（不适用）
    uint8_t  vdo_version;
    uint16_t vendor_id;          // ID Header Bit15:0
    uint16_t product_id;         // Product VDO Bit31:16
    /* ---- 按线缆类型分别填充 ---- */
    uint8_t  plug_type;          // Bit19:18: 2=Type-C, 3=Captive
    uint8_t  termination;        // 被动:0=不需VCONN/1=需；主动:2/3=需VCONN
    uint8_t  max_vbus_50v;       // 1=50V, 0=20V
    uint8_t  sbu_supported;      // 仅主动（Bit8，反逻辑）
    uint8_t  sbu_active;         // 仅主动（Bit7）
    uint8_t  vbus_through;       // 仅主动（Bit4）
    uint8_t  sop2_present;       // 仅主动（Bit3）
    /* ---- 仅主动线缆 Active Cable VDO2 ---- */
    uint8_t  max_temp, shutdown_temp;   // 最高工作温度 / 关断温度（℃）
    uint8_t  optical, retimer;          // 1=光纤 / 1=Re-timer
    uint8_t  usb4_supported, usb2_supported, usb3_supported;  // 反逻辑位，1=支持
    uint8_t  lanes;                     // USB3 lane 数：1 或 2
} emarker_info_t;
```

**电流能力编码（R3.2 Table 6.42，易踩点）**：

| Bit6:5 | 含义 |
|---|---|
| 00b | Invalid，接收方按 01b（3A）处理 |
| 01b | 3A |
| 10b | **5A** |
| 11b | Invalid，按 01b（3A）处理 |

早期按 PD3.0 草案实现成「00b=3A / 01b=5A」是错的，会把 3A 线显示成 5A。
主动线缆另有约束：`VBUS Through Cable = 0` 时该字段整段忽略，此时 `current_capability`
置 0xFF，界面显示 `--`。

**读取流程（分步状态机，`EMarker_Task_Step`）**：
1. 检测 E-Mark 所在 CC 通道 → 确定 VCONN 供电通道与 PD 通信通道（逆关系，见 3.4.1）
2. **VBUS 与 VCONN 同时上电，只等一次建立时间**：正常 5ms，失败后的加时重试 40ms。
   VBUS 供电型 E-Marker 必须打开 `V_PWR_EN`（3.3V 经 50Ω 限流，实测线端约 3.22V）
3. 设置 `CC_SEL` 指向 PD 通信通道，并按候选配置右口 Rd 方向（PA1/PA2/浮空）
4. 发送 `Discover Identity`（SOP' 或 SOP''）
5. 收到响应后**先回 GoodCRC 再解析**：GoodCRC 必须在 tReceiverResponse（≤750µs）内发出，
   解析放前面会白白耗掉时间
6. 解析 VDO；成功后立即撤 VBUS、关 VCONN、恢复 CC 为浮空输入
7. 失败则推进下一个候选，共 18 个（6 快读 + 6 加时重试 + 3 VCONN-only + 3 VBUS-only）；
   相邻两步之间至少让出 20ms 给主循环

**候选组合**：`SOP' {Rd 浮空 / PA2 / PA1}` → `SOP'' {同}`，覆盖正反插与芯片自带终端的差异。

### 3.3 工作流程（V1.7 状态机版本）

#### 3.3.1 主流程（事件驱动）

```
系统初始化（GPIO、ADC、SPI1、SysTick、TFT）
    ↓
显示欢迎界面
    ↓
┌→ 读取系统时间戳 ←──────────────┐
│   ↓                            │
│ 按键扫描（非阻塞消抖）          │
│   ↓ 如果按下                    │
│ 切换显示模式（立即生效）        │
│   ↓                            │
│ 执行测试状态机                  │
│   ↓                            │
│ ├→ IDLE: 检查是否到达测试周期（通断页 200ms / E-Mark 页 50ms）│
│ ├→ START: 启动相应测试（E-Mark 每拍只推进一步）  │
│ ├→ RUNNING: 执行状态机步骤     │
│ └→ COMPLETE: 更新显示          │
│   ↓                            │
└────────────────────────────────┘
```

**关键改进：**
- ✅ **无阻塞延时**：所有Delay_Ms()改为时间戳判断
- ✅ **快速响应**：按键随时可响应，不受测试阻塞
- ✅ **并发执行**：测试和按键可同时处理
- ✅ **精确时序**：通过SysTick中断维护精确时间戳

#### 3.3.2 电阻测量流程（V1.7 状态机版本）

```
状态: RESISTANCE_STATE_IDLE
    ↓ (每1秒触发)
状态: RESISTANCE_STATE_MOS_ON
    ├→ 开启MOS管
    └→ 记录时间戳
    ↓
状态: RESISTANCE_STATE_STABILIZING
    ├→ 等待RC滤波稳定 (5ms)
    └→ 检查: elapsed_time >= 5ms?
    ↓ (是)
状态: RESISTANCE_STATE_SAMPLING
    ├→ 执行单次ADC采样 (10ms)
    ├→ 累加电阻值
    ├→ 采样计数+1
    └→ 检查: count >= 5?
    ↓ (否，继续采样)
    ↓ (是)
状态: RESISTANCE_STATE_MOS_OFF
    ├→ 关闭MOS管
    └→ 计算平均值
    ↓
状态: RESISTANCE_STATE_COMPLETE
    └→ 返回IDLE状态
```

**状态机变量：**
```c
typedef enum {
    RESISTANCE_STATE_IDLE,
    RESISTANCE_STATE_MOS_ON,
    RESISTANCE_STATE_STABILIZING,
    RESISTANCE_STATE_SAMPLING,
    RESISTANCE_STATE_MOS_OFF,
    RESISTANCE_STATE_COMPLETE
} resistance_state_t;

static resistance_state_t resistance_state;
static uint32_t state_start_time;
static uint8_t resistance_sample_count;
static uint32_t resistance_sum;
```

**时序说明（V1.7非阻塞）：**
- **MOS开启策略：每1秒开启一次，持续约55ms**
- RC稳定时间：5ms（通过时间戳判断，非阻塞）
- ADC采样时间：~10ms/次（10次×1ms间隔）
- 5次连续测量：5 × 10ms = 50ms
- **MOS总开启时间：5ms + 50ms = 55ms**
- **占空比：约5.5%（更低的功耗）**

**关键改进（V1.7）：**
- ✅ **非阻塞执行**：每个状态通过时间戳判断是否转移
- ✅ **可中断性**：状态机可随时被按键等事件打断
- ✅ **精确控制**：SysTick提供1ms精度的时间基准
- ✅ **代码清晰**：状态转换逻辑清晰，易于维护

#### 3.3.3 通断测试流程

```
CC信号（双向驱动互读，V2.2）：
    1. 右口PA1/PA2推挽输出低 → 读左口PC14/PC15（1ms稳定）
    2. 右口释放；左口PC14/PC15推挽输出低 → 读右口PA2/PA1（1ms稳定）
    3. 双向均拉低 → 连通；任一方向读高 → 开路
    4. 恢复PC14/PC15、PA1/PA2为浮空

其他信号线（环回法）:
    1. 设置发送端为高电平
    2. 延时等待稳定 (1ms)
    3. 读取接收端电平
    4. 判断通断状态
    5. 恢复引脚状态
```

#### 3.3.4 E-mark检测流程

**前提条件**：检测阶段 CC_E1(PA2)、CC_E2(PA1) 必须保持浮空输入，避免 5.1kΩ 影响 USBPD 引脚电平。

**触发时机**：
- **通断模式**：200ms 一拍；检测到芯片即发起读取（分步，每拍最多一次事务）
- **E-Mark 模式**：50ms 一拍推进读取；在线检测单独节流为 200ms 一次，
  连续 2 次（≈400ms）检测不到 Ra 判定拔线并清结果；插入边沿立即重读（回拨限流时间戳）

```
① PA1 = PA2 = 浮空输入

② USBPD 硬件比较器检测 CC 线上的 Rd（1kΩ 下拉）
   检测到 E-Mark 后确定 CC 通道（CC1/CC2）

③ 建立供电（VCONN 与 PD 通信通道为逆关系）：
   - E-Mark 在 CC1 → VCONN 走 PC3，PD 通信走 CC2(PC15)
   - E-Mark 在 CC2 → VCONN 走 PC0，PD 通信走 CC1(PC14)
   - 同时把 CONFIG.CC_SEL 设为 PD 通信通道；复位默认 CC1，漏设时翻面后
     PD 命令仍发往 CC1，表现为"检测到 E-Marker 但读取失败"（必现）
   - VBUS（V_PWR_EN）与 VCONN 同时上电，只等一次建立时间：正常 5ms，
     失败后的加时重试 40ms

④ 右口 Rd 方向逐候选尝试：Rd 浮空（芯片自带终端）/ PA2 接地 / PA1 接地

⑤ 发送 Discover Identity：先 SOP'，再 SOP''（部分芯片只应 SOP''）

⑥ 收到响应后先回 GoodCRC（≤750µs），再解析 VDO；成功即停

⑦ 撤 VBUS、关 VCONN、CC 恢复浮空输入

⑧ 全失败则该轮结束（共 18 个候选），按间隔自动重试
```

### 3.4 USBPD硬件控制

#### 3.4.1 VCONN与PD通信通道关系

USB Type-C规范要求VCONN供电与PD通信使用不同CC通道（本设计实测为逆关系）：

```
VCONN在CC1 → PD通信使用CC2（PC15）
VCONN在CC2 → PD通信使用CC1（PC14）
```

VCONN经辅助GPIO（PC3或PC0）供电，PD通信走USBPD硬件引脚（PC14或PC15），形成逆关系。**必须将USBPD CONFIG的CC_SEL设置为PD通信通道**（`EMarker_Set_CC_Channel(pd_comm_cc)`）：复位值0=CC1，若不设置，线缆翻面后E-Mark在CC1、PD通信应走CC2时命令仍发往CC1直通线，表现为"检测到E-Marker但读取失败"（必现）。

**上电时序（V2.4）**：VBUS（`V_PWR_EN`）与 VCONN **同时上电**，只等一次建立时间——
正常路径 5ms（实测 40/20/10/5ms 逐级压缩均能首试读通），失败后的加时重试 40ms，
兼容回退档用 VCONN-only 60ms / VBUS-only 30~100ms。早期「先 VBUS 等 settle、
再 VCONN 等 60ms」的两段串行写法已废弃，单次事务可省 60ms。

#### 3.4.2 CC_LVE寄存器控制

CH32X035 USBPD模块的CC端口有两种工作模式，由`CC_LVE`位控制：

| CC_LVE | 模式 | 电平 | 用途 |
|--------|------|------|------|
| 0 | 恒流源上拉（接收） | ~1.68V（330μA×5.1k） | 等待响应、空闲态 |
| 1 | 推挽输出（发送） | 3.3V/0V | BMC编码发送 |

**控制流程**：发送前启用LVE（推挽输出），发送完成后立即禁用LVE（恢复恒流源），否则无法接收E-Mark响应。

#### 3.4.3 BMC时序配置

BMC编码时序关键参数（48MHz系统时钟）：

```
BMC_CLK_CNT = UPD_TMR_TX_48M = 0x0050 = 80
UI时间 = (80 + 1) / 48MHz = 1.6875μs
Bit时间 = 2 × UI = 3.375μs → 速率 ≈ 296kHz（在±10%范围内）
```

**接收流程**：
1. 清除USBPD接收状态（`IF_RX_ACT`等标志）
2. 设置DMA缓冲区地址（`USBPD->DMA`）
3. 启动BMC接收（`USBPD->CONTROL |= BMC_START`）
4. 等待`IF_RX_ACT`标志（超时保护）

**常见问题**：

| 现象 | 原因 | 解决 |
|------|------|------|
| IF_RX_ACT不触发 | CC_LVE发送后未禁用 | 发送后清除CC_LVE |
| IF_RX_ACT不触发 | USBPD模块未复位 | 用PD_ALL_CLR复位 |
| IF_RX_ACT不触发 | DMA缓冲区未设置 | 设置USBPD->DMA |
| BMC速率偏差 | BMC_CLK_CNT配置错误 | 使用UPD_TMR_TX_48M |

#### 3.4.4 右口Rd下拉自动检测与交换（V2.2）

**背景**：E-Marker的CC通信线经线缆延伸至右口CC脚（CC1↔PA2, CC2↔PA1）。若通信线上没有E-Mark芯片自身的Rd（芯片Rd在VCONN侧或换位），通信线悬空会使USBPD上拉无泄放回路，BMC直流偏置错乱、PD通信超时（表现为"检测到E-Marker但读取失败"）。由于通信线落在PA1还是PA2由**线缆物理插入方向**决定（线缆翻面后交换），与`pd_comm_cc`无关，无法预先静态指定。

**复用单颗5.1kΩ电阻**：右口PA1(CC_E2)↔PA2(CC_E1)间只有1颗5.1kΩ电阻。采用"接地脚推挽输出低"的方式形成Rd：

```
PA1接地（PA2浮空）→ PA2（若为通信线）经5.1kΩ对GND → Rd
PA2接地（PA1浮空）→ PA1（若为通信线）经5.1kΩ对GND → Rd
```

与USBPD上拉分压后通信线静态约1.68V（330μA×5.1k），满足PD BMC接收电平要求（见3.4.2）。

**自动检测与交换（试错法）**：`EMarker_Full_Detect`在使能VCONN后，依次尝试两种方向，以"是否收到E-Mark响应（GoodCRC/VDM响应）"为判据自动保留正确方向：

```
EMarker_Try_Comm(1)  → PA1接地，完整PD通信
   失败（发送超时/无响应/解析失败）？
       ↓ 是
EMarker_Try_Comm(2)  → 交换：PA2接地，再次完整PD通信
```

- 方向错误时通信线无偏置 → 收不到GoodCRC → 判定失败并交换
- 方向正确时E-Mark正常应答 → 保留该方向，后续周期直接命中
- 每次尝试后PA1/PA2均恢复浮空；GoodCRC在关闭Rd前发送，保证应答时序

**时序开销**：方向错误时多一次约225ms的发送重试超时（3次×75ms），正确方向下无额外开销。

**相关API**：
- `EMarker_Enable_Rd_Pulldown(gnd_pin)`：gnd_pin=1→PA1接地，2→PA2接地
- `EMarker_Disable_Rd_Pulldown()`：PA1/PA2恢复浮空
- `EMarker_Try_Comm(gnd_pin, info)`：指定方向的单次完整PD通信（内部静态函数）

### 3.5 时序要求

1. **电阻测量时序**
   - V_PWR_EN开启后延时5ms等待RC滤波稳定（5τ）
   - ADC采样时间：11个ADC时钟周期
   - 每次测量完成后立即关闭V_PWR_EN
   - **测量周期：1秒（开启100ms，关闭900ms，占空比10%）**
   - RC时间常数：τ = 10kΩ × 0.1μF = 1ms
   - 稳定时间：5τ = 5ms（达到99.3%稳定度）
2. **SPI1通信时序**
   - CPOL = High (空闲时SCK为高)
   - CPHA = 2Edge (第二个边沿采样)
   - 波特率分频：4 (12MHz @ 48MHz系统时钟)
   - 数据传输：等待TXE标志，写入数据，等待BSY标志

3. **按键消抖**
   - 消抖时间：20ms
   - 长按检测：1秒

4. **E-Mark PD 时序（V2.4）**
   - BMC 300kbps，UI = 1.6875µs（`BMC_CLK_CNT = 80` @48MHz）
   - GoodCRC 必须在 tReceiverResponse（≤750µs）内发出 → 收到响应**先回 GoodCRC 再解析**
   - 发送完成等待 `IF_TX_END`：正常 3ms 窗口（扫描模式 2ms），用 `system_tick_ms` 截止，
     不再用「忙等迭代次数 × 估算耗时」
   - 等待 VDM 响应：正常 100ms，分步路径 35ms（规范 tSenderResponse 为 24~30ms，留余量）
   - 分步读取相邻两步之间至少让出 20ms，保证按键/显示不被饿死

## 4. 算法设计

### 4.1 电阻计算算法（整数运算）

**公式推导**：
```
原始公式：
V_vbus(mV) = VREF × R_cable / (SERIES_RESISTOR + R_cable)
V_amp(mV) = V_vbus(mV) × OPAMP_GAIN
adc_value = V_amp(mV) / VREF(mV) × ADC_RESOLUTION

合并推导（放大通道）：
adc_value = R_cable × OPAMP_GAIN × ADC_RESOLUTION / (SERIES_RESISTOR + R_cable)

求解 R_cable（放大通道）：
R_cable = adc_value × SERIES_RESISTOR / (OPAMP_GAIN × ADC_RESOLUTION - adc_value)

代入数值：
SERIES_RESISTOR = 50000 mΩ
OPAMP_GAIN = 101（运放同向放大，增益 = 1 + 10kΩ / 100Ω = 101）
ADC_RESOLUTION = 4095
R_cable(mΩ) = adc_value × 50000 / (101 × 4095 - adc_value)

未放大通道（OPAMP_GAIN = 1）：
R_cable(mΩ) = adc_value × 50000 / (4095 - adc_value)
```

**性能分析**：
- CPU周期：~15-35 cycles（vs 浮点500-800 cycles）
- 精度误差：< 0.2%
- 无浮点库依赖

### 4.2 平均值滤波算法

```c
uint16_t ADC_Average_Read(uint8_t channel, uint8_t times)
{
    uint32_t sum = 0;
    uint8_t i;

    for(i = 0; i < times; i++) {
        sum += ADC_Read_Channel(channel);
        Delay_Us(10);
    }

    return (uint16_t)(sum / times);
}
```

**应用场景**：
- 电阻测量：10次平均
- 电压读取：5次平均
- 提高测量稳定性

### 4.3 多次测量取平均

``c
void Test_Cable_Resistance(test_result_t *result)
{
    uint32_t sum = 0;
    uint8_t i;

    for(i = 0; i < 5; i++) {
        sum += Calculate_Cable_Resistance();
        Delay_Ms(15);  // 每次测量间隔15ms，总耗时约100ms
    }

    result->resistance_mohm = sum / 5;
}
```

**优势**：
- 进一步降低噪声影响
- 提高测量可靠性
- **RC滤波充分稳定（5ms延时）**
- **总耗时约100ms（符合低功耗设计要求）**

## 5. 错误处理

### 5.1 ADC异常
- ADC 读数超出量程：界面显示 `----`（开路/超范围）
- ADC 通信失败：重试 3 次

### 5.2 SPI1异常
- TFT 初始化失败：显示空白屏幕
- SPI 等待 TXE/Idle 带 10ms 上界（`SPI_WAIT_TIMEOUT = 50000` 次轮询）：
  超时只丢弃本次字节，**不会阻塞主循环**。
  早期是无上界 `while` 死等，一旦 SPI 状态异常整机界面就卡死

### 5.3 测试异常
- 开路检测：电阻显示 `----`
- 短路检测：电阻显示 `0 mΩ`

## 6. 性能指标

### 6.1 测量性能

| 指标 | 要求 | 实际 | 状态 |
|------|------|------|------|
| 电阻测量范围 | ≤1Ω / ≤10kΩ | 0-900mΩ(V_PWR_L) / 900mΩ-9.99kΩ(V_PWR_H) | ✅ 达成 |
| 电阻测量分辨率 | 1mΩ / 0.1Ω | 0.24mΩ(V_PWR_L) / 24mΩ(V_PWR_H, 近10kΩ处~240mΩ) | ✅ 达成 |
| 电阻测量精度 | ±5% | <±2% | ✅ 超额 |
| 挡位切换点 | - | ~900mΩ（带≥10%回差） | ✅ 自动切换 |
| 测量周期 | 1秒 | 1秒 | ✅ 达成 |
| **RC稳定时间** | - | **5ms (5τ)** | ✅ **优化** |
| **测量占空比** | - | **~5.5% (55ms/1s)** | ✅ **低功耗** |
| 单次计算耗时 | - | ~15-35 cycles | ✅ 优秀 |

### 6.2 显示性能

| 指标 | 要求 | 实际 | 状态 |
|------|------|------|------|
| 帧周期 | - | 通断页 200ms（5Hz）/ E-Mark 页 50ms（20Hz，便于分步读取推进） | ✅ |
| SPI时钟频率 | - | 12MHz | ✅ 优秀 |
| 帧刷新时间 | - | ~15ms（实测单帧绘制最坏值见 `g_draw_max_ms`） | ✅ 优秀 |

### 6.3 系统性能（V2.4）

| 指标 | 要求 | 实际 | 状态 |
|------|------|------|------|
| 按键响应时间 | <100ms | 典型 <10ms；E-Mark 读取失败重试期间最坏约 50ms（`g_loop_max_gap` 实测，随候选数与线缆浮动） | ✅ 达标 |
| 代码体积 | - | 21.4KB（text 21420 + data 176，诊断默认关闭） | ✅ 正常 |
| RAM 占用 | - | 约 2.6KB（bss 2404 + data 176） | ✅ 正常 |
| 功耗（待机） | - | <1mA | ✅ 优秀 |
| **系统实时性** | - | **事件驱动 + 分步状态机** | ✅ **优秀** |
| **主循环阻塞** | - | **无阻塞（单次 PD 事务 ≤72ms）** | ✅ **优秀** |

### 6.4 V1.7 架构优势

| 对比项 | V1.6（阻塞式） | V1.7（状态机） | 改进 |
|--------|---------------|---------------|------|
| 按键响应 | 测试期间阻塞 | **随时响应** | **实时性↑** |
| 代码复杂度 | 简单 | **稍复杂** | 可维护性↑ |
| 并发能力 | 无 | **支持并发** | **功能增强** |
| 时序精度 | 依赖Delay | **SysTick中断** | **精度↑** |
| 扩展性 | 差 | **好** | **易扩展** |
| CPU利用率 | 低（阻塞等待） | **高（事件驱动）** | **效率↑** |

**V1.7核心优势：**
1. **非阻塞设计**：所有操作通过时间戳判断，无Delay_Ms()阻塞
2. **事件驱动**：按键、测试等事件独立处理，互不干扰
3. **精确时序**：SysTick提供1ms精度的时间基准
4. **高可扩展性**：易于添加新功能和后台任务
5. **更好的用户体验**：按键随时响应，无卡顿感

### 6.5 延时函数实现

**SysTick 配置：** 1ms 周期，`CNT` 从 0 递增到 `CMP` 后由硬件归零并触发中断，
`system_tick_ms` 在中断里自增，作为全局时间基准。

**实现方式：** `Delay_Us()` 用 `SysTick->CNT` 的差值累加计时，`Delay_Ms()` 按 1ms
分段调用它。每微秒的周期数由 `SysTick->CMP + 1`（即 1ms 的周期数）反推，
不依赖 `SystemCoreClock` 是否已经 Update。

**历史问题（已修复，重要）：**
早期版本用 `while (SysTick->CNT >= start);` 等待计数值回绕，有两个坑：

1. `start` 恰好为 0 时条件恒真，函数立即返回，**延时严重不足**；
2. 跨越回绕时要先白等接近 1ms，PD 时序窗口会被直接拖爆。

第 2 条曾导致「发送完成 → 切到接收」之间出现约 0.5ms 盲区，而 E-Marker 收到
Discover Identity 后约 30µs 就回 GoodCRC，应答正好落在盲区里丢失 —— 这是早期
「所有组合都收不到 GoodCRC」的根因。当时用私有忙等 `PD_Busy_Us()` 绕过，
属于治标；现在 `Delay_Us()` 已修好，该包装函数及其 `PD_Wait_Ms()` 已删除。

**当前实现要点：**
- 读 `CNT` 差值累加，跨 1ms 回绕时补 `(CMP + 1 - last) + now`，不做相位等待；
- `spin` 上限只用于防止 SysTick 停摆（例如调试器 halt）时死等，正常远用不到；
- 精度由硬件计数器保证，不受编译器优化和中断影响。

```c
void Delay_Us(uint32_t n)
{
    uint32_t cycles = Delay_Cycles_Per_Us() * n;
    uint32_t last = SysTick->CNT, acc = 0u, spin = cycles + 1024u;

    while ((acc < cycles) && (spin-- != 0u)) {
        uint32_t now = SysTick->CNT;
        if (now >= last) {
            acc += now - last;
        } else {                                    /* 跨过 1ms 回绕 */
            acc += (SysTick->CMP + 1u - last) + now;
        }
        last = now;
    }
}
```

**使用建议：**
- ✅ 初始化阶段的短延时（RC 稳定、TFT 复位）直接调用
- ✅ 主循环优先用 `system_tick_ms` 时间戳判断，只有 PD 这类 µs 级时序才用 `Delay_Us`
- ⚠️ 不要在中断里长时间调用（阻塞期间主循环停摆）

### 6.6 编译期调试开关

为兼顾可调试性与正式固件的体积/时序，所有诊断代码都由编译宏控制，**默认全部关闭**。

| 宏 | 位置 | 默认 | 作用 |
|---|---|---|---|
| `EMARK_DEBUG_DIAG` | `emarker_pd.h` | `0` | E-Mark 诊断：`g_emark_diag` 16 组采样、CC 电压档位、VCONN 上升/下降指纹、PHY 波形翻转计数、`EMarker_Debug_Scan()` 全量扫描、`g_t_setup/g_t_tx/g_task_max_ms` 分段计时 |
| `CABLE_DEBUG_TIMING` | `cable_test.h` | `0` | 整机时序：`g_loop_max_gap`（主循环最大间隔＝按键最坏延迟）、`g_det_max_ms`、`g_draw_max_ms` |

关闭时的收益（实测）：

| 配置 | text | bss |
|---|---|---|
| 两个宏都开 | 22700 | 3216 |
| 默认（都关） | 21420 | 2404 |

即省约 1.2KB Flash、812B RAM，并且不再有每步的比较/采样开销。
需要用 WCH-Link + GDB 回读诊断数据时，把对应宏改成 `1` 重编译即可，
上层调用点无需改动（关闭时诊断函数编译为空操作或空实现）。

## 7. 测试计划

### 7.1 单元测试
- GPIO初始化测试
- ADC采样测试（整数运算验证）
- SPI1通信测试（12MHz稳定性）
- TFT显示测试

### 7.2 集成测试
- 通断测试功能验证
- 电阻测量精度验证（<2%误差）
- E-mark检测验证
- 快充识别验证
- PMOS控制时序验证

### 7.3 系统测试
- 长时间稳定性测试（24小时）
- 不同线缆类型测试
- 边界条件测试（0mΩ, 1000mΩ）
- 功耗测试（1%占空比）

## 8. 注意事项

### 8.1 硬件相关

1. **PMOS控制极性**
   - V_PWR_EN低电平使能，高电平关闭
   - 与NPN三极管相反

2. **SPI1专用引脚**
   - SCK必须在PA5
   - MOSI必须在PA7
   - 不能更改到其他引脚

3. **E-mark检测时序**
   - 检测时短暂配置为上拉输入
   - 检测后必须恢复浮空输入
   - 避免影响正常CC通信

4. **VCONN供电引脚**
   - VCONN由PC3(CC1)和PC0(CC2)提供，非PC14/PC15（USBPD硬件引脚）
   - 2个GPIO并联约40mA，E-mark芯片通常<5mA，足够使用

5. **CC_E1/CC_E2浮空要求（V2.2更新）**
   - E-Marker**检测阶段**（探测Rd）PA1(CC_E2)和PA2(CC_E1)必须为浮空输入
   - E-Marker **PD通信阶段**（V2.2起）：一脚接地（推挽低）+ 一脚浮空，
     经5.1kΩ形成通信线Rd，方向由固件自动试错交换（见3.4.4）
   - 两端插线缆时PA2经线缆CC1直连PC14(USBPD_CC1)，检测阶段5.1k负载会破坏Rp/Rd分压

### 8.2 软件相关

1. **整数运算溢出保护**
   - 使用32位中间变量：`((uint32_t)adc_value * 4990UL)`
   - 避免16位乘法溢出

2. **单位统一**
   - 电阻：毫欧 (mΩ)
   - 电压：毫伏 (mV)
   - 电流：微安 (μA)

3. **显示格式化**
   - 使用 `%lu` 而非 `%.0f`
   - 添加 `(unsigned long)` 类型转换

4. **SPI1时序配置**
   - CPOL=High, CPHA=2Edge
   - 必须严格按此配置

## 9. 进一步优化建议

### 9.1 短期优化（可选）

1. **查表法加速**
   - 预计算ADC值到电阻的映射表
   - 牺牲8KB RAM换取O(1)查询速度

2. **DMA传输**
   - 使用DMA自动采集ADC数据
   - 减少CPU干预，降低功耗

### 9.2 中期优化（可选）

1. **UI优化**
   - 改进显示效果
   - 添加动画效果
   - 优化用户体验

2. **双缓冲技术**
   - 在SRAM中维护帧缓冲区
   - 后台渲染，前台刷新
   - 减少闪烁

> 原「PD 协议集成（读取 E-mark 详细信息、解析 PDO）」已在 V1.1~V2.4 完成。

## 10. 相关文档

- **CH32X035数据手册**: `doc/CH32X035DS0.PDF`
- **CH32X035参考手册**: `doc/CH32X035RM.PDF`
- **USB PD 规范**: `doc/USB_PD_R3.2_V1.2_2026_03_17.pdf`

---

# 附录A：USB PD协议位域定义

## A.1 Structured VDM Header（32位）

```
MSB                                                                  LSB
┌──────────────────┬─────┬──────┬──────┬────────┬──────┬───┬────────┐
│     31:16        │  15 │14-13 │12-11 │ 10:8   │ 7:6  │ 5 │  4:0   │
├──────────────────┼─────┼──────┼──────┼────────┼──────┼───┼────────┤
│      SVID        │Type │ Ver  │ Rsvd │Obj Pos │CmdTyp│ R │ Command│
└──────────────────┴─────┴──────┴──────┴────────┴──────┴───┴────────┘
```

| 位域 | 说明 | Discover Identity值 |
|------|------|-------------------|
| 31:16 | SVID（Standard/Vendor ID） | 0xFF00（USB-IF标准） |
| 15 | VDM Type（1=Structured） | 1 |
| 14:13 | VDM Version | 01（Version 2.0） |
| 12:11 | Reserved | 0 |
| 10:8 | Object Position | 1 |
| 7:6 | Command Type | 01（Initiator） |
| 5 | Reserved | 0 |
| 4:0 | Command | 0x01（Discover Identity） |

## A.2 Cable VDO（被动 / 主动）

依据 `doc/USB_PD_R3.2_V1.2_2026_03_17.pdf`：

- 被动线缆：**Table 6.42**（p.161）
- 主动线缆：**Table 6.43** VDO1（p.164）、**Table 6.44** VDO2（p.166）

位位置大体相同，但同一位置含义不同，**必须按线缆类型分别解析**。

### A.2.1 VDO1 位定义对照

| 位 | 被动线缆（6.42） | 主动线缆（6.43） |
|---|---|---|
| 31:28 | Hardware Version | Hardware Version |
| 27:24 | Firmware Version | Firmware Version |
| 23:21 | VDO Version（000b=1.0，其余无效） | VDO Version（000b/010b 弃用，011b=1.3） |
| 20 | Reserved | Reserved |
| 19:18 | 10b=Type-C，11b=Captive | 同 |
| 17 | EPR Capable | EPR Capable |
| 16:13 | Latency：0001b~0111b=1~7m，**1000b=">7m"**，其余无效 | Latency：0001b~0111b=1~7m，**1000b~1010b=100/200/300m**，其余无效 |
| 12:11 | 00b=不需 VCONN，01b=需持续 VCONN，10b/11b 无效 | 10b=一端有源一端无源需 VCONN，11b=两端有源需 VCONN，00b/01b 无效 |
| 10:9 | 00b=20V，01b/10b 弃用按 20V，11b=50V | 同 |
| 8 | **Reserved** | SBU Supported（**0=支持**，反逻辑） |
| 7 | **Reserved** | SBU Type（0=无源，1=有源） |
| 6:5 | 电流：00b/11b 无效按 3A，01b=3A，**10b=5A** | 同上，但 **VBUS Through=0 时整段忽略** |
| 4 | **Reserved** | VBUS Through Cable（0=无直通 VBUS） |
| 3 | **Reserved** | SOP'' Controller Present |
| 2:0 | USB Highest Speed | USB Highest Speed |
| 2:0 取值 | 000=USB2 / 001=Gen1 / 010=Gen2 / 011=USB4 G3 / 100=USB4 G4 | 同 |

### A.2.2 Active Cable VDO2（Table 6.44）

| 位 | 字段 | 说明 |
|---|---|---|
| 31:24 | Maximum Operating Temperature | 最高工作温度（℃） |
| 23:16 | Shutdown Temperature | 关断温度（℃） |
| 15 | Reserved | |
| 14:12 | U3/CLd Power | >10mW … <50µW 共 7 档 |
| 11 | U3→U0 Mode | 0=直接，1=经 U3S |
| 10 | Physical Connection | 0=铜，1=光纤 |
| 9 | Active Element | 0=Re-driver，1=Re-timer |
| 8 | USB4 Supported | **0=支持**（反逻辑） |
| 7:6 | USB2 Hub Hops | |
| 5 | USB2 Supported | **0=支持**（反逻辑） |
| 4 | USB3.2 Supported | **0=支持**（反逻辑） |
| 3 | USB Lanes | 0=1 lane，1=2 lanes |
| 2 | Optically Isolated | |
| 1 | USB4 Asymmetric | |
| 0 | USB Gen | 0=Gen1，1=Gen2+ |

### A.2.3 界面呈现差异

| 位置 | 被动线缆 | 主动线缆 |
|---|---|---|
| 标题栏右侧 | `Passive` | `Active` |
| 第 2 行左 | `VCON:Req/No`（Termination） | `SBU:Act/Pas/No` |
| 第 5 行 | `HW:0xX FW:0xX` | `T:最高/关断` + `Cu|Opt / Rdrv|RTmr`（取自 VDO2） |

## A.3 GoodCRC消息头

GoodCRC是PD协议层的ACK确认，必须在收到消息后750μs（tReceive）内发送：

```
Header（16位）：
┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
│15:12 │11:10 │  9:8 │   7  │  6:5 │  4:3 │  2:1 │   0  │
├──────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┤
│Rsvd  │Rsvd  │Port  │Rsvd  │MsgType│Rsvd  │MsgID │Rsvd  │
│      │      │No    │      │       │      │      │      │
└──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
```

- Message Type = 0x01（GoodCRC）
- Message ID = 与被确认消息的Message ID一致

## A.4 Product VDO（32位）

```
MSB                                                                  LSB
┌──────────────────────┬──────────────────────┐
│       31:16          │        15:0          │
├──────────────────────┼──────────────────────┤
│   USB Product ID     │      bcdDevice       │
└──────────────────────┴──────────────────────┘
```

| 位域 | 说明 |
|------|------|
| 31:16 | USB Product ID：由厂商分配给端口的Product ID（本固件解析为 product_id 字段） |
| 15:0 | bcdDevice：厂商分配给端口的设备版本号 |

## A.5 线缆长度解析（Cable VDO1 Latency 位 16:13）

Latency 表示信号在线缆内的延迟，用于近似估算长度。
**被动线与主动线在 1000b 以上的编码完全不同，不能共用一张表**：

| Latency | 被动线缆（Table 6.42） | 主动线缆（Table 6.43） |
|---|---|---|
| 0000b | 无效（不得使用） | 无效（不得使用） |
| 0001b | ~1m（<10ns） | ~1m |
| 0010b | ~2m（10-20ns） | ~2m |
| 0011b | ~3m（20-30ns） | ~3m |
| 0100b | ~4m（30-40ns） | ~4m |
| 0101b | ~5m（40-50ns） | ~5m |
| 0110b | ~6m（50-60ns） | ~6m |
| 0111b | ~7m（60-70ns） | ~7m |
| 1000b | **">7m"（>70ns）** | ~100m（1000ns） |
| 1001b | 无效 | ~200m（2000ns） |
| 1010b | 无效 | ~300m（3000ns） |
| 1011b~1111b | 无效 | 无效 |

固件中对应两张查表：`passive_lat_len_str[]` / `active_lat_len_str[]`，
由 `cable_type` 选择。

---

**文档结束**
