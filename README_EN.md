# ymgui-h743 — A phone-style prototype running YMGUI on STM32H743VIT6

> This is the English translation. The primary (Chinese) version is [`README.md`](README.md).

Porting the domestic GUI library [YMGUI](https://github.com/Yao-Mi/YMGUI) to an STM32H743VIT6 system board, running its built-in Chinese phone Shell (`project_Demo/phone_shell`): a 320×480 parallel-interface display + capacitive touch + 16 MB QSPI Flash + an external **ES8311 audio codec** + an external **ESP32 network/Bluetooth coprocessor**, bare-metal (no RTOS), built with CMake + GCC, with all resources verified on real hardware.

Project details are in [`docs/`](docs/) (pin-header map [`docs/HEADER_PINOUT.md`](docs/HEADER_PINOUT.md), hardware drawings [`docs/hw_ref/`](docs/hw_ref/)); the development narrative is in [`docs/DEVELOPMENT_LOG.md`](docs/DEVELOPMENT_LOG.md).

---

## Hardware target board

| Part | Model / Parameters | Connection |
| --- | --- | --- |
| MCU | **STM32H743VIT6**, Cortex-M7 @ 400 MHz, 2 MB Flash / 1 MB SRAM | — |
| Display | **320×480 ST7796**, RGB565 | 8080 parallel bus via FMC (NE1, A19 as RS) |
| Touchscreen | **GT9xxx** capacitive touch | I²C (**PB11=SCL / PB14=SDA / PB12=RST / PB13=INT**, per actual measurement, not vendor docs) |
| External Flash | **W25Q128**, 16 MB | QSPI, memory-mapped XIP @ `0x9000_0000` |
| TF card slot | Micro SD (no card-detect pin) | SDMMC1 4-bit: D0–D3 = PC8–PC11, CLK = PC12, CMD = PD2 |
| VBAT | BAT54C switches VCC3.3 / VBAT_IN (no backup battery ⇒ RTC resets on power loss) | — |

> The authoritative hardware source is the vendor package `E:\BaiduNetdiskDownload\慧勤智远 STM32H743VIT6 V1.2小系统板\` (pins/components are governed by `2. 原理图\STM32H743VIT6 V1.2_SCH.pdf`).

---

## External module wiring

**ES8311 audio codec** (I²S slave, STM32 provides the clock; control over I²C, 7-bit address **0x18**)

| STM32 | Alternate | To module | Notes |
| --- | --- | --- | --- |
| PB6 / PB7 | I2C1 (AF4, open-drain) | SCL / SDA | I²C control (no external pull-up on board, relies on MCU internal ~40 kΩ) |
| PC6 | I2S2_MCK | MCLK | master clock |
| PD3 / PB9 | I2S2_CK / I2S2_WS | BCLK / LRCK | ⚠ **PB9 is shared with UART4_TX**: used as LRCK while audio is active, returned to UART4 when idle |
| PB15 / PC2 | I2S2_SDO / I2S2_SDI | DIN / DOUT | playback PB15→DIN; recording DOUT→PC2 |
| 5V / GND | — | VCC / GND | module uses external 5V; a speaker must be connected |

**ESP32 network / Bluetooth coprocessor** (STM32 itself has no wireless; WiFi/BT live on the ESP32)

- **UART4 control/uplink** (921600 8N1, no flow control): `PB9=TX→GPIO22` / `PB8=RX←GPIO1`
- **SPI1 bulk data** (STM32 master / ESP32 slave): `PA5=SCK` / `PA7=MOSI` / `PB4=MISO` / `PC5=CS(software)` / `PC0=READY←GPIO21`

The full pin-header map is in [`docs/HEADER_PINOUT.md`](docs/HEADER_PINOUT.md).

---

## Software composition

Bare-metal main loop (no RTOS): a self-built board layer (startup / two-tier heap / fault snapshot) + STM32H7 HAL/LL + FatFs + the YMGUI library + `phone_shell` (desktop / launcher / status bar / 16 apps / Chinese IME). Read-only large resources (GB2312 font 982 KB, phrase dictionary 1.3 MB, etc.) reside in QSPI and are accessed in-place via memory mapping (XIP), with zero RAM footprint.

---

## Build & flash

Dependencies: `arm-none-eabi-gcc 14.3.rel1`, `Ninja`, `CMake ≥ 3.20`, `probe-rs`, `pyserial`.

```bash
cd /e/ymgui-h743
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build --target ymgui-bin          # produce bin / hex and print sizes

P="--probe 0416:5021:0123456789AB --chip STM32H743VITx"
probe-rs download $P build/ymgui-h743.elf
probe-rs reset    $P                            # ⚠ must reset after download, or the core stays halted
```

> The build directory must be named `build` (the linker map path is hardcoded); flashing goes over SWD (FireDAP), not via COM3.

---

## Firmware capabilities

- **Desktop Shell**: three desktop pages (swipe to switch), app drawer, recent tasks, quick dropdown panel, status bar, settings page (with scrolling container + scrollbar, added in this project).
- **16 apps**: Tasks · Music · Gallery · Settings · Clock · Weather · Notes · Calculator · Phone · SMS · Contacts · Recorder · Files · Gyroscope · Camera · Browser.
- **Chinese IME**: pinyin → candidates → commit (7291 single chars + 47280 phrases; the phrase dictionary resides in external Flash with a zero-RAM index).
- **Time / Weather**: fetched via ESP32 (UART4) as NTP time + weather, shown in the status bar and the Weather app.
- **Bluetooth file receive**: phone SPP → ESP32 → SPI1 → board TF card → gallery (the File Manager "Bluetooth received" category shows a progress bar).
- **Music / Recorder**: via ES8311 (I2S2 TX DMA playback / I2S2 master-receive recording).
- **Image gallery**: TF-card BMP/JPEG → RGB565 → written to W25Q128 → XIP zero-RAM display.

---

## Directory structure

```
src/          Firmware source (bare-metal); linker/ link scripts (DTCM/SRAM1/SRAM2/AXI partitions)
third_party/  Third-party libraries (YMGUI / FatFs / fonts)
tools/        Host-side Python diagnostics/acceptance scripts (SWD read/write, no recompile or reset needed)
docs/         Design / pin-map / acceptance docs (HEADER_PINOUT.md, hw_ref/, etc.)
ci/           Firmware reference hashes + byte-by-byte comparison scripts
build/        Build artifacts (must be named build, gitignored)
```

---

## Known limitations

- Gallery JPEG only supports ≤320×320 (hardware JPEG has no scaling ⇒ oversized images are rejected; large images must be pre-scaled on the card).
- Bluetooth file-receive GET occasionally needs a retransmission (inherent jitter of the DuPont-wire SPI link, not a software bug).
