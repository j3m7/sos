
# Small/Secure OS

## Objective

SOS turns a Raspberry Pi Pico 2 W into a small, headless, low-power general purpose computer
that you use remotely over Wi-Fi. There is no screen or keyboard: the device sits on the
network, mostly asleep, and you connect to it to get basic tasks done.

### Core capabilities

- **Remote shell**: log in over the network to a command shell to run commands, inspect the
  system and manage files.
- **Email**: check a mailbox (list, read and delete messages) over IMAP, and send short
  messages over SMTP.
- **File transfer**: upload files to and download files from the device's storage (microSD
  card on the Adalogger).
- **Timekeeping**: keep accurate wall-clock time across power loss using the Adalogger's
  battery-backed RTC, synchronised over the network (NTP).

### Design principles

- **Secure by default**: every network connection is encrypted and authenticated; no
  plaintext logins or protocols. Credentials and keys never leave the device unencrypted.
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

- RPI Pico2W
- Pi Cowbell Adalogger - https://www.adafruit.com/product/5703

## Software Stack

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.3.1 (`~/slop/pico-sdk`)
- `arm-none-eabi-gcc` toolchain, CMake, Ninja
- `picotool` for flashing, `openocd` (Raspberry Pi fork) for SWD debugging
- [wolfSSL](https://github.com/wolfSSL/wolfssl) 5.9.4 and [wolfSSH](https://github.com/wolfSSL/wolfssh) 1.6.0
  for cryptography and the SSH protocol
- SSH server glue in `src/ssh/`, derived from [pico-sshd](https://github.com/tjko/pico-sshd)
  (see [`THIRD_PARTY.md`](THIRD_PARTY.md))

Reference documentation (datasheets, SDK manuals) lives in [`docs/`](docs/README.md).

## Setup

Large dependencies live outside the repository, next to it in `~/slop`:

```sh
cd ~/slop
git clone --depth 1 -b 2.3.1 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk.git
git clone --depth 1 -b v5.9.4-stable https://github.com/wolfSSL/wolfssl.git
git clone --depth 1 -b v1.6.0-stable https://github.com/wolfSSL/wolfssh.git
```

`PICO_SDK_PATH` is taken from the environment, falling back to `~/slop/pico-sdk`. wolfSSL and
wolfSSH are looked up in `SOS_DEPS_DIR` (default `~/slop`); override with
`-DSOS_DEPS_DIR=...` or the individual `WOLFSSL_ROOT` and `WOLFSSH_ROOT`.

Per-machine settings (Wi-Fi network, hostname, SSH user and authorized key) go in
`local.cmake`, which is git-ignored:

```sh
cp local.cmake.example local.cmake
$EDITOR local.cmake   # SOS_SSH_AUTHORIZED_KEY is the full line from ~/.ssh/id_ed25519.pub
```

These values are compiled into the firmware image, so treat `build/` outputs as secret.

## Building

```sh
cmake --preset debug          # or: release
cmake --build --preset debug  # produces build/sos.uf2 and build/sos.elf
```

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

## Remote access (SSH)

On boot SOS joins the Wi-Fi network from `local.cmake` and starts an SSH server on port 22.
The boot log on the console shows the IP address and the host key fingerprint:

```
wifi: connected, ip 192.168.1.89
sshd: listening on port 22, host key ssh-ed25519 SHA256:...
```

Check that fingerprint (also shown by the `ssh` shell command over USB) against the one your
client prints on first connection, then log in:

```sh
ssh <user>@<device-ip>
```

- Only public-key login for the single user/key in `local.cmake` is accepted (Ed25519).
- The Ed25519 host key is generated on the device at first boot and stored in the last 4 KB
  flash sector; it survives reflashing. The private key never leaves the device.
- Up to 3 concurrent sessions (`SOS_SSH_MAX_SESSIONS`). While all are in use, further
  connections are refused with an SSH disconnect message the client displays
  ("Too many connections: all 3 SSH sessions are in use, try again later"). Each session gets its own shell, separate from the others and from the USB
  console; server log messages (connections, failed logins) go to the console only.
- `help` lists commands; `ssh` shows the active sessions (yours marked `*`); `logout` or
  Ctrl-D ends the session.

## Memory layout

| Region | Use |
|---|---|
| Main RAM (512 KB) | data, heap, and the core 0 stack (top 8 KB, `SOS_CORE0_STACK_SIZE`) |
| SCRATCH_X (4 KB) | core 1 stack (`PICO_CORE1_STACK_SIZE`), as in the SDK default |
| SCRATCH_Y (4 KB) | free |
| Flash, last 4 KB sector | SSH host key |

The core 0 stack is moved out of SCRATCH_Y by the linker script overrides in `config/ld/`.
Interrupt handlers, including lwIP/CYW43 network processing, share it. The `mem` shell command
reports the stack high-water mark; an SSH login peaks at about 3.2 KB.

## License

SOS is licensed under the GNU General Public License v3.0 or later; see [`LICENSE`](LICENSE).
Code copied from other projects, the libraries linked into the firmware and their licenses are
listed in [`THIRD_PARTY.md`](THIRD_PARTY.md), including an open question about the Wi-Fi
driver's license that matters before distributing firmware images.
