# Auto Watering System — Project Guide

## What this is

ESP32-C3 firmware that reads soil moisture and automatically runs a water pump when the soil is dry.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | **ESP32-C3 Super Mini** (note: platformio.ini targets `esp32-c3-devkitm-1` — same chip, slightly different pin labels) |
| Pump | USB water pump — 5V, 0.2A (200mA) |
| Sensor | Capacitive Soil Moisture Sensor V2.0.0 — analog voltage output |
| Switch | **KT829A** NPN Darlington transistor (TO-220). hFE ≥ 750, Vbe ≈ 1.4V. Pinout flat-side: E–C–B |

## ESP32-C3 Super Mini pinout

USB-C connector at the top. Pins numbered top → bottom.

```
              [USB-C]
         ┌─────────────┐
      5V ┤             ├ GPIO5
     GND ┤             ├ GPIO6
     3V3 ┤             ├ GPIO7
  GPIO4  ┤             ├ GPIO8   (onboard blue LED, active LOW)
  GPIO3  ┤             ├ GPIO9   (BOOT button)
  GPIO2  ┤             ├ GPIO10
  GPIO1  ┤             ├ GPIO21  ⚠ USB D+ — avoid
  GPIO0  ┤             ├ GPIO22  ⚠ USB D− — avoid
         └─────────────┘
```

ADC-capable pins (analog input): GPIO0–GPIO4 (left side, lower half)
⚠ GPIO2 is pulled to 3V3 by a strapping resistor on the Super Mini — ADC always reads 4095. Do not use for analog input.

## Pin assignments (this project)

| Signal | GPIO | Side | Notes |
|--------|------|------|-------|
| Onboard LED | GPIO8 | Right-4 | Active LOW — confirmed working |
| Moisture sensor AOUT | GPIO1 | Left-7 | ADC1 channel 1 — confirmed working |
| Moisture sensor VCC | GPIO4 | Left-4 | Driven HIGH only during measurement (saves power). 3.3V — recalibrate raw_dry/raw_wet after rewiring |
| Moisture sensor GND | GND | Left-2 | — |
| Pump control | GPIO5 | Right-1 | Output → KT829A base via 1kΩ resistor |
| Water level (reed switch) | GPIO3 | Left-5 | Input, pull-up. LOW = water present, HIGH = tank empty |
| Tank-empty warning LED | GPIO0 | Left-8 | External LED, active HIGH. 220Ω resistor in series to GND |

Calibrated ADC bounds (this board + sensor): dry=3950, wet=1360

## Software stack

- **PlatformIO** — build system & IDE integration (VS Code extension)
- **Framework** — ESP-IDF (native Espressif SDK, FreeRTOS underneath)
- **Language** — C (ESP-IDF) or C++ (Arduino)

## Build & flash commands

```bash
# Build only
pio run

# Build + flash over USB
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor

# Build + flash + monitor in one go
pio run --target upload && pio device monitor
```

## Project structure

```
src/main.c          — watering loop (app_main, ADC averaging, pump control)
src/config.h/.c     — NVS-backed config (g_config struct, load/save/print)
src/water_level.h/.c — reed switch sensor (GPIO3, pull-up)
include/            — shared headers (currently unused)
lib/                — local libraries (currently unused)
platformio.ini      — board, framework, dependencies
CLAUDE.md           — this file
```

## Architecture (target)

```
[Moisture Sensor] --analog--> [ESP32-C3 ADC] --> [Threshold logic]
                                                        |
                                               [GPIO output pin]
                                                        |
                                               [MOSFET / Relay]
                                                        |
                                               [USB Water Pump]
```

## Current status

- [x] PlatformIO project scaffold created
- [x] Framework decision: ESP-IDF chosen
- [ ] Confirm board variant pin mapping (Super Mini vs DevKit M1)
- [ ] Confirm MOSFET/relay component available
- [x] Milestone 1 — Blink onboard LED (verify toolchain works end-to-end)
- [x] Milestone 2 — Read ADC value from moisture sensor and print to serial
- [x] Milestone 3 — Toggle pump GPIO and verify pump runs
- [x] Milestone 4 — Combine: automatic watering loop with threshold
- [x] Milestone 5 — NVS config storage + ADC averaging
- [x] Milestone 5b — Water level sensor (reed switch on GPIO3, blink LED + skip pump when empty)
- [ ] Milestone 6 — Deep sleep  ← current
- [ ] Milestone 7 — Bluetooth + Android app

## Key concepts (for learning)

- **ADC** (Analog-to-Digital Converter): converts a voltage (0–3.3V) into a number (0–4095 for 12-bit). Used to read the moisture sensor.
- **GPIO** (General Purpose I/O): digital pins you can set HIGH/LOW. Used to control the pump switch.
- **MOSFET**: a transistor used as a switch — a small GPIO signal controls a larger load (the pump).
- **ESP-IDF**: Espressif's native SDK for ESP32. Lower level, more control, steeper learning curve.
- **Arduino framework**: wraps ESP-IDF in a simpler API (`setup()`/`loop()`). Better for beginners, huge community.

## Notes & decisions log

| Date | Decision | Reason |
|------|----------|--------|
| 2026-06-08 | Project started | New project, hardware in hand |
| 2026-06-08 | Chose ESP-IDF over Arduino | User wants to learn the hardware deeply, not just get things running fast |
