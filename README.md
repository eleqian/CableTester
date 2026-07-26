# USB线缆测试仪 (CableTester)

## 功能
- 第一组母座：type A（usb3.0）、type-C（24p）；
- 第二组母座：micro B（usb2.0）、type-C（24p）；
- 支持两组接口中各一个组合测量；
- 支持type-c所有线通断测试：屏蔽层、电源、地、cc、sbu、usb2.0、tx1、tx2、rx1、rx2；
- 支持电源和地线测量阻值（相加总阻值），最大1欧姆；
- 支持cc通断测量和e-mark信息读取；
- **支持使用USB PD协议读取E-Mark芯片详细信息**（线缆类型、速度等级、电流能力等）；
- 支持屏蔽层测量，判断是否联通或跟地短接；
- 支持usb供电和通信；
- 支持判断线缆是否支持快充（VOOC/华为快充等）；
- 1个按键切换显示内容；

## 方案
- 使用0.96 160*80 spi tft显示信息，支持多种线状态：电源线电阻、usb2.0、usb3.0、usb3.2、sbu、cc；
- mcu使用ch32x035，带adc、usb和pd功能，可直接usb和锂电池供电；
- 支持电源线和地线电阻测量，第二组接口电源和地短接，第一组输出3.3V串联100欧姆电阻输出电流（通过mos开关），环回测量电压，ina199a2放大100倍后adc测量，分辨率1mΩ；
- mos开关线阻测量电流，每s打开100ms测量，降低功耗；
- sbu、usb2.0、tx1、tx2、rx1、rx2通过环回测量信号线通断；
- **e-mark检测**：使用IO口输出vconn供电，使用mcu内置usbpd模块发送Discover Identity命令读取E-Mark芯片详细信息；

## 固件代码

### 代码结构
固件代码位于 `CH32X035G8U/User/` 目录，包含以下模块：

```
CH32X035G8U/User/
├── main.c              # 主程序
├── cable_gpio.h/c      # GPIO配置和控制
├── cable_adc.h/c       # ADC和电阻测量
├── cable_spi.h/c       # SPI通信（软件SPI）
├── cable_tft.h/c       # ST7735S TFT驱动
├── cable_test.h/c      # 线缆测试功能
├── emarker_pd.h/c      # E-Mark PD通信模块（新增）
└── README_Code.md      # 代码使用说明
```

### 主要功能模块

1. **GPIO模块** (`cable_gpio.c/h`)
   - 所有引脚初始化配置
   - 测试电压使能控制
   - E-mark检测
   - 快充识别

2. **ADC模块** (`cable_adc.c/h`)
   - ADC初始化和采样
   - 电阻值计算
   - 多次采样滤波

3. **TFT显示模块** (`cable_tft.c/h`)
   - ST7735S驱动
   - 图形绘制
   - 结果显示

4. **测试模块** (`cable_test.c/h`)
   - 通断测试
   - 电阻测量
   - E-mark检测（基础存在性检测）
   - 快充识别

5. **E-Mark PD通信模块** (`emarker_pd.c/h`) 【新增】
   - USB PD协议栈实现
   - Discover Identity命令发送
   - VDM消息解析
   - E-Mark详细信息提取：
     * 线缆类型（被动/主动）
     * USB速度等级（USB 2.0/3.2/USB4）
     * 电流能力（3A/5A）
     * EPR支持状态
     * Vendor ID和Product ID
   - VCONN电源自动控制
   - 多CC通道自动选择

### 编译和下载

**开发环境：**
- IDE: MounRiver Studio 或 WCH IDE
- 编译器: GCC for RISC-V
- 调试器: WCH-Link

**编译步骤：**
1. 打开MounRiver Studio
2. 导入CH32X035G8U工程
3. 确保User目录下所有源文件已添加
4. 编译工程

**下载步骤：**
1. 连接WCH-Link调试器
2. 下载程序到MCU
3. 复位运行

详细使用说明请参考：[CH32X035G8U/User/README_Code.md](CH32X035G8U/User/README_Code.md)

## 文档

- **详细设计文档**: [doc/CableTester_Design.md](doc/CableTester_Design.md)
- **代码使用说明**: [CH32X035G8U/User/README_Code.md](CH32X035G8U/User/README_Code.md)
- **CH32X035数据手册**: [doc/CH32X035DS0.PDF](doc/CH32X035DS0.PDF)
- **CH32X035参考手册**: [doc/CH32X035RM.PDF](doc/CH32X035RM.PDF)

## 示例代码

外设示例代码位于 `EVT/EXAM/` 目录，包含：
- ADC示例
- GPIO示例
- SPI示例
- USART示例
- USB示例
- 等等...

## 硬件电路

电路网表文件：[Netlist.asc](doc/Netlist.asc)

引脚映射：
| 引脚编号 | 引脚名 | ADC | TIM1/2                   | TIM3   | SYS | SPI  | USB | OPA       | PIOC     | 外设网络名 |
|------|------|-----|-------------------------------|--------|-----|------|-----|-----------|----------|----------|
| 5    | PA0  | A0  | T2C1/T2C1_2                   | -      | -   | -    | -   | -         | -        | EH       |
| 6    | PA1  | A1  | T2C2/T2C2_2                   | -      | -   | -    | -   | O1N2/O2N2 | -        | CC_E     |
| 7    | PA2  | A2  | T2C3/T2C3_1/T2ET_5/T2ET_6     | -      | -   | -    | -   | O2O1      | -        | KEY_M    |
| 8    | PA3  | A3  | T2C4/T2C4_1                   | T3C1_3 | -   | -    | -   | O1O0      | -        | TFT_RST  |
| 9    | PA4  | A4  | -                             | T3C2_3 | -   | CS   | -   | O2O0      | -        | TFT_CS   |
| 10   | PA5  | A5  | -                             | -      | -   | SCK  | -   | O2N0      | -        | TFT_SCK  |
| 11   | PA6  | A6  | T1BK_1                        | T3C1   | -   | MISO | -   | O1N0      | -        | TFT_RS   |
| 12   | PA7  | A7  | T1C1N_1                       | T3C2   | -   | MOSI | -   | O2P0      | -        | TFT_SDA  |
| 13   | PB0  | A8  | T1C2N_1                       | -      | -   | -    | -   | O1P0      | -        | V_PWR    |
| 16   | PB1  | A9  | T1C3N_1                       | -      | -   | -    | -   | O2N1      | -        | V_PWR_EN |
| 14   | PB3  | -   | T2C3_2/T2C3_3/T2C3N_5/T2C3N_6 | -      | -   | -    | -   | O2P1      | -        | NC       |
| 15   | PB4  | -   | T2C4_2/T2C4_3/T2BK_5/T2BK_6   | T3C1_1 | -   | -    | -   | O1P2      | -        | NC       |
| 17   | PB6  | -   | T1C1N/T1C1N_2                 | -      | -   | -    | -   | O1N1      | -        | AUX_G    |
| 18   | PB7  | -   | T1C2N/T1C2N_2                 | -      | RST | -    | -   | O2P2      | -        | SUB      |
| 19   | PB8  | -   | T1C3N/T1C3N_2                 | -      | -   | -    | -   | O1P1      | -        | RX2      |
| 20   | PB9  | -   | T1C1/T1C1_1/T1C1_2            | -      | MCO | -    | -   | -         | -        | TX2      |
| 21   | PB10 | -   | T1C2/T1C2_1/T1C2_2            | -      | -   | -    | -   | -         | -        | T_D      |
| 22   | PB11 | -   | T1C3/T1C3_1/T1C3_2/T2C1N_6    | -      | -   | -    | -   | -         | -        | RX1      |
| 23   | PB12 | -   | T1C4_2/T2C2N_5/T2C2N_6        | -      | -   | -    | -   | -         | -        | TX1      |
| 2    | PC0  | A10 | T1C1_3/T2C4_5/T2C4_6/T2BK_4   | -      | -   | -    | -   | -         | -        | CC2      |
| 3    | PC3  | A13 | T1C4_3/T2C3N_4/T2C1N_2        | -      | RST | -    | -   | -         | -        | CC1      |
| 28   | PC14 | -   | T1C3_4/T2C2_6                 | -      | -   | CC1  | -   | -         | -        | CC1      |
| 1    | PC15 | -   | T1ET_4/T2C3_6                 | -      | -   | CC2  | -   | -         | -        | CC2      |
| 26   | PC16 | -   | T1C4/T1C4_1                   | -      | -   | -    | UDM | -         | -        | UDM      |
| 27   | PC17 | -   | T1ET/T1ET_1                   | -      | -   | -    | UDP | -         | -        | UDP      |
| 25   | PC18 | -   | T1ET_2/T1ET_3/T2C1N_5         | T3C2_2 | DIO | -    | -   | -         | PIOC_IO0 | DIO      |
| 24   | PC19 | -   | T2C1_5/T2C1_6                 | T3C1_2 | DCK | -    | -   | -         | PIOC_IO1 | DCK      |

USB环回测试连接：
- TX1/TX2/RX1/RX2都是正负信号短接；
- D+和D-短接；
- 两个SBU短接；

## E-Mark PD通信功能

### 功能概述
本项目的E-Mark检测功能已升级为完整的USB PD协议通信，可以读取E-Mark芯片的详细信息，包括：
- **线缆类型**：被动线缆（Passive）或主动线缆（Active）
- **USB速度等级**：USB 2.0、USB 3.2 Gen1/Gen2、USB4 Gen3/Gen4
- **电流能力**：3A或5A
- **EPR支持**：是否支持Extended Power Range
- **厂商信息**：Vendor ID和Product ID

### 技术实现
- 使用CH32X035内置USBPD硬件模块
- 符合USB PD 3.0规范
- 发送Discover Identity VDM命令
- 自动选择CC1或CC2通道
- VCONN电源自动控制

### 使用方法
在E-Mark显示模式下，系统会自动：
1. 检测E-Mark芯片存在性
2. 使能VCONN供电（5V）
3. 初始化USBPD模块
4. 发送Discover Identity命令
5. 接收并解析响应数据
6. 在TFT屏幕上显示详细信息
7. 关闭VCONN以节省功耗

### 相关文件
- `emarker_pd.h/c`: E-Mark PD通信核心模块
- `cable_test.h/c`: 集成E-Mark检测功能
- `README_EMarker.md`: 详细使用说明
- `emarker_test_example.c`: 测试示例代码

### 支持的E-Mark芯片
本模块符合USB-IF标准，支持所有标准E-Mark芯片，包括：
- Cypress CYPD3177
- TI TPS65987D
- CH224K
- 其他符合USB PD规范的E-Mark芯片

## 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| V1.0 | 2026-04-26 | 初始版本，完成固件代码开发 |
| **V3.1** | **2026-05-05** | **新增E-Mark PD通信功能，支持读取详细信息** |

## 许可证

本项目的硬件设计和固件代码遵循相关开源协议。
