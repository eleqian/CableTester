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
│   ├── cable_gpio.c/h            # Pin config, V_PWR_EN(PB6), VCONN(PC3/PC0), CC_E1/CC_E2(PA2/PA1)
│   ├── cable_adc.c/h             # Dual-channel ADC, auto-switch thresholds
│   ├── cable_spi.c/h             # Hardware SPI1 for TFT (CPOL=High, CPHA=2Edge, 12MHz)
│   ├── cable_tft.c/h             # ST7735S 160×80 driver, RGB565, RST via hardware RC
│   ├── cable_test.c/h            # Test state machine, key scan, dual-mode display
│   ├── emarker_pd.c/h            # USB PD protocol for E-Mark communication
│   ├── cable_font.c/h            # Font data for TFT (5×8, 8×16, 12×24)
│   ├── ch32x035_conf.h           # Peripheral HAL include aggregator
│   └── ch32x035_it.c/h           # Interrupt handlers (SysTick increments system_tick_ms)
└── Ld/Link.ld                    # Linker script
```

## Key design facts

- **Main loop is non-blocking**: 1ms SysTick drives `system_tick_ms`. `Test_State_Machine()` in `cable_test.c` handles resistance measurement as a state machine. Never add blocking delays.
- **Two display modes**: `MODE_CONTINUITY` (shows continuity + resistance + fast charge on one screen) and `MODE_EMARK` (shows E-Mark info).
- **Resistance measurement**: 100Ω series resistor through VBUS/GND loop. Two ADC channels: 101x for <1Ω, 1x for up to 9.99kΩ. Auto-switch with hysteresis. Measurement active ~55ms per 1s cycle.
- **VCONN / PD channel relationship** (critical): VCONN is driven on **auxiliary CC GPIO pins** (PC3 for CC1, PC0 for CC2). USBPD hardware uses dedicated pins (PC14 for CC1, PC15 for CC2). If E-Mark is on CC1, VCONN goes on PC3 and PD communication uses CC2 (PC15). This is an inverse relationship.

## Documentation (repo-specific, not generic)

- `README.md` (root) — overall function intro + usage instructions + troubleshooting
- `doc/CableTester_Design.md` — system design document
- `doc/CH32X035DS0.PDF` & `doc/CH32X035RM.PDF` — MCU datasheet and reference manual
- `EVT/EXAM/` — WCH official peripheral examples (ADC, SPI, USB, etc.)

## Code style

- Bilingual comments (Chinese + English). New code should follow this pattern.
- Key design decisions must have a comment explaining **why** (not just what).
- Module headers include `(C)` block, author `CableTester Team`, version and date.
- Include guards: `__MODULENAME_H` (double underscore prefix).
- `extern "C"` guards in all `.h` files.
- Doxygen-style `@brief` / `@param` / `@return` / `@note` comments on functions.
