#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Writes the seed inputs in tests/fuzz/corpus/<target>/seed-*: a few well-formed inputs per
# fuzz target, so fuzzing starts from the real formats rather than from nothing. Each seed is
# laid out as its fuzz_<target>.c reads input. Run it again after changing a format; inputs
# that once crashed a target are kept there under other names (crash-*) and not touched.
#
#   python3 -B tests/fuzz/seeds.py      (-B: no __pycache__ next to it)

import os
import struct
import subprocess
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def write(target, name, data):
    d = os.path.join(HERE, "corpus", target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "seed-" + name), "wb") as f:
        f.write(data)


# --- http: [flags][max_body][piece-1] then the response (the whole input is also a URL) ---------

def http():
    responses = {
        "length": b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello",
        "chunked": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   b"5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\nX-Trailer: 1\r\n\r\n",
        "head": b"HTTP/1.1 301 Moved\r\nLocation: https://letsencrypt.org/\r\nContent-Length: 99\r\n\r\n",
        "to-close": b"HTTP/1.0 200 OK\r\nServer: x\r\n\r\nbody runs to the end",
    }
    for name, r in responses.items():
        flags = 1 if name == "head" else 2
        write("http", name, bytes([flags, 64, 7]) + r)
    write("http", "url", b"https://user@example.org:8443/a/b?c=d#e")


# --- nts_ke: the server's records --------------------------------------------------------------

def rec(type_, body, critical=False):
    return struct.pack(">HH", type_ | (0x8000 if critical else 0), len(body)) + body


def nts_ke():
    cookies = b"".join(rec(5, bytes([i]) * 100) for i in range(8))
    write("nts_ke", "basic", rec(1, b"\x00\x00", True) + rec(4, b"\x00\x0f") + cookies +
          rec(0, b"", True))
    write("nts_ke", "server-port", rec(1, b"\x00\x00", True) + rec(4, b"\x00\x0f") +
          rec(5, b"c" * 64) + rec(6, b"ntp.example.org") + rec(7, b"\x00\x7b") +
          rec(0, b"", True))
    write("nts_ke", "warning", rec(1, b"\x00\x00", True) + rec(4, b"\x00\x0f") +
          rec(5, b"c" * 64) + rec(3, b"\x00\x01") + rec(0, b"", True))
    write("nts_ke", "error", rec(2, b"\x00\x01", True) + rec(0, b"", True))


# --- nts_ntp: [uid 32][xmt 8] then the reply (the harness's AEAD: 16-byte tag, then plaintext) --

def ef(type_, body):
    total = 4 + ((len(body) + 3) & ~3)
    return struct.pack(">HH", type_, total) + body + b"\0" * (total - 4 - len(body))


def nts_ntp():
    uid = bytes(range(0x40, 0x60))
    xmt = bytes([0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4])
    hdr = bytearray(48)
    hdr[0] = (4 << 3) | 4                         # NTPv4, server
    hdr[1] = 3
    hdr[12:16] = b"\x0a\x00\x00\x01"
    hdr[24:32] = xmt                               # origin = our transmit
    hdr[32:40] = bytes([0xed, 0, 0, 1, 0x80, 0, 0, 0])
    hdr[40:48] = bytes([0xed, 0, 0, 1, 0x80, 0x10, 0, 0])
    for n in (0, 1, 8):
        plain = b"".join(ef(0x0204, bytes([0xc0 + i]) * 96) for i in range(n))
        ct = b"\0" * 16 + plain
        nonce = bytes([7, 7, 7]) + b"\0" * 13
        body = struct.pack(">HH", len(nonce), len(ct)) + nonce + ct
        body += b"\0" * ((4 - len(body) % 4) % 4)
        auth = struct.pack(">HH", 0x0404, 4 + len(body)) + body
        write("nts_ntp", f"cookies-{n}", uid + xmt + bytes(hdr) + ef(0x0104, uid) + auth)


# --- certbundle: PEM files ---------------------------------------------------------------------

def certbundle():
    with tempfile.TemporaryDirectory() as t:
        key, crt = os.path.join(t, "k.pem"), os.path.join(t, "c.pem")
        pems = []
        for curve in ("prime256v1", "secp384r1"):
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                            f"ec_paramgen_curve:{curve}", "-nodes", "-keyout", key, "-out", crt,
                            "-subj", f"/CN=SOS fuzz seed {curve}", "-days", "1"],
                           check=True, capture_output=True)
            with open(crt, "rb") as f:
                pems.append(f.read())
    write("certbundle", "one", pems[0])
    write("certbundle", "two-crlf", (pems[0] + b"\n# a comment\n" + pems[1]).replace(b"\n", b"\r\n"))


# --- config: per slot [selector][seq 4] then the payload (see fuzz_config.c) --------------------

def config():
    full = 8                                       # record_lengths index: the current length
    payload = bytearray(4096)                       # all fields empty: a valid record
    write("config", "empty", bytes([full]) + struct.pack(">I", 1) + bytes(payload))
    p = bytearray(payload)
    p[0:4] = b"wifi"                                # the first field: the SSID
    write("config", "two-slots", bytes([full]) + struct.pack(">I", 2) + bytes(p[:3000]) +
          bytes([0]) + struct.pack(">I", 1) + bytes(payload[:1200]))


# --- ping: [id 2] then the IPv4 packet ----------------------------------------------------------

def checksum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(f">{len(b) // 2}H", b))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def ip(payload, ttl=64):
    h = bytearray(struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0, ttl, 1, 0,
                              bytes([192, 168, 1, 1]), bytes([192, 168, 1, 89])))
    h[10:12] = struct.pack(">H", checksum(bytes(h)))
    return bytes(h) + payload


def icmp(type_, rest, data):
    m = bytearray(struct.pack(">BBH", type_, 0, 0) + rest + data)
    m[2:4] = struct.pack(">H", checksum(bytes(m)))
    return bytes(m)


def ping():
    ident = 0x1234
    data = bytes(ord("a") + i % 26 for i in range(32))
    request = icmp(8, struct.pack(">HH", ident, 7), data)
    write("ping", "reply", struct.pack(">H", ident) + ip(icmp(0, struct.pack(">HH", ident, 7), data)))
    quoted = ip(request)[:28]                       # its IP header and 8 bytes of ICMP
    write("ping", "unreachable", struct.pack(">H", ident) + ip(icmp(3, b"\0" * 4, quoted)))
    write("ping", "ttl", struct.pack(">H", ident) + ip(icmp(11, b"\0" * 4, quoted)))


# --- terminal: [piece size] then bytes from a terminal ------------------------------------------

def terminal():
    write("terminal", "keys", b"\x03ls -l\r\x1b[A\x1b[1;5C\x1b[3~\x1bOP\x1b[200~paste\x1b[201~\x1bx\x7f")
    write("terminal", "text", b"\x05caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80 \xc3 \x1b]0;title\x07\r\n\xed\xa0\x80")


# --- fsck: [selector] then a sparse image (fsck_image.h): [sectors] then [lba][offset][len][bytes] --

def lfn_entries(name, sfn):
    """The long-name entries for `name`, in the order they are stored (last part first)."""
    s = 0
    for b in sfn:
        s = (((s >> 1) | (s << 7)) + b) & 0xFF
    units = [ord(c) for c in name] + [0]
    units += [0xFFFF] * (-len(units) % 13)
    parts = [units[i:i + 13] for i in range(0, len(units), 13)]
    if len(name) % 13 == 0:
        parts = parts[:-1]                         # a full last part needs no terminator
    out = []
    for i in reversed(range(len(parts))):
        u = parts[i]
        e = bytearray(32)
        e[0] = (i + 1) | (0x40 if i == len(parts) - 1 else 0)
        e[11], e[13] = 0x0F, s
        for k, off in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
            e[off:off + 2] = struct.pack("<H", u[k])
        out.append(bytes(e))
    return out


def dirent(sfn, attr, clus, size, nt=0):
    return sfn + struct.pack("<BBBHHHHHHHI", attr, nt, 0, 0, 0, 0, clus >> 16, 0, 0,
                             clus & 0xFFFF, size)


def fat32_image(sectors, part_lba, nfats=1, spc=1, layout="mbr"):
    """A small FAT32 volume laid out as f_mkfs lays one out: files, a long name and a
    subdirectory. Returns {lba: bytearray(512)} for the sectors that aren't all zeros."""
    sec = {}

    def put(lba, off, data):
        s = sec.setdefault(lba, bytearray(512))
        s[off:off + len(data)] = data

    end = sectors - (33 if layout == "gpt" else 0)    # GPT's backup table at the end
    vol, rsv = end - part_lba, 32
    fatsz = 1
    while True:
        clusters = (vol - rsv - nfats * fatsz) // spc
        need = ((clusters + 2) * 4 + 511) // 512
        if need <= fatsz:
            break
        fatsz = need
    assert clusters > 65525
    bpb = bytearray(512)
    bpb[0:11] = b"\xEB\xFE\x90MSDOS5.0"
    struct.pack_into("<HBHBHHBHHHII", bpb, 11, 512, spc, rsv, nfats, 0, 0, 0xF8, 0, 63, 255,
                     part_lba, vol)
    struct.pack_into("<IHHIHH", bpb, 36, fatsz, 0, 0, 2, 1, 6)
    bpb[64], bpb[66] = 0x80, 0x29
    struct.pack_into("<I", bpb, 67, 0x12345678)
    bpb[71:90] = b"NO NAME    FAT32   "
    bpb[510:512] = b"\x55\xAA"
    # Clusters: 2 root, 3 README.TXT, 4-5 the long name, 6 DIR, 7-9 DIR/SUB.BIN.
    chains = {2: [2], 3: [3], 4: [4, 5], 6: [6], 7: [7, 8, 9]}
    used = sum(len(c) for c in chains.values())
    fsi = bytearray(512)
    struct.pack_into("<I", fsi, 0, 0x41615252)
    struct.pack_into("<III", fsi, 484, 0x61417272, clusters - used, 10)
    fsi[510:512] = b"\x55\xAA"
    for off, data in ((0, bpb), (1, fsi), (6, bpb), (7, fsi)):
        put(part_lba + off, 0, data)
    fat = {0: 0x0FFFFFF8, 1: 0x0FFFFFFF}
    for chain in chains.values():
        for a, b in zip(chain, chain[1:] + [0x0FFFFFFF]):
            fat[a] = b
    for copy in range(nfats):
        for c, v in fat.items():
            put(part_lba + rsv + copy * fatsz + c // 128, c % 128 * 4, struct.pack("<I", v))
    data = part_lba + rsv + nfats * fatsz

    def clus(c):
        return data + (c - 2) * spc

    sfn = b"ALONGF~1TXT"
    root = (dirent(b"README  TXT", 0x20, 3, 100) + b"".join(lfn_entries("A long file name.txt", sfn))
            + dirent(sfn, 0x20, 4, 600) + dirent(b"DIR        ", 0x10, 6, 0)
            + dirent(b"NOTES   TXT", 0x20, 0, 0, nt=0x18))
    put(clus(2), 0, root)
    put(clus(6), 0, dirent(b".          ", 0x10, 6, 0) + dirent(b"..         ", 0x10, 0, 0)
        + dirent(b"SUB     BIN", 0x20, 7, 1500))
    for c in (3, 4, 5, 7, 8, 9):
        put(clus(c), 0, b"x")
    if layout == "mbr":
        put(0, 446, struct.pack("<B3sB3sII", 0, b"\0\1\0", 0x0C, b"\xFE\xFF\xFF", part_lba, vol))
        put(0, 510, b"\x55\xAA")
    elif layout == "gpt":
        put(0, 446, struct.pack("<B3sB3sII", 0, b"\0\2\0", 0xEE, b"\xFE\xFF\xFF", 1, sectors - 1))
        put(0, 510, b"\x55\xAA")
        table = bytearray(128 * 128)
        table[0:16] = bytes([0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                             0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7])
        table[16:32] = bytes(range(1, 17))
        struct.pack_into("<QQ", table, 32, part_lba, end - 1)
        for i in range(0, len(table), 512):
            if any(table[i:i + 512]):
                put(2 + i // 512, 0, table[i:i + 512])
        hdr = bytearray(92)
        hdr[0:16] = b"EFI PART\0\0\1\0\x5C\0\0\0"
        struct.pack_into("<QQQQ", hdr, 24, 1, sectors - 1, 34, end - 1)
        hdr[56:72] = bytes(range(0x20, 0x30))
        struct.pack_into("<QIII", hdr, 72, 2, 128, 128, zlib.crc32(table))
        struct.pack_into("<I", hdr, 16, zlib.crc32(hdr))
        put(1, 0, hdr)
    return sec


def sparse(sectors, sec):
    """An image as fsck_image.h reads it: only the non-zero bytes of each sector."""
    out = bytearray(struct.pack("<I", sectors))
    for lba in sorted(sec):
        s = sec[lba]
        nz = [i for i, b in enumerate(s) if b]
        if nz:
            a, b = nz[0], nz[-1] + 1
            out += struct.pack("<IHH", lba, a, b - a) + s[a:b]
    return bytes(out)


def fsck():
    write("fsck", "mbr", b"\0" + sparse(70000, fat32_image(70000, 2048)))
    write("fsck", "sfd-2fat", b"\x25" + sparse(70000, fat32_image(70000, 0, nfats=2, layout="sfd")))
    write("fsck", "gpt", b"\x4a" + sparse(72000, fat32_image(72000, 2048, layout="gpt")))
    exfat = bytearray(512)
    exfat[0:11] = b"\xEB\x76\x90EXFAT   "
    exfat[510:512] = b"\x55\xAA"
    write("fsck", "exfat", b"\0" + sparse(70000, {0: exfat}))


# --- words: a typed command line --------------------------------------------------------------

def words():
    write("words", "quotes", b"echo \"a   b\" 'x|y' a\\ b \"\\\"q\\\\\" a\"b c\"d '' \"\"")
    write("words", "pipes", b"dmesg|grep -i 'no route'|head 3 >> /log/d.txt")
    write("words", "lua", b"lua -e 'print(\"hi\" .. 1 > 0)' > /out")
    write("words", "errors", b"wall don't \\")


for f in (http, nts_ke, nts_ntp, certbundle, config, ping, terminal, words, fsck):
    f()
