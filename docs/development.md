# Development setup

The [README](../README.md) covers the default setup: dependencies as git submodules, the Pico SDK
from `PICO_SDK_PATH`, and the board on the same machine. This page covers optional variations.
Machine-specific values (paths, network, VM names) belong in git-ignored files such as
`local.cmake`, never in the repository.

## Dependencies outside the repository

wolfSSL and wolfSSH are about 220 MB checked out. To share one copy between projects, or keep
large trees out of the source directory, clone them anywhere at the versions pinned by the
submodules and point the build at them in `local.cmake`:

```sh
cd <deps-dir>
git clone --depth 1 -b 2.3.1 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk.git
git clone --depth 1 -b v5.9.4-stable https://github.com/wolfSSL/wolfssl.git
git clone --depth 1 -b v1.6.0-stable https://github.com/wolfSSL/wolfssh.git
```

```cmake
# local.cmake
set(WOLFSSL_ROOT "<deps-dir>/wolfssl")
set(WOLFSSH_ROOT "<deps-dir>/wolfssh")
```

```sh
# shell profile
export PICO_SDK_PATH="<deps-dir>/pico-sdk"
```

The submodules can then stay uninitialized. `git submodule status` shows the pinned commits;
when they change, check out the same commits in your copies. `-DWOLFSSL_ROOT=...` on the
command line or a git-ignored `CMakeUserPresets.json` work too.

## Editor

`.vscode/` configures VS Code to use [clangd](https://clangd.llvm.org/) (extension
`llvm-vs-code-extensions.vscode-clangd`) with the compile database CMake writes to
`build/compile_commands.json`, and disables Microsoft C/C++ IntelliSense, which reports bogus
include errors for this project. `--query-driver` assumes the toolchain is in `/usr/bin`; adjust
it in `.vscode/settings.json` if yours is elsewhere. Run `cmake --preset debug` once so the
compile database exists.

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
