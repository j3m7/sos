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
        printf '%b' "\\x${hex:i:2}"
    done
}
rep() { local i; for ((i = 0; i < $2; i++)); do printf '%s' "$1"; done; }

start="d3deffff" imagedef="42012110" end="00000000 793512ab"
# An unsigned image: one block with only the image definition.
bytes $start $imagedef "ff010000" "$end" > "$tmp/unsigned.bin"
# Signed: the same with a SIGNATURE item (0x21 words: header, 64 key bytes, 64 signature bytes).
signed() {      # signed <key byte> <file>
    bytes $start $imagedef "09210001" "$(rep "$1" 64)" "$(rep ee 64)" "ff220000" "$end" > "$2"
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
    out=$(env -u SOS_FLASH_UNGUARDED XDG_CONFIG_HOME="$tmp/noconfig" SOS_FLASH_SERIAL=$board \
          "${env[@]}" \
          "$flash" -n -w "$tmp/work" "$1" 2>&1) || status=$?
    if [[ $status -ne $want || $out != *"$text"* ]]; then
        echo "FAIL: $what: status $status (wanted $want), output:"
        echo "$out" | sed 's/^/    /'
        failed=$((failed + 1))
    fi
}

list() { printf '%s\n' "$@" > "$tmp/keys"; }
board=AAAAAAAAAAAAAAAA          # the board named in every run unless a run says otherwise
other=BBBBBBBBBBBBBBBB

# No list: nothing is checked, anything goes, and it says so.
run 0 "flash guard: off" "no list, unsigned image" SOS_FLASH_KEYS="$tmp/absent" -- "$tmp/unsigned.bin"
run 0 "flash guard: off" "no list in the default place" -- "$tmp/key1.bin"

# The default place is ${XDG_CONFIG_HOME}/sos/flash-keys.
mkdir -p "$tmp/config/sos"
printf '%s\n' "$hash1" > "$tmp/config/sos/flash-keys"
run 0 "accepted for every board ($hash1)" "list in the default place" XDG_CONFIG_HOME="$tmp/config" -- "$tmp/key1.bin"
run 3 "REFUSED" "list in the default place refuses" XDG_CONFIG_HOME="$tmp/config" -- "$tmp/key2.bin"

# One key listed.
list "$hash1"
run 0 "($hash1)" "listed key" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
run 3 "is not signed" "unsigned image with a list" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/unsigned.bin"
run 3 "$hash2" "another key: refused, its hash shown" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key2.bin"
run 3 "doesn't look like a firmware image" "not an image" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/zeros.bin"

# Two keys, with comments, blank lines, upper case and a comment after a hash.
list "# the boards on this machine" "" "${hash1^^}   # in use" "  $hash2" "#$hash2"
run 0 "accepted for" "first of two" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
run 0 "accepted for" "second of two" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key2.bin"
run 3 "is not signed" "unsigned with two keys listed" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/unsigned.bin"

# A list that can't be trusted stops everything, signed or not.
list "$hash1" "not a hash"
run 1 "line 2 is not a key hash" "a bad line" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "${hash1:0:63}"
run 1 "is not a key hash" "a short hash" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "${hash1}0"
run 1 "is not a key hash" "a long hash" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "# nothing yet"
run 3 "lists no key" "an empty list accepts nothing" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

# A hash that differs in one digit is another key.
last=${hash1: -1}; digit=0; [[ $last == 0 ]] && digit=1
list "${hash1%?}$digit"
run 3 "REFUSED" "one digit off" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

# The way around it is explicit.
list "$hash1"
run 0 "skipped" "SOS_FLASH_UNGUARDED" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_UNGUARDED=1 -- "$tmp/unsigned.bin"

# With a list the board must be named, whatever the image; without a list it need not be.
list "$hash1"
run 3 "Name the board" "no serial with a list" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL= -- "$tmp/key1.bin"
run 3 "Name the board" "a serial that isn't one" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=dev-1 -- "$tmp/key1.bin"
run 0 "flash guard: off" "no serial and no list" SOS_FLASH_KEYS="$tmp/absent" SOS_FLASH_SERIAL= -- "$tmp/key1.bin"
run 0 "($hash1)" "a lower-case serial" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=aaaaaaaaaaaaaaaa -- "$tmp/key1.bin"

# Keys per board: a board takes its own section's keys and only those.
list "[$other]   # one board" "$hash1" "[$board]   # this one" "$hash2"
run 0 "accepted for board $board ($hash2)" "the board's own key" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key2.bin"
run 3 "doesn't list for board $board" "another board's key is not this board's" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
run 0 "accepted for board $other ($hash1)" "the other board takes its own" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=$other -- "$tmp/key1.bin"
run 3 "REFUSED" "and not this board's" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=$other -- "$tmp/key2.bin"
run 3 "is not signed" "unsigned for a board with a section" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/unsigned.bin"
run 0 "accepted for board" "the section is found whatever the case" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=${board,,} -- "$tmp/key2.bin"

# Once there are sections, a board without one is refused, whatever the image: the list is
# then a list of the boards this machine flashes, and this isn't one of them.
list "[$other]" "$hash1"
run 3 "Board $board has no section" "an unlisted board" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
run 3 "Board $board has no section" "an unlisted board, unsigned image" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/unsigned.bin"
# A hash before the first section would belong to no board.
list "$hash1" "[$board]" "$hash2"
run 1 "before its first [serial] line" "a hash outside the sections" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key2.bin"
# A section with nothing in it accepts nothing.
list "[$board]"
run 3 "lists no key for board $board" "an empty section" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

# An open board takes anything; that is said, and only for that board.
list "[$other]" "$hash1" "[$board]" "open   # the second development board"
run 0 "listed as open" "an open board, unsigned image" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/unsigned.bin"
run 0 "listed as open" "an open board, any key" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key2.bin"
run 3 "is not signed" "the other board is not open" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=$other -- "$tmp/unsigned.bin"

# Mistakes in the sections stop everything.
list "open"
run 1 "is not a key hash" "'open' outside a section" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "[$board]" "open" "$hash1"
run 1 "can't be both" "open and keys" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "[$board]" "[dev-1]"
run 1 "line 2 is not a key hash" "a section that isn't a serial" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"
list "[$board]" "$hash1" "[$other]" "nonsense"
run 1 "line 4 is not a key hash" "a bad line in another board's section" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

# An image signed with a public development key: a warning for any board that isn't a
# development board, still flashed; nothing extra for dev-1. The keys here are the public
# halves of keys/dev-INSECURE-1.pem and -2.pem.
dev1=F94525FD61D9A1CC
devkey1=0c66c643cee65d14a55fd8cf2142a1fab2a769d555238f551b4836af46d61d1779efb62425ec5d3586fe42a473bdc5bd24b16df92d074d127b7301651b867124
devkey2=7c8778fd06d7f8faaaf4912910b441397aba3f9af7ba6f5fd05a819b4a2d03bba5c3a916fd26ccf8d92a44d11d170f30ba142997583df93ab0223710ecf8ea5d
bytes $start $imagedef "09210001" "$devkey1" "$(rep ee 64)" "ff220000" "$end" > "$tmp/dev1.bin"
bytes $start $imagedef "09210001" "$devkey2" "$(rep ee 64)" "ff220000" "$end" > "$tmp/dev2.bin"
devhash1=e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51
devhash2=b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910
list "$devhash1" "$devhash2" "$hash1"
run 0 "WARNING. $tmp/dev1.bin is signed with a PUBLIC" "dev key, another board" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/dev1.bin"
run 0 "and board $board is not a development board" "second dev key, another board" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/dev2.bin"
run 0 "would flash" "dev key, another board: still flashed" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/dev1.bin"
run 0 "($devhash1)" "dev key on dev-1" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=$dev1 -- "$tmp/dev1.bin"
run 0 "($devhash2)" "second dev key on dev-1, lower-case serial" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=${dev1,,} -- "$tmp/dev2.bin"
# no_warning <description> [VAR=value...] -- <image> : accepted, and no warning in the output.
no_warning() {
    local what=$1 out
    shift
    local env=()
    while [[ $1 != -- ]]; do env+=("$1"); shift; done
    shift
    out=$(env -u SOS_FLASH_UNGUARDED XDG_CONFIG_HOME="$tmp/noconfig" SOS_FLASH_SERIAL=$board \
          "${env[@]}" "$flash" -n -w "$tmp/work" "$1" 2>&1)
    if [[ $? -ne 0 || $out == *WARNING* ]]; then
        echo "FAIL: $what: warned or refused:"
        echo "$out" | sed 's/^/    /'
        failed=$((failed + 1))
    fi
}
no_warning "no warning on dev-1" SOS_FLASH_KEYS="$tmp/keys" SOS_FLASH_SERIAL=$dev1 -- "$tmp/dev1.bin"
no_warning "no warning for another key" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

# With -n nothing is flashed, and it says what it would do.
list "$hash1"
run 0 "would flash" "dry run" SOS_FLASH_KEYS="$tmp/keys" -- "$tmp/key1.bin"

if [[ $failed -eq 0 ]]; then
    echo "flash guard: all checks passed"
fi
exit "$failed"
