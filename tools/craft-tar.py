#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Crafted tar fixtures for tests/test_tar_parse.c and tests/test_tar.c: what no tar writes on
# purpose, made with python3's tarfile (and a few header bytes changed by hand). Called by
# tools/make-archive-fixtures.sh; run alone as
#
#   python3 -I tools/craft-tar.py tests/data/archives/tar
#
# evil.tar is also the archive for the board test of `tar xf`'s safety rules (§6.3 of
# docs/design/archives.md): copied to the card over SFTP, it must create only the safe entries.

import calendar
import io
import os
import sys
import tarfile

OUT = sys.argv[1]
MTIME = calendar.timegm((2026, 3, 4, 5, 6, 7))


def entry(name, data=b"", type_=tarfile.REGTYPE, link=""):
    ti = tarfile.TarInfo(name)
    ti.type = type_
    ti.size = len(data) if type_ in (tarfile.REGTYPE, tarfile.AREGTYPE) else 0
    ti.mtime = MTIME
    ti.mode = 0o755 if type_ == tarfile.DIRTYPE else 0o644
    ti.linkname = link
    return ti, data


def write(name, entries, fmt=tarfile.GNU_FORMAT, pax_headers=None):
    path = os.path.join(OUT, name)
    with tarfile.open(path, "w", format=fmt, pax_headers=pax_headers) as tf:
        for ti, data in entries:
            tf.addfile(ti, io.BytesIO(data) if ti.size else None)
    return path


def checksum(h, signed=False):
    h[148:156] = b" " * 8
    s = sum((b - 256 if b > 127 else b) if signed else b for b in h)
    h[148:156] = b"%06o\0 " % (s & 0o777777)


def blocks(path):
    with open(path, "rb") as f:
        return bytearray(f.read())


def save(path, data):
    with open(path, "wb") as f:
        f.write(data)


# Names that must be refused (or normalised), links and devices, a case clash, a name given
# twice: the safe entries are ok/x/y.txt, abs.txt, same.txt (the second one), Readme.txt and
# certs/evil.pem (the console only).
write("evil.tar", [
    entry("../evil.txt", b"evil\n"),
    entry("/abs.txt", b"abs\n"),
    entry("ok/../../up.txt", b"up\n"),
    entry("ok/./x//y.txt", b"y\n"),
    entry("bad:name.txt", b"colon\n"),
    entry("trail.", b"dot\n"),
    entry("ctl\x01.txt", b"control\n"),
    entry("back\\..\\slash.txt", b"backslash\n"),
    entry("Readme.txt", b"first\n"),
    entry("README.TXT", b"second\n"),
    entry("same.txt", b"one\n"),
    entry("same.txt", b"two\n"),
    entry("hl", type_=tarfile.LNKTYPE, link="same.txt"),
    entry("sl", type_=tarfile.SYMTYPE, link="/etc/passwd"),
    entry("cdev", type_=tarfile.CHRTYPE),
    entry("bdev", type_=tarfile.BLKTYPE),
    entry("fifo", type_=tarfile.FIFOTYPE),
    entry("a/" + "b" * 120 + "/../../long-up.txt", b"long\n"),     # a GNU 'L' name
    entry("certs/evil.pem", b"not a certificate\n"),
])

# The same tricks in pax path and linkpath records.
write("pax-evil.tar", [
    entry("../pax-up.txt", b"up\n"),
    entry("/" + "p" * 120 + "/pax-abs.txt", b"abs\n"),
    entry("sl", type_=tarfile.SYMTYPE, link="../" + "q" * 120),
    entry("pax-ok.txt", b"ok\n"),
], fmt=tarfile.PAX_FORMAT)

# Too long: a component of 256 bytes (a pax path; FAT holds 255), a path past TAR_NAME_MAX
# (refused by the parser), and pax records over 4 KB (a long comment). ok.txt last.
write("toolong.tar", [
    entry("c" * 256 + ".txt", b"x\n"),
    entry("/".join(["d" * 200] * 4), b"x\n"),
    entry("ok.txt", b"ok\n"),
], fmt=tarfile.PAX_FORMAT)
big = tarfile.TarInfo("big-pax.txt")
big.size, big.mtime, big.pax_headers = 3, MTIME, {"comment": "c" * 5000}
with tarfile.open(os.path.join(OUT, "paxbig.tar"), "w", format=tarfile.PAX_FORMAT) as tf:
    tf.addfile(big, io.BytesIO(b"x\n\n"))
    ok, data = entry("ok.txt", b"ok\n")
    tf.addfile(ok, io.BytesIO(data))

# A pax global header ('g') whose path and comment apply to nothing, then a long name ('x').
write("global.tar", [entry("g/" + "n" * 150 + ".txt", b"global\n")], fmt=tarfile.PAX_FORMAT,
      pax_headers={"comment": "hello", "path": "ignored.txt"})

# A size in base-256 (GNU's form for files over 8 GB), here for a small file.
p = write("b256.tar", [entry("b256.txt", b"base-256 size\n")], fmt=tarfile.USTAR_FORMAT)
d = blocks(p)
h = d[0:512]
h[124:136] = b"\x80" + (14).to_bytes(11, "big")
checksum(h)
d[0:512] = h
save(p, d)

# A checksum as the signed sum (old tars), on a name with bytes over 127 (UTF-8).
p = write("signed.tar", [entry("café.txt", b"signed\n")], fmt=tarfile.USTAR_FORMAT)
d = blocks(p)
h = d[0:512]
checksum(h, signed=True)
d[0:512] = h
save(p, d)
