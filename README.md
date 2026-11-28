# Small/Secure OS

## Objective

SOS turns a Raspberry Pi Pico 2 W into a small, headless, low-power general purpose computer
that you use remotely over Wi-Fi. There is no screen or keyboard: the device sits on the
network, mostly asleep, and you connect to it to get basic tasks done.

### Core capabilities

- **Remote shell**: log in over the network to a command shell to run commands, inspect the
  system and manage files.
- **Communication**: tools for sending and receiving messages with people and other systems
  over standard messaging protocols, encrypted and authenticated like every other connection.
- **File transfer**: upload files to and download files from the device's storage (microSD
  card on the Adalogger).
- **Timekeeping**: keep accurate wall-clock time across power loss using the Adalogger's
  battery-backed RTC, synchronised over the network with authenticated NTP (NTS).

### Design principles

- **Secure by default**: every network connection is encrypted and authenticated; no
  plaintext logins or protocols. Credentials and keys leave the device unencrypted only on the
  serial console (an encrypted SD card's recovery key, shown once when the card is formatted).
- **Low power**: idle in low-power sleep, wake on network activity or timers, use Wi-Fi power
  saving, and do as little work as possible while nobody is connected.
- **Headless**: fully usable and recoverable without a screen; the USB serial console is for
  development and recovery only.
- **Small**: fit comfortably in the RP2350's 520 KB of RAM and 4 MB of flash, with bulk data
  on the microSD card. Prefer a few well-chosen features over many partial ones.

### Non-goals

- Graphical interfaces, displays or local input devices.
- Running unmodified Linux/POSIX software.
- Serving many users or many simultaneous connections.

## Hardware

- Raspberry Pi Pico 2 W
- Adafruit [PiCowbell Adalogger](https://www.adafruit.com/product/5703) (RTC and microSD slot)

## Quick start

You need `arm-none-eabi-gcc` with newlib, CMake, Ninja, `picotool` 2.x and a Pico SDK 2.3.1
checkout in `PICO_SDK_PATH` ([details](docs/building.md#setup)).

```sh
git clone --recurse-submodules <repository-url> sos && cd sos
cmake --preset debug                         # or: release, minsize
cmake --build --preset debug                 # build/sos.uf2 and build/sos.elf
cmake --build --preset debug --target flash  # load it onto the board over USB
picocom -b 115200 /dev/ttyACM0               # the serial console
```

On the console of a new board, `setup` asks for a console password, the Wi-Fi network and your
SSH public key ([Configuration](docs/configuration.md#configuration)). Once the board is on the
network, the log shows its address and SSH host key fingerprint, and you can log in:

```sh
ssh <device-ip>
```

`help` lists the commands. `man <command>` explains one once the manual pages are on the SD
card: `man install` fetches them ([manual pages](docs/editor.md#manual-pages)). If [just](https://github.com/casey/just) is installed, `just` lists shortcuts for building,
flashing, provisioning and testing.

## Documentation

| Topic | |
|---|---|
| [Building, flashing and testing](docs/building.md) | software stack, setup, build presets, flashing, unit and hardware tests |
| [Console and configuration](docs/configuration.md) | the serial console, `setup`, Wi-Fi and keys, the console password, provisioning a board from a file |
| [Command reference](docs/commands.md) | every shell command, and which work only on the console |
| [Remote access](docs/remote-access.md) | the SSH server, messages and chat between sessions |
| [Files (SD card)](docs/files.md) | file commands, encrypted cards, SFTP |
| [Timekeeping](docs/time.md) | the RTC, authenticated network time (NTS), RTC calibration |
| [TLS and HTTPS](docs/tls.md) | trusted root certificates, the HTTPS client, downloading files (`fetch`) |
| [Editor, pager and manual pages](docs/editor.md) | `edit`, `more` and `\| more`, `man` and installing the pages |
| [Lua scripts](docs/lua.md) | running scripts, autorun, the `sos` library, modules, HTTPS from Lua |
| [Logging, watchdog, temperature and memory](docs/system.md) | the system log and log files, the watchdog and crash reports, chip temperature calibration, memory layout |
| [Securing a board](docs/security/README.md) | the root secret in OTP and secure boot: design, decisions, runbooks |
| [Development environment](docs/development.md) | dependencies outside the repository, VMs, SWD debugging, signed builds, the flash guard |
| [Reference documentation](docs/README.md) | datasheets and manuals for the hardware and SDK |

The source code is grouped by subsystem under `src/`; [`src/README.md`](src/README.md) gives
the overview (main loop, interrupt context, core 1) and each directory's README explains how
its subsystem works. The manual pages are written in [`man/`](man/README.md).

## License

SOS is licensed under the GNU General Public License v3.0 or later; see [`LICENSE`](LICENSE).
Code copied from other projects, the libraries linked into the firmware and their licenses are
listed in [`THIRD_PARTY.md`](THIRD_PARTY.md), including an open question about the Wi-Fi
driver's license that matters before distributing firmware images.
