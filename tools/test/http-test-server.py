#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Julio Martinez
#
# Plain-http server for testing `fetch http://...` on a board (issue #265, phase 3). Run it on a
# machine on the board's LAN; each path gives one way a response can end:
#
#   /length/<n>       <n> bytes with Content-Length (default 5 MB)
#   /chunked/<n>      <n> bytes, chunked (4 KB chunks, then the last chunk)
#   /close/<n>        <n> bytes with neither: the body ends when the server closes (FIN)
#   /reset/<n>        Content-Length <n>, but the connection is reset (RST) after half of it
#   /reset-close/<n>  no length; reset after half: must fail, unlike /close
#   /to-https         302 to https://letsencrypt.org/robots.txt (http to https: followed)
#   /to-http          302 to /length/1000 on this server (http to http: followed)
#
# The body is a repeating pattern of printable bytes, the same for a given <n> every time, so its
# SHA-256 can be checked: the server prints it for each request it answers. Nothing is read from
# disk. Without arguments it listens on port 8080 on all addresses.
#
#   tools/test/http-test-server.py [--port 8080] [--bind 0.0.0.0]

import argparse
import hashlib
import http.server
import socket
import socketserver
import struct

DEFAULT_SIZE = 5 * 1024 * 1024
PATTERN = bytes(range(0x20, 0x7f)) + b"\n"


def body(n):
    reps = n // len(PATTERN) + 1
    return (PATTERN * reps)[:n]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def size(self, parts):
        try:
            return int(parts[2]) if len(parts) > 2 and parts[2] else DEFAULT_SIZE
        except ValueError:
            return DEFAULT_SIZE

    def note(self, what, data):
        print(f"  {self.path}: {what}, {len(data)} bytes, "
              f"sha256 {hashlib.sha256(data).hexdigest()}", flush=True)

    def reset(self):
        # SO_LINGER with a zero timeout: close() sends RST instead of FIN.
        self.wfile.flush()
        self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.connection.close()
        self.close_connection = True

    def redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def do_GET(self):
        parts = self.path.split("?")[0].split("/")
        kind = parts[1] if len(parts) > 1 else ""
        if kind == "to-https":
            return self.redirect("https://letsencrypt.org/robots.txt")
        if kind == "to-http":
            return self.redirect("/length/1000")
        if kind not in ("length", "chunked", "close", "reset", "reset-close"):
            self.send_error(404)
            return
        data = body(self.size(parts))
        self.close_connection = True
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Connection", "close")
        if kind in ("length", "reset"):
            self.send_header("Content-Length", str(len(data)))
        if kind == "chunked":
            self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        if kind == "chunked":
            for i in range(0, len(data), 4096):
                piece = data[i:i + 4096]
                self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.note("chunked", data)
        elif kind in ("reset", "reset-close"):
            self.wfile.write(data[:len(data) // 2])
            self.note("reset after half of", data)
            self.reset()
        else:
            self.wfile.write(data)
            self.note("Content-Length" if kind == "length" else "no length, then close", data)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="plain-http test server for fetch")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()
    with Server((args.bind, args.port), Handler) as s:
        print(f"listening on {args.bind}:{args.port}", flush=True)
        try:
            s.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
