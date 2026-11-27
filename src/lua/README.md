# lua: scripts on core 1 (prototype)

Lua 5.4 (`lib/lua`) runs on the second core, so a script can compute flat out without slowing
the main loop on core 0. Core 1 only computes. Anything that touches the system (the card, the
log, the clock, a shell) is a request that core 0 answers.

```
core 1: lua_core1.c                     core 0: lua_host.c (main loop)
  interpreter, `sos` library  ──req──►  lua_host_poll(): one request per pass
                              ◄──ans──  (files, log, time, HTTPS)
  print() ──► output ring buffer ─────► drained into the shell that started the script
```

## `lua_bridge.h`: the shared state

- **One request mailbox** (`lua_req`): core 1 fills in the request (`LR_LOG`, `LR_TIME`,
  `LR_READ`, `LR_WRITE`, `LR_LIST`, `LR_REMOVE`, `LR_MKDIR`, and `LR_HTTP_REQUEST`,
  `LR_HTTP_READ`, `LR_HTTP_CLOSE` for `sos.http`), sets a flag, and waits. Core 0 answers, then
  clears the flag.
- **A 2 KB output ring buffer** for the script's output.
- **Memory:** the mailbox's 4 KB payload and the output ring are one heap block (`lua` in
  `mem`), allocated by core 0 when a script starts and freed when it has ended. Core 0 sets the
  pointers before it starts the job and clears them only after core 1 has reported it done, so
  core 1 never sees them change. With no script running they cost nothing.
- **A cancel flag** that core 0 sets on Ctrl-C, or when the session ends.
- **Hand-over:** one side writes the data, then sets the flag after a memory barrier
  (`__dmb`); the other side sees the flag, then reads the data. `__sev`/`__wfe` wake the
  waiting core.

## `lua_core1.c`: the interpreter

- **Each script gets a fresh Lua state** with the safe standard libraries (base, string, table,
  math, utf8, coroutine, package) plus `sos`. There's no `io`, `os` or `debug`.
- **`require`:** the standard package library, with `package.searchers` replaced by
  `{preload, sd}`. The sd searcher tries each `package.path` template (default
  `/lib/?.lua;/lib/?/init.lua;./?.lua;./?/init.lua`, set in `lua_port.h`) by reading the file
  through core 0, like `sos.read`. Only "no such file" (`LUA_REQ_NOT_FOUND`) moves on to the next
  template; any other failure, such as no SD card, is raised. `package.searchpath` is replaced to
  search the card too. Chunks load as text only, and there are no C modules (`package.loadlib`
  is removed, `cpath` is empty). Each nested `require` is one C nesting level.
- **Stopping:** a hook runs every 1000 instructions and checks the cancel flag. A request
  always gets an answer, so waiting for one never hangs.
  - **`pcall` can't keep a script going.** Once the flag is set, `interrupt()` makes the hook
    run before every instruction, in the current thread and the main one, and raise
    "interrupted" each time. A script that catches the error ends at its next instruction
    outside a `pcall`, and the main chunk is never inside one. Coroutines inherit the hook from
    the thread that creates them.
  - **Two places Lua runs code with hooks off** are closed off. `xpcall` is wrapped, so its
    message handler is skipped while stopping: an error raised by the hook runs the handler with
    hooks off. `setmetatable` is wrapped to refuse `__gc`, since finalizers also run with hooks
    off, including all of them at `lua_close()`.
  - **Not covered:** one long call into C (a pattern match that backtracks for minutes, say)
    can't be interrupted until it returns.
- **Memory:** the heap is shared with core 0 (malloc is mutex-protected). A custom allocator
  caps a script at 128 KB.
- **Core 1's stack:** 32 KB, allocated from the heap when the first script runs (the 4 KB
  SCRATCH_X stack is too small for Lua). Each script paints the unused part first, so
  `lua_core1_stack_peak()` (shown by `mem`) reports how deep that script went.
- **Flash writes:** `flash_safe_execute_core_init()` lets core 0 pause core 1 while it writes
  flash.

## `lua_host.c`: core 0's side

- **The `lua` command** (`lua -e '…'` or `lua <file>`) runs the script as a shell job: the
  shell drains the output ring buffer as fast as its channel takes it.
- **Background scripts** (`lua -b <file>`, and `/autorun.lua` at boot) have no owner shell:
  `background` is set instead, and `script_alive()` (owner or background) is what lets requests
  change files. `lua_host_poll()` drains their output into the log a line at a time (control
  characters as `.`; long lines continue on the next), and logs how they ended. Each pass drains
  only what core 1 had written when it started: core 1 refills the buffer as it's drained, so
  "until it's empty" would never end with a script printing in a loop. At most 10 lines a second
  are logged; the rest are counted and reported. `lua ps` and `lua stop` work on whatever runs;
  a prompt stopped while its shell waits for a line is closed from `lua_host_poll()`, which hands
  the shell back (`shell_cancel_question()`).
- **Autorun:** `autorun_check()` runs once, 3 s after boot, from `lua_host_poll()`. It starts
  `/autorun.lua` if the card has one, unless the last reset was a fault (`fault_last()`) or the
  watchdog (`wdog_caused_reset()`): a script that brings the device down then doesn't run again
  at every boot.
- **The prompt** (`lua` alone) is a question (`shell_ask_eof`) whose answers are lines of Lua.
  Each line runs as a short shell job (`repl_step`): it hands the line to core 1
  (`lua_job.repl_line`, then `repl_cmd`), shows the output, and asks again (`>` or `>>`).
  Ctrl-C while a line runs sets `cancel` and goes back to the prompt at once; the next line waits
  until core 1 has stopped the old one, and its leftover output is dropped. Ctrl-D (`SHELL_EOF`)
  or `exit` sets `repl_quit`. If the owner's shell is neither answering the prompt nor running
  one of its jobs, its session has ended, and `lua_host_poll()` closes the prompt.
- **Timers** (`sos.every`, `sos.after`, `sos.cancel`) are kept in a registry table, id ->
  `{fn, due, every}` (`time_us_64()` microseconds). `run_timers()` runs after the main chunk
  returns: it waits for the earliest (checking `cancel` every 5 ms), runs it with the traceback
  handler, and reschedules a repeating one (skipping runs missed meanwhile). The script ends when
  no timers are left, when stopped, or when a callback raises an error.
- **Core 1's side of the prompt** (`run_repl` in `lua_core1.c`) keeps one Lua state for the
  session and waits for each line. A first line is tried as `return <line>` so expressions print
  their value; a syntax error at `<eof>` means the statement continues, and the text is kept for
  the next line. After a Ctrl-C it resets the hook that `interrupt()` left running every
  instruction, and doesn't print the "interrupted" error.
- **`lua_host_poll()`** answers at most one request per main-loop pass, each a bounded amount
  of work, so the watchdog, SSH and the console keep going whatever the script does.
- **HTTPS (`sos.http`):** core 0 owns the script's one request (`net/http.h`). Core 1 sends
  `LR_HTTP_REQUEST` with a pointer to an `http_opts_t` on its stack, whose strings are in the
  script's Lua state: core 0 reads them while core 1 waits, and `http_start()` copies them.
  These requests stay pending across passes: each pass polls the connection once, and answers
  when the headers are in (status, reason, and the headers as `http_resp_t` stores them), or
  when there's body to hand over (`LR_HTTP_READ`, up to 4 KB per answer). They're served only
  from the main loop (`lua_host_poll(true)`), never from a shell job, because the TLS
  handshake needs stack and an SSH session's job runs deep inside `sshd_poll()`. Ctrl-C answers
  a pending one with "interrupted" at once. `http_request()` keeps a guard in a to-be-closed
  slot, so however the call ends with the body unread (an error, "not enough memory", Ctrl-C,
  `max_body`), it sends `LR_HTTP_CLOSE` and the TLS connection is freed while the script goes on.
  The `LR_HTTP_READ` answer that ends the body (`len` 0) carries the request's timings and TLS
  figures (`lua_http_stats_t`), taken just before core 0 frees the request; the script adds
  them to the response table.
  The exception is a coroutine that dies with an error, which isn't unwound until
  `coroutine.close()`: core 0 then drops the request when the next one starts, when a prompt line
  ends, or when the script ends. The whole body goes into one Lua string, limited by `max_body` (default 32 KB),
  so a script stays within its 128 KB.
- **File rights:** script file changes pass `files_write_allowed()` with no shell, so a script
  has SFTP's rights even when started on the console. The console's extra rights (`/certs`)
  are for what the person types there, not for code they run. Once the script's shell is gone,
  its requests change nothing.

## Board health: `lua_info.c`, `lua_health.c`

`sos.sensors`, `sos.mem`, `sos.version`, `sos.reset_reason`, `sos.glitch` and `sos.wifi.status`
are one request, `LR_INFO`, with the table's number (`lua_health_table_t`) in `offset`. Core 0
(`lua_health_fill()`) writes the table into the request's data as `lua_info.h` records: named
integers, floats, booleans and strings, and nested tables up to 4 deep. Core 1
(`lua_info_push()`) turns them into one Lua table. So a new table needs code on core 0 only, and
the format is tested on the host (`tests/test_lua_info.c`). Each table is read without waiting
(the sensors' latest readings, not new ones), so `LR_INFO` is answered in the same pass, from a
shell job too. `sos.mem` adds `script`, the script's own allocation, which core 1 keeps.

`sos.dmesg` is `LR_INFO` too (`LH_DMESG`), with its cursor in `lua_req.arg`. A record with an
empty name is appended to its table as a list item. Core 0 (`dmesg_table()`) writes as many
entries as fit (`log_history_entry()`, which also says whether entries after the cursor were
overwritten), with the cursor after the last one and `more`; core 1 asks again from that cursor
while `more` is set (at most 8 pages) and joins the pages into one list.

## `lua_port.h`

`lua_port.h` is force-included into the Lua sources (see `CMakeLists.txt`).

- **Output:** it sends Lua's own output (`lua_writestring`) to the output ring buffer
  (`sos_lua_write()`), since stdio isn't usable on core 1.
- **Errors:** scripts get errors back as return values, so Lua's own error printing is
  dropped.
- **C stack:** it limits nested C calls (`LUAI_MAXCCALLS` 20) to fit core 1's stack. Only
  nesting through C counts (pcall, callbacks such as `string.gsub`'s, metamethods, coroutines,
  parser nesting); Lua-to-Lua calls don't. Measured per level: `string.gsub` callback 1168
  bytes (the worst), `__tostring` via `string.format` 968, pcall 520, coroutine 456. The worst
  case ends at about 24 KB of the 32 KB. The earlier limit of 120 overflowed the stack, and
  the stack guard fault reset the board. An error close to the limit can leave the traceback
  handler no room (Lua: "error in error handling"); `lua_core1.c` reports that as a C stack
  overflow too.

The same CMake rule makes Lua use the C library's `snprintf` (`-Dsnprintf=__real_snprintf`).
The SDK's lightweight `snprintf` prints doubles with only about 10 digits.
