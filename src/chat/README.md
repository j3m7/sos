# chat: wall, chat and who

Messages between the people logged in: the console and each SSH shell. `chat.c` is pure C (it
uses only the shell, `termtext` and the C library) and is host tested (`tests/test_chat.c`).
The platform gives it the time through `chat_hooks_t` (`main.c`).

```
wall / chat line ──► post() ──► ring (16 messages)
                                   │  each shell, after its shell_poll():
                                   └─► chat_deliver(sh): what's new for it, as far as it fits
```

## Peers

Every live shell is a peer (`chat_register()`): `main.c` registers the console, `sshd.c` each
SSH shell when it starts, and unregisters it next to `shell_end()`. SFTP sessions have no shell
and aren't peers. A peer has:

- **a name:** for SSH, the comment of the trusted key the login was signed with (pico-sshd
  keeps that key in `ssh_session_t.auth_key`), or `key <n>`. It's fixed at login, so a later
  `key del` doesn't rename a live session. Names go through `termtext_filter()` (a key
  comment is outside text), are kept to one line and cut on a UTF-8 boundary. A name another
  peer already has gets `/<slot>` added.
- **an id,** unique per registration, so a message is never delivered back to its sender, nor
  to a later login that reuses the sender's slot.
- **a read position** (`next`, a message sequence number) and, while it's in the room, the
  sequence number it joined at, so it doesn't get chat from before it joined.

## The ring and delivery

Posting never writes to another session. `ssh_session_write()` waits (bounded) when a
session's output buffer is full, so writing straight into a slow client's session could stall
the main loop. Instead a message goes into a static ring of `CHAT_RING` (16) messages, each
with its sequence number, sender id and name (copied: the sender may be gone by the time it's
delivered), time stamp, kind (wall, chat, join, leave) and text.

`chat_deliver(sh)` walks the ring from the peer's `next`. A wall is for every peer except the
sender. Chat, join and leave are for peers in the room since before the message. Each message
is written only if it fits in `shell_write_space()` together with the prompt redraw;
otherwise delivery stops until a later pass. While the shell is in raw mode (a full-screen
program such as `edit` owns the screen, `shell_raw_mode()`), nothing is delivered; messages
wait in the ring as for a slow client. Output goes between `shell_hide_input()` and
`shell_show_input()`, like a log line: the typed line is cleared and redrawn after it (a secret
answer stays hidden), and during a job the message gets its own line.

**A full ring.** A pasted burst is posted in one `shell_poll()`, before anyone can receive it,
so overwriting freely would drop lines even for readers that keep up. So when a wall or chat
line would overwrite a message some peer hasn't had yet, and that message is younger than
`CHAT_WAIT_US` (2 s), the sender waits: `wait_for_room()` runs as a job, which also holds back
the rest of the sender's typed input, and posts once the readers have caught up or the
message is old enough. A stalled client therefore holds a sender back for at most 2 s. Ctrl-C
while waiting drops the message (and in the chat leaves the room). Joins and leaves never wait.

When a message is about to be overwritten, every peer that hasn't got past it yet is moved
past it, and if the message was for that peer, its `missed` count goes up. The next delivery
starts with `(N messages missed)`. So the count only covers messages that peer would have
seen.

## `chat`

Joining prints the last `CHAT_HISTORY` (5) chat lines still in the ring and who is in the room,
announces the join, and asks with `shell_ask_eof("chat> ")`. Each answer is handled and the
question asked again, so the room isn't a job, and the session's EOF and busy handling stay as
they are. A NULL answer (Ctrl-C), `SHELL_EOF` (Ctrl-D on an empty line) or `/quit` leaves.
When a session ends in the room, `chat_unregister()` announces that it left.

## Memory

About 7 KB of BSS (5.2 KB ring, 1.6 KB peers with a waiting line each), no heap. Message text
exists only in the ring: it's never logged or written to the card.
