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
| `src/ssh/pico-sshd/server.c` | `src/server.c` | Restructured for multiple concurrent sessions (`ssh_session_t`), stdio driver replaced by a session API; password authentication removed, only public key advertised; the default authentication callback (a user name and key table) removed, so the application must set `auth_cb`; all wolfSSH work moved from lwIP callbacks to `ssh_server_service()` (thread context); graceful disconnect (exit status, channel close, `CS_CLOSING`); full-buffer input handled with `ERR_MEM` instead of being dropped; private key array bounds and buffer release fixed; refused connections get an SSH disconnect message ("too many connections") instead of a reset; session buffers allocated per connection instead of at startup; timing statistics; output batched into few packets (`ssh_session_send_output()`, `ssh_session_write_space()`); Nagle disabled; SFTP subsystem served (`ssh_session_sftp()`), default path `/`; the public key a login was signed with is kept in the session (`auth_key`, recorded by a user-auth result callback after the signature is verified); `tcp_poll()` intervals given in milliseconds (`SSH_POLL_TICKS`); connection and login messages marked with `LOG_AUTH`, and accepted and failed keys logged with the client's address |
| `src/ssh/pico-sshd/ringbuffer.c` | `src/ringbuffer.c` | none |
| `src/ssh/pico-sshd/include/pico-sshd.h` | `include/pico-sshd.h` | `ssh_session_t` split out of `ssh_server_t`; session API; `ssh_server_service()`; `CS_CLOSING`; statistics; `ssh_session_send_output()`, `ssh_session_write_space()`; `sftp` session flag; `auth_key`/`auth_key_len`; `ssh_user_auth_entry_t` removed |
| `src/ssh/pico-sshd/include/pico-sshd/ringbuffer.h` | `include/pico-sshd/ringbuffer.h` | none |
| `src/ssh/pico-sshd/include/pico-sshd/log.h` | `include/pico-sshd/log.h` | `LOG_PRIMASK` and `LOG_AUTH` (syslog's facility) added |
| `cmake/wolfssl_pico.cmake` | `cmake/wolfssl_pico.cmake` | SP assembly selection scoped to the `wolfssl` target |
| `cmake/wolfssh_pico.cmake` | `cmake/wolfssh_pico.cmake` | `src/wolfsftp.c` left out of the glob (SOS compiles it through `src/ssh/sftp_wolf.c`) |
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
git -C /tmp/pico-sshd diff 9c025dc HEAD   # upstream changes since the last check (see below)
diff -u /tmp/pico-sshd/src/server.c src/ssh/pico-sshd/server.c                       # SOS changes
```

The copy was taken at upstream 697e33c. The only upstream commit since then is 9c025dc
(2026-11-25), which is SOS's own fixes (below) merged upstream; the copy already has all of them,
so it's in step with upstream as of that commit.

### Fixes offered upstream

Generic bugs found while integrating pico-sshd that apply to upstream as well.

Reported 2026-11-23: items 1, 2, 3, 5, 8 and 9 as fixes in
[tjko/pico-sshd#2](https://github.com/tjko/pico-sshd/pull/2) (tested on a Pico 2 W), together
with two lwIP error-handling fixes added during its review (return `ERR_ABRT` from callbacks
that abort their connection; don't close the pcb again in `ssh_server_err()`). **Merged
upstream on 2026-11-25** as 9c025dc. Items 4, 6, 7 and 10, the ML-KEM stack finding and the
multi-session work are notes only, in
[tjko/pico-sshd#3](https://github.com/tjko/pico-sshd/issues/3) (no reply yet); item 11 hasn't
been reported. Also noted in #3: with an rx buffer smaller than the SSH window (4096),
`ssh_process_input()` can leave channel data unread and stall the session (SOS's copy has the
same code path; it uses 4096, which avoids it).

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
11. `src/server.c`: every write to the session (the stdio driver's `write`) flushes at once,
   so output written in small pieces (`printf` per line, a newline translated separately)
   becomes one encrypted SSH packet and one TCP segment per piece. On SOS, printing a text
   file ran at about 33 KB/s; queuing the output and sending once per pass gave about
   130 KB/s (aes128-gcm, debug build). Nagle is also left enabled, which delays sub-MSS
   segments behind delayed ACKs.

### wolfSSH: issues found adding SFTP (FatFs)

Found with wolfSSH 1.6.0 (`WOLFSSH_SFTP` + `WOLFSSH_FATFS`); SOS works around them in
`src/ssh/sftp_wolf.c` / `src/ssh/sftp_fs.c` without patching wolfSSH. Not reported yet.

1. `wolfssh/port.h`, FatFs port: `WPREAD`/`WPWRITE` drop the offset argument
   (`ff_pread(fd,b,s)`), so reads and writes happen wherever the file position is: resumed or
   out-of-order transfers read or write the wrong place.
2. Same port: `WOLFSSH_O_CREAT` includes `FA_CREATE_NEW`, so `f_open()` fails with `FR_EXIST`
   when an upload overwrites an existing file.
3. Same port: `WREMOVE`/`WRENAME` return FatFs codes (> 0 on failure), but `wolfsftp.c` tests
   them for `< 0`, so a failed remove or rename is reported to the client as success.
4. Same port: `fd_pool[WOLFSSH_FATFS_MAX_FILES]` (32 `FIL`s, ~19 KB) is static, and `ff_open()`
   traces with `PRINTF`.
5. `wolfSSH_SFTP_RecvRead()`: a READ for more than `WOLFSSH_MAX_SFTP_RW` is a fatal error
   (`WS_BUFFER_E` ends the session) instead of a shorter reply, so lowering the limit breaks
   OpenSSH's `sftp`, which asks for 32 KB.
6. `wolfSSH_accept()` returns `WS_SFTP_COMPLETE` after it has run the SFTP version exchange
   itself; calling `wolfSSH_SFTP_accept()` again then returns `WS_FATAL_ERROR` with a stale
   `ssh->error` (here `WS_CHAN_RXD`), which a caller retrying on notices loops on forever. Not a
   bug as such, but the API docs don't say which of the two to use.

## Linked into the firmware (not copied)

| Component | Source | License |
|---|---|---|
| wolfSSL / wolfCrypt 5.9.4 | `lib/wolfssl` (submodule) | GPL-3.0 (commercial license also available) |
| wolfSSH 1.6.0 | `lib/wolfssh` (submodule) | GPL-3.0 (commercial license also available) |
| no-OS-FatFS-SD-SDIO-SPI-RPi-Pico 3.6.2 (SD card driver, includes FatFs R0.15) | `lib/sdcard` (submodule) | Apache-2.0; FatFs: BSD-1-Clause style |
| Lua 5.4.9 (core and base/string/table/math/utf8/coroutine libraries) | `lib/lua` (submodule) | MIT |
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
