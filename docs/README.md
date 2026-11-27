# Reference Documentation

The guides to using and building SOS (building, configuration, remote access, files, time, TLS,
the editor, Lua, logging, securing a board) are listed in the
[Documentation index](../README.md#documentation) of the top-level README. This page lists the
datasheets and manuals.

For development environment options (dependencies outside the repository, developing in a VM,
SWD debugging), see [development.md](development.md).

Design notes for work that is decided but not yet built live in [design/](design/)
(e.g. [shell-pipes.md](design/shell-pipes.md), #182; [health.md](design/health.md), #233).

Datasheets and manuals for the SOS hardware (Raspberry Pi Pico 2 W + Adafruit PiCowbell Adalogger).
Downloaded 2026-11-23.

## rp2350/ — the microcontroller
| File | Description | Source |
|---|---|---|
| rp2350-datasheet.pdf | Full RP2350 datasheet: memory map, bootrom, Cortex-M33 and Hazard3 RISC-V cores, security (TrustZone, OTP, secure boot), all peripherals and registers | https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf |
| rp2350-product-brief.pdf | Short overview of the RP2350 | https://datasheets.raspberrypi.com/rp2350/rp2350-product-brief.pdf |
| hardware-design-with-rp2350.pdf | Guide to designing boards with the RP2350 | https://datasheets.raspberrypi.com/rp2350/hardware-design-with-rp2350.pdf |

## pico2w/ — the board
| File | Description | Source |
|---|---|---|
| pico-2-w-datasheet.pdf | Pico 2 W board datasheet: power, GPIO, wireless (the CYW43439 chip, connected over SPI) | https://datasheets.raspberrypi.com/picow/pico-2-w-datasheet.pdf |
| pico-2-w-schematic.pdf | Board schematic | https://datasheets.raspberrypi.com/picow/pico-2-w-schematic.pdf |
| pico-2-w-pinout.pdf | Pinout diagram | https://datasheets.raspberrypi.com/picow/pico-2-w-pinout.pdf |
| connecting-to-the-internet-with-pico-w.pdf | Wi-Fi and Bluetooth with the CYW43 driver, lwIP and BTstack | https://datasheets.raspberrypi.com/picow/connecting-to-the-internet-with-pico-w.pdf |

## boards/ — other boards (candidates, not the current hardware)
| File | Description | Source |
|---|---|---|
| pimoroni_pico_plus2/Pimoroni_Pico_Plus_2_W_Schematic.pdf | Pimoroni Pico Plus 2 W schematic (title block reads "Pico Plus 2 (PIM726)", dated 2024-09-26, but shows the RM2 Wi-Fi module): RP2350B, 16 MB flash, 8 MB PSRAM on GPIO 47, AP7366 LDO for 3V3, VBUS→VSYS Schottky diode, VBUS sense on the Wi-Fi chip's GPIO 2, VSYS sense on GPIO 43, SWD connector. Evaluated 2026-11-25 as a production candidate; Pimoroni's stock was then A2 silicon | https://cdn-shop.adafruit.com/product-files/6243/Pimoroni_Pico_Plus_2_W_Schematic.pdf (Adafruit's product files, downloaded 2026-11-25) |
| pimoroni_lipo_shim/lipo_shim_for_pico_schematic.pdf | Pimoroni LiPo SHIM for Pico (PIM557) schematic, dated 2021-04-16: uses only VBUS, VSYS, 3V3_EN, 3V3 and GND (no GPIO, so no clash with the Adalogger); MCP73831 charger at about 220 mA, XB6096I2S battery protection, P-FETs that switch VSYS to the battery when USB goes, a toggle power button on 3V3_EN, an always-on white power LED | https://cdn.shopify.com/s/files/1/0174/1800/files/lipo_shim_for_pico_schematic.pdf?v=1618573454 (downloaded 2026-11-25) |

## sdk/ — software
| File | Description | Source |
|---|---|---|
| getting-started-with-pico.pdf | Toolchain setup, building, flashing, debugging (SWD/OpenOCD) | https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf |
| raspberry-pi-pico-c-sdk.pdf | Pico C/C++ SDK reference | https://datasheets.raspberrypi.com/pico/raspberry-pi-pico-c-sdk.pdf |
| raspberry-pi-pico-python-sdk.pdf | MicroPython on Pico | https://datasheets.raspberrypi.com/pico/raspberry-pi-pico-python-sdk.pdf |

## cpu/ — Arm Cortex-M33 core
| File | Description | Source |
|---|---|---|
| cortex-m33-trm.pdf | Cortex-M33 Technical Reference Manual (100230, issue 08) | https://documentation-service.arm.com/static/66be1ce8882fec713ef49954 |
| cortex-m33-devices-generic-user-guide.pdf | Programmer's model, instruction set, NVIC, MPU, SysTick, SAU (100235) | https://documentation-service.arm.com/static/641b2ec1de84571cc27f3ca3 |

The Hazard3 RISC-V core is documented in the RP2350 datasheet (Processor subsystem chapter).

## peripherals/
| File | Description | Source |
|---|---|---|
| adafruit-picowbell-adalogger-for-pico.pdf | Adafruit learn guide for the PiCowbell Adalogger: pinout and usage of the PCF8523 RTC and microSD slot | https://learn.adafruit.com/adafruit-picowbell-adalogger-for-pico |
| PCF8523-datasheet.pdf | NXP PCF8523 real-time clock (I2C) | Adafruit-hosted copy |
| adalogger-pcb/Adafruit PiCowbell Adalogger.sch | Eagle schematic | https://github.com/adafruit/Adafruit-PiCowbell-Adalogger-PCB |
| CYW43439-datasheet.pdf | Infineon/Cypress CYW43439 Wi-Fi/Bluetooth chip (002-30348 Rev *B; newer revisions may exist) | https://www.farnell.com/datasheets/3758304.pdf |

## Not included (registration or click-through required)
- Armv8-M Architecture Reference Manual (Arm DDI0553): https://developer.arm.com/documentation/ddi0553/latest
- SD Physical Layer Simplified Specification (SPI mode): https://www.sdcard.org/downloads/pls/
- Pico SDK API docs (HTML): https://www.raspberrypi.com/documentation/pico-sdk/
