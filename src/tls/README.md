# tls: TLS 1.3 client and trusted roots

A small, non-blocking TLS client over lwIP's raw TCP API, built on wolfSSL. Its users are NTS
key exchange (`time/nts.c`) and the HTTP client (`net/http.c`), which also opens plain TCP
connections through it for plain http (`tls_open_plain()`, below). The wolfSSL build options are in
`config/user_settings.h`.

## `tls_admit.c`: how many connections (pure C, host tested)

Up to three connections can be open at once (`TLS_MAX_CONNS`). `tls_open()` says whose each one
is: requests someone started (`http`, `sos.http`, `fetch`) are `TLS_USER` and may hold two of them
(`TLS_MAX_USER_CONNS`); the third is kept for the firmware's own (`TLS_SYSTEM`, NTS). So four
shells running `http` at once can't make time sync fail. A connection also needs
`TLS_HEAP_NEEDED` of free heap to start, about what one takes at the peak of its handshake (the
11.7 KB receive buffer plus wolfSSL); with less, it's refused with "not enough free memory"
rather than failing halfway, or making SSH sessions, Lua or the editor fail to allocate. A
refused connection fails at once (`tls_poll()` returns `TLS_FAILED`, `tls_error()` says why); there
is no queue, so the caller reports it and the person tries again. Plain connections take the same
slots; they need only `TLS_PLAIN_HEAP_NEEDED` (32 KB) and don't count as handshakes to come.

## `tls.c`: connections

`tls_open(host, port, alpn, timeout_ms)` returns at once. `tls_poll()` then moves the
connection through:

```
TLS_RESOLVING (DNS) → TLS_CONNECTING (TCP) → TLS_HANDSHAKE → TLS_READY → TLS_CLOSED
                                                    any step can end in TLS_FAILED
```

The timeout covers DNS, connect and handshake together. When ready, `tls_read()` and
`tls_write()` never block.

`tls_open_plain(host, port, timeout_ms, use)` is the same connection without wolfSSL: it goes from
`TLS_CONNECTING` straight to `TLS_READY`, and `tls_read()`/`tls_write()` move the TCP payload as
it is, with the same ring buffer and window (`tcp_recved()` only as the caller reads). With the
buffer empty, a FIN gives `TLS_CLOSED` and a reset or TCP error `TLS_FAILED`. It's here, not in a
module of its own, so DNS, connect, timeouts, flow control and the connection slots are shared. `tls_error()` says why a connection failed, and `tls_stats()` gives
timings and heap use.

When wolfSSL reports an error, `tls_error()` gives SOS's own wording for the ones a person
meets and can act on (an untrusted or expired certificate, a wrong host name, a fatal alert from
the server, a failed or closed connection, no TLS 1.3, no memory), followed by wolfSSL's number; any other error is
"wolfSSL error N". wolfSSL's own table of error texts is not built (`NO_ERROR_STRINGS`),
which saves about 20 KB of flash. A build with `SOS_WOLF_DEBUG` keeps it.

- **I/O:** the lwIP callbacks (interrupt context) only queue received data in a ring buffer
  and set flags. They don't log: a close or a TCP error is logged from the main loop, by `io_recv`
  when it next reads or in the warning when wolfSSL next fails.
  wolfSSL's custom I/O callbacks (`io_recv`, `io_send`) read and write it from the main loop.
- **Handshake:**
  - TLS 1.3 only, with SNI, ALPN when given, and host-name verification.
  - Key exchange groups are restricted to X25519 and P-256, with an X25519 key share. P-521
    took about 1.4 s per operation on the RP2350, long enough for Cloudflare to close the
    connection.
  - The handshake keys are kept (`wolfSSL_KeepArrays`) so `time/nts.c` can use the keying
    material exporter.
- **Server time limits:** servers drop clients that stall; `time.cloudflare.com` closes a key
  exchange after 2 s without progress. So:
  - The session and its roots (including the SD card read) are set up before the TCP connect,
    and the ClientHello follows the connect at once.
  - `LWIP_TCP_RTO_TIME` is 1 s (`config/lwipopts.h`), so a packet lost early in a connection
    is resent in time. With lwIP's default of 3 s, about 3% of key exchanges failed.
  - Nagle is off, so a record never waits for the previous one to be acknowledged.
  - When a server closes during the handshake or a read, the log says how long after
    accepting the connection it did so.
- **Certificate checks:** the chain must end at a trusted root (below), and match the host
  name. While the clock isn't set, `verify_cb()` accepts a chain whose only fault is its
  dates, and roots load without checking their own dates (`tls_load_root()`); the connection
  then logs `certificate dates not checked`.
- **Heap accounting:** `tls_init()` installs wolfSSL allocators that track the heap in use by
  every wolfSSL user, SSH included. Each open connection records its own peak, counted from
  its `tls_open()`, so two connections at once don't disturb each other's figure (it still
  includes whatever other wolfSSL users allocated meanwhile). The NTS key-exchange log line and
  the `http` command show it.

## Trusted roots

Each connection's context loads two sets of roots:

- **Built in: `tls_roots.c`.** DER copies of SSL.com Root Certification Authority ECC
  (Cloudflare's NTS chain) and ISRG Root X1 (Let's Encrypt, used by most other NTS servers).
  Each comes with its SHA-256 fingerprint, to check before changing it.
- **From the SD card: `certstore.c`,** only while the file is exactly the one approved on the
  console.
  - **Pin:** the SHA-256 of `/certs/roots.pem` is pinned in the configuration
    (`certs_hash`).
  - **`certstore_load()`:** with nothing approved it doesn't touch the card. Otherwise it reads
    the file (up to 64 KB), compares its hash with the pin, and loads each certificate. A file
    that's missing, changed or unreadable means built-in roots only; the change of state is
    logged once, not per connection.
  - **`tls roots approve`** (console) lists each certificate (name, key, CA flag, expiry,
    fingerprint) and test-loads it. After `yes` it pins the hash of exactly the bytes it listed.
    `tls roots forget` clears the pin.
  - **Console-only directory:** only the console can write `/certs`
    (`files/files_write_allowed()`).
- **`certbundle.c`** (pure C, host tested) turns a PEM bundle, or concatenated DER
  certificates, into DER for wolfSSL. Comment text between PEM blocks is allowed. Any other
  PEM block (a private key, say), a malformed block, or more than 32 certificates makes the
  file invalid.
