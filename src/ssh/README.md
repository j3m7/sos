# ssh: SSH server, shell sessions and SFTP

```
lwIP (interrupt context)        main loop (thread context)
  TCP callbacks ──► ring buffers ──► ssh_server_service(): wolfSSH ──► session channel
                                       sshd_poll(): one shell_t per session, or SFTP
```

## `pico-sshd/`: the SSH server

This is adapted from [pico-sshd](https://github.com/tjko/pico-sshd). [`THIRD_PARTY.md`](../../THIRD_PARTY.md)
lists every change. It runs wolfSSH over lwIP's raw TCP API.

- **lwIP callbacks only buffer.** TCP callbacks run in interrupt context, so they just copy
  input into per-session ring buffers (`ringbuffer.c`) and flag work. If a buffer is full they
  return `ERR_MEM`, so lwIP delivers the data again later.
- **All wolfSSH work is in thread context.** `ssh_server_service()` runs the key exchange,
  authentication and channel I/O there, and so does connection teardown.
- **Sessions:** up to `SOS_SSH_MAX_SESSIONS` (3) run at once, each an `ssh_session_t`. Its
  buffers are allocated when a connection is set up and freed when it closes, so an idle slot
  costs no heap. When every slot is busy, the client gets an SSH disconnect message saying so
  (no bare reset). That message is built once when the server starts, and the refusal is
  logged from the main loop, counted, at most one line every 10 s: the accept callback runs in
  interrupt context and anyone who can reach the port can cause it, so it only queues the
  message.
- **Connections that don't log in:** the login grace time is 30 s, or 5 s while every slot is
  in use (`SSH_LOGIN_GRACE_BUSY_US`). Each such connection counts against its address
  (`ssh_penalty_t`, 4 addresses tracked): at `SSH_PENALTY_FAILS` (30) within
  `SSH_PENALTY_WINDOW_US` (60 s), new connections from it are reset until it has been quiet
  for a whole window. The table is only changed under the lwIP lock.
- **Disconnect:** queued output is sent, then the exit status and channel close, then the
  server waits briefly for the client to close the TCP connection.
- **Authentication:** public key only (password support was removed).
- **Log facility:** messages about connections and logins (accepted, refused, timed out,
  disconnected, address penalized) carry syslog's `LOG_AUTH`; accepted and failed keys name the
  client's address. `sshd.c`'s `log_cb` puts those in the auth log and the rest in daemon.

## `sshd.c`: SOS's side

- **`sshd_start()`** creates the server once Wi-Fi is up and at least one key is trusted. Login
  needs one of the trusted keys from the settings; any user name is accepted.
- **`sshd_poll()`** calls `ssh_server_service()` and serves each session. A new login gets a
  fresh `shell_t` whose `shell_io_t` reads and writes the session channel (`admin` is false).
  The shell is registered with `chat/` under the comment of the trusted key the login was
  signed with (pico-sshd keeps that key in `ssh_session_t.auth_key`), or `key <n>` if the
  key has no comment; after each `shell_poll()`, `chat_deliver()` prints messages from the
  others.
  An SFTP session gets no shell: wolfSSH serves it directly.
- **Output** is batched into few packets (`ssh_session_send_output()`). `write_space()` lets
  long jobs such as `cat` slow down for a slow client instead of losing output.
- **`ssh` command:** `sshd_status()` prints the host key fingerprint, statistics (including the
  longest single wolfSSH step, the guide to watchdog margin) and the active sessions.

## Key exchange, and what the post-quantum part costs

The server offers `mlkem768x25519-sha256` ahead of plain Curve25519. It is a hybrid: the
session key comes from both an X25519 exchange and an ML-KEM-768 one, so it stays secret
unless both are broken. The point is traffic recorded today: someone who keeps a capture of an
SSH session and later has a machine that breaks elliptic curves still can't read it. Current
OpenSSH clients (9.9 and later) pick this exchange by default, so logins from them use it.
The login itself (the host key and the client's key) is Ed25519 either way.

It costs about 30 KB of flash (ML-KEM and the SHA-3 it needs; measured by building without
them, issue #120) and a few KB of heap during the exchange. To build without it, turn off
the `WOLFSSL_HAVE_MLKEM` block and `WOLFSSL_SHA3` in `config/user_settings.h`; clients then
fall back to `curve25519-sha256`. That is a decision about what the board protects against,
not a tuning knob, which is why it is on.

## Host key (`hostkey.c`)

The Ed25519 host key is the first page of the key sector, the last 4 KB of flash
(`core/keysector.c`). It's generated on
the device when the board is set up (`hostkey_get()`) and survives reflashing; the
private key never leaves the device. The flash write goes through `flash_safe_execute()`.

- **Sealed for the owner:** the record (`hostkey_record.c`, pure C, host tested) holds the key
  sealed with AES-256-GCM, the record's header as associated data. The seal key is
  HMAC-SHA256(base key, owner secret): the base key comes from the root secret
  (`core/keyprov.h`, derived at boot and kept in RAM), the owner secret from the sealed
  configuration, the same construction as for encrypted cards (`files/cardkey.h`). Erase the
  configuration and the host key can't be opened again; the next owner gets a new one.
- **When:** `hostkey_get()` runs at boot. A board that isn't set up has no owner secret and so
  no host key; `main.c` asks again when SSH is about to start, so a board set up on its
  console gets its host key then, without a reboot.
- **What it does with what it finds** is `hostrec_decide()`, a table tested on the host. An
  open board takes over a record from older firmware once (plain, or sealed under the base
  key alone), keeping the key and its fingerprint. A console-only board doesn't: such a record
  isn't tied to an owner, and it makes a new key.
- **Upgrade:** taking a record over rewrites the key sector (erase, then program), with
  the same power-cut window as any change to it.
- **A sealed key that doesn't open** (another root secret, or damage) is replaced by a new
  key, and the log says clients will see a changed fingerprint. **With no usable root
  secret** no key is opened or made: there is no host key, so no SSH, and the log says why.
- **Downgrade:** older firmware doesn't know the sealed record and makes itself a new host key.
- **A secured board** must not take over a plain key (someone who can write the flash could
  plant one they know): `refuse_plain` in `hostkey.c`, off on an open board, goes with the
  configuration's.

## SFTP (`sftp_wolf.c`, `sftp_fs.c`)

wolfSSH's SFTP server (`wolfsftp.c`) is compiled through `sftp_wolf.c`, so its file functions
can be replaced without patching wolfSSH. Its FatFs port ignores the request's file offset,
can't overwrite a file, and reports failed removes and renames as successes.

- **`sftp_fs.c`** provides the replacements: positioned reads and writes (so resumed transfers
  work), overwriting, and correct error codes.
- **Write check:** every change passes `files_write_allowed()` (`files/`), so SFTP can't write
  `/certs`.
- **Open files:** files a transfer has open count as in use (`files_hold()`/`files_release()`),
  like a running `cp`. A client that disconnects mid-transfer releases them.
