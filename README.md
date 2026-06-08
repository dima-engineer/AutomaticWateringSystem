# Auto Watering System

ESP32-C3 firmware that reads soil moisture and automatically runs a USB water pump when the soil is dry. Built with ESP-IDF on a Super Mini board.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-C3 Super Mini |
| Pump | USB water pump — 5V, 0.2A |
| Sensor | Capacitive Soil Moisture Sensor V2.0.0 |
| Switch | KT829A NPN Darlington transistor (TO-220) |

## Wiring

```
              [USB-C]
         ┌─────────────┐
      5V ┤             ├ GPIO5  → pump control (via 1kΩ → KT829A base)
     GND ┤             ├ GPIO6
     3V3 ┤             ├ GPIO7
  GPIO4  ┤             ├ GPIO8  (onboard LED, active LOW)
  GPIO3  ┤             ├ GPIO9  (BOOT button)
  GPIO2  ┤             ├ GPIO10
  GPIO1  ┤             ├ GPIO21 ⚠ USB D+
  GPIO0  ┤             ├ GPIO22 ⚠ USB D−
         └─────────────┘
```

| Signal | GPIO | Notes |
|--------|------|-------|
| Moisture sensor AOUT | GPIO1 | ADC input |
| Moisture sensor VCC | GPIO4 | Powered only during measurement |
| Moisture sensor GND | GND | — |
| Reed switch (water level) | GPIO3 | Pull-up. LOW = water present, HIGH = tank empty |
| Pump control | GPIO5 | Output → KT829A base via 1kΩ |
| Tank-empty warning LED | GPIO0 | Active HIGH, 220Ω in series to GND |

**Pump circuit:** KT829A Collector → pump (−), Emitter → GND, 5V → pump (+). Add a flyback diode across the pump terminals (stripe toward 5V).

## How it works

On each wake from deep sleep the firmware:

1. Reads soil moisture (average of 16 ADC samples, sensor powered only during measurement)
2. Checks the water level reed switch
3. If the tank is empty — turns on the warning LED, sleeps 30 s, repeats
4. If moisture is below the threshold — runs the pump for `pump_duration_ms`, then sleeps for `pump_cooldown_ms`
5. Otherwise — sleeps for `check_interval_ms`

GPIO output pins are held LOW during deep sleep so the pump cannot accidentally activate.

## Configuration

Settings are stored in NVS (survives reboot) and loaded on startup. Defaults:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `threshold_pct` | 30% | Water when moisture drops below this |
| `raw_dry` | 3950 | ADC reading in open air (calibrate per board) |
| `raw_wet` | 1360 | ADC reading fully submerged (calibrate per board) |
| `pump_duration_ms` | 3000 | How long to run the pump each cycle |
| `pump_cooldown_ms` | 300000 | Wait after watering before next check (5 min) |
| `check_interval_ms` | 1800000 | Check interval when soil is OK (30 min) |

To recalibrate `raw_dry`/`raw_wet`, read the raw ADC values printed on serial with the sensor in air and fully submerged, then update the config.

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
# Build only
pio run

# Build + flash
pio run --target upload

# Serial monitor (115200 baud)
pio device monitor

# Flash + monitor in one step
pio run --target upload && pio device monitor
```

## Project structure

```
src/main.c          — app_main, ADC averaging, pump control, sleep
src/config.h/.c     — NVS-backed config (g_config struct, load/save/print)
src/water_level.h/.c — reed switch sensor
platformio.ini      — board, framework, build flags
```

## Roadmap

- [x] Blink onboard LED
- [x] Read moisture sensor over ADC
- [x] Control pump via GPIO
- [x] Automatic watering loop with threshold
- [x] NVS config + ADC averaging
- [x] Water level sensor with warning LED
- [x] Deep sleep between checks
- [ ] Bluetooth configuration via Android app
