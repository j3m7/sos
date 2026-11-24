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
- **Output:** `shell_printf()` and `shell_write()` go to the channel. `write_space()` (SSH) lets
  a job slow down for a slow client instead of losing output.

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
