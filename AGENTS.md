# AGENTS.md

## 项目概述

基于 CH32X035G8U（RISC-V RV32IMAC，沁恒 WCH）的 USB 线缆测试仪固件。裸机嵌入式 C，无操作系统。

## 构建

- **构建命令**：在 `CH32X035G8U/obj/` 下执行 `make -j16 all`。每次改动代码后都要执行，确认编译通过。
- **工具链**：`riscv-none-embed-gcc`（GCC 8.2.0，RISC-V），GNU99（`-fsigned-char -fno-common`）、`-Os`、`-msave-restore`、newlib-nano。
- **ISA/ABI**：`-march=rv32imacxw -mabi=ilp32`（含 WCH 自定义 XW 扩展）。
- **工程配置**：以下三个 IDE 文件都有效，必须保持一致：
  - `.cproject` — Eclipse CDT 托管构建配置（头文件路径、`rv32imacxw`/`ilp32`、`-Os`、`-msave-restore`、GNU99、`Ld/Link.ld`、hex/lst/map 生成）。`obj/` 下的 Makefile 由它生成。
  - `CH32X035G8U.wvproj` — MRS2 原生工程配置：`buildConfig`、`flashConfig`、`debugConfigurations`（RISC-V 目标、OpenOCD 端口、GDB 命令、SVD 路径）。**它不是空占位符**，MRS2 会把完整配置写在这里。`.project` 用 `*.wvproj` 过滤器把它从 IDE 视图里隐藏，所以容易被忽略。
  - `.template` — MRS 芯片/烧录元数据（`CH32X035G8U6`、WCH-Link、`0x08000000`、`obj/CH32X035G8U.hex`、擦除/编程/校验/复位）。
  - `obj/` 下的 Makefile 是自动生成的，不要手工修改。
- **构建产物**：`obj/CH32X035G8U.elf`（62K Flash、20K RAM，链接脚本 `Ld/Link.ld`），以及 `.hex`、`.lst`、`.map`、`.siz`。
- **调试开关**（两者默认 `0`，正式版本保持关闭）：
  `User/emarker_pd.h` 中的 `EMARK_DEBUG_DIAG`（`g_emark_diag` 诊断 + 扫描）、
  `User/cable_test.h` 中的 `CABLE_DEBUG_TIMING`（`g_loop_max_gap` 及计时计数器）。
  打开后约增加 1.2K Flash / 812B bss，但可通过 GDB 回读内部状态。
- **烧录/调试**：WCH-Link + OpenOCD（`wch-riscv.cfg`），GDB 端口 3333（由 MounRiver Studio 接管）。
- **头文件搜索顺序**：`Debug/`、`Core/`、`User/`、`Peripheral/inc/`。
- **无 CI、无 linter、无测试** —— 靠编译验证正确性。

## 版本控制（IDE 工程文件）

以下 5 个 MRS/Eclipse 工程文件都已纳入版本控制，应一起提交：

| 文件 | 为什么要进仓库 |
|---|---|
| `.cproject` | 编译/链接参数的唯一真源 |
| `.project` | Eclipse 工程身份、CDT natures 与 genmakebuilder；缺了它 MRS 不会把这个目录识别为 CDT 工程 |
| `.template` | 芯片选型与 WCH-Link 烧录设置 |
| `CH32X035G8U.wvproj` | MRS2 的构建/烧录/调试配置 |
| `CH32X035G8U.launch` | 共享的 OpenOCD/GDB 启动设置（`wch-riscv.cfg`、GDB 3333 / telnet 4444、`set disassembler-options xw`） |

这些文件里的路径全部使用 IDE 变量（`${workspace_loc:...}`、`${ProjName}`、`${eclipse_home}`、`${WCH:...}`），没有本机绝对路径，可跨机器使用。MRS2 每次打开工程都会重写它们（属性重排、把默认选项显式写出），但**不会改变实际编译参数**；这类重写建议一次性提交，否则每次开 IDE 都会产生 diff 噪音。`.gitignore` 已排除 `obj/`、`EVT/`、`backup/`、`.workbuddy/`、`.mrs/`；根目录 `*.hex` 之类的构建产物不要提交。

## 代码结构

```
CH32X035G8U/
├── Startup/startup_ch32x035.S    # 向量表、复位处理
├── Core/core_riscv.c             # RISC-V 内核初始化（SystemCoreClockUpdate、Delay）
├── Peripheral/inc/, src/         # WCH HAL（ch32x035_gpio.h、ch32x035_adc.h 等）
├── Debug/debug.c                 # USART printf
├── User/                         # 应用代码（应该改这里）
│   ├── main.c                    # SysTick（1ms）、初始化流程、主循环
│   ├── cable_gpio.c/h            # 引脚配置：V_PWR_EN(PB6)、VCONN(PC3/PC0)、CC_E1/CC_E2(PA2/PA1)
│   ├── cable_adc.c/h             # 双通道 ADC、阈值自动切换
│   ├── cable_spi.c/h             # 硬件 SPI1 驱动 TFT（CPOL=High、CPHA=2Edge、12MHz）
│   ├── cable_tft.c/h             # ST7735S 160×80 驱动、RGB565、RST 由硬件 RC 复位
│   ├── cable_test.c/h            # 测试状态机、按键扫描、双模式显示
│   ├── emarker_pd.c/h            # 与 E-Marker 通信的 USB PD 协议
│   ├── cable_font.c/h            # TFT 字库（5×8、8×16、12×24）
│   ├── ch32x035_conf.h           # 外设 HAL 头文件汇总
│   └── ch32x035_it.c/h           # 中断处理（SysTick 递增 system_tick_ms）
└── Ld/Link.ld                    # 链接脚本
```

## 关键设计事实

- **主循环非阻塞**：1ms SysTick 驱动 `system_tick_ms`。`cable_test.c` 中的 `Test_State_Machine()` 以状态机方式完成电阻测量。绝不要加阻塞延时。
- **两种显示模式**：`MODE_CONTINUITY`（单屏显示通断 + 电阻 + 快充）和 `MODE_EMARK`（显示 E-Mark 信息）。
- **电阻测量**：VBUS/GND 回路中串 50Ω 电阻（硬件已从 100Ω 改为 50Ω，好让 VBUS 供电的 E-Marker 在读取期间能越过 UVLO —— `cable_test.c` 里的公式必须和硬件值保持一致）。两个 ADC 通道：<1Ω 用 101x 增益，最高 9.99kΩ 用 1x 增益，带滞回自动切换。每 1s 周期中测量生效约 55ms。
- **VCONN 与 PD 通道的关系**（关键）：VCONN 由**辅助 CC GPIO 引脚**驱动（CC1 用 PC3、CC2 用 PC0）。USBPD 硬件使用专用引脚（CC1 用 PC14、CC2 用 PC15）。若 E-Marker 在 CC1 上，则 VCONN 走 PC3，而 PD 通信走 CC2（PC15）。二者是交叉对应关系。

## 项目文档（仅限本仓库，非通用）

- `README.md`（根目录）— 功能总览、使用说明、故障排查
- `doc/CableTester_Design.md` — 系统设计文档
- `doc/CH32X035DS0.PDF` 与 `doc/CH32X035RM.PDF` — MCU 数据手册与参考手册
- `EVT/EXAM/` — WCH 官方外设例程（ADC、SPI、USB 等）

## 代码风格

- 注释优先使用中文，继承的英文注释继续保留。新增代码沿用此约定。
- 关键设计决策必须写注释说明**为什么**，而不只是「是什么」。
- 模块文件头包含 `(C)` 版权块、作者 `CableTester Team`、版本和日期。
- 头文件保护宏：`__MODULENAME_H`（双下划线前缀）。
- 所有 `.h` 文件都要有 `extern "C"` 保护。
- 函数使用 Doxygen 风格的 `@brief` / `@param` / `@return` / `@note` 注释。
