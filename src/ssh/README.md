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
  (no bare reset).
- **Connections that don't log in:** the login grace time is 30 s, or 5 s while every slot is
  in use (`SSH_LOGIN_GRACE_BUSY_US`). Each such connection counts against its address
  (`ssh_penalty_t`, 4 addresses tracked): at `SSH_PENALTY_FAILS` (30) within
  `SSH_PENALTY_WINDOW_US` (60 s), new connections from it are reset until it has been quiet
  for a whole window. The table is only changed under the lwIP lock.
- **Disconnect:** queued output is sent, then the exit status and channel close, then the
  server waits briefly for the client to close the TCP connection.
- **Authentication:** public key only (password support was removed).

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

## Host key (`hostkey.c`)

The Ed25519 host key is the first page of the key sector, the last 4 KB of flash
(`core/keysector.c`; the second page holds the SD cards' device key, `core/devkey.c`). It's generated on
the device at first boot (`hostkey_get()`, before Wi-Fi starts) and survives reflashing; the
private key never leaves the device. The flash write goes through `flash_safe_execute()`.

- **Sealed:** the record (`hostkey_record.c`, pure C, host tested) holds the key sealed with
  AES-256-GCM under the key provider's host-key key (`core/seal.h`, `core/keyprov.h`), with the
  record's header as associated data. `hostkey_get()` opens it at boot into RAM, where the SSH
  server uses it; the seal key is wiped again before `hostkey_get()` returns.
- **Upgrade:** a plain record from firmware before sealing is read, and stored sealed at once:
  the same key, the same fingerprint. That rewrites the key sector (erase, then program), with
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
