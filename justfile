# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Shortcuts for everyday commands. Optional: everything here calls CMake, Ninja or a script in
# tools/, and works the same typed by hand. `just` with no arguments lists the recipes.
#
# The build itself is CMake + Ninja; nothing is built by this file. Paths, keys and board
# configuration stay where they are (local.cmake, provision.conf, the environment), never here.
# Writing OTP memory is deliberately not a recipe: it is a hand-run procedure (docs/otp/).

set shell := ["bash", "-euo", "pipefail", "-c"]

# List the recipes
default:
    @just --list --unsorted

# Configure (if needed) and build the firmware; preset: debug or release
build preset="debug":
    #!/usr/bin/env bash
    set -euo pipefail
    dir={{ if preset == "release" { "build-release" } else { "build" } }}
    [[ -f $dir/build.ninja ]] || cmake --preset {{ quote(preset) }}
    cmake --build --preset {{ quote(preset) }}

# Configure and build in another directory: `just build-in build-unsigned -DSOS_SIGN_KEY=none`
build-in dir *cmake_args:
    cmake --preset debug -B {{ quote(dir) }} {{ cmake_args }}
    cmake --build {{ quote(dir) }}

# Host unit tests; extra arguments go to ctest, e.g. `just test -R config`
test *ctest_args:
    cmake -S tests -B build-tests
    cmake --build build-tests
    ctest --test-dir build-tests --output-on-failure {{ ctest_args }}

# Which key an image is signed with, and the flash guard's verdict; flashes nothing
flash-check dir="build":
    tools/flash.sh -n {{ quote(dir / "sos.bin") }}

# flash-check for every build directory that has an image
flash-check-all:
    #!/usr/bin/env bash
    set -uo pipefail
    status=0
    for bin in build/sos.bin build-*/sos.bin; do
        [[ -e $bin ]] || continue
        echo "== $bin"
        tools/flash.sh -n "$bin" || status=1
    done
    exit $status

# Build, show the flash guard's verdict, ask, then flash (serial: default $SOS_FLASH_SERIAL)
flash serial="" dir="build":
    #!/usr/bin/env bash
    set -euo pipefail
    cmake --build {{ quote(dir) }} --target sos
    [[ -z {{ quote(serial) }} ]] || export SOS_FLASH_SERIAL={{ quote(serial) }}
    tools/flash.sh -n {{ quote(dir / "sos.bin") }}
    read -rp "Flash {{ dir }}/sos.bin to board ${SOS_FLASH_SERIAL:-(whichever is on USB)}? [y/N] " answer
    [[ $answer =~ ^([yY]|[yY][eE][sS])$ ]] || { echo "not flashed"; exit 1; }
    tools/flash.sh {{ quote(dir / "sos.bin") }}

# Build the configuration record and say where it would go; writes nothing
provision-check serial="" file="":
    tools/provision.sh -n {{ if serial != "" { "-s " + quote(serial) } else { "" } }} {{ quote(file) }}

# Show where it goes, ask, then replace a board's configuration (serial: default $SOS_FLASH_SERIAL; file: provision.conf)
provision serial="" file="":
    #!/usr/bin/env bash
    set -euo pipefail
    [[ -z {{ quote(serial) }} ]] || export SOS_FLASH_SERIAL={{ quote(serial) }}
    [[ -z {{ quote(file) }} ]] || export SOS_PROVISION_FILE={{ quote(file) }}
    tools/provision.sh -n
    read -rp "Replace that board's saved configuration? [y/N] " answer
    [[ $answer =~ ^([yY]|[yY][eE][sS])$ ]] || { echo "not provisioned"; exit 1; }
    cmake --build build --target provision

# SSH hardware test, e.g. `just hw-ssh 192.168.1.20 -u me -q`
hw-ssh host *opts:
    tools/test/ssh-hw-test.sh {{ opts }} {{ quote(host) }}

# SFTP hardware test (needs a formatted SD card), e.g. `just hw-sftp 192.168.1.20 -u me`
hw-sftp host *opts:
    tools/test/sftp-hw-test.sh {{ opts }} {{ quote(host) }}

# Delete one build directory's outputs (ninja clean), keeping its configuration
clean dir="build":
    cmake --build {{ quote(dir) }} --target clean

# Delete build/ and every build-*/ (not local.cmake, provision.conf or other ignored files)
[confirm("Delete build/ and every build-*/ directory? [y/N]")]
clean-all:
    rm -rf build build-*/
