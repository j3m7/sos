
# Small/Secure OS

## Features

## Hardware

- RPI Pico2W
- Pi Cowbell Adalogger - https://www.adafruit.com/product/5703

## Software Stack

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.3.1 (`~/slop/pico-sdk`)
- `arm-none-eabi-gcc` toolchain, CMake, Ninja
- `picotool` for flashing, `openocd` (Raspberry Pi fork) for SWD debugging

Reference documentation (datasheets, SDK manuals) lives in [`docs/`](docs/README.md).

## Building

```sh
cmake --preset debug          # or: release
cmake --build --preset debug  # produces build/sos.uf2 and build/sos.elf
```

`PICO_SDK_PATH` is taken from the environment, falling back to `~/slop/pico-sdk`.

## Flashing

```sh
cmake --build --preset debug --target flash
```

This runs `picotool load -f -v -x`, which reboots a running SOS image into BOOTSEL over USB,
loads, verifies and starts it. For a board running some other firmware, hold BOOTSEL while
plugging it in first.

Developing inside a libvirt VM? Install the host-side udev hotplug forwarding in
[`tools/host/`](tools/host/README.md) so the Pico follows its reboots into and out of BOOTSEL
without manual USB redirection.

## Console

The console runs over USB CDC (`/dev/ttyACM0` on Linux) and is mirrored on UART0 (GP0 TX / GP1 RX):

```sh
picocom -b 115200 /dev/ttyACM0
```

