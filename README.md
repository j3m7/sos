
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

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.3.1
- `arm-none-eabi-gcc` toolchain, CMake (3.13+), Ninja
- `picotool` for flashing, `openocd` (Raspberry Pi fork) for SWD debugging
- [wolfSSL](https://github.com/wolfSSL/wolfssl) 5.9.4 and [wolfSSH](https://github.com/wolfSSL/wolfssh) 1.6.0
  for cryptography and the SSH protocol (git submodules in `lib/`)
- SSH server glue in `src/ssh/`, derived from [pico-sshd](https://github.com/tjko/pico-sshd)
  (see [`THIRD_PARTY.md`](THIRD_PARTY.md))

Reference documentation (datasheets, SDK manuals) lives in [`docs/`](docs/README.md).

## Setup

1. **Toolchain:** install `arm-none-eabi-gcc` with newlib, CMake, Ninja and
   [`picotool`](https://github.com/raspberrypi/picotool) (2.x, for RP2350). See
   [Getting started with Raspberry Pi Pico](docs/sdk/getting-started-with-pico.pdf) for
   per-OS instructions.

2. **Source,** including the wolfSSL and wolfSSH submodules:

   ```sh
   git clone --recurse-submodules <repository-url> sos
   # or, in an existing clone:
   git submodule update --init
   ```

3. **Pico SDK:** point `PICO_SDK_PATH` at a Pico SDK 2.3.1 checkout (with its submodules), or
   let the build download it by configuring with
   `-DPICO_SDK_FETCH_FROM_GIT=ON -DPICO_SDK_FETCH_FROM_GIT_TAG=2.3.1`.

   ```sh
   git clone -b 2.3.1 --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
   export PICO_SDK_PATH=$PWD/pico-sdk
   ```

4. **Optional build-time defaults:** the device is configured over the serial console (see
   [Configuration](#configuration)). For development, `local.cmake` (git-ignored) can provide
   first-boot defaults for the Wi-Fi network, hostname, SSH user and one trusted key, so a
   freshly flashed board comes up configured:

   ```sh
   cp local.cmake.example local.cmake
   $EDITOR local.cmake   # SOS_SSH_AUTHORIZED_KEY is the full line from ~/.ssh/id_ed25519.pub
   ```

   Defaults that are set are compiled into the firmware image, so treat `build/` outputs as
   secret. Without `local.cmake` the image contains no Wi-Fi password or keys.

To keep wolfSSL/wolfSSH outside the repository, develop in a VM, or debug over SWD, see
[`docs/development.md`](docs/development.md).

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

Developing inside a VM? See [`docs/development.md`](docs/development.md) for forwarding the
Pico's USB connection so flashing works unattended.

## Console

The console runs over USB CDC (`/dev/ttyACM0` on Linux) and is mirrored on UART0 (GP0 TX / GP1 RX):

```sh
picocom -b 115200 /dev/ttyACM0
```

## Configuration

The Wi-Fi network, hostname, SSH user and trusted public keys are set on the serial console
(USB or UART) and saved in flash. On a new board, connect to the console and run `setup`:

```
sos> setup
Wi-Fi network (SSID): my-network
Wi-Fi password (empty for an open network):        (not echoed)
SSH user name [admin]: julio
Public key: ssh-ed25519 AAAAC3Nza... julio@laptop    (paste ~/.ssh/id_ed25519.pub)
```

| Command | |
|---|---|
| `config` | show the configuration |
| `wifi` / `wifi set` / `wifi connect` | show / change the network (password not echoed) / reconnect |
| `key list` / `key add [line]` / `key del <n>` | trusted keys (ssh-ed25519, ecdsa-sha2-nistp256; up to 8) |
| `hostname <name>`, `user <name>` | hostname (applies at the next Wi-Fi connect), SSH user name |
| `config reset` | erase everything (back to the build-time defaults, if any) |

- **Changes only on the console.** SSH sessions can view the configuration but not change it,
  so a remote session can't lock itself out; physical access to the console is the trust
  boundary (it already allows reflashing).
- Changes apply immediately: a new key works for the next login, `wifi set` reconnects.
- Wi-Fi connects in the background, retries after failures (wrong password, network not
  found) and reconnects if the link drops; the console is usable throughout.
- Storage: one record in two alternating flash sectors below the host key, with a sequence
  number and CRC, so a power cut while saving keeps the previous version. It survives
  reflashing. The Wi-Fi password and keys are stored unencrypted, like the build-time defaults.

## Logging

Boot, Wi-Fi, configuration and SSH server messages go to the system log:

- The serial console prints them as they happen. A message arriving while you type clears your
  input line, prints, and redraws the prompt and what you typed (a hidden password isn't
  redrawn).
- `dmesg` shows the last 4 KB of messages in any shell, SSH sessions included.
- `log console <level>` sets what the console prints (default `notice`); `log history <level>`
  what's kept for `dmesg` (default `info`). Levels: err, warning, notice, info, debug.

## Remote access (SSH)

Once the device has a Wi-Fi network and at least one trusted key, it starts an SSH server on
port 22. The log shows the IP address and the host key fingerprint:

```
wifi: connected, ip 192.168.1.89
sshd: listening on port 22, host key ssh-ed25519 SHA256:...
```

Check that fingerprint (also shown by the `ssh` shell command over USB) against the one your
client prints on first connection, then log in:

```sh
ssh <user>@<device-ip>
```

- Only public-key login is accepted: the configured user with one of the trusted keys.
- The Ed25519 host key is generated on the device at first boot and stored in the last 4 KB
  flash sector; it survives reflashing. The private key never leaves the device.
- Up to 3 concurrent sessions (`SOS_SSH_MAX_SESSIONS`). While all are in use, further
  connections are refused with an SSH disconnect message the client displays
  ("Too many connections: all 3 SSH sessions are in use, try again later"). Each session gets
  its own shell, separate from the others and from the USB console.
- `help` lists commands; `ssh` shows the active sessions (yours marked `*`); `logout` or
  Ctrl-D ends the session.

## Watchdog

SOS enables the RP2350 hardware watchdog right after the boot banner. Everything runs from the
main loop, which feeds it on every pass; if the loop stops coming round for `SOS_WATCHDOG_MS`
(default 8000 ms), the chip resets. A hang during boot therefore resets and retries too.
Nothing blocks the loop for long: Wi-Fi connects in the background and console questions
(`setup`, passwords) wait for input without stopping it.

- After a watchdog reset the boot log shows `last reset: watchdog timeout` and `uptime`
  appends `(last reset: watchdog)`. Power-on and the `reboot` command don't count.
- `ssh` shows the longest single SSH work step (`longest ... ms`), a guide to how much margin
  the timeout has.
- Set the timeout when configuring, e.g. `cmake --preset debug -DSOS_WATCHDOG_MS=12000`; the
  RP2350 maximum is 16777 ms and `0` disables the watchdog. It pauses while a debugger halts
  the core.

## Testing

**Unit tests** run on the development machine with the host compiler (no board, no Pico SDK),
under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

They cover the SSH I/O ring buffer (`src/ssh/ringbuffer.c`, including a randomized comparison
against a reference model), the shell's line editing, questions and output (`src/shell_core.c`,
with the commands faked), the configuration store (`src/config.c`, with a fake flash that can
fail or lose power mid-write) and the log (`src/log.c`). `tests/test.h` is a minimal harness; add a suite as
`tests/test_<name>.c` and register it in `tests/CMakeLists.txt`.

**Hardware test:** `tools/test/ssh-hw-test.sh` checks the SSH server on a board that is flashed
with SOS and on the network:

```sh
tools/test/ssh-hw-test.sh -u <user> <device-ip>     # -q for a quick run, -h for options
```

It covers login and logout, authentication (only public key; unknown key, wrong user and
password refused), concurrent sessions kept separate, the "too many connections" message, slot
reuse, a large input burst, clients that disappear, back-to-back logins, and heap use before and
after. It exits with the number of failed checks. It uses every session slot for a few seconds,
so run it while nobody else is using the board; it warns if the device rebooted mid-run.

## Memory layout

| Region | Use |
|---|---|
| Main RAM (512 KB) | data, heap, and the core 0 stack (top 8 KB, `SOS_CORE0_STACK_SIZE`) |
| SCRATCH_X (4 KB) | core 1 stack (`PICO_CORE1_STACK_SIZE`), as in the SDK default |
| SCRATCH_Y (4 KB) | free |
| Flash, last 4 KB sector | SSH host key |
| Flash, 2 sectors below it | configuration (A/B records) |

The core 0 stack is moved out of SCRATCH_Y by the linker script overrides in `config/ld/`.
Interrupt handlers, including lwIP/CYW43 network processing, share it. The `mem` shell command
reports the stack high-water mark; an SSH login peaks at about 3.2 KB.

## License

SOS is licensed under the GNU General Public License v3.0 or later; see [`LICENSE`](LICENSE).
Code copied from other projects, the libraries linked into the firmware and their licenses are
listed in [`THIRD_PARTY.md`](THIRD_PARTY.md), including an open question about the Wi-Fi
driver's license that matters before distributing firmware images.
