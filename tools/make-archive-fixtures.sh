#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# The archive fixtures in tests/data/archives/, made with GNU gzip (and python3 for random
# bytes): what tests/test_gz.c and tests/test_gzip.c decode (src/files/gz.c, gunzip, zcat), and
# the seeds of the gz fuzz target (tests/fuzz/seeds.py reads them). docs/design/archives.md §7.
# The tar fixtures in tests/data/archives/tar/, made with GNU tar and bsdtar when installed (each
# skipped with a note when not) and python3's tarfile for what tar won't write: what
# tests/test_tar_parse.c and tests/test_tar.c read (src/files/tar_parse.c, tar).
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

# --- tar ---------------------------------------------------------------------------------------

tout=$out/tar
rm -rf "$tout"
mkdir -p "$tout"

# The tree: a text file over two 4 KB buffers, random bytes, an empty file in a subdirectory, and
# a path of 145 bytes that ustar holds with its prefix field. long/ adds a name of 155 bytes in
# one component, which only GNU's 'L' record and pax's path hold. One date for everything.
t=$tmp/t
mkdir -p "$t/dir/sub" "$t/dir/d$(printf 'x%.0s' {1..88})" "$t/long"
head -c 10000 "$out/text.txt" > "$t/dir/lines.txt"
head -c 3000 "$out/random.bin" > "$t/dir/random.bin"
: > "$t/dir/sub/empty"
deep=dir/d$(printf 'x%.0s' {1..88})/f$(printf 'y%.0s' {1..48}).txt
echo deep > "$t/$deep"
echo long > "$t/long/l$(printf 'z%.0s' {1..150}).txt"
find "$t" -exec touch -h -d '2026-03-04 05:06:07 UTC' {} +

gnutar=(tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-03-04 05:06:07 UTC'
        -C "$t")
if tar --version 2>/dev/null | grep -q 'GNU tar'; then
    "${gnutar[@]}" --format=ustar -cf "$tout/ustar.tar" dir
    "${gnutar[@]}" --format=gnu -cf "$tout/gnu.tar" dir long
    "${gnutar[@]}" --format=pax --pax-option=delete=atime,delete=ctime -cf "$tout/pax.tar" dir long
    "${gnutar[@]}" --format=v7 -cf "$tout/v7.tar" dir/lines.txt dir/sub dir/random.bin

    # Links and a FIFO (mkfifo needs no root; devices are crafted below).
    mkdir -p "$tmp/l/dir"
    cp "$t/dir/lines.txt" "$tmp/l/dir/lines.txt"
    ln "$tmp/l/dir/lines.txt" "$tmp/l/dir/hard"
    ln -s lines.txt "$tmp/l/dir/sym"
    mkfifo "$tmp/l/dir/fifo"
    tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='2026-03-04 05:06:07 UTC' \
        --format=gnu -C "$tmp/l" -cf "$tout/links.tar" dir

    # A date before 1970, which GNU writes in base-256.
    mkdir -p "$tmp/o"
    echo old > "$tmp/o/old.txt"
    tar --owner=0 --group=0 --numeric-owner --mtime='1901-01-01 00:00:00 UTC' --format=gnu \
        -C "$tmp/o" -cf "$tout/old.tar" old.txt

    # Gzipped, and damaged: cut in the middle of a file's data, the end blocks missing, a
    # header's byte changed, the gzip cut.
    gzip -9 -n -c "$tout/ustar.tar" > "$tout/ustar.tgz"
    gzip -9 -n -c "$tout/gnu.tar" > "$tout/gnu.tar.gz"
    head -c 6000 "$tout/ustar.tar" > "$tout/cut.tar"
    # The data ends where the end's zero blocks (and GNU's padding to 10 KB) start.
    end=$(python3 -I -c 'import sys
d = open(sys.argv[1], "rb").read()
n = len(d)
while n >= 512 and d[n - 512:n] == bytes(512):
    n -= 512
print(n)' "$tout/ustar.tar")
    head -c "$end" "$tout/ustar.tar" > "$tout/noend.tar"
    cp "$tout/ustar.tar" "$tout/badsum.tar"
    flip "$tout/badsum.tar" 1024
    gsize=$(stat -c %s "$tout/ustar.tgz")
    head -c $((gsize / 2)) "$tout/ustar.tgz" > "$tout/cut.tgz"
else
    echo "make-archive-fixtures: GNU tar not installed: its tar fixtures not made" >&2
fi

if command -v bsdtar >/dev/null; then
    bsdtar=(bsdtar --uid 0 --gid 0 --numeric-owner --no-xattrs --no-acls --no-fflags -C "$t")
    "${bsdtar[@]}" --format=pax -cf "$tout/bsd-pax.tar" dir long
    "${bsdtar[@]}" --format=ustar -cf "$tout/bsd-ustar.tar" dir
else
    echo "make-archive-fixtures: bsdtar not installed: its tar fixtures not made" >&2
fi

# What no tar writes on purpose (python3's tarfile): names that must be refused, devices,
# a case clash, a pax global header, a base-256 size, a signed checksum. tools/craft-tar.py
# documents each.
python3 -I "$(pwd)/tools/craft-tar.py" "$tout"

ls -l "$out" "$tout"
