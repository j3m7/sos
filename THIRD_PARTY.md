# Third-party code

SOS is licensed under the GNU General Public License v3.0 or later (see [`LICENSE`](LICENSE)).
This file lists code from other projects that is copied into this repository or linked into the
firmware, with its origin and license.

## Copied into this repository

### pico-sshd

- Upstream: <https://github.com/tjko/pico-sshd>
- Copied at commit `697e33c65a3a36231e91b0d2d121611110978454` (2026-03-09)
- Copyright (C) 2025 Timo Kokkonen
- License: GPL-3.0-or-later

| SOS path | Upstream path | SOS changes |
|---|---|---|
| `src/ssh/server.c` | `src/server.c` | Restructured for multiple concurrent sessions (`ssh_session_t`), stdio driver replaced by a session API; password authentication removed, only public key advertised, key length checked before comparison; all wolfSSH work moved from lwIP callbacks to `ssh_server_service()` (thread context); graceful disconnect (exit status, channel close, `CS_CLOSING`); full-buffer input handled with `ERR_MEM` instead of being dropped; private key array bounds and buffer release fixed; refused connections get an SSH disconnect message ("too many connections") instead of a reset; session buffers allocated per connection instead of at startup; timing statistics |
| `src/ssh/ringbuffer.c` | `src/ringbuffer.c` | none |
| `src/ssh/include/pico-sshd.h` | `include/pico-sshd.h` | `ssh_session_t` split out of `ssh_server_t`; session API; `ssh_server_service()`; `CS_CLOSING`; statistics |
| `src/ssh/include/pico-sshd/ringbuffer.h` | `include/pico-sshd/ringbuffer.h` | none |
| `src/ssh/include/pico-sshd/log.h` | `include/pico-sshd/log.h` | none |
| `cmake/wolfssl_pico.cmake` | `cmake/wolfssl_pico.cmake` | SP assembly selection scoped to the `wolfssl` target |
| `cmake/wolfssh_pico.cmake` | `cmake/wolfssh_pico.cmake` | none |
| `config/user_settings.h` | `wolfssl/user_settings.h` (a wolfSSL example, Copyright wolfSSL Inc., GPL-2.0-or-later) | See the header comment in the file |

### Raspberry Pi Pico SDK / pico-examples

- License: BSD-3-Clause, Copyright (c) 2020 Raspberry Pi (Trading) Ltd.

| SOS path | Upstream path | SOS changes |
|---|---|---|
| `config/ld/section_end.incl` | `pico-sdk` 2.3.1 `src/rp2_common/pico_standard_link/script_include/section_end.incl` | `__StackTop` points at the `STACK0` region |
| `config/lwipopts.h` | `pico-examples` `pico_w/wifi/lwipopts_examples_common.h` | Trimmed and tuned for SOS |

### pico-sshd: not copied

Not copied: `src/sha512crypt.c` (password hashing; SOS does not support password login).

Each modified file carries a comment describing the SOS changes. To compare with upstream:

```sh
git clone https://github.com/tjko/pico-sshd.git /tmp/pico-sshd
git -C /tmp/pico-sshd diff 697e33c65a3a36231e91b0d2d121611110978454 HEAD   # upstream changes since the copy
diff -u /tmp/pico-sshd/src/server.c src/ssh/server.c                       # SOS changes
```

### Fixes to offer upstream

Generic bugs found while integrating pico-sshd that apply to upstream as well.

Reported 2026-11-23: items 1, 2, 3, 5, 8 and 9 as fixes in
[tjko/pico-sshd#2](https://github.com/tjko/pico-sshd/pull/2) (tested on a Pico 2 W); items 4,
6, 7 and 10, the ML-KEM stack finding and the multi-session work as notes in
[tjko/pico-sshd#3](https://github.com/tjko/pico-sshd/issues/3). Also noted there: with an rx
buffer smaller than the SSH window (4096), `ssh_process_input()` can leave channel data unread
and stall the session (SOS's copy has the same code path; it uses 4096, which avoids it).

1. `CMakeLists.txt`: `target_sources(pico-telnetd-lib ...)` adds the library's sources to the
   wrong target, so `pico-sshd-lib` only builds in projects that also use pico-telnetd.
2. `src/server.c`, `ssh_server_default_auth_cb()`: the public key comparison is
   `memcmp(auth, u->auth, auth_len)` without checking `auth_len == u->auth_len`, so it can
   read past the stored key and compares only a prefix when the offered key is shorter.
3. `cmake/wolfssl_pico.cmake`: `add_compile_definitions(wolfssl WOLFSSL_SP_...)` defines a
   global `wolfssl` macro and applies the SP define to every target; should be
   `target_compile_definitions(wolfssl INTERFACE ...)`.
4. `wolfssl/user_settings.h`: defines `WOLFSSH_ALLOW_USERAUTH_NONE`, which (with
   `auth_enabled = false`) allows unauthenticated logins; and lacks `HAVE_ED25519_MAKE_KEY`,
   which `wolfSSH_MakeEd25519Key()` needs. Worth at least a warning in the README.
5. `src/server.c`, `ssh_server_recv()`: when the input ring buffer is full, the remaining
   pbufs are neither consumed nor refused (no `ERR_MEM`), so their data is lost and the pbufs
   leak.
6. `src/server.c`: during the handshake, `wolfSSH_accept()` only runs from the `tcp_poll`
   callback (every 500 ms), so a login takes ~5 s; and all wolfSSH work runs inside lwIP
   callbacks (interrupt context with `threadsafe_background`), blocking for up to ~110 ms.
7. `src/server.c`, `ssh_server_disconnect_client()`: closes TCP immediately without sending
   exit status or channel close; clients report exit 255, and data still in flight can turn
   the close into a reset that discards the session's last output.
8. `src/server.c`, `ssh_server_add_priv_key()`: loops `i <= MAX_SERVER_PKEYS`, writing one
   entry past `pkeys[]`; `ssh_server_open()` walks `pkeys[]` until a NULL key with no bound.
9. `src/server.c`, `ssh_server_destroy()`: calls `ssh_ringbuffer_free()` (which returns the
   free space) instead of `ssh_ringbuffer_destroy()`, so the buffers are never released.
10. Interactive `ssh` sessions hang after "PTY allocation request failed" unless wolfSSH is
   built with `WOLFSSH_TERM` (plus `NO_TERMIOS` on the Pico); the bundled settings omit it.

## Linked into the firmware (not copied)

| Component | Source | License |
|---|---|---|
| wolfSSL / wolfCrypt 5.9.4 | `lib/wolfssl` (submodule) | GPL-3.0 (commercial license also available) |
| wolfSSH 1.6.0 | `lib/wolfssh` (submodule) | GPL-3.0 (commercial license also available) |
| Raspberry Pi Pico SDK 2.3.1 | `PICO_SDK_PATH` | BSD-3-Clause |
| lwIP (via Pico SDK) | `pico-sdk/lib/lwip` | BSD-3-Clause |
| TinyUSB (via Pico SDK) | `pico-sdk/lib/tinyusb` | MIT |
| cyw43-driver (via Pico SDK) | `pico-sdk/lib/cyw43-driver` | Restricted, see below |

### Open question: cyw43-driver license

The CYW43439 Wi-Fi driver and the firmware blob it loads are offered under two licenses, neither
of them a free software license:

- `LICENSE.RP`: use and redistribution only with Raspberry Pi semiconductor devices.
- `LICENSE`: redistribution and use only for personal, non-commercial purposes.

Both add restrictions beyond those GPLv3 permits, which may conflict with distributing SOS
firmware binaries under GPL-3.0-or-later. Building and running SOS for yourself is not affected.
Before distributing firmware images, get this reviewed; other GPL projects on the Pico W face the
same question.
