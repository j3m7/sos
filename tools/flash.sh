#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Flash a firmware image over USB with picotool, after checking which key it is signed with.
# `ninja flash` runs this.
#
#   tools/flash.sh [-n] [-w workdir] <sos.bin>
#
# The guard: a board with secure boot runs only firmware signed with a key whose hash is in its
# OTP memory. Flash anything else and it doesn't start; it sits in the USB bootloader until a
# good image is loaded. That is easy to do by accident, from a checkout whose build isn't set
# up to sign. So when this machine has a list of accepted key hashes, an image that isn't
# signed with one of them is refused before anything is sent to the board.
#
# The list is a file of SHA-256 hashes of public keys, one per line (64 hex digits; the rest of
# a line and lines starting with # are comments), the hash `bootkey` prints on a board and that
# a board's OTP holds. It holds no private key. It is looked for in:
#   $SOS_FLASH_KEYS, else ${XDG_CONFIG_HOME:-~/.config}/sos/flash-keys
# It belongs to the machine, not to a checkout, so that every checkout and worktree on the
# machine is guarded, including one with no build settings of its own.
#
# Boards differ in the keys they accept, so the list can say which board takes which:
#
#   <hash>                  # before any [serial] line: for every board not named below
#   [F94525FD61D9A1CC]      # a board, by the serial `version` and picotool show
#   <hash>                  #   the keys that board accepts, and only those
#   [0123456789ABCDEF]
#   open                    #   an open board: takes anything, nothing is checked
#
# With a list, the board must be named: SOS_FLASH_SERIAL=<serial>. The keys can't be chosen
# without knowing the board, and with two boards attached picotool would otherwise pick one.
# A board with a [serial] section takes that section's keys only; any other board takes the
# keys listed before the first section, and is refused if there are none.
#
# No list: no guard, and the image is flashed as before (this says so). SOS_FLASH_UNGUARDED=1
# flashes without the check.
#
# It guards this script only. `picotool load` typed by hand goes around it.
#
#   -n  check only: say what would happen, send nothing to the board
#   -w  where to build the helper (default: a temporary directory)
#
# Exit status: 0 flashed (or, with -n, would be), 3 refused by the guard, 1 anything else.

set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dry= work=
while getopts "nw:" opt; do
    case $opt in
    n) dry=1 ;;
    w) work=$OPTARG ;;
    *) sed -n '8p' "$0" >&2; exit 1 ;;
    esac
done
shift $((OPTIND - 1))
image=${1:-}
[[ -n $image && -r $image ]] || { echo "usage: $0 [-n] [-w workdir] <sos.bin>" >&2; exit 1; }

keys=${SOS_FLASH_KEYS:-${XDG_CONFIG_HOME:-$HOME/.config}/sos/flash-keys}

if [[ -n ${SOS_FLASH_UNGUARDED:-} ]]; then
    echo "flash guard: skipped (SOS_FLASH_UNGUARDED is set)"
elif [[ ! -e $keys ]]; then
    echo "flash guard: off (no key list at $keys); the image's signature is not checked"
else
    [[ -r $keys ]] || { echo "flash guard: can't read the key list $keys" >&2; exit 1; }
    serial=${SOS_FLASH_SERIAL:-}
    serial=${serial^^}
    if [[ ! $serial =~ ^[0-9A-F]{16}$ ]]; then
        echo "flash guard: REFUSED. Name the board: SOS_FLASH_SERIAL=<its 16-digit serial>." >&2
        echo "  This machine has a key list ($keys)," >&2
        echo "  and which keys are accepted depends on the board. \`version\` on the board's" >&2
        echo "  console and \`picotool info\` show the serial." >&2
        exit 3
    fi
    # The accepted hashes for this board, lower case: its own section's if it has one, else
    # the ones before the first section. A line that is none of comment, hash, [serial] or
    # "open" is a mistake in the list, and a list with a mistake is not trusted.
    default=() own=() own_open= has_own= section=
    lineno=0
    while IFS= read -r line || [[ -n $line ]]; do
        lineno=$((lineno + 1))
        line=${line%%#*}
        [[ $line =~ ^[[:space:]]*$ ]] && continue
        if [[ $line =~ ^[[:space:]]*\[([0-9a-fA-F]{16})\][[:space:]]*$ ]]; then
            section=${BASH_REMATCH[1]^^}
            [[ $section == "$serial" ]] && has_own=1
        elif [[ $line =~ ^[[:space:]]*([0-9a-fA-F]{64})([[:space:]].*)?$ ]]; then
            if [[ -z $section ]]; then
                default+=("${BASH_REMATCH[1],,}")
            elif [[ $section == "$serial" ]]; then
                own+=("${BASH_REMATCH[1],,}")
            fi
        elif [[ $line =~ ^[[:space:]]*open[[:space:]]*$ && -n $section ]]; then
            [[ $section == "$serial" ]] && own_open=1
        else
            echo "flash guard: $keys line $lineno is not a key hash (64 hex digits)," >&2
            echo "             a [serial] line (16 hex digits) or 'open' under one" >&2
            exit 1
        fi
    done < "$keys"
    if [[ -n $has_own && -n $own_open && ${#own[@]} -gt 0 ]]; then
        echo "flash guard: $keys lists board $serial as open and with keys; it can't be both" >&2
        exit 1
    fi
    accepted=()
    if [[ -n $has_own ]]; then
        accepted=("${own[@]}")
        whose="board $serial"
    else
        accepted=("${default[@]}")
        whose="boards without a section of their own"
    fi
fi

if [[ -n ${SOS_FLASH_UNGUARDED:-} || ! -e $keys ]]; then
    :
elif [[ -n $own_open ]]; then
    echo "flash guard: board $serial is listed as open; the image's signature is not checked"
else
    if [[ ${#accepted[@]} -eq 0 ]]; then
        echo "flash guard: REFUSED. $keys lists no key for $whose," >&2
        echo "  so no image can be accepted for board $serial. Add its keys under [$serial]," >&2
        echo "  or 'open' there if it takes anything." >&2
        exit 3
    fi

    if [[ -z $work ]]; then
        work=$(mktemp -d)
        trap 'rm -rf "$work"' EXIT
    fi
    mkdir -p "$work"
    ${HOSTCC:-cc} -std=c11 -O1 -Wall -Wextra -I"$root/src" \
        -o "$work/sos-imgkey" "$root/tools/host/imgkey.c" "$root/src/core/bootkey.c"
    status=0
    "$work/sos-imgkey" "$image" > "$work/pubkey" || status=$?
    case $status in
    0) ;;
    3)
        echo "flash guard: REFUSED. $image is not signed." >&2
        echo "  The key list ($keys) says board $serial takes signed firmware only." >&2
        echo "  Set SOS_SIGN_KEY for this checkout (docs/development.md, \"Signed builds\")," >&2
        echo "  configure again and rebuild." >&2
        exit 3 ;;
    4)
        echo "flash guard: REFUSED. $image doesn't look like a firmware image" >&2
        echo "  (no image description found), so its signature can't be checked." >&2
        exit 3 ;;
    *)
        echo "flash guard: checking $image failed" >&2
        exit 1 ;;
    esac
    [[ $(stat -c %s "$work/pubkey") -eq 64 ]] || { echo "flash guard: bad key length" >&2; exit 1; }
    hash=$(sha256sum < "$work/pubkey")
    hash=${hash%% *}
    found=
    for h in "${accepted[@]}"; do
        [[ $h == "$hash" ]] && found=1
    done
    if [[ -z $found ]]; then
        echo "flash guard: REFUSED. $image is signed with a key that $keys" >&2
        echo "  doesn't list for $whose:" >&2
        echo "  $hash" >&2
        echo "  A board with secure boot would not start it." >&2
        exit 3
    fi
    echo "flash guard: signed with a key accepted for $whose ($hash)"
fi

if [[ -n $dry ]]; then
    echo "would flash $image"
    exit 0
fi
command -v picotool >/dev/null || { echo "picotool not found" >&2; exit 1; }
# -f: reboot a running board into BOOTSEL for the load. -v: verify. -x: run it afterwards.
# The .bin is what was checked above, so it is what is loaded; flash is mapped at 0x10000000.
# $SOS_FLASH_SERIAL picks one board when several are connected.
ser=()
[[ -n ${SOS_FLASH_SERIAL:-} ]] && ser=(--ser "$SOS_FLASH_SERIAL")
# picotool wants the file first, then its type and offset, then the options that pick and
# reboot the device, together at the end.
picotool load -v -x "$image" -t bin -o 0x10000000 -f "${ser[@]}"
