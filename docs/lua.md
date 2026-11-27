# Lua scripts (prototype)

Lua 5.4 runs on the second core (core 1), so a script can compute flat out without slowing the
console, SSH or the watchdog on core 0. Scripts live on the SD card:

```sh
lua -e 'print(1 + 1)'            # a one-liner
lua /scripts/report.lua          # a file (relative paths start at the working directory)
lua                              # an interactive prompt
```

Quote the code after `-e` with `'...'`: the shell splits a line into words and takes quotes,
`|` and `>` for itself (QUOTING in `man intro`), so `lua -e print("hi")` would lose its quotes
and `lua -e print(2 > 1)` would write a file named `1)`. `lua -e 'print("hi")'` passes the
code as typed.

At the prompt (`>`), each line runs as you enter it, in one Lua state for the whole session, so
variables and loaded modules stay. An expression's value is printed (`> 2^10` prints `1024.0`),
and a statement that isn't finished continues on the next line (`>>`). Ctrl-C interrupts the
running line, or drops an unfinished statement; Ctrl-D on an empty line, or `exit`, leaves. Lines
typed or piped in while one runs wait their turn, so `printf 'lua\nprint(1)\nexit\n' | ssh sos`
works too.

The script's output goes to the shell that started it; Ctrl-C stops it, as does ending the SSH
session. One script runs at a time.

## Background scripts and autorun

`lua -b <file>` runs a script with no shell: its output goes to
the log, a line at a time (`dmesg`; INFO, so not on the console), and its end is logged
(finished, stopped, or failed with the error). `lua ps` shows the running script, and `lua stop`
stops it, wherever it runs. At most 10 lines a second of a script's log lines (a background
script's output and `sos.log`, together) reach the log; the rest are counted ("... N lines
dropped"), so a chatty script can't push everything else out of the log history. If the card has `/autorun.lua`, it starts in the background a few seconds
after boot. After a crash or watchdog reset it's skipped (and that's logged), so a script that
brings the device down can't do it at every boot. It stays off until the next normal boot
(`reboot`, or power), even if the reset had nothing to do with it: on an unattended board, check
the log, or start it by hand with `lua -b /autorun.lua`. Remove or rename the file to turn
autorun off. Timers (`sos.every`, `sos.after`) are how a background script does periodic work:

```lua
-- /autorun.lua: log how many entries the card's top directory has, every 10 minutes
sos.every(600, function()
  local n = 0
  for _, e in ipairs(sos.list("/") or {}) do n = n + 1 end
  sos.log("autorun: " .. n .. " entries in /")
end)
```

Background scripts can write files like any other script (not `/certs`).

## What a script can use

The base, string, table, math, utf8 and
coroutine libraries, `require` (see [Modules](#modules)), and `sos`:

| Function | |
|---|---|
| `sos.read(path)` | file contents as a string |
| `sos.write(path, s)`, `sos.append(path, s)` | write / append (through `files_write_allowed()`) |
| `sos.list([path])` | `{ {name=, size=, dir=}, ... }` |
| `sos.remove(path)`, `sos.mkdir(path)` | delete a file or empty directory, create a directory |
| `sos.time()`, `sos.uptime()` | seconds since 1970 (UTC), seconds since boot |
| `sos.sleep(ms)`, `sos.log(msg [, level])` | wait (Ctrl-C still works), write a line to the system log: facility `user`, tagged with the script's path (`lua -e` for code given with `-e`, `lua prompt` at the prompt), at `"notice"` unless a level (`"err"`, `"warning"`, `"notice"`, `"info"`, `"debug"`) is given. Shares the background output's limit of 10 lines a second |
| `sos.every(s, fn)`, `sos.after(s, fn)` | call `fn(id)` every `s` seconds (at least 0.01) / once after `s`; returns the timer's id. Timers run once the script's main code has returned: the script then keeps going until no timers are left, it's stopped, or a callback fails. Not at the prompt |
| `sos.cancel(id)` | stop a timer; true if it existed |
| `sos.http.get(url [, opts])` | HTTPS GET: `{status=, reason=, headers=, body=, headers_ms=, elapsed_ms=, tls=}` (see [HTTPS](#https)) |
| `sos.http.post(url, body [, opts])` | HTTPS POST of the string `body` |
| `sos.http.request(opts)` | any method: `opts.url` and `opts.method` (default `GET`, or `POST` with a body) |
| `sos.sensors()`, `sos.mem()`, `sos.version()` | tables about the board (see [Board health](#board-health)) |
| `sos.reset_reason()`, `sos.glitch()`, `sos.wifi.status()` | the same |
| `sos.dmesg([cursor])` | the system log's entries, and a cursor to read only newer ones next time (see [The log](#the-log)) |

Failures return `nil` and a message. There's no `io`, `os` or `debug`: scripts reach the system
only through `sos`. `__gc` metamethods aren't supported (`setmetatable` refuses them; use a
`<close>` variable).

## Modules

`require "name"` loads a Lua module from the card, searching `package.path`. The
default is `/lib/?.lua;/lib/?/init.lua;./?.lua;./?/init.lua`, where `./` is the working
directory and dots in the name become directories (`require "net.util"` tries
`/lib/net/util.lua`, ...). A script may change `package.path` before calling `require`.
Modules are loaded once per script (`package.loaded`), and `package.preload` works as usual.
Only Lua source is accepted: no precompiled chunks and no C modules.

```lua
-- /lib/greet.lua
local M = {}
function M.hello(who) return "hello, " .. who end
return M

-- a script
local greet = require "greet"
print(greet.hello("pico"))
```

## HTTPS

`sos.http` makes one request at a time with the HTTPS client (`src/net/README.md`),
from scripts started on the console or over SSH. The server certificate is checked as for every
TLS connection, and redirects aren't followed (the script sees the 3xx and `headers.location`).
Any status is a response; `nil` and a message mean there was none (DNS, TLS, timeout, a bad
option, a body over `max_body`). Options, all optional except `url` for `request`:

| Option | |
|---|---|
| `headers` | extra request headers, `{["X-Name"] = "value"}`. `Content-Type` here is the same as `content_type`; `Host`, `Content-Length`, `Connection` and the like are set by the client and refused |
| `body`, `content_type` | request body (up to 16 KB) and its type (no `Content-Type` header if not given) |
| `timeout` | milliseconds until the headers arrive, then between pieces of the body (default 30000) |
| `max_body` | the most response body to accept, in bytes (default 32 KB; the script's 128 KB covers it) |

The response's `headers` table has lower-case names; a repeated header's values are joined with
`, `. The body is the server's bytes as they are: a script that prints it prints whatever the
server sent, escape sequences included.

The response also says how long the request took, for scripts that check a server:

| Field | |
|---|---|
| `headers_ms` | from the start of the request (DNS, TCP and TLS included) until the status line and headers were in |
| `elapsed_ms` | from the start until the end of the body, as the script received it: the body comes to core 1 4 KB per main-loop pass, so a large one adds a few milliseconds per 4 KB |
| `tls.handshake_ms` | the TLS handshake, from the TCP connection to the handshake done |
| `tls.longest_step_us` | the longest single wolfSSL call, in microseconds: how long the main loop was held up at most |
| `tls.heap_peak` | the most heap wolfSSL used at once, in bytes |
| `tls.dates_unchecked` | `true` if the certificate dates weren't checked because the clock wasn't set |

A request that fails returns `nil` and a message, without timings.

```lua
local r, err = sos.http.get("https://letsencrypt.org/")
if r then
  print(r.status, r.headers_ms .. " ms to headers", r.elapsed_ms .. " ms in all",
        "handshake " .. r.tls.handshake_ms .. " ms")
end
```

```lua
local r, err = sos.http.post("https://ntfy.example/alerts", "disk almost full",
                             {headers = {Title = "sos"}})
if not r then print("failed: " .. err) elseif r.status ~= 200 then print(r.status, r.reason) end
```

## Board health

Six functions return a table about the board: what `temp`, `power`, `mem`, `version`, `uptime`,
`glitch` and `net` show, read as values. Nothing waits for new measurements: the
temperature and supply are the latest readings, which the main loop takes every 10 seconds. A
field the board doesn't know (no reading yet, Wi-Fi not connected) is `nil`.

**`sos.sensors()`**

| Field | |
|---|---|
| `temp`, `temp_min`, `temp_max` | the chip's temperature in degrees Celsius (a float, 0.1 steps): latest, lowest and highest since boot |
| `temp_calibrated` | whether `temp cal` has set an offset (uncalibrated readings are a few degrees off) |
| `vsys`, `vsys_min`, `vsys_max` | the supply voltage (VSYS) in volts: latest, lowest and highest since boot |
| `usb` | whether USB power is present |

**`sos.mem()`** (bytes)

| Field | |
|---|---|
| `heap.used`, `heap.peak`, `heap.total` | the heap, as the first line of `mem` |
| `tags[name].used`, `tags[name].peak` | what each feature holds: `tags["ssh/tls"]`, `tags.lua`, `tags.http`, ... |
| `lwip.used`, `lwip.peak`, `lwip.total` | lwIP's own heap |
| `stack.peak`, `stack.size` | core 0's stack |
| `core1_stack.peak`, `core1_stack.size` | core 1's stack, which the running script uses |
| `script.used`, `script.peak`, `script.limit` | what this script has allocated, out of its 128 KB |

**`sos.version()`**

| Field | |
|---|---|
| `firmware` | the firmware's commit, as `version` prints it |
| `serial` | the board's serial number |
| `chip`, `chip_revision`, `rom_version` | the chip (`"RP2350 A2 (revision 2, ROM 2)"`), and its revision and ROM version as numbers |
| `secure_boot` | `true` if only firmware signed with a boot key in OTP starts |
| `debug` | the debug port: `"enabled"`, `"secure disabled"` or `"disabled"` |
| `root_secret` | where the root secret is: `"otp"`, `"flash"` or `"none"` |
| `console_only` | `true` if settings files written over USB are refused |

**`sos.reset_reason()`**

| Field | |
|---|---|
| `cause` | why the board last started: `"fault"` (a crash), `"glitch"` (the glitch detector), `"watchdog"`, or `"other"` (power on, `reboot`, flashing, the reset button) |
| `detail` | for a fault, what `uptime` prints after "last reset:"; `nil` otherwise |

**`sos.glitch()`**

| Field | |
|---|---|
| `mode` | `"on"` (a trigger resets the board), `"watch"` (a trigger is only logged) or `"off"` |
| `sensitivity` | 0 (lowest) to 3 (highest); `nil` when off |
| `armed` | whether it's armed now (`false` if the loop guard kept it off at this boot: `guard_tripped`) |
| `resets` | detector resets since power on |
| `caused_reset` | whether the last reset was the detector's; then `last = {detectors=, uptime=, temp=, vsys=}`: which detectors fired (bits 0-3), after how many seconds up, and the last readings before it |
| `watched` | when watching, after a trigger: `{detectors=, uptime=, temp=, vsys=}` for the first |

**`sos.wifi.status()`**

| Field | |
|---|---|
| `state` | `"connected"`, `"connecting"`, `"scanning"`, `"waiting to retry"`, `"stopped"` or `"not configured"` |
| `up` | `true` when connected |
| `ssid`, `bssid` | the network and its access point (when connected) |
| `rssi`, `noise` | signal and the chip's noise floor, in dBm |
| `channel`, `rate_mbps`, `txpower_dbm` | the channel, the current transmit rate and the power limit |
| `ip`, `gateway` | the addresses (when connected) |
| `arp_received`, `arp_sent` | ARP counters, as `net` shows them |

```lua
-- /autorun.lua: log a warning when the chip is hot or the supply sags
sos.every(60, function()
  local s = sos.sensors()
  if (s.temp or 0) > 60 or (s.vsys or 5) < 4.2 then
    sos.log(string.format("health: %.1f C, %.2f V", s.temp or 0, s.vsys or 0), "warning")
  end
end)
```

## The log

`sos.dmesg([cursor])` reads the system log kept in memory, the lines `dmesg` shows. It returns
three values:

- a list of entries, oldest first, each `{time=, level=, facility=, text=}`: seconds since boot
  (a float, millisecond steps), `"err"`, `"warning"`, `"notice"`, `"info"` or `"debug"`,
  `"kern"`, `"auth"`, `"daemon"`, `"user"` or `"none"`, and the message without the time and the
  level's tag;
- a cursor: pass it to the next call to get only the entries logged since;
- `lost`: `true` if entries after the cursor you passed were dropped before this call.

The log keeps only the last 4 KB of messages (about 50 lines), so a script that watches it should
read it often enough; `lost` says when it didn't. Without a cursor (or with 0) it returns all that
is kept. A script's own `sos.log` lines and a background script's output are in it too.

```lua
-- /autorun.lua: send each warning or error to a phone, once a minute at most
local _, cursor = sos.dmesg()                   -- start from now: skip what's already there
sos.every(60, function()
  local entries, next_cursor, lost = sos.dmesg(cursor)
  if not entries then return end                -- nil and an error: try again next minute
  cursor = next_cursor
  local lines = {}
  for _, e in ipairs(entries) do
    if (e.level == "err" or e.level == "warning") and e.facility ~= "user" then
      lines[#lines + 1] = string.format("[%.0f] %s", e.time, e.text)
    end
  end
  if lost then lines[#lines + 1] = "(some log lines were lost)" end
  if #lines > 0 then
    sos.http.post("https://ntfy.example/sos", table.concat(lines, "\n"))
  end
end)
```

Leaving out `user` keeps the script from sending its own `sos.log` lines and output. A post
that fails is logged as a `daemon` warning, which the next run sends: at most one line a minute.

## Limits and internals

- **Stopping:** Ctrl-C ends the script even if it catches errors with `pcall` or `xpcall`: from
  then on, any code it runs outside a `pcall` raises "interrupted" again. The exception is a
  single long call into C, such as a pattern match that backtracks for minutes, which can't be
  interrupted until it returns. Core 0, SSH and the watchdog keep running meanwhile, but no
  other script can start.

- **How it works:** core 1 never touches the card, network or shells. Each `sos` call is a
  request that core 0's main loop answers (one per pass; an HTTPS request is answered once its
  headers or the next piece of its body are in, with the main loop going on meanwhile); output goes through a buffer that the
  shell drains as its channel takes it. The heap is shared (malloc is mutex-protected), and each
  script may allocate up to 128 KB ("not enough memory" beyond). Core 1 gets a 32 KB stack from
  the heap when the first script runs, and pauses while core 0 writes flash.
- **Nesting:** plain Lua recursion is limited only by memory, but nesting through `pcall`,
  callbacks from C (`string.gsub`, `table.sort`), metamethods and coroutines uses core 1's C
  stack and stops at 20 levels with "C stack overflow". `mem` shows how much of core 1's stack
  the last script used.
- **Speed** (debug build): `fib(25)` takes 1.4 s; a million-iteration floating-point loop 2.6 s.
- **Prototype limits:** numbers are formatted with newlib's `snprintf`
  on core 1 (its float conversion has state that isn't safe to share between cores, and core 0
  doesn't use it today).
