#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# The archive fixtures in tests/data/archives/, made with GNU gzip (and python3 for random
# bytes): what tests/test_gz.c and tests/test_gzip.c decode (src/files/gz.c, gunzip, zcat), and
# the seeds of the gz fuzz target (tests/fuzz/seeds.py reads them). docs/design/archives.md §7.
#
#   tools/make-archive-fixtures.sh
#
# The files are committed; run this again only to change them. gzip -n (no name, date 0) keeps
# the output the same from one run to the next, except name.txt.gz, which has a name and a fixed
# date on purpose. Another gzip version may compress differently: the tests check what the files
# decode to, not their bytes.

set -euo pipefail
cd "$(dirname "$0")/.."

out=tests/data/archives
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Overwrite the byte at `offset` (from the start, or from the end if negative) with its
# complement.
flip() {
    local file=$1 offset=$2 size byte
    size=$(stat -c %s "$file")
    ((offset < 0)) && offset=$((size + offset))
    byte=$(od -An -tu1 -j "$offset" -N1 "$file" | tr -d ' ')
    printf '%b' "\\0$(printf '%03o' $((byte ^ 255)))" |
        dd of="$file" bs=1 seek="$offset" conv=notrunc status=none
}

# The data: 6000 numbered lines (about 29 KB), compressible, more than one 4 KB buffer.
seq 1 6000 > "$out/text.txt"
gzip -9 -n -c "$out/text.txt" > "$out/text.txt.gz"

# A header with the original name and a date (gunzip ignores both).
cp "$out/text.txt" "$tmp/name.txt"
touch -d '2026-01-01 00:00:00 UTC' "$tmp/name.txt"
gzip -9 -c "$tmp/name.txt" > "$out/name.txt.gz"

# Three members, the middle one empty: decodes to text.txt.
head -c 14000 "$out/text.txt" > "$tmp/a"
tail -c +14001 "$out/text.txt" > "$tmp/b"
: > "$tmp/empty"
{ gzip -n -c "$tmp/a"; gzip -n -c "$tmp/empty"; gzip -n -c "$tmp/b"; } > "$out/multi.gz"

# Block padding: zeros up to the next 10 KB block (at least 512): decodes to text.txt, with
# "ignoring trailing zeros".
cp "$out/text.txt.gz" "$out/zeros.gz"
head -c 512 /dev/zero >> "$out/zeros.gz"
truncate -s %10240 "$out/zeros.gz"

# Bytes after the last member that aren't another member: errors.
{ cat "$out/text.txt.gz"; printf 'junk\n'; } > "$out/junk.gz"
{ cat "$out/text.txt.gz"; head -c 100 /dev/zero; printf 'x'; } > "$out/zerosjunk.gz"
{ cat "$out/text.txt.gz"; printf '\037\213XYZ'; } > "$out/badmember.gz"

# Damaged: the CRC-32, the length, a byte of the data, the end cut off.
cp "$out/text.txt.gz" "$out/badcrc.gz"
flip "$out/badcrc.gz" -8
cp "$out/text.txt.gz" "$out/badlen.gz"
flip "$out/badlen.gz" -4
cp "$out/text.txt.gz" "$out/baddata.gz"
flip "$out/baddata.gz" 2000
size=$(stat -c %s "$out/text.txt.gz")
head -c $((size - 5)) "$out/text.txt.gz" > "$out/truncated.gz"
head -c $((size / 2)) "$out/text.txt.gz" > "$out/truncated-half.gz"

# Empty: no bytes at all (an error), and a member with no data (fine: an empty file).
: > "$out/empty.gz"
gzip -n -c "$tmp/empty" > "$out/emptydata.gz"

# Not gzip at all.
head -c 1000 "$out/text.txt" > "$out/notgz.gz"

# Stored blocks: 20000 random bytes (seeded, so the same each run), which gzip can't shrink and
# stores as they are.
python3 -c 'import random, sys
r = random.Random(265)
sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(20000)))' > "$out/random.bin"
gzip -9 -n -c "$out/random.bin" > "$out/random.bin.gz"

# 1 MB of zeros in about 1 KB (a small "zip bomb"): output bounded per call whatever the input.
head -c 1048576 /dev/zero | gzip -9 -n -c > "$out/zeros1m.gz"

ls -l "$out"
