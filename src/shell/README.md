# shell: the command shell

Every channel gets its own `shell_t`: the USB/UART console, and one per SSH session. Shells
share nothing, each with its own line buffer, working directory and running job. The channel is
a `shell_io_t`, with a non-blocking `read()`, a `write()`, an optional `logout()`, newline
translation (`crlf`), and `admin` (true for the console only).

## `shell_core.c`: line editing and control (pure C, host tested)

- **`shell_poll()`** reads whatever input is waiting and edits the line, handling backspace,
  arrow keys, Ctrl-C and Ctrl-D. On Enter it calls `shell_execute()`. It never blocks.
- **Questions:** `shell_ask(sh, prompt, secret, fn, arg)` replaces the `sos>` prompt. The next
  line typed goes to `fn` instead of being run as a command; with `secret` it isn't echoed (Wi-Fi
  passwords). Ctrl-C cancels. `setup`, `wifi set` and `tls roots approve` use it, and the loop
  keeps running while the question waits. `shell_ask_eof()` is the same for a stream of lines
  (the Lua prompt): there, Ctrl-D on an empty line answers `SHELL_EOF` instead of logging out.
- **Jobs:** a long command (`cat`, `cp`, `ls`, a Lua script) runs as a job,
  `shell_run_job(sh, step, arg)`. `step()` is called once per main-loop pass, does a bounded
  amount of work, and returns false when it has finished. Ctrl-C, or the session ending, calls
  it with `cancel` so it can clean up. Input typed meanwhile is kept and run afterwards.
- **Raw input:** `shell_run_raw(sh, step, key, arg)` runs a job in raw input mode, for
  full-screen programs: every key typed goes to `key()` as it arrives (decoded by `keys.c`)
  instead of being kept for afterwards, and Enter is always `'\r'`. Ctrl-C is an ordinary key
  there, so the program needs its own way out; the session ending still cancels it. At most 256
  input bytes are handled a pass, then one `step()`. `shell_raw_mode()` says a shell is in it: then
  `shell_hide_input()`/`shell_show_input()` leave the screen alone, and the log holds console
  messages back (`log_hooks_t.hold`) until the program ends.
- **Output:** `shell_printf()` and `shell_write()` go to the channel. `write_space()` (SSH) lets
  a job slow down for a slow client instead of losing output.

## `login.c`: the console's lock (pure C over the shell API, host tested)

The console asks for the console password before it takes any command (issue #64).

- **Locked** means the shell has a pending secret question, "Password: ". A right answer opens
  the console; a wrong one asks again. Nothing else is printed meanwhile: `main.c` starts the
  console with `shell_bind()` (no greeting), holds the log (`log_hooks_t.hold`) and skips
  `chat_deliver()` while `login_locked()`.
- **Everything else on stdio is closed too.** Libraries print straight to stdout (the Wi-Fi
  driver's boot banner includes the MAC address). While the console is locked `main.c` selects
  a stdio driver that is never registered (`stdio_filter_driver()`), so nothing is sent or
  read; the shell's own reads and writes open that gate for as long as they take.
- **The gate:** `shell_execute()` calls `login_gate()` for every console command line, and it
  refuses while locked. So the lock doesn't depend on the question staying pending.
- **Checking** a password is slow on purpose (`core/pwverify.c`: PBKDF2, then keyed with a
  root-derived key). It runs as a shell job, a slice per main-loop pass, through
  `login_hooks_t`; `core/settings.c` holds the verifier and the key.
- **Delays:** after the nth wrong answer since the last right one the next check waits 1 s,
  2 s, 4 s ... up to 30 s. Ctrl-C drops a check or a wait but not the delay. The typed password
  is wiped from the line buffer and from `login.c`'s copy as soon as its check starts.
- **The count survives a reboot** through three optional hooks (`kept_failures`,
  `count_attempt`, `clear_count`; on the board `core/failflash.c`). An answer is counted before
  its check begins and the count is cleared after a right answer, so a check cut short by a
  reset or by Ctrl-C counts as wrong, and `login_start()` begins with the wait already earned.
- **No password set:** the console is open and `first_use` starts setting one (`setup` on a
  board that was never set up).
- **No usable root secret:** the console stays locked whatever the configuration says, and
  takes only `rootsecret`, `rootsecret forget` and `reboot`.
- **`logout`** on the console locks it again (`console_io.logout` in `main.c`).
- **It locks itself** (`login_poll()`, called from the main loop): when `stdio_usb_connected()`
  goes from true to false, and when nothing was typed (`login_activity()`, from the console's
  read) for the idle limit. It ends a running job (`shell_end()`), discards the line being
  typed and any typed-ahead input (`shell_discard_input()`), drops a pending question, and asks
  for the password. Not while no password is set.

## `keys.c`: terminal keys (pure C, host tested)

`keys_feed()` takes input one byte at a time and turns escape sequences into key codes: the
arrows, Home/End, Insert/Delete, PgUp/PgDn (CSI, SS3 and vt220 `~` forms, modifiers ignored),
and the terminal's answer to a cursor position query (`KEY_CURSOR_POS`). Other bytes come back
as themselves. A control character inside a sequence abandons it, so a stray ESC can't swallow
Enter or Ctrl-C. The line editor uses it too, and ignores the special keys.

## `charwidth.c`: columns on the terminal (pure C, host tested)

`charwidth(cp)` says how many columns a code point takes: 2 for East Asian wide and fullwidth
characters (CJK, Hangul, most emoji), 0 for combining marks, ZWNJ/ZWJ and Hangul medial and
final jamo, 1 otherwise. The editor lays out its lines with it. The range tables in
`charwidth_table.h` are generated from the Unicode database by `tools/gen_charwidth.py`
(Unicode 16.0 now); rerun it with a newer Python to update them.

## `termtext.c`: outside text on the terminal (pure C, host tested)

Text that comes from outside (an HTTP response, say) goes through `termtext_filter()` before
it's printed, so it can't send the terminal escape sequences. Printable ASCII, newline, tab
and valid UTF-8 from U+00A0 up pass; C0 controls, DEL, the C1 controls (raw bytes 0x80–0x9F,
where 0x9B is CSI on 8-bit terminals, and U+0080–U+009F in UTF-8) and invalid UTF-8 become
`.`. A UTF-8 character split between two pieces of input is held until it's complete.

## `shell.c`: the commands

`shell_execute()` dispatches a command line. System commands are implemented here: `help`,
`config`, `wifi`, `net`, `key`, `user`, `hostname`, `setup`, `time`, `log`, `dmesg`, `mem`,
`uptime`, `reboot`, `ssh`, `nts`, `tls`, `logout`/`exit`. Commands that belong to a subsystem are handed to it:
`files_command()` (`files/`), `sdcard_command()` (`sd`), `certstore_command()`
(`tls roots`), and the `lua` command (`lua/`).

Commands that change the configuration first call `require_admin()`, which refuses on any
channel but the console.

## Adding a command

Add it to `shell_execute()` and to `help` in `shell.c`, and to `docs/commands.md`. If it takes
more than a few milliseconds, make it a job so the watchdog and the other sessions keep running.
If it changes the configuration or other protected state, call `require_admin()`.
