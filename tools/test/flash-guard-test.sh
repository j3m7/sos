#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Test of the guard in tools/flash.sh: which images it accepts for which key lists. It runs
# flash.sh with -n (check only), so no board and no picotool are needed. The images are a few
# bytes built here from the block format (RP2350 datasheet section 5.9): one unsigned, two
# signed with made-up keys. Exits with the number of failed checks.
#
# Usage: tools/test/flash-guard-test.sh

set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
flash=$root/tools/flash.sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failed=0

# bytes <hex digits...> : write those bytes to standard output.
bytes() {
    local hex="$*" i
    hex=${hex// /}
    for ((i = 0; i < ${#hex}; i += 2)); do
        printf "\\x${hex:i:2}"
    done
}
rep() { local i; for ((i = 0; i < $2; i++)); do printf '%s' "$1"; done; }

start="d3deffff" imagedef="42012110" end="00000000 793512ab"
# An unsigned image: one block with only the image definition.
bytes $start $imagedef "ff010000" $end > "$tmp/unsigned.bin"
# Signed: the same with a SIGNATURE item (0x21 words: header, 64 key bytes, 64 signature bytes).
signed() {      # signed <key byte> <file>
    bytes $start $imagedef "09210001" "$(rep "$1" 64)" "$(rep ee 64)" "ff220000" $end > "$2"
}
signed 11 "$tmp/key1.bin"
signed 22 "$tmp/key2.bin"
hash1=$(bytes "$(rep 11 64)" | sha256sum); hash1=${hash1%% *}
hash2=$(bytes "$(rep 22 64)" | sha256sum); hash2=${hash2%% *}
head -c 64 /dev/zero > "$tmp/zeros.bin"

# run <expected status> <text expected in the output> <description> [VAR=value...] -- <image>
run() {
    local want=$1 text=$2 what=$3 out status=0
    shift 3
    local env=()
    while [[ $1 != -- ]]; do env+=("$1"); shift; done
    shift
    out=$(env -u SOS_FLASH_UNGUARDED XDG_CONFIG_HOME="$tmp/noconfig" "${env[@]}" \
          "$flash" -n -w "$tmp/work" "$1" 2>&1) || status=$?
    if [[ $status -ne $want || $out != *"$text"* ]]; then
        echo "FAIL: $what: status $status (wanted $want), output:"
        echo "$out" | sed 's/^/    /'
        failed=$((failed + 1))
    fi
}

list() { printf '%s\n' "$@" > "$tmp/keys"; }

# No list: nothing is checked, anything goes, and it says so.
run 0 "flash guard: off" "no list, unsigned image" SOS_FLASH_KEYS="$tmp/absent" -- "$tmp/unsigned.bin"
run 0 "flash guard: off" "no list in the default place" -- "$tmp/key1.bin"

# The default place is ${XDG_CONFIG_HOME}/sos/flash-keys.
mkdir -p "$tmp/config/sos"
printf '%s\n' "$hash1" > "$tmp/config/sos/flash-keys"
run 0 "accepted key ($hash1)" "list in the default place" XDG_CONFIG_HOME="$tmp/config" -- "$tmp/key1.bin"
run 3 "REFUSED" "list in the default place refuses" XDG_CONFIG_HOME="$tmp/config" -- "$tmp/key2.bin"

# One key listed.
list "$hash1"
run 0 "accepted key ($hash1)" "listed key" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
run 3 "is not signed" "unsigned image with a list" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/unsigned.bin"
run 3 "$hash2" "another key: refused, its hash shown" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key2.bin"
run 3 "doesn't look like a firmware image" "not an image" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/zeros.bin"

# Two keys, with comments, blank lines, upper case and a comment after a hash.
list "# the boards on this machine" "" "${hash1^^}   # in use" "  $hash2" "#$hash2"
run 0 "accepted key" "first of two" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
run 0 "accepted key" "second of two" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key2.bin"
run 3 "is not signed" "unsigned with two keys listed" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/unsigned.bin"

# A list that can't be trusted stops everything, signed or not.
list "$hash1" "not a hash"
run 1 "line 2 isn't a key hash" "a bad line" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "${hash1:0:63}"
run 1 "isn't a key hash" "a short hash" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "${hash1}0"
run 1 "isn't a key hash" "a long hash" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "# nothing yet"
run 3 "lists no key" "an empty list accepts nothing" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

# A hash that differs in one digit is another key.
last=${hash1: -1}; other=0; [[ $last == 0 ]] && other=1
list "${hash1%?}$other"
run 3 "REFUSED" "one digit off" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

# The way around it is explicit.
list "$hash1"
run 0 "skipped" "SOS_FLASH_UNGUARDED" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_UNGUARDED=1 -- "$tmp/unsigned.bin"

# With -n nothing is flashed, and it says what it would do.
run 0 "would flash" "dry run" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

if [[ $failed -eq 0 ]]; then
    echo "flash guard: all checks passed"
fi
exit "$failed"
