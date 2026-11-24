
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
- Pi Cowbell Adalogger - https://www.adafruit.com/product/5703 (RTC and microSD slot)

## Software Stack

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
  (see [`THIRD_PARTY.md`](THIRD_PARTY.md))

wolfSSL, wolfSSH, the SD card library and Lua are git submodules in `lib/`.

The source code is grouped by subsystem under `src/`; [`src/README.md`](src/README.md) gives
the overview (main loop, interrupt context, core 1) and each directory's README explains how
its subsystem works. Reference documentation (datasheets, SDK manuals) lives in
[`docs/`](docs/README.md).

## Setup

1. **Toolchain:** install `arm-none-eabi-gcc` with newlib, CMake, Ninja and
   [`picotool`](https://github.com/raspberrypi/picotool) (2.x, for RP2350). See
   [Getting started with Raspberry Pi Pico](docs/sdk/getting-started-with-pico.pdf) for
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
| `time auto <hours>` / `time auto off` | periodic NTS sync (default off; see [Timekeeping](#timekeeping)) |
| `time server <name>` / `time server default` | NTS server: host name or IPv4 address / back to `SOS_NTS_SERVER` |
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
  The record format only grows at the end: a record saved by older firmware still loads, with
  newer settings at their defaults. Firmware older than a saved record ignores it and starts
  from the build-time defaults.

## Logging

Boot, Wi-Fi, configuration and SSH server messages go to the system log:

- The serial console prints them as they happen. A message arriving while you type clears your
  input line, prints, and redraws the prompt and what you typed (a hidden password isn't
  redrawn).
- `dmesg` shows the last 4 KB of messages in any shell, SSH sessions included.
- `log console <level>` sets what the console prints (default `notice`); `log history <level>`
  what's kept for `dmesg` (default `info`). Levels: err, warning, notice, info, debug. Console
  only to change; `log` shows the levels in any shell.
- Only printable ASCII is logged: other bytes in quoted text (a user name, a file name) become
  spaces or `?`.

## Remote access (SSH)

Once the device has a Wi-Fi network and at least one trusted key, it starts an SSH server on
port 22. The log shows the IP address and the host key fingerprint:

```
wifi: connected, ip 192.168.1.89
sshd: listening on port 22 (up to 3 sessions), host key ssh-ed25519 SHA256:...
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

## Timekeeping

The Adalogger's PCF8523 RTC (I2C0: SDA GP4, SCL GP5), kept running by its CR1220 coin cell,
is the source of truth for wall-clock time. At boot SOS sets the system clock (`time()`,
`gettimeofday()`) from it and turns on the chip's battery switch-over, which is off by default.
All times are UTC.

Network time comes from **NTS** (Network Time Security, RFC 8915): NTP whose replies are
authenticated. SOS does a TLS key exchange with the NTS server (TCP port 4460), then sends
NTP requests that carry a single-use cookie and an AES-SIV tag, and accepts only replies that
check out under the server's key. Each sync sends 4 queries and uses the one with the shortest
round trip, which keeps Wi-Fi power-saving delays out of the result. The server is set on the
console with `time server <name>`; the default is the build-time `SOS_NTS_SERVER`
(`time.cloudflare.com`). The server's certificate chain must end at a trusted root: built in
(`src/tls/tls_roots.c`: SSL.com Root Certification Authority ECC for Cloudflare, ISRG Root X1 for
Let's Encrypt, which most other NTS servers use) or approved from the SD card (see
[Trusted root certificates](#trusted-root-certificates)).

SOS syncs in three cases:

- **The RTC has no valid time:** no chip, a first power-up, or a coin cell that ran out while
  the board was unpowered. A failed sync is retried after 30 s, then with doubling delays up to
  30 minutes, until one works.
- **`time sync`**, typed in any shell, SSH included.
- **Periodically, if turned on** with `time auto <hours>` on the console (1 to 720; default off).
  It syncs once the network is up (unless the clock was synced since boot), then one interval
  after the last successful sync, whatever started it. This corrects RTC drift, which is up to
  about 10 s a week. Without it, run `time sync` now and then.

A successful sync sets the system clock and the RTC. NTS time is authenticated, so a sync may
move the clock any distance; a correction of more than 5 minutes is logged as a warning. Any
time earlier than the firmware's configure time is rejected.

While the clock isn't set, certificate dates can't be checked, so that first key exchange
accepts certificates whatever their dates (signatures and the host name are still checked) and
logs `certificate dates not checked (clock not set)`. Once the clock is set, dates are checked
as usual.

There is no automatic fallback to plain, unauthenticated NTP: if the NTS server can't be
reached (for example because the network blocks port 4460) and the RTC has no time, the clock
stays unset until NTS works. `time sync sntp` (console only) does one plain SNTP sync on request,
from the build-time `SOS_NTP_SERVER` (`pool.ntp.org`); `time` then shows the time as
unauthenticated.

- `time` shows the date and time and where it came from (RTC, NTS, or SNTP), the RTC's reading
  and backup-battery state, the last sync with its correction and round trip, a failed attempt
  and when it's retried, and the server and periodic settings.
- `time auto` and `time server` (without a value) show those settings; `config` lists both.
- `nts` shows the NTS state (cookies, last key exchange and query). `nts ke [host]` and
  `nts query` run the two steps by hand, for diagnosis; they don't set the clock. Naming another
  host is console only.

## Files (SD card)

The Adalogger's microSD slot (SPI0: SCK GP18, MOSI GP19, MISO GP16, CS GP17, at 25 MHz) holds
the device's files. Cards over 2 GB (SDHC/SDXC) with exFAT or FAT32 work; the card is mounted
when first used. The file commands work in every shell, SSH included, and each shell has its own
working directory:

| Command | |
|---|---|
| `ls [path]`, `cd [dir]`, `pwd` | list, change and show the working directory |
| `cat <file>` | print a file |
| `cp <from> <to>`, `mv <from> <to>` | copy, move or rename (to a directory: keeps the name) |
| `rm <path>`, `mkdir <dir>` | delete a file or an empty directory, create a directory |
| `echo <text> [> file \| >> file]` | print, or write/append a line |
| `sd` | card and volume information, SPI clock |
| `sd format [exfat\|fat32]` | console only: erase the card and format it (one partition) |
| `sd test [KB [keep]]`, `sd speed [MHz]` | console only: write/read benchmark (everything else waits meanwhile); SPI clock |
| `sd unmount` | unmount the card, to remove it |

- **Long commands run in steps.** `ls`, `cat` and `cp` do a little work per pass of the main
  loop, so SSH sessions, the console and the watchdog keep going; Ctrl-C stops them (a stopped
  `cp` removes its partial copy). Commands typed meanwhile run afterwards.
- **A slow SSH client** slows `cat` down instead of losing output. Printing a file over SSH runs
  at about 130 KB/s (encryption-bound); copying on the card at about 580 KB/s.
- **Open files are protected:** a file that a running command has open can't be deleted or
  renamed, and the card can't be formatted or unmounted meanwhile.
- File names with spaces aren't supported by the commands yet. Files are dated with the system
  clock (UTC), which comes from the RTC or NTS.
- No card-detect switch is wired by default (solder jumper SJ1 connects it to GP15), so SOS
  doesn't notice a card swap: run `sd unmount` before taking the card out; the next file command
  initializes and mounts the new card.

### File transfer (SFTP)

The SSH server also serves SFTP on the card, with the same login:

```sh
sftp <user>@<device-ip>            # or scp -s, FileZilla, WinSCP ...
sftp> put photo.jpg
sftp> get notes.txt
```

- Upload, download (resumable: `reput`, `reget`), `ls`, `mkdir`, `rmdir`, `rm`, `rename`. The root
  is the card's root. Changing attributes (`chmod`, times) isn't supported: FAT has no Unix
  permissions.
- About 170 KB/s up and 120 KB/s down (2 MB files; encryption-bound). Each request is handled in
  one pass of the main loop: a 32 KB read reply holds the console and other sessions for up to
  ~0.1 s.
- Each session uses up to ~100 KB of heap while transferring. A transfer's open files count as in
  use, like a running `cp` (see above); a client that disconnects mid-transfer releases them.
- Every change to the card, from SFTP or the file commands, passes one check,
  `files_write_allowed()` in `src/files/files.c`. Everything is allowed except changing `/certs`, which
  only the console may do (below).

### Trusted root certificates

TLS (used by NTS) trusts the built-in roots and, optionally, extra ones from the card in
`/certs/roots.pem`: a PEM bundle (text between the certificates, as in system CA files, is fine)
or DER certificates, up to 64 KB and 32 certificates. The file is used only while it's exactly the
file approved on the console:

1. Upload it anywhere with SFTP, e.g. `put roots.pem incoming/roots.pem`.
2. On the console: `mkdir /certs` (once), then `mv /incoming/roots.pem /certs/roots.pem`.
3. On the console: `tls roots approve`. It lists each certificate (common name, key type, whether
   it's a CA, expiry, SHA-256 fingerprint) and checks TLS can use it; type `yes` to trust them.
   The file's SHA-256 is saved with the configuration.

- **Only the console can change `/certs`.** SSH shells and SFTP can read it but not write,
  delete or rename anything in it, under any spelling FatFs accepts (`/CERTS.`, `\certs`,
  `/x/../certs`).
- **A changed file isn't used** until it's approved again: TLS then logs a warning once and uses
  the built-in roots alone. The same happens if the file or the card is missing, so NTS keeps
  working without the card.
- `tls roots` shows the built-in roots and the file's state; `tls roots forget` (console) stops
  trusting it. A private key, or any PEM block other than a certificate, makes the file invalid.

## Lua scripts (prototype)

Lua 5.4 runs on the second core (core 1), so a script can compute flat out without slowing the
console, SSH or the watchdog on core 0. Scripts live on the SD card:

```sh
lua -e 'print(1 + 1)'            # a one-liner
lua /scripts/report.lua          # a file (relative paths start at the working directory)
```

The script's output goes to the shell that started it; Ctrl-C stops it, as does ending the SSH
session. One script runs at a time. Available: the base, string, table, math, utf8 and
coroutine libraries, and `sos`:

| Function | |
|---|---|
| `sos.read(path)` | file contents as a string |
| `sos.write(path, s)`, `sos.append(path, s)` | write / append (through `files_write_allowed()`) |
| `sos.list([path])` | `{ {name=, size=, dir=}, ... }` |
| `sos.remove(path)`, `sos.mkdir(path)` | delete a file or empty directory, create a directory |
| `sos.time()`, `sos.uptime()` | seconds since 1970 (UTC), seconds since boot |
| `sos.sleep(ms)`, `sos.log(msg)` | wait (Ctrl-C still works), write to the system log |

Failures return `nil` and a message. There's no `io`, `os`, `package` or `debug`: scripts reach
the system only through `sos`.

- **How it works:** core 1 never touches the card, network or shells. Each `sos` call is a
  request that core 0's main loop answers (one per pass); output goes through a buffer that the
  shell drains as its channel takes it. The heap is shared (malloc is mutex-protected), and each
  script may allocate up to 128 KB ("not enough memory" beyond). Core 1 gets a 32 KB stack from
  the heap when the first script runs, and pauses while core 0 writes flash.
- **Speed** (debug build): `fib(25)` takes 1.4 s; a million-iteration floating-point loop 2.6 s.
- **Prototype limits:** no interactive prompt; numbers are formatted with newlib's `snprintf`
  on core 1 (its float conversion has state that isn't safe to share between cores, and core 0
  doesn't use it today).

## Testing

**Unit tests** run on the development machine with the host compiler (no board, no Pico SDK),
under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

They cover the SSH I/O ring buffer (`src/ssh/pico-sshd/ringbuffer.c`, including a randomized comparison
against a reference model), the shell's line editing, questions and output (`src/shell/shell_core.c`,
with the commands faked), the configuration store (`src/core/config.c`, with a fake flash that can
fail or lose power mid-write), the log (`src/core/log.c`), the date and RTC register conversions
(`src/time/timeconv.c`, compared against the host C library), path handling for the file commands
(`src/files/path.c`), the NTS key-exchange messages and NTS-protected NTP packets (`src/time/nts_ke.c`,
`src/time/nts_ntp.c`) and the root certificate bundle parser (`src/tls/certbundle.c`). `tests/test.h` is
a minimal harness; add a suite as `tests/test_<name>.c` and register it in
`tests/CMakeLists.txt`.

**Hardware tests** run against a board that is flashed with SOS and on the network:

```sh
tools/test/ssh-hw-test.sh -u <user> <device-ip>     # -q for a quick run, -h for options
tools/test/sftp-hw-test.sh -u <user> <device-ip>    # needs a formatted SD card
```

`tools/test/ssh-hw-test.sh` checks the SSH server. It covers login and logout, authentication
(only public key; unknown key, wrong user and password refused), concurrent sessions kept
separate, the "too many connections" message, slot reuse, a large input burst, clients that
disappear, back-to-back logins, and heap use before and after. It exits with the number of failed checks. It uses every session slot for a few seconds,
so run it while nobody else is using the board; it warns if the device rebooted mid-run.

`tools/test/sftp-hw-test.sh` checks SFTP on the SD card. It puts and gets a 2 MB random file
and a small one, overwrites a file with a smaller one, resumes both ways (`reget`, `reput`), and
checks that a failed rename or rmdir is reported. During a download it checks that unmounting
the card and removing the file are refused, then kills the client and checks that the file is
released. It ends by comparing heap use before and after. It works in `/sftp-hw-test` (removed
at the end), unmounts the card, and exits with the number of failed checks. Run it while
nobody else is using the board.

## Memory layout

| Region | Use |
|---|---|
| Main RAM (512 KB) | data, heap, and the core 0 stack (top 8 KB, `SOS_CORE0_STACK_SIZE`) |
| SCRATCH_X (4 KB) | core 1 stack (`PICO_CORE1_STACK_SIZE`), as in the SDK default; while Lua runs, core 1 uses a 32 KB stack from the heap instead |
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
