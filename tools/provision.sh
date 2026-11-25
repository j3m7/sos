#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Write a board's configuration from a provisioning file, over USB with picotool.
#
#   tools/provision.sh [-n] [-f flash-bytes] [-w workdir] [provision.conf]
#
# The firmware image holds no configuration: a board is set up on its console (`setup`) or with
# this. It builds the saved-configuration record on this machine (tools/host/mkconfig.c, with the
# device's own src/core/config.c) and loads it into the two configuration sectors, replacing
# whatever was saved there. The firmware and the SSH host key are not touched. Like flashing, it
# needs the board on USB; it reboots it into BOOTSEL and back.
#
# The record is written unsealed: this machine doesn't have the board's key. An open board
# loads it, stores it sealed at its first boot and erases the unsealed copy. (A board with
# secure boot will refuse an unsealed record; it's set up on its console.)
#
# Use the source tree the board's firmware was built from: firmware older than the record's
# format ignores it and starts empty.
#
#   -n  build the image and say where it would go, but don't write it
#   -f  flash size in bytes (default 4194304, the Pico 2 W)
#   -w  where to put the tool and the image (default: a temporary directory)
#   provision.conf  default: $SOS_PROVISION_FILE, or provision.conf in the source tree
#                   (see provision.conf.example)

set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dry= flash=$((4 * 1024 * 1024)) work=
while getopts "nf:w:" opt; do
    case $opt in
    n) dry=1 ;;
    f) flash=$OPTARG ;;
    w) work=$OPTARG ;;
    *) sed -n '5,7p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
conf=${1:-${SOS_PROVISION_FILE:-$root/provision.conf}}
[[ -r $conf ]] || { echo "no provisioning file: $conf (copy provision.conf.example)" >&2; exit 1; }

if [[ -z $work ]]; then
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT      # the image holds the Wi-Fi password
fi
mkdir -p "$work"

${HOSTCC:-cc} -std=c11 -O1 -Wall -Wextra -I"$root/src" -I"$root/tools/host" \
    -o "$work/sos-mkconfig" "$root/tools/host/mkconfig.c" "$root/src/core/config.c" \
    "$root/src/time/timeconv.c"
"$work/sos-mkconfig" "$conf" "$work/config.bin"

# The configuration slots are the two 4 KB sectors below the last one (the key sector: the SSH
# host key); see src/core/settings.c. The sector below them holds the board's root secret
# (src/core/rootflash.h) and is not written here. Flash is mapped at 0x10000000.
addr=$(printf '0x%08x' $((0x10000000 + flash - 3 * 4096)))
if [[ -n $dry ]]; then
    echo "would load $(stat -c %s "$work/config.bin") bytes at $addr"
    exit 0
fi
command -v picotool >/dev/null || { echo "picotool not found" >&2; exit 1; }
# -f: reboot a running board into BOOTSEL for the load, and back afterwards.
picotool load -f -v --ignore-partitions -t bin "$work/config.bin" -o "$addr"
echo "provisioned from $conf; the board restarts with this configuration"
