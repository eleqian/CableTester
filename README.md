# USB线缆测试仪 (CableTester)

## 功能

- 两组测试接口：Type-A + Type-C（左侧）、Micro-B + Type-C（右侧）
- 支持Type-C全信号通断测试：屏蔽层(EH)、VBUS/GND、CC、SBU、D+/D-、TX1/2、RX1/2
- 电源线电阻测量：双通道自动切换，0~900mΩ（高精度）和900mΩ~100Ω量程
- CC通断检测（合并测试）
- **USB PD协议读取E-Mark芯片详细信息**（线缆类型、速度等级、电流能力、EPR、VID/PID）
- 屏蔽层连通性及对地短路检测
- 快充协议识别（华为56k电阻检测）
- USB供电和通信
- 1个按键切换两种显示模式（合并通断/电阻/快充 → E-Mark信息）

## 固件代码

### 主要功能模块

1. **GPIO模块** (`cable_gpio.c/h`) — 引脚初始化、测试电压控制(PMOS)、CC_E驱动、VCONN供电
2. **ADC模块** (`cable_adc.c/h`) — 双通道ADC（V_PWR_L/V_PWR_H）、电阻计算、自动挡位切换
3. **TFT显示模块** (`cable_tft.c/h`) — ST7735S驱动、多字体渲染、结果展示
4. **测试模块** (`cable_test.c/h`) — 通断测试、电阻测量状态机、E-Mark检测、快充识别
5. **E-Mark PD通信模块** (`emarker_pd.c/h`) — USB PD协议栈、Discover Identity、VDM解析、VCONN控制

### 编译和下载

**硬件需求**：
- CH32X035G8U目标板
- WCH-Link调试器
- USB-C数据线（供电和调试）

**软件需求**：
- MounRiver Studio（推荐）或 WCH IDE
- WCH-Link驱动程序

**编译步骤**：
1. 打开MounRiver Studio，选择 `File → Import → Existing Projects into Workspace`
2. 浏览到 `CH32X035G8U` 目录，点击 `Finish`
3. 确认以下源文件已添加到工程：`main.c`、`cable_gpio.c/h`、`cable_adc.c/h`、`cable_spi.c/h`、`cable_tft.c/h`、`cable_test.c/h`、`emarker_pd.c/h`、`cable_font.c/h`
4. 确认芯片型号为CH32X035G8U，时钟频率48MHz
5. 点击编译（`Ctrl+B`），确认控制台输出 `BUILD SUCCESSFUL`

**下载步骤**：
1. 连接WCH-Link到电脑USB口
2. 连接WCH-Link到目标板：DIO→PC18，DCK→PC19，GND→GND
3. 在IDE中选择 `Run → Debug`（F11），程序自动下载并运行

**验证**：TFT屏幕显示"Welcome"欢迎界面。如屏幕无显示，检查电源和TFT连接线。

## 文档

- **详细设计文档**: [doc/CableTester_Design.md](doc/CableTester_Design.md)
- **CH32X035数据手册**: [doc/CH32X035DS0.PDF](doc/CH32X035DS0.PDF)
- **CH32X035参考手册**: [doc/CH32X035RM.PDF](doc/CH32X035RM.PDF)

## E-Mark PD通信功能

### 功能概述
本项目的E-Mark检测功能已升级为完整的USB PD协议通信，可以读取E-Mark芯片的详细信息，包括：线缆类型（被动/主动）、USB速度等级（USB 2.0~USB4 Gen4）、电流能力（3A/5A）、EPR支持状态、Vendor ID和Product ID。

### 使用方法
在MODE_EMARK显示模式下，系统自动完成：
1. 检测E-Mark芯片存在性
2. 使能VCONN供电（PC3或PC0，等待50ms稳定）
3. 配置PD通信CC线为Rp上拉
4. 通过USBPD硬件发送Discover Identity VDM命令
5. 接收GoodCRC确认，解析VDM响应
6. 在TFT上显示线缆详细信息
7. 关闭VCONN

### 校验方法
使用**双头Type-C线缆**（全功能线），两端分别插入测试仪左右接口，按下KEY键切换至E-Mark模式观察显示。

### 故障排查
| 现象 | 可能原因 | 解决 |
|------|---------|------|
| 显示"E-Mark: NO" | 线缆不支持E-Mark | 换全功能USB3/4线缆测试 |
| 显示"PD Comm Failed" | VCONN时序问题 | 检查VCONN与PD通信通道逆关系是否正确 |
| 通信超时 | CC_LVE未正确控制 | 确认发送后禁用LVE恢复恒流源模式 |

### 支持的E-Mark芯片
符合USB-IF标准的所有E-Mark芯片（Cypress CYPD3177、TI TPS65987D、CH224K等）。

## 故障排查

| 问题 | 可能原因 | 解决 |
|------|---------|------|
| 屏幕无显示 | TFT连接错误 | 检查SPI线：CS→PA4, SCK→PA5, SDA→PA7, RS→PA6 |
| 屏幕无显示 | TFT供电异常 | 检查3.3V供电 |
| 电阻显示"OL" | 线缆未正常连接 | 确保两端都插到位 |
| 电阻测量偏差大 | 接触不良 | 清洁接口触点 |
| 按键无响应 | 引脚配置错误 | 确认KEY连接至PA3(KEY_M) |
| 下载失败 | WCH-Link连接 | 检查SWD线：DIO→PC18, DCK→PC19 |

## 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| V1.0 | 2026-04-26 | 初始版本，完成固件代码开发 |
| **V3.1** | **2026-05-05** | **新增E-Mark PD通信功能，支持读取详细信息** |

## 许可证

本项目的硬件设计和固件代码遵循相关开源协议。
