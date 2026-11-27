# Development setup

[Building, flashing and testing](building.md#setup) covers the default setup: dependencies as
git submodules, the Pico SDK from `PICO_SDK_PATH`, and the board on the same machine. This page
covers optional variations.
Machine-specific values (paths, network, VM names) belong in git-ignored files such as
`local.cmake`, never in the repository. A board's own configuration (Wi-Fi, trusted keys) is
never part of the build: it comes from `setup` on the console or from `provision.conf` (see
[Provisioning](configuration.md#provisioning)). With several checkouts or worktrees, point them
all at one file with `SOS_PROVISION_FILE` in the environment.

## Dependencies outside the repository

wolfSSL and wolfSSH are about 220 MB checked out (the SD card library is about 5 MB). To share one copy between projects, or keep
large trees out of the source directory, clone them anywhere at the versions pinned by the
submodules and point the build at them in `local.cmake`:

```sh
cd <deps-dir>
git clone --depth 1 -b 2.3.1 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk.git
git clone --depth 1 -b v5.9.4-stable https://github.com/wolfSSL/wolfssl.git
git clone --depth 1 -b v1.6.0-stable https://github.com/wolfSSL/wolfssh.git
git clone --depth 1 -b v3.6.2 https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git
git clone --depth 1 -b v5.4.9 https://github.com/lua/lua.git
```

```cmake
# local.cmake
set(WOLFSSL_ROOT "<deps-dir>/wolfssl")
set(WOLFSSH_ROOT "<deps-dir>/wolfssh")
set(SDCARD_ROOT  "<deps-dir>/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico")
set(LUA_ROOT     "<deps-dir>/lua")
```

```sh
# shell profile
export PICO_SDK_PATH="<deps-dir>/pico-sdk"
```

The submodules can then stay uninitialized. `git submodule status` shows the pinned commits;
when they change, check out the same commits in your copies. `-DWOLFSSL_ROOT=...` on the
command line or a git-ignored `CMakeUserPresets.json` work too.

## Signed builds

The build can sign the firmware image, which is what a board with secure boot will require
(issue #64). A board without secure boot runs a signed image like any other, so signing can be
on in everyday builds.

For a development board, use the public dev keys in [`keys/`](../keys/README.md)
(`-DSOS_SIGN_KEY=$PWD/keys/dev-INSECURE-1.pem`). They are not secret, so never put their
hashes into a production board's OTP.

For any other board, make a key once, outside the repository, and keep it private (it is a secp256k1 private key in
PEM form):

```sh
(umask 077; openssl ecparam -name secp256k1 -genkey -noout -out <keys-dir>/sos-sign.pem)
```

Point the build at it with `SOS_SIGN_KEY`, in `local.cmake`, with `-DSOS_SIGN_KEY=...`, or in
the environment (read when CMake configures):

```cmake
# local.cmake
set(SOS_SIGN_KEY "<keys-dir>/sos-sign.pem")
```

A key given on the command line wins over `local.cmake`, for that build directory, so another
key or an unsigned image can be built beside the everyday one without editing anything:

```sh
cmake --preset debug -B build-key2 -DSOS_SIGN_KEY=<keys-dir>/other.pem    # signed with another key
cmake --preset debug -B build-unsigned -DSOS_SIGN_KEY=none                 # not signed
```

CMake says which it is when it configures (`SOS: the image is signed with ...` or `is not
signed`). To see which key an image is really signed with, `tools/flash.sh -n <dir>/sos.bin`
prints the hash of its key and flashes nothing.

`picotool` then hashes and signs the image after linking: `sos.elf`, `sos.uf2` and the `flash`
target are the signed image. Without `SOS_SIGN_KEY` the image is unsigned, as before; CMake
prints which it is when it configures. To check an image:

```sh
picotool info -a build/sos.elf      # look for "hash: verified" and "signature: verified"
```

- **The key decides who can update a secured board.** Once a board has secure boot enabled, it
  runs only images signed with the key whose hash was written to its one-time-programmable (OTP)
  memory. If that key is lost, the board can never take new firmware; if it leaks, anyone can
  sign firmware for the board. A key used for a real board belongs offline, with a second key
  as a spare. A key made for trying this out on an open board protects nothing and needs no
  such care.
- **The build writes nothing to a board's OTP memory and makes no OTP file.** By default the
  Pico SDK has `picotool` write `sos.otp.json` next to a signed image. That file is not just
  the key's hash: it holds the hash, the "key valid" flag and `crit1.secure_boot_enable`, so a
  single `picotool otp load` of it writes the key **and turns secure boot on, permanently**.
  From then on the board refuses every image not signed with that key. The SOS build switches
  the file off. Securing a board is a separate step with its own written procedure; don't
  improvise it with `picotool seal`, which produces the same file if given a name for it.
- Signing doesn't change what the firmware does: `picotool` adds the hash and signature to the
  image's metadata after the firmware is linked.

## Flashing, and the flash guard

`ninja flash` (`cmake --build --preset debug --target flash`) loads `build/sos.bin` over USB
with `tools/flash.sh`. Set `SOS_FLASH_SERIAL` to a board's serial number (`version` on its
console, or `picotool info`) to say which board; with a key list (below) it is required.
`tools/provision.sh` (`ninja provision`) writes to the same board when it is set, and with a
key list it refuses to run without it.

A board with secure boot starts only firmware signed with a key whose hash is in its
one-time-programmable memory. Flash it anything else and it doesn't start: it waits in the USB
bootloader until a good image is loaded. That is easy to do by accident from a checkout whose
build isn't set up to sign, or by flashing the wrong one of two boards, so `tools/flash.sh` can
refuse first.

- **The list.** A file saying which key hashes each board accepts. A hash is the SHA-256 of a
  public key, 64 hex digits: what `bootkey` prints on a board and what a board's OTP holds.
  The rest of a line after `#` is a comment. It holds no private key.

  With one kind of board, the hashes alone: every board this machine flashes accepts them.

  ```
  # ~/.config/sos/flash-keys
  <64 hex digits>         # the key in use
  <64 hex digits>         # the spare
  ```

  With boards that accept different keys, a section for each board instead:

  ```
  [0123456789ABCDEF]      # a board, by its serial
  <64 hex digits>         #   the keys that board accepts, and only those

  [FEDCBA9876543210]
  open                    #   an open board: it takes anything, nothing is checked
  ```

  It is `$SOS_FLASH_KEYS` if set, else `${XDG_CONFIG_HOME:-~/.config}/sos/flash-keys`: one per
  machine, outside every checkout, so that a fresh checkout or worktree with no `local.cmake`
  is guarded too.
- **With a list, the board must be named** with `SOS_FLASH_SERIAL`, and flashing is refused
  without it (exit status 3). Which keys are accepted depends on the board, and with two
  boards attached `picotool` would otherwise pick one.
- **Which keys apply.** In a list without sections, all of them, for every board. In a list
  with sections, a board takes its own section's keys only, and **a board with no section is
  refused**: the list is then the list of boards this machine flashes. A hash before the
  first section is an error. A section with `open` turns the check off for that board.
- **What is refused** before anything is sent to the board: an image that is unsigned, or
  signed with a key the list doesn't give for that board. The message says which, with the
  hash of the image's key. A list with a line it can't read is an error for every board.
- **Without a list** there is no guard: the image is flashed as before, and the output says the
  signature was not checked.
- **`ninja flash-check`** runs the check and flashes nothing.
- **`SOS_FLASH_UNGUARDED=1`** flashes without the check.
- **What it doesn't cover:** `picotool load` typed by hand, and `tools/provision.sh`, which
  writes a configuration record and no firmware. It trusts the serial it is given: it doesn't
  ask the board which keys it holds. It compares the key's hash; it doesn't verify the
  signature (`picotool info -a build/sos.elf` does).

To get a key's hash for the list without a board:
`openssl ec -in <key>.pem -pubout -outform DER | tail -c 64 | sha256sum`. A refusal also prints
the hash of the key the refused image is signed with.

## Glitch detector resets

SOS arms the RP2350's glitch detector by software at boot, at the highest sensitivity, on every
board (`glitch` shows it; `glitch off` turns it off). It is there to learn whether the detector
ever resets a healthy board, before it is enabled for good in OTP on a secured board, where it
can't be turned off again
([decision 5](security/decisions.md#5-glitch-detector-otp-bit-deferred)). So far it has had one
3-hour run under SFTP load without a reset; a week-long test waits for a spare board.

**Whoever works with a board, person or agent, watches for it:**

- At boot the log says `last reset: glitch detector (detectors 0x.. fired, after N s up; ...)`
  and, on the next line, the chip temperature and supply voltage before the reset; `uptime` says
  `(last reset: glitch detector)`, and `glitch` shows the count since power-on.
- A detector reset that nobody forced with `glitch trig` is a finding. Record it on issue #100:
  the firmware version, the detectors that fired, the uptime, the chip temperature and supply
  voltage, what the board was doing (Wi-Fi traffic, card access, a script, flashing), and how
  it was powered (`power` says whether USB power is present).
- An unexplained reboot is worth a look at `glitch` before anything else: a detector reset
  loses the fault record, so `uptime` shows no fault for it.
- If it gets in the way of other work, `glitch off` is fine. Say so in the same place, with
  what it was doing, and leave the setting off rather than lowering the sensitivity silently.

## Test commands: running the heap short

An image configured with `-DSOS_TEST_COMMANDS=ON` (off by default) has two more console
subcommands, for making the out-of-memory paths happen on a board (issue #188). They take heap
memory with plain `malloc` and keep it until `mem release`; `mem` counts it under `other` and
adds a `held:` line.

| Command | What it holds |
|---|---|
| `mem hold <KB>` | One more block of that size. Repeat to stack blocks. |
| `mem hold leave <KB>` | Everything but about `<KB>` of the heap, so any request larger than that fails. |
| `mem hold holes <count> <KB>` | Everything but `<count>` holes of `<KB>` each, each between two held blocks: `<count> × <KB>` is free, but no single request larger than `<KB>` fits (fragmentation). |
| `mem release` | Frees all of it. |
| `mem release first [<seconds>]` | Frees the block held first, now or that many seconds later: memory comes back while a command waits for an answer (the console can't take `mem release` then). |

Each prints what it holds, the bytes left unallocated and the largest block that still fits
(found by trying allocations). Sizes may have a fraction (`7.5`). The commands work on the
console only, like configuration changes.

For example, `sd format`'s encrypted path asks for the card's 7.5 KB block only after the
question, and after its 32 KB work buffer: `mem hold 34`, `sd unmount`, `mem hold leave 3`,
`mem release first 12`, `sd format`, then wait for the 34 KB to come back before typing `yes`.

With the heap held down, other parts of the system fail too (a new SSH session, a TLS
connection, the log writer's card retry), so run these from the console and release before
handing the board on. Don't leave such an image on a board in use.

## Editor

`.vscode/` configures VS Code to use [clangd](https://clangd.llvm.org/) (extension
`llvm-vs-code-extensions.vscode-clangd`) with the compile database CMake writes to
`build/compile_commands.json`, and disables Microsoft C/C++ IntelliSense, which reports bogus
include errors for this project. `--query-driver` assumes the toolchain is in `/usr/bin`; adjust
it in `.vscode/settings.json` if yours is elsewhere. Run `cmake --preset debug` once so the
compile database exists.

## Git hooks

`.githooks/pre-commit` refuses a commit whose generated files are stale. Today that is the
manual pages: `man/pages.bundle` and `docs/commands.md` are built from `man/*.txt` by
`tools/man-bundle.sh`, and `man install` downloads the bundle from GitHub as committed. Turn it
on once per clone (every worktree of the clone shares the setting):

```sh
git config core.hooksPath .githooks
```

It runs `tools/man-bundle.sh --check` on the staged files, not the working tree, and only when
the commit touches `man/`, `docs/commands.md` or the two scripts. If it refuses, run
`tools/man-bundle.sh` and stage `man/pages.bundle` and `docs/commands.md`. After merging a branch
that also changed the pages, regenerate the two files instead of resolving their conflicts by
hand. `git commit --no-verify` skips the hook. Nothing checks this on GitHub; the host test
`man-bundle` (`ctest`) runs the same check.

The hook also runs [shellcheck](https://www.shellcheck.net/) on the staged shell scripts when
the commit touches one (or `.shellcheckrc`), and refuses the commit if it reports anything. Fix
the script, or, for a finding that is wrong in that place, add a
`# shellcheck disable=SCnnnn` line above it saying why. Without shellcheck installed the hook
skips this with a note. `just lint` (or `tools/lint-shell.sh`) checks every script in the working
tree; `.shellcheckrc` holds the settings.

## Developing in a VM

With the source on a host folder shared into a libvirt VM (for example virtiofs), the Pico's
USB connection has to reach the VM. It disconnects and comes back under a different USB ID every
time it switches between running firmware (`2e8a:0009`) and BOOTSEL (`2e8a:000f`), including
twice during every `flash`, so forwarding a single device once is not enough:

- **Host-side hotplug forwarding (recommended):** [`tools/host/`](../tools/host/README.md)
  installs a udev rule on the host that attaches every Raspberry Pi USB device to the VM through
  libvirt as it appears. Flashing then works unattended.
- **SPICE USB redirection:** `virt-viewer` can redirect devices automatically
  (`--spice-usbredir-auto-redirect-filter='-1,0x2e8a,-1,-1,1|-1,-1,-1,-1,0'`), but in practice
  only while the viewer window has focus, so reboots of the Pico are missed otherwise.

Inside the VM, the user needs access to the USB device and the serial port. The picotool udev
rules (`picotool/udev/60-picotool.rules`) grant it to group `plugdev`, which some distributions
(Arch, for example) don't create; the serial port belongs to `uucp` (Arch) or `dialout`
(Debian/Ubuntu):

```sh
sudo groupadd -r plugdev        # if it doesn't exist
sudo usermod -aG plugdev,uucp $USER
```

Group changes apply to new login sessions only. Editors that keep a remote server running
(VS Code Remote-SSH) and tools started from them keep the old groups until that server
restarts; rebooting the VM is the simplest way.

## Debugging over SWD

A Raspberry Pi Debug Probe (or a Pico running the
[debugprobe](https://github.com/raspberrypi/debugprobe) firmware) connected to the Pico 2 W's SWD
pins allows flashing without BOOTSEL and source-level debugging. Its USB ID (`2e8a:000c`) never
changes, so in a VM it can be forwarded once like any other device.

RP2350 support needs Raspberry Pi's `openocd` fork (release 0.12.0 predates it):

```sh
git clone --depth 1 -b rpi-common --recurse-submodules --shallow-submodules https://github.com/raspberrypi/openocd.git
cd openocd
./bootstrap
./configure --prefix=$HOME/.local --disable-werror --enable-internal-jimtcl --enable-cmsis-dap-v2
make -j$(nproc) && make install
```

USB access for the probe, without root:

```
# /etc/udev/rules.d/99-rpi-debugprobe.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000c", MODE="660", GROUP="plugdev", TAG+="uaccess"
```

```sh
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000"
arm-none-eabi-gdb build/sos.elf -ex "target extended-remote :3333"
# flash over SWD:
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" \
    -c "program build/sos.elf verify reset exit"
```
