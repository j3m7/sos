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


# --- words: a typed command line --------------------------------------------------------------

def words():
    write("words", "quotes", b"echo \"a   b\" 'x|y' a\\ b \"\\\"q\\\\\" a\"b c\"d '' \"\"")
    write("words", "pipes", b"dmesg|grep -i 'no route'|head 3 >> /log/d.txt")
    write("words", "lua", b"lua -e 'print(\"hi\" .. 1 > 0)' > /out")
    write("words", "errors", b"wall don't \\")


# --- fsck_exfat: [part] then the part's input (see fuzz_fsck_exfat.c) ---------------------------

def rotr_add(width, s, b):
    top = 1 << (width - 1)
    return ((top if s & 1 else 0) + (s >> 1) + b) & ((1 << width) - 1)


def ascii_upper(c):
    return c - 32 if ord("a") <= c <= ord("z") else c


def exfat_boot(clusters=1000, shift=3):
    """Sectors 0-11 of an exFAT boot region as f_mkfs writes them."""
    s0 = bytearray(512)
    s0[0:11] = b"\xEB\x76\x90EXFAT   "
    fat_len = ((clusters + 2) * 4 + 511) // 512
    heap = 2048
    struct.pack_into("<QQIIIIIIHHBBBB", s0, 64, 0, heap + clusters * (1 << shift), 32, fat_len,
                     heap, clusters, 4, 0x12345678, 0x100, 0, 9, shift, 1, 0x80)
    s0[120:122] = b"\xEB\xFE"
    s0[510:512] = b"\x55\xAA"
    ext = bytearray(512)
    ext[510:512] = b"\x55\xAA"
    region = [bytes(s0)] + [bytes(ext)] * 8 + [bytes(512)] * 2
    s = 0
    for k, sec in enumerate(region):
        for i, b in enumerate(sec):
            if k or i not in (106, 107, 112):
                s = rotr_add(32, s, b)
    return b"".join(region) + struct.pack("<I", s) * 128


def exfat_set(name, first, length, attr=0x20, contiguous=True):
    units = [ord(c) for c in name]
    names = max(1, (len(units) + 14) // 15)
    h = 0
    for u in units:
        u = ascii_upper(u)
        h = rotr_add(16, h, u & 0xFF)
        h = rotr_add(16, h, u >> 8)
    es = [bytearray(32) for _ in range(2 + names)]
    es[0][0], es[0][1] = 0x85, 1 + names
    struct.pack_into("<H", es[0], 4, attr)
    es[1][0], es[1][1], es[1][3] = 0xC0, 3 if contiguous else 1, len(units)
    struct.pack_into("<HxxQxxxxIQ", es[1], 4, h, length, first, length)
    for k in range(names):
        es[2 + k][0] = 0xC1
        for i, u in enumerate(units[15 * k:15 * k + 15]):
            struct.pack_into("<H", es[2 + k], 2 + 2 * i, u)
    s = 0
    for k, e in enumerate(es):
        for i, b in enumerate(e):
            if k or i not in (2, 3):
                s = rotr_add(16, s, b)
    struct.pack_into("<H", es[0], 2, s)
    return b"".join(es)


def fsck_exfat():
    write("fsck_exfat", "boot", b"\0\0" + struct.pack("<I", 1 << 20) + exfat_boot())
    label = bytes([0x83, 0]) + bytes(30)
    bitmap = bytes([0x81]) + bytes(19) + struct.pack("<IQ", 2, 125)
    upcase_table = struct.pack("<97H", *range(97)) + struct.pack("<26H", *range(65, 91))
    upcase_table += struct.pack("<HH", 0xFFFF, 0x10000 - 123)
    s = 0
    for b in upcase_table:
        s = rotr_add(32, s, b)
    upcase = bytes([0x82, 0, 0, 0]) + struct.pack("<I", s) + bytes(12) + struct.pack(
        "<IQ", 3, len(upcase_table))
    root = (label + bitmap + upcase + exfat_set("README.txt", 5, 100)
            + exfat_set("A long name with more than thirty characters", 6, 70000, contiguous=False)
            + exfat_set("dir", 30, 4096, attr=0x10) + exfat_set("empty", 0, 0)
            + bytes([0xA0, 0]) + bytes(30) + bytes(32))
    write("fsck_exfat", "root", b"\x01\x01" + struct.pack("<IB", 1000, 3) + root)
    write("fsck_exfat", "upcase", b"\x02" + struct.pack("<IB", len(upcase_table), 63) + upcase_table)
    write("fsck_exfat", "window", b"\x03" + struct.pack("<IH", 2, 256) +
          bytes([0xFF, 0x0F, 0, 0xF0] * 4) + bytes([0xFF, 0xFF, 0, 0x30] * 4))


for f in (http, nts_ke, nts_ntp, certbundle, config, ping, terminal, words, fsck_exfat):
    f()
