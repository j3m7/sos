# Remote access

## SSH

Once the device has a Wi-Fi network and at least one trusted key, it starts an SSH server on
port 22. The log shows the IP address and the host key fingerprint:

```
wifi: connected, ip 192.168.1.89
sshd: listening on port 22 (up to 3 sessions), host key ssh-ed25519 SHA256:...
```

Check that fingerprint (also shown by the `ssh` shell command over USB) against the one your
client prints on first connection, then log in:

```sh
ssh <device-ip>
```

- Only public-key login is accepted, with one of the trusted keys. The user name doesn't
  matter: any name logs in with a trusted key, and the name is only shown (`ssh`, `who`, the
  log). The keys are what decide who gets in.
- The Ed25519 host key is generated on the device when it is first set up and stored in the
  last 4 KB flash sector (the key sector), sealed; it survives reflashing. The private key
  never leaves the device.
- **The host key belongs to the board's owner.** It is sealed under a key that depends on the
  board and on the owner secret in its configuration. Erasing the configuration (`config
  reset`, provisioning it again, or erasing it over USB) therefore makes the host key
  unreadable, and the next setup makes a new one: SSH clients see a changed host key and warn.
  That is intended. Someone who wipes a board and sets it up as theirs is a new SSH identity,
  not the old board.
- Up to 3 concurrent sessions (`SOS_SSH_MAX_SESSIONS`). While all are in use, further
  connections are refused with an SSH disconnect message the client displays
  ("Too many connections: all 3 SSH sessions are in use, try again later"). Each session gets
  its own shell, separate from the others and from the USB console.
- Connections that don't log in can't hold the server. A connection has 30 s to log in, but
  only 5 s while every session is in use. An address whose connections end without a login 30
  times within a minute is refused (its connections are reset) until it has stayed away for a
  minute; successful logins don't count. `ssh` shows how often either happened.
- Keys and nonces come from wolfCrypt's Hash-DRBG, seeded from the RP2350's hardware random
  number generator through the SDK.
- `help` lists commands; `ssh` shows the active sessions (yours marked `*`) and `who` who is
  logged in; `logout` or Ctrl-D ends the session.

## Messages between sessions

The people logged in (the console and each SSH shell) can message each other:

- `wall <message>` shows a message in every other shell at once.
- `chat` joins a chat room: each line typed at the `chat> ` prompt goes to everyone else in it.
  `/who` lists the room; `/quit`, Ctrl-C, or Ctrl-D on an empty line leaves.
- `who` lists each shell: slot, name, where it's connected from, whether it's in the chat.

Each person is named after the comment of the trusted key they logged in with (`alice@laptop`;
`key <n>` without one), the console is `console`, and a second login with the same key gets its
slot added (`alice@laptop/2`). A message arrives between keystrokes: the line being typed is
redrawn after it, and a full-screen `edit` holds messages until it quits. Messages are kept only
in memory (the last 16), never in the log or on the card. A client that stops reading holds
a sender up for at most 2 seconds; after that it misses messages, and is told how many. See
[man/wall.txt](../man/wall.txt), [man/chat.txt](../man/chat.txt) and [`src/chat/`](../src/chat/README.md).
