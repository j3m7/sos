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


def fat32_image(sectors, part_lba, nfats=1, spc=1, layout="mbr", damage=None, copy1=None,
                fsi_off=0):
    """A small FAT32 volume laid out as f_mkfs lays one out: files, a long name and a
    subdirectory. Returns {lba: bytearray(512)} for the sectors that aren't all zeros.
    For repair seeds: `damage` FAT entries written to every copy, `copy1` to the second copy
    only, and `fsi_off` added to FSInfo's free count."""
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
    struct.pack_into("<III", fsi, 484, 0x61417272, clusters - used + fsi_off, 10)
    fsi[510:512] = b"\x55\xAA"
    for off, data in ((0, bpb), (1, fsi), (6, bpb), (7, fsi)):
        put(part_lba + off, 0, data)
    fat = {0: 0x0FFFFFF8, 1: 0x0FFFFFFF}
    for chain in chains.values():
        for a, b in zip(chain, chain[1:] + [0x0FFFFFFF]):
            fat[a] = b
    fat.update(damage or {})
    for copy in range(nfats):
        for c, v in ({**fat, **(copy1 or {})} if copy else fat).items():
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
    # Repair (the selector's top bit): README.TXT's chain runs on into 200-201, clusters 300-302
    # are a lost chain, FSInfo's count is stale; with two FATs, cluster 400 differs in copy 2.
    longer = {3: 200, 200: 201, 201: 0x0FFFFFFF, 300: 301, 301: 302, 302: 0x0FFFFFFF}
    write("fsck", "repair-mbr", b"\x90" + sparse(70000, fat32_image(70000, 2048, damage=longer,
                                                                   fsi_off=7)))
    write("fsck", "repair-sfd-2fat", b"\xa5" + sparse(70000, fat32_image(
        70000, 0, nfats=2, layout="sfd", damage=longer, copy1={400: 0x0FFFFFFF})))
    write("fsck", "exfat", b"\x13" + sparse(131072, exfat_image(131072, 0)))
    write("fsck", "exfat-mbr", b"\x61" + sparse(131072, exfat_image(131072, 2048)))
    write("fsck", "repair-exfat", b"\x93" + sparse(131072, exfat_image(131072, 0, damage=True)))
    write("fsck", "repair-exfat-mbr", b"\xe1" + sparse(131072, exfat_image(131072, 2048,
                                                                          damage=True)))


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


# fsck.c's case folding (fsck_upcase(): FatFs's for ASCII, Latin-1, Latin Extended-A, Greek and
# Cyrillic), which the checker uses when given no ff_wtoupper(), as the fuzz target is.
FOLD_BLOCKS = [(0x0061, 26, -32), (0x00E0, 23, -32), (0x00F8, 7, -32), (0x0100, 48, 0),
               (0x0132, 6, 0), (0x0139, 16, 0), (0x014A, 46, 0), (0x0179, 6, 0),
               (0x03B1, 17, -32), (0x03C4, 8, -32), (0x03D8, 24, 0), (0x0430, 32, -32),
               (0x0450, 16, -80), (0x0460, 34, 0), (0x048A, 54, 0), (0x04C1, 14, 0),
               (0x04D0, 68, 0)]
FOLD_SINGLE = {0x00FF: 0x0178, 0x037B: 0x03FD, 0x037C: 0x03FE, 0x037D: 0x03FF, 0x03AC: 0x0386,
               0x03AD: 0x0388, 0x03AE: 0x0389, 0x03AF: 0x038A, 0x03C2: 0x03A3, 0x03C3: 0x03A3,
               0x03CC: 0x038C, 0x03CD: 0x038E, 0x03CE: 0x038F, 0x03F2: 0x03F9, 0x03F8: 0x03F7,
               0x03FB: 0x03FA, 0x04CF: 0x04C0}


def fold(c):
    for base, count, shift in FOLD_BLOCKS:
        if base <= c < base + count:
            return c + shift if shift else c - ((c - base) & 1)
    return FOLD_SINGLE.get(c, c)


def upcase_table():
    """fold() as a compressed up-case table (0xFFFF, n: n characters map to themselves); the
    characters past its end map to themselves too."""
    last = max(c for c in range(0x10000) if fold(c) != c)
    units, c = [], 0
    while c <= last:
        n = 0
        while c + n <= last and fold(c + n) == c + n:
            n += 1
        if n > 1:
            units += [0xFFFF, n]
            c += n
        else:
            units.append(fold(c))
            c += 1
    return struct.pack("<%dH" % len(units), *units)


def exfat_image(sectors, part_lba, damage=False):
    """An exFAT volume of 2 KB clusters laid out as f_mkfs lays one out (boot regions, FAT,
    then bitmap, up-case table and root at the heap's start), with a FAT-chained file, a
    NoFatChain file across window boundaries and a subdirectory. {lba: bytearray(512)}.
    With `damage`, what repair fixes: the FAT-chained file's chain runs on into two clusters,
    9000-9002 are set in the bitmap for nothing, README.txt's bit is clear, and two bits are
    set past big.bin's run."""
    sec = {}

    def put(lba, off, data):
        s = sec.setdefault(lba, bytearray(512))
        s[off:off + len(data)] = data

    shift, vol = 2, sectors - part_lba
    fat_off = 32
    fat_len = ((vol >> shift) + 2) * 4 // 512 + 1
    heap = (fat_off + fat_len + 7) // 8 * 8
    count = (vol - heap) >> shift
    csize = 1 << shift
    table = upcase_table()
    tsum = 0
    for b in table:
        tsum = rotr_add(32, tsum, b)
    bm_clusters = ((count + 7) // 8 + 2047) // 2048
    up_first = 2 + bm_clusters
    up_clusters = (len(table) + 2047) // 2048
    root = up_first + up_clusters
    readme, dirc, inner = root + 1, root + 2, root + 3       # inner: 3 clusters
    longf = [root + 6, root + 7, root + 9]                    # fragmented: a FAT chain
    big_first, big_n = 15000, 5000                            # over cluster 16386
    chains = {2: list(range(2, up_first)), up_first: list(range(up_first, root)), root: [root],
              longf[0]: longf}
    used = set(range(2, root + 1)) | {readme, dirc} | set(range(inner, inner + 3)) | set(longf)
    used |= set(range(big_first, big_first + big_n))
    if damage:
        longf += [root + 20, root + 21]
        used |= {root + 20, root + 21, 9000, 9001, 9002, big_first + big_n, big_first + big_n + 1}
        used -= {readme}

    s0 = bytearray(512)
    s0[0:11] = b"\xEB\x76\x90EXFAT   "
    struct.pack_into("<QQIIIIIIHHBBBB", s0, 64, part_lba, vol, fat_off, fat_len, heap, count,
                     root, 0x12345678, 0x100, 0, 9, shift, 1, 0x80)
    s0[120:122] = b"\xEB\xFE"
    s0[510:512] = b"\x55\xAA"
    ext = bytearray(512)
    ext[510:512] = b"\x55\xAA"
    region = [bytes(s0)] + [bytes(ext)] * 8 + [bytes(512)] * 2
    bsum = 0
    for k, b512 in enumerate(region):
        for i, b in enumerate(b512):
            if k or i not in (106, 107, 112):
                bsum = rotr_add(32, bsum, b)
    region.append(struct.pack("<I", bsum) * 128)
    for copy in (0, 12):
        for k, b512 in enumerate(region):
            if any(b512):
                put(part_lba + copy + k, 0, b512)

    fat = {0: 0xFFFFFFF8, 1: 0xFFFFFFFF}
    for chain in chains.values():
        for a, b in zip(chain, chain[1:] + [0xFFFFFFFF]):
            fat[a] = b
    for c, v in fat.items():
        put(part_lba + fat_off + c // 128, c % 128 * 4, struct.pack("<I", v))

    def clus(c):
        return part_lba + heap + (c - 2) * csize

    bitmap = bytearray((count + 7) // 8)
    for c in used:
        bitmap[(c - 2) // 8] |= 1 << ((c - 2) % 8)
    for i in range(0, len(bitmap), 512):
        if any(bitmap[i:i + 512]):
            put(clus(2) + i // 512, 0, bitmap[i:i + 512])
    for i in range(0, len(table), 512):
        put(clus(up_first) + i // 512, 0, table[i:i + 512])
    label = bytes([0x83, 3]) + "SOS".encode("utf-16-le") + bytes(24)
    bm_e = bytes([0x81]) + bytes(19) + struct.pack("<IQ", 2, len(bitmap))
    up_e = bytes([0x82, 0, 0, 0]) + struct.pack("<I", tsum) + bytes(12) + struct.pack(
        "<IQ", up_first, len(table))
    put(clus(root), 0, label + bm_e + up_e + exfat_set("README.txt", readme, 100)
        + exfat_set("A long name with more than thirty characters", longf[0], 5000,
                    contiguous=False)
        + exfat_set("dir", dirc, 2048, attr=0x10)
        + exfat_set("big.bin", big_first, big_n * 2048)
        + exfat_set("empty", 0, 0))
    put(clus(dirc), 0, exfat_set("inner.bin", inner, 5000) + exfat_set("Empty Too", 0, 0))
    for c in (readme, inner, longf[0]):
        put(clus(c), 0, b"x")
    if part_lba:
        put(0, 446, struct.pack("<B3sB3sII", 0, b"\0\1\0", 0x07, b"\xFE\xFF\xFF", part_lba, vol))
        put(0, 510, b"\x55\xAA")
    return sec


# --- tzrule: [year - 1970] then a POSIX TZ rule ------------------------------------------------

def tzrule():
    with open(os.path.join(HERE, "..", "..", "tz", "zones.bundle"), "rb") as f:
        rules = sorted({line.split(b" ")[1].strip() for line in f if not line.startswith(b"#")})
    for i, r in enumerate(rules):                   # each distinct rule the zones use
        write("tzrule", "bundle-%03d" % i, bytes([56]) + r)
    write("tzrule", "all-year-dst", bytes([56]) + b"EST5EDT,0/0,J365/25")
    write("tzrule", "limits", bytes([255]) + b"AAA-24BBB+24,J1/167,J2/-167")
    write("tzrule", "julian", bytes([58]) + b"AAA0BBB,J60/0,59/-1:30:15")
    write("tzrule", "negative-dst", bytes([56]) + b"IST-1GMT0,M10.5.0,M3.5.0/1")
    write("tzrule", "past-24h", bytes([56]) + b"<+05>-5<+06>,J100/100,J300/-100:59:59")


# --- gz: [piece - 1][buffer / 16 - 1] then a gzip file -----------------------------------------

def gz_member(data, level=6, header=None, wbits=31):
    if header is None:
        c = zlib.compressobj(level, zlib.DEFLATED, wbits)
        return c.compress(data) + c.flush()
    # A hand-made header (all of FTEXT FHCRC FEXTRA FNAME FCOMMENT), raw deflate, the trailer.
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    h = b"\x1f\x8b\x08\x1f" + struct.pack("<I", 0) + b"\x00\x03"
    h += struct.pack("<H", 4) + b"ab\x00\x00" + b"name.txt\x00" + b"comment\x00"
    h += struct.pack("<H", zlib.crc32(h) & 0xffff)
    return h + body + struct.pack("<II", zlib.crc32(data), len(data) & 0xffffffff)


def gz():
    text = b"".join(b"%d\n" % i for i in range(1, 700))
    one = gz_member(text)
    write("gz", "one", bytes([255, 255]) + one)
    write("gz", "small-pieces", bytes([0, 0]) + one)
    write("gz", "members", bytes([6, 3]) + gz_member(text[:900]) + gz_member(b"") +
          gz_member(text[900:], 1))
    write("gz", "zeros", bytes([30, 10]) + one + bytes(600))
    write("gz", "junk", bytes([30, 10]) + one + b"junk")
    write("gz", "zeros-junk", bytes([30, 10]) + one + bytes(20) + b"\x1f\x8b")
    write("gz", "stored", bytes([100, 255]) + gz_member(bytes(range(256)) * 4, 0))
    write("gz", "fixed", bytes([1, 0]) + gz_member(b"hello, hello, hello", 9))
    write("gz", "header", bytes([3, 7]) + gz_member(text[:500], 6, header=True))
    bad = bytearray(one)
    bad[-8] ^= 0xff
    write("gz", "bad-crc", bytes([50, 50]) + bytes(bad))
    write("gz", "cut", bytes([50, 50]) + one[:len(one) // 2])
    write("gz", "zlib-not-gzip", bytes([50, 50]) + gz_member(text[:300], 6, wbits=15))
    write("gz", "bomb", bytes([255, 255]) + gz_member(bytes(200000), 9))


for f in (http, nts_ke, nts_ntp, certbundle, config, ping, terminal, words, fsck, fsck_exfat,
          tzrule, gz):
    f()
