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
# No list: no guard, and the image is flashed as before (this says so). The list is about the
# boards this machine flashes, all of them: with two boards that accept different keys, list
# both. SOS_FLASH_UNGUARDED=1 flashes without the check, for an open board that takes anything.
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
    # The accepted hashes, lower case. A line that isn't a comment and doesn't start with 64
    # hex digits is a mistake in the list, and a list with a mistake is not trusted.
    accepted=()
    lineno=0
    while IFS= read -r line || [[ -n $line ]]; do
        lineno=$((lineno + 1))
        line=${line%%#*}
        [[ $line =~ ^[[:space:]]*$ ]] && continue
        if [[ $line =~ ^[[:space:]]*([0-9a-fA-F]{64})([[:space:]].*)?$ ]]; then
            accepted+=("${BASH_REMATCH[1],,}")
        else
            echo "flash guard: $keys line $lineno isn't a key hash (64 hex digits)" >&2
            exit 1
        fi
    done < "$keys"
    if [[ ${#accepted[@]} -eq 0 ]]; then
        echo "flash guard: $keys lists no key, so no image can be accepted. Add a hash, or" >&2
        echo "             remove the file to turn the guard off" >&2
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
        echo "  This machine's key list ($keys) says its boards take signed firmware only." >&2
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
        echo "flash guard: REFUSED. $image is signed with a key that isn't in $keys:" >&2
        echo "  $hash" >&2
        echo "  A board with secure boot would not start it." >&2
        exit 3
    fi
    echo "flash guard: signed with an accepted key ($hash)"
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
