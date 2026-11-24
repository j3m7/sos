# lua: scripts on core 1 (prototype)

Lua 5.4 (`lib/lua`) runs on the second core, so a script can compute flat out without slowing
the main loop on core 0. Core 1 only computes. Anything that touches the system (the card, the
log, the clock, a shell) is a request that core 0 answers.

```
core 1: lua_core1.c                     core 0: lua_host.c (main loop)
  interpreter, `sos` library  ──req──►  lua_host_poll(): one request per pass
                              ◄──ans──  (files, log, time)
  print() ──► output ring buffer ─────► drained into the shell that started the script
```

## `lua_bridge.h`: the shared state

- **One request mailbox** (`lua_req`): core 1 fills in the request (`LR_LOG`, `LR_TIME`,
  `LR_READ`, `LR_WRITE`, `LR_LIST`, `LR_REMOVE`, `LR_MKDIR`), sets a flag, and waits. Core 0
  answers, then clears the flag.
- **A 2 KB output ring buffer** for the script's output.
- **A cancel flag** that core 0 sets on Ctrl-C, or when the session ends.
- **Hand-over:** one side writes the data, then sets the flag after a memory barrier
  (`__dmb`); the other side sees the flag, then reads the data. `__sev`/`__wfe` wake the
  waiting core.

## `lua_core1.c`: the interpreter

- **Each script gets a fresh Lua state** with the safe standard libraries (base, string, table,
  math, utf8, coroutine) plus `sos`. There's no `io`, `os`, `package` or `debug`.
- **Stopping:** a hook runs every 1000 instructions and checks the cancel flag. A request
  always gets an answer, so waiting for one never hangs.
- **Memory:** the heap is shared with core 0 (malloc is mutex-protected). A custom allocator
  caps a script at 128 KB.
- **Core 1's stack:** 32 KB, allocated from the heap when the first script runs (the 4 KB
  SCRATCH_X stack is too small for Lua).
- **Flash writes:** `flash_safe_execute_core_init()` lets core 0 pause core 1 while it writes
  flash.

## `lua_host.c`: core 0's side

- **The `lua` command** (`lua -e '…'` or `lua <file>`) runs the script as a shell job: the
  shell drains the output ring buffer as fast as its channel takes it.
- **`lua_host_poll()`** answers at most one request per main-loop pass, each a bounded amount
  of work, so the watchdog, SSH and the console keep going whatever the script does.
- **File rights:** script file changes pass `files_write_allowed()` with no shell, so a script
  has SFTP's rights even when started on the console. The console's extra rights (`/certs`)
  are for what the person types there, not for code they run. Once the script's shell is gone,
  its requests change nothing.

## `lua_port.h`

`lua_port.h` is force-included into the Lua sources (see `CMakeLists.txt`).

- **Output:** it sends Lua's own output (`lua_writestring`) to the output ring buffer
  (`sos_lua_write()`), since stdio isn't usable on core 1.
- **Errors:** scripts get errors back as return values, so Lua's own error printing is
  dropped.
- **C stack:** it limits nested C calls (`LUAI_MAXCCALLS` 120) to fit core 1's stack.

The same CMake rule makes Lua use the C library's `snprintf` (`-Dsnprintf=__real_snprintf`).
The SDK's lightweight `snprintf` prints doubles with only about 10 digits.
