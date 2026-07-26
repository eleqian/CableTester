# AGENTS.md

## Project identity

USB cable tester firmware for CH32X035G8U (RISC-V RV32IMAC, WCH/Qinheng). Bare-metal embedded C, no OS.

## Build

- **Build command**: `make -j16 all` from `CH32X035G8U/obj/`. After any code change, run this to verify compilation succeeds.
- **Toolchain**: `riscv-none-embed-gcc` (GCC 8.2.0, RISC-V), GNU99 (`-fsigned-char -fno-common`), `-Os`, `-msave-restore`, newlib-nano.
- **ISA/ABI**: `-march=rv32imacxw -mabi=ilp32` (includes WCH XW custom extension).
- **Project config**: `.cproject` (Eclipse CDT managed build). The `.wvproj` is an empty MounRiver Studio placeholder. The Makefiles under `obj/` are auto-generated — do not edit them by hand.
- **Build output**: `obj/CH32X035G8U.elf` (62K flash, 20K RAM, linker script `Ld/Link.ld`), plus `.hex`, `.lst`, `.map`, `.siz`.
- **Flash/debug**: WCH-Link via OpenOCD (`wch-riscv.cfg`), GDB on port 3333 (MounRiver Studio handles this).
- **Include path order**: `Debug/`, `Core/`, `User/`, `Peripheral/inc/`.
- **No CI, no linter, no tests** — verify correctness by compiling.

## Architecture

```
CH32X035G8U/
├── Startup/startup_ch32x035.S    # Vector table, reset handler
├── Core/core_riscv.c             # RISC-V core init (SystemCoreClockUpdate, Delay)
├── Peripheral/inc/, src/         # WCH HAL (ch32x035_gpio.h, ch32x035_adc.h, etc.)
├── Debug/debug.c                 # printf over USART
├── User/                         # Application code (the code you should edit)
│   ├── main.c                    # SysTick (1ms), init sequence, main loop
│   ├── cable_gpio.c/h            # Pin config, V_PWR_EN, VCONN control (PC0/PC3)
│   ├── cable_adc.c/h             # ADC read, averaging, single-channel only
│   ├── cable_spi.c/h             # Software SPI for TFT (CPOL=High, CPHA=2Edge)
│   ├── cable_tft.c/h             # ST7735S 160×80 driver, RGB565
│   ├── cable_test.c/h            # Test state machine, key scan, display modes, results struct
│   ├── emarker_pd.c/h            # USB PD protocol for E-Mark communication
│   ├── cable_font.c/h            # Font data for TFT
│   ├── ch32x035_conf.h           # Peripheral HAL include aggregator
│   └── ch32x035_it.c/h           # Interrupt handlers (SysTick increments system_tick_ms)
└── Ld/Link.ld                    # Linker script
```

## Key design facts

- **Main loop is non-blocking**: 1ms SysTick drives `system_tick_ms`. `Test_State_Machine()` in `cable_test.c` handles resistance measurement as a state machine (RC settle wait, ADC read, calculation). Never add blocking delays.
- **Two display modes only** (V3.0): `MODE_CONTINUITY` (shows continuity + resistance + fast charge on one screen) and `MODE_EMARK` (shows E-Mark info).
- **Resistance measurement**: 100Ω series resistor @ 3.3V → ~33mA through VBUS/GND loop. Two ADC channels: V_PWR_L (PB0, op-amp 101x non-inverting, gain=1+10k/100) for <1Ω, V_PWR_H (PB1, direct) for up to 100Ω. Auto-switch at ~900mΩ (V_PWR_L adc > 3686) with hysteresis. Range ≤1Ω (resolution ~0.24mΩ) or ≤100Ω (resolution ~24mΩ). Measurement active ~55ms per 1s cycle.
- **VCONN / PD channel relationship** (critical): VCONN is driven on **auxiliary CC GPIO pins** (PC3 for CC1, PC0 for CC2). USBPD hardware uses dedicated pins (PC14 for CC1, PC15 for CC2). If E-Mark is on CC1, VCONN goes on PC3 and PD communication uses CC2 (PC15). This is an inverse relationship.
- **CC_E1/CC_E2 (PA2/PA1)**: Right-port CC pins, coupled by 5.1kΩ. **Must be floating during E-Marker PD communication** to avoid loading USBPD pins. During CC continuity test: both driven low (combined CC test). During Huawei fast-charge detect: PA2=high, PA1=ADC (5.1k acts as pull-up). No CC_R pin anymore.

## Documentation (repo-specific, not generic)

- `doc/CableTester_Design.md` — system design document
- `EVT/EXAM/` — WCH official peripheral examples (ADC, SPI, USB, etc.)
- `doc/CH32X035DS0.PDF` & `doc/CH32X035RM.PDF` — MCU datasheet and reference manual

## Code style

- Bilingual comments (Chinese + English). New code should follow this pattern.
- Module headers include `(C)` block, author `CableTester Team`, version and date.
- Include guards: `__MODULENAME_H` (double underscore prefix).
- `extern "C"` guards in all `.h` files.
- Doxygen-style `@brief` / `@param` / `@return` / `@note` comments on functions.
