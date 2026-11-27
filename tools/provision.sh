#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Write a board's configuration from a provisioning file, over USB with picotool.
#
#   tools/provision.sh [-n] [-s serial] [-f flash-bytes] [-w workdir] [provision.conf]
#
# The firmware image holds no configuration: a board is set up on its console (`setup`) or with
# this. It builds the saved-configuration record on this machine (tools/host/mkconfig.c, with the
# device's own src/core/config.c) and loads it into the two configuration sectors, replacing
# whatever was saved there. The firmware and the SSH host key are not touched. Like flashing, it
# needs the board on USB; it reboots it into BOOTSEL and back.
#
# The record is written unsealed: this machine doesn't have the board's key. An open board
# loads it, stores it sealed at its first boot and erases the unsealed copy. A console-only
# board (src/core/consoleonly.h) refuses an unsealed record: don't provision one. This would
# replace its sealed configuration with a record it won't load, leaving it with none.
#
# Use the source tree the board's firmware was built from: firmware older than the record's
# format ignores it and starts empty.
#
#   -n  build the image and say where it would go, but don't write it
#   -s  the board's serial (16 hex digits, as `version` and `picotool info` show it); default
#       $SOS_FLASH_SERIAL, the variable `ninja flash` uses. Required when this machine has a
#       flash key list (see tools/flash.sh), which means it works with more than one board.
#       Without one, picotool takes the board that is attached
#   -f  flash size in bytes (default 4194304, the Pico 2 W)
#   -w  where to put the tool and the image (default: a temporary directory)
#   provision.conf  default: $SOS_PROVISION_FILE, or provision.conf in the source tree
#                   (see provision.conf.example)

set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dry= flash=$((4 * 1024 * 1024)) work=
serial=${SOS_FLASH_SERIAL:-}
while getopts "ns:f:w:" opt; do
    case $opt in
    n) dry=1 ;;
    s) serial=$OPTARG ;;
    f) flash=$OPTARG ;;
    w) work=$OPTARG ;;
    *) sed -n '7p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
conf=${1:-${SOS_PROVISION_FILE:-$root/provision.conf}}
[[ -r $conf ]] || { echo "no provisioning file: $conf (copy provision.conf.example)" >&2; exit 1; }
# Upper case, as the board reports it.
serial=${serial^^}
ser=() board="the board on USB"
if [[ -n $serial ]]; then
    [[ $serial =~ ^[0-9A-F]{16}$ ]] || { echo "not a board serial (16 hex digits): $serial" >&2; exit 2; }
    ser=(--ser "$serial") board="board $serial"
else
    keys=${SOS_FLASH_KEYS:-${XDG_CONFIG_HOME:-$HOME/.config}/sos/flash-keys}
    if [[ -e $keys ]]; then
        echo "REFUSED: name the board: -s <serial> or SOS_FLASH_SERIAL=<serial>." >&2
        echo "  This machine has a flash key list ($keys)," >&2
        echo "  so it works with more than one board. \`version\` on the board's console and" >&2
        echo "  \`picotool info\` show the serial." >&2
        exit 3
    fi
fi

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
    echo "would load $(stat -c %s "$work/config.bin") bytes at $addr on $board"
    exit 0
fi
command -v picotool >/dev/null || { echo "picotool not found" >&2; exit 1; }
# -f: reboot a running board into BOOTSEL for the load, and back afterwards. --ser picks the board.
# picotool wants the file, type and offset first, then the options that pick and reboot it.
picotool load -v --ignore-partitions "$work/config.bin" -t bin -o "$addr" -f "${ser[@]}"
echo "provisioned $board from $conf; it restarts with this configuration"
echo "note: this made the board a new owner: SD cards it encrypted before are now locked"
echo "      (readable only with the key shown when each was formatted)"
