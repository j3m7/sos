# Building, flashing and testing

## Software stack

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.3.1
- `arm-none-eabi-gcc` toolchain, CMake (3.13+), Ninja
- `picotool` for flashing, `openocd` (Raspberry Pi fork) for SWD debugging
- [wolfSSL](https://github.com/wolfSSL/wolfssl) 5.9.4 and [wolfSSH](https://github.com/wolfSSL/wolfssh) 1.6.0
  for cryptography, TLS and the SSH protocol
- [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
  for the SD card driver and FatFs
- [Lua](https://www.lua.org) 5.4 for scripts
- lwIP and the CYW43 Wi-Fi driver, from the Pico SDK
- SSH server glue in `src/ssh/`, derived from [pico-sshd](https://github.com/tjko/pico-sshd)
  (see [`THIRD_PARTY.md`](../THIRD_PARTY.md))

wolfSSL, wolfSSH, the SD card library and Lua are git submodules in `lib/`. The source layout
is described in [`src/README.md`](../src/README.md); datasheets and SDK manuals are listed in
[docs/README.md](README.md).

## Setup

1. **Toolchain:** install `arm-none-eabi-gcc` with newlib, CMake, Ninja and
   [`picotool`](https://github.com/raspberrypi/picotool) (2.x, for RP2350). See
   [Getting started with Raspberry Pi Pico](sdk/getting-started-with-pico.pdf) for
   per-OS instructions.

2. **Source,** including the submodules (wolfSSL, wolfSSH, the SD card library and Lua):

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

4. **Optional, for development: a provisioning file.** The firmware image holds no
   configuration (no Wi-Fi password, no keys): a board is set up on its serial console (see
   [Configuration](configuration.md#configuration)). To set one up without typing, describe it in
   `provision.conf` (git-ignored) and write that to the board after flashing (see
   [Provisioning](configuration.md#provisioning)):

   ```sh
   cp provision.conf.example provision.conf
   $EDITOR provision.conf   # ssh_key is the full line from ~/.ssh/id_ed25519.pub
   ```

To keep wolfSSL/wolfSSH outside the repository, develop in a VM, or debug over SWD, see
[`docs/development.md`](development.md).

## Building

```sh
cmake --preset debug          # or: release, minsize
cmake --build --preset debug  # produces build/sos.uf2 and build/sos.elf
```

The three presets differ in how the compiler optimises (`build/`, `build-release/` and
`build-minsize/`). Measured on 2026-11-26, on dev-1 where a board was involved:

| Preset | Compiler | Image | SSH login handshake | TLS handshake |
|---|---|---|---|---|
| `debug` (the default) | `-Og` | 1,168 KB | 260 to 285 ms | 284 ms |
| `minsize` | `-Os` | 1,050 KB | 158 to 215 ms | 278 to 281 ms |
| `release` | `-O3` | 1,386 KB | not run on a board | not run on a board |

`minsize` is 118 KB smaller than `debug` and no slower on the handshakes measured; the SSH and
SFTP hardware tests pass with it. One 2 MB SFTP download took longer (10.8 s against 8.6 s, a
single reading over Wi-Fi each). Its main-loop stack peak was a little higher (3,720 against
about 3,550 of 8,192 bytes). `debug` remains the default.

### Build features

Parts of SOS can be left out of the image at compile time
([design note](design/build-features.md)). Each is a CMake option, on by default:

| Option | What it is | Needs |
|---|---|---|
| `SOS_FEATURE_NET` | Wi-Fi, lwIP, `net`, `wifi`, `ping`, SNTP | the Wi-Fi chip (Pico 2 W) |
| `SOS_FEATURE_TLS` | TLS 1.3 client, `tls` | NET |
| `SOS_FEATURE_HTTP` | `http`, `fetch`, `man install`, `time zone install` | TLS |
| `SOS_FEATURE_NTS` | authenticated time: `nts`, `time sync`, `time server` | TLS |
| `SOS_FEATURE_SSH` | the SSH server and SFTP, `ssh`, `key` | NET |
| `SOS_FEATURE_LUA` | Lua scripts on core 1, `lua` | |

The security core (root secret, sealed configuration, console password, health checks,
watchdog) is always built. Turning on a feature whose dependency is off stops the configure
with a message saying what to turn off too; a feature left unset follows its dependencies, so
`-DSOS_FEATURE_NET=OFF` also leaves out TLS, HTTP, NTS and SSH. Each option is `AUTO` (the
default), `ON` or `OFF`, and a build directory remembers only what it was given, so turning NET
back on there brings the others back. A build directory configured before the options became
`AUTO`/`ON`/`OFF` has them stored as plain `ON`/`OFF`, which now count as asked for: configure a
fresh directory, or clear them once with `cmake -B build -U 'SOS_FEATURE_*'`.

Presets for the usual combinations, each in its own build directory:

```sh
cmake --preset offline && cmake --build --preset offline   # nothing over the air
cmake --preset pico2 && cmake --build --preset pico2       # the plain Pico 2 (no Wi-Fi chip)
cmake --preset minimal && cmake --build --preset minimal   # the security core, shell and card
```

On the board, `version` and `health` show the features line (`features: LUA (off: NET TLS HTTP
NTS SSH)`), and Lua scripts read it with `sos.features()`. `help` and Tab list only what the
image has. A Pico 2 W built without NET doesn't drive the Wi-Fi chip at all: its LED stays off,
and `power` can't read VSYS or USB power (they go through the chip).

`just features` builds every combination worth checking into `build-features/<name>` and prints
the sizes; any warning fails it. Run it before review on a change that adds an
`#if SOS_HAVE_<NAME>`.

The [`justfile`](../justfile) has shortcuts for these and the other everyday commands (tests,
flashing, provisioning, hardware tests, cleaning), if [just](https://github.com/casey/just) 1.30 or newer
is installed: `just` lists them. It is optional: each recipe runs the commands documented here.

## Flashing

```sh
cmake --build --preset debug --target flash
```

This runs `picotool load -f -v -x`, which reboots a running SOS image into BOOTSEL over USB,
loads, verifies and starts it. For a board running some other firmware, hold BOOTSEL while
plugging it in first. Flashing leaves the board's saved configuration and SSH host key
alone; a new board has neither a network nor a trusted key until it's set up
(`setup` on the console, or [provisioning](configuration.md#provisioning)).

The image can be signed as part of the build (`SOS_SIGN_KEY`; see
[Signed builds](development.md#signed-builds)). A board without secure boot runs signed
and unsigned images alike. Every image is hashed, signed or not, and the boot ROM refuses to
start one whose flash no longer matches its hash: reflash it (the board waits in the USB boot
loader).

Developing inside a VM? See [`docs/development.md`](development.md) for forwarding the
Pico's USB connection so flashing works unattended.

## Testing

### Unit tests

Unit tests run on the development machine with the host compiler (no board, no Pico SDK),
under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

They cover:

- **Shell:** line editing, questions and output (`src/shell/shell_core.c`, with the commands
  faked), terminal key decoding (`src/shell/keys.c`), character widths (`src/shell/charwidth.c`),
  and the filter that keeps outside text from sending the terminal escape sequences
  (`src/shell/termtext.c`).
- **Editor:** its text, keys and screen (`src/edit/editor.c`).
- **Messages:** `wall`, `chat` and `who` between shells (`src/chat/chat.c`).
- **SSH:** the session I/O ring buffer (`src/ssh/pico-sshd/ringbuffer.c`, including a randomized
  comparison against a reference model).
- **Storage:** the configuration store (`src/core/config.c`, with a fake flash that can fail or
  lose power mid-write), path handling for the file commands (`src/files/path.c`), and SD card
  encryption (`src/files/sdcrypt.c`, with wolfCrypt built for the host and cross-checked against
  `tools/host/sdcrypt.c`, which uses OpenSSL's libcrypto; that suite needs the wolfSSL source and
  OpenSSL's development files, and is skipped with a warning without them).
- **Time:** date and RTC register conversions (`src/time/timeconv.c`, compared against the host C
  library), the NTS key-exchange messages and NTS-protected NTP packets (`src/time/nts_ke.c`,
  `src/time/nts_ntp.c`).
- **TLS and HTTPS:** the root certificate bundle parser (`src/tls/certbundle.c`), which new TLS
  connections may open (`src/tls/tls_admit.c`), and the HTTPS client's URLs, requests and
  response parser (`src/net/http_msg.c`, including corrupted responses fed in random pieces).
- **Other:** the log (`src/core/log.c`) and the provisioning tool (`tools/host/mkconfig.c`).

`tests/test.h` is a minimal harness; add a suite as `tests/test_<name>.c` and register it in
`tests/CMakeLists.txt`.

Host tests use the host's C library, so they can't catch a difference in the board's. In
particular, on the board SOS's own code gets the Pico SDK's lightweight `snprintf`, which
prints doubles with about 10 digits. The Lua sources get the C library's instead, through
`-Dsnprintf=__real_snprintf` in `CMakeLists.txt`. An SOS file whose output must match Lua's
number text (`tostring`) needs the same definition. `src/lua/lua_json.c` has it, as a
`COMPILE_DEFINITIONS` entry, because the firmware warnings overwrite every SOS source's
`COMPILE_OPTIONS`. Such a file also needs a check on the board against `tostring`: the host
tests pass either way.

### Hardware tests

Hardware tests run against a board that is flashed with SOS and on the network:

```sh
tools/test/ssh-hw-test.sh -u <user> <device-ip>     # -q for a quick run, -h for options
tools/test/sftp-hw-test.sh -u <user> <device-ip>    # needs a formatted SD card
```

`tools/test/ssh-hw-test.sh` checks the SSH server. It covers login and logout, authentication
(only public key; unknown key and password refused; any user name accepted, shown without
control characters), concurrent sessions kept
separate, the "too many connections" message, slot reuse, a large input burst, clients that
disappear, back-to-back logins, and heap use before and after. It exits with the number of failed checks. It uses every session slot for a few seconds,
so run it while nobody else is using the board; it warns if the device rebooted mid-run.

`tools/test/sftp-hw-test.sh` checks SFTP on the SD card. It puts and gets a 2 MB random file
and a small one, overwrites a file with a smaller one, resumes both ways (`reget`, `reput`), and
checks that a failed rename or rmdir is reported. During a download it checks that the card
is reported as in use and that removing the file is refused, then kills the client and checks
that the file is released. It ends by comparing heap use before and after. It works in
`/sftp-hw-test` (removed at the end) and exits with the number of failed checks. Run it while
nobody else is using the board.
