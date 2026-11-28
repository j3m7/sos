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
| `sos.rename(from, to [, replace])` | rename or move a file or directory. `to` mustn't exist, unless `replace` is true and it's a file, which is then replaced (there's a moment when only `from` exists). The same path, or one differing only in case, is refused |
| `sos.copy(from, to)` | copy a file (4 KB per request each way); `to` is created or replaced; returns the size. Copying a file onto itself (however it's spelled) is refused. A failure after part of it was written removes the copy |
| `sos.stat(path)`, `sos.exists(path)` | `{size=, dir=, time=}` (`time` in seconds since 1970, from the card's timestamp) or `nil` and `"no such file"`; `true` or `false` |
| `sos.sd.info()` | what `sd info` shows: `{mounted=, card=, card_bytes=, encrypted=, fs=, total=, free=, cluster=}` (bytes; `mounted` false and nothing else without a card). On a FAT32 card the first call counts the free space, which can take seconds on a large card (later ones are quick; exFAT is always quick) |
| `sos.time()`, `sos.uptime()` | seconds since 1970 (UTC), seconds since boot |
| `sos.date([format [, t]])`, `sos.time(table)` | dates, as Lua's `os.date` and `os.time` but in UTC (see [Dates and the clock](#dates-and-the-clock)) |
| `sos.localdate([format [, t]])`, `sos.localtime(table)`, `sos.zone()` | the same in the board's time zone (`time zone`), and that zone's name and rule (the same section) |
| `sos.clock()` | the clock's state: set or not, the last sync, the RTC (the same section) |
| `sos.clock_sync()` | start an NTS sync, as `time sync` does: `true`, or `nil` and why (one is already running, or a script started one less than 5 minutes ago). `sos.clock()` shows the result |
| `sos.who()` | who is logged in, as `who` lists them: `{ {slot=, name=, from=, seconds=, chat=}, ... }` |
| `sos.ssh.status()` | what `ssh` shows: `{running=, key_type=, fingerprint=, max_sessions=, logins=, refused_busy=, refused_penalty=, dropped_stale=, sessions = { {slot=, login=, address=, state=, sftp=, seconds=}, ... }}` |
| `sos.tls.roots()` | the roots TLS trusts, as `tls roots` shows them: `{builtin = {names...}, file = {path=, state=, used=, count=, why=}}` |
| `sos.wall(msg)` | send `msg` to every shell, with the script's path as the sender; returns how many (0 if nobody is logged in), or `nil` and why: other shells haven't read the last 16 messages yet (try again shortly), or more than 10 a second (the limit `sos.log` shares) |
| `sos.random(n)` | `n` random bytes (up to 128 KB, a script's memory) from the chip's random number generator, through wolfCrypt's DRBG on core 0 |
| `sos.sleep(ms)`, `sos.log(msg [, level])` | wait (Ctrl-C still works), write a line to the system log: facility `user`, tagged with the script's path (`lua -e` for code given with `-e`, `lua prompt` at the prompt), at `"notice"` unless a level (`"err"`, `"warning"`, `"notice"`, `"info"`, `"debug"`) is given. Shares the background output's limit of 10 lines a second |
| `sos.every(s, fn)`, `sos.after(s, fn)` | call `fn(id)` every `s` seconds (at least 0.01) / once after `s`; returns the timer's id. Timers run once the script's main code has returned: the script then keeps going until no timers are left, it's stopped, or a callback fails. Not at the prompt |
| `sos.cancel(id)` | stop a timer; true if it existed |
| `sos.wifi.scan()` | the Wi-Fi networks around, strongest first: `{ {ssid=, rssi=}, ... }` (a scan takes about half a second; the names a question's Tab offers come from the same scan) |
| `sos.net.resolve(host)` | the host's IPv4 address as text (a DNS lookup, up to 20 s), or `nil` and why |
| `sos.ping(host [, count])` | `count` echo requests (4 by default, at most 100), one a second, as `ping` sends them; then `{address=, sent=, received=, errors=, not_sent=, min_ms=, avg_ms=, max_ms=}` (the times only if something answered), or `nil` and why (Wi-Fi down, the lookup failed). The script waits meanwhile; Ctrl-C stops it |
| `sos.http.get(url [, opts])` | HTTPS GET: `{status=, reason=, headers=, body=, headers_ms=, elapsed_ms=, tls=}` (see [HTTPS](#https)) |
| `sos.http.post(url, body [, opts])` | HTTPS POST of the string `body` |
| `sos.http.head(url [, opts])` | HTTPS HEAD: the status and headers |
| `sos.http.download(url, path [, opts])` | HTTPS GET into a file on the card (see [HTTPS](#https)) |
| `sos.http.request(opts)` | any method: `opts.url` and `opts.method` (default `GET`, or `POST` with a body) |
| `sos.sensors()`, `sos.mem()`, `sos.version()` | tables about the board (see [Board health](#board-health)) |
| `sos.reset_reason()`, `sos.glitch()`, `sos.wifi.status()`, `sos.health()` | the same |
| `sos.reboot([why])` | restart the board, logging `why` and the script's name. Refused (`nil` and a message) until the board has been up 10 minutes: a normal restart runs `/autorun.lua` again, so a script that reboots at once would loop |
| `sos.hash(alg, data [, raw])`, `sos.hmac(alg, key, data [, raw])` | SHA digests and HMACs, in hex (see [Hashes and base64](#hashes-and-base64)) |
| `sos.base64.encode(s [, "url"])`, `sos.base64.decode(s [, "url"])` | base64, standard or URL-safe |
| `sos.json.decode(s)`, `sos.json.encode(v)`, `sos.json.null` | JSON to Lua values and back (see [JSON](#json)) |
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

`sos.http.download(url, path [, opts])` writes the body to the file at `path` (as `sos.write`
does, so not in `/certs`), 4 KB at a time, so it can be far larger than a script's memory. Only a
2xx response is written: the response then has `size` (the bytes written) instead of `body`. Any
other status leaves the file alone and comes back with its `body` as `get` would (an error page,
say). With no `max_body`, a download has no size limit (the error page still stops at 32 KB). If
the transfer fails (the connection, the card, a body over `max_body`), the call returns `nil` and
the message. The body is written to `<path>.part` and renamed over `path` only once it's all
there, so a failed download never touches a file already at `path` (except for the instant of the
replace itself): refreshing a file is safe.
A failure removes the `.part` file once anything was written to it; Ctrl-C leaves it.

```lua
local r, err = sos.http.download("https://example.org/firmware-notes.txt", "/notes.txt")
if not r then print("failed: " .. err)
elseif r.status ~= 200 then print(r.status, r.body)
else print("saved " .. r.size .. " bytes in " .. r.elapsed_ms .. " ms") end
```

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
| `tls.cert` | the server's own certificate, once the handshake accepted it: `{subject=, issuer=, not_before=, not_after=}`: its common name, the issuer's name as wolfSSL writes it (`/C=US/O=Let's Encrypt/CN=R11`, cut at 127 characters), and when it's valid from and until, in seconds since 1970 (UTC). Missing if it couldn't be read |

A request that fails returns `nil` and a message, without timings.

```lua
-- warn two weeks before a server's certificate expires
local r = sos.http.head("https://example.org/")
local cert = r and r.tls and r.tls.cert
if cert and cert.not_after and cert.not_after - sos.time() < 14 * 86400 then
  sos.log("certificate of example.org expires " .. sos.date("%F", cert.not_after), "warning")
end
```

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

## Dates and the clock

SOS keeps time in UTC, and `sos.date` and `sos.time` work in UTC whatever the board's time
zone is: what a script writes into a file, a log or a request stays UTC. Local time is there
only when a script asks for it, with `sos.localdate` and `sos.localtime` (below).

`sos.date([format [, t]])` is Lua's `os.date`: the time `t` (seconds since 1970; the time now
if not given) as text, with strftime's conversions (`%Y-%m-%d %H:%M:%S` if no format is given),
or as a table with `"*t"`: `{year, month, day, hour, min, sec, wday, yday, isdst}`, where `wday`
1 is Sunday and `isdst` is always `false`. A `!` before the format, as in `"!*t"`, changes nothing.
The conversions are C99's in the C locale (`%a %A %b %B %c %C %d %D %e %F %h %H %I %j %m %M %n %p
%r %R %S %t %T %u %w %x %X %y %Y %z %Z %%`), plus `%s` for the seconds; others are an error.
Years 1 to 9999.

`sos.time(table)` is `os.time`: the time of a date given as a table. `year`, `month` and `day`
are needed; `hour` is 12 if not given, `min` and `sec` 0. A field out of its range carries over
(`month = 13` is January of the next year, `day = 0` the last day of the month before), which
makes date arithmetic easy.

Without `t`, `sos.date` needs the clock: it returns `nil` and a message if the clock isn't set
yet, like `sos.time()`.

```lua
print(sos.date())                               -- 2026-11-29 14:03:05
print(sos.date("%A %e %B", 0))                  -- Thursday  1 January
-- seconds until the next 03:00 UTC
local now = sos.time()
local d = sos.date("*t", now)
local next3 = sos.time{year = d.year, month = d.month, day = d.day + (d.hour >= 3 and 1 or 0),
                       hour = 3}
print(next3 - now)
```

### Local time

`sos.localdate([format [, t]])` and `sos.localtime(table)` are `sos.date` and `sos.time(table)`
in the board's time zone, the one `time zone` sets (see
[Time zones](time.md#time-zones)). Without a zone they are the same as the UTC ones.

- `sos.localdate`'s fields are local time; `%Z` is the zone's abbreviation (`CET`), `%z` its
  offset (`+0100`), and `"*t"`'s `isdst` says whether daylight saving time is in force. `%s` is
  still seconds since 1970 UTC. A `!` before the format means UTC, as in `os.date`.
- `sos.localtime(table)` reads the table as local time. On the night the clock goes back, an
  hour comes twice: the earlier is taken, unless the table's `isdst` says which (`true` the
  first, daylight saving one; `false` the second). On the night it goes forward, an hour never
  comes: a time in it is read with the offset before the change, so `02:30` on a night that
  skips from `02:00` to `03:00` is `03:30`.
- `sos.zone()` returns the zone's name and POSIX rule (`"Europe/Madrid"`,
  `"CET-1CEST,M3.5.0,M10.5.0/3"`), or `"UTC"` and `nil` when none is set.

A script uses the zone the board had when it started: changing it with `time zone` while the
script runs changes nothing for that script.

```lua
print(sos.localdate("%H:%M %Z"))                -- 15:03 CET
-- seconds until the next 07:00 local time
local now = sos.time()
local d = sos.localdate("*t", now)
local t = sos.localtime{year = d.year, month = d.month, day = d.day, hour = 7}
if t <= now then t = sos.localtime{year = d.year, month = d.month, day = d.day + 1, hour = 7} end
print(t - now)
```

`sos.clock()` returns what `time` shows:

| Field | |
|---|---|
| `set` | whether the clock has a time |
| `source` | where it came from: `"nts"` (authenticated), `"sntp"`, `"rtc"` (the battery-backed clock at boot), or `nil` |
| `time` | seconds since 1970, when set |
| `server`, `server_default` | the NTS server, and whether it's the build's default |
| `auto_hours` | how often it syncs (`time auto`), 0 for never |
| `syncing`, `syncing_sntp` | the server of a sync running now, and whether it's plain SNTP |
| `last_sync` | `{ago=, step_ms=, delay_ms=}`: seconds since the last sync, how far it moved the clock (`nil` if it set an unset clock) and its round trip (NTS) |
| `last_failure` | `{ago=, why=, retry_in=}` after a failed sync |
| `rtc` | `{status=, time=, battery_low=, cal=}`: the RTC's state (`"ok"` or why not), its time, its battery and its calibration |

## Board health

Seven functions return a table about the board: what `temp`, `power`, `mem`, `version`, `uptime`,
`glitch`, `net` and `health` show, read as values. Nothing waits for new measurements: the
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

**`sos.health()`**: the self-checks' latest results (`health`). A script reads them and can't
run a check: running them is the console's (`health run`).

| Field | |
|---|---|
| `worst` | the worst status of any check: `"pass"`, `"warn"` or `"fail"` (a check not run yet, or skipped, counts as a pass) |
| `boot_ms` | how long the boot checks took, in milliseconds |
| `checks` | a list in `health`'s order: `{name=, status=, tier=, text=, ago_s=, runs=, fails=}` |

In each check, `status` is `"none"` (not run yet), `"skip"`, `"pass"`, `"warn"` or `"fail"`;
`tier` is `"boot"`, `"background"` or `"full"`; `text` is the reason `health` shows and `ago_s`
how many seconds ago the result was taken, both `nil` for a check that hasn't run; `runs` and
`fails` count its runs since boot and those that failed.

```lua
for _, c in ipairs(sos.health().checks) do
  if c.status == "warn" or c.status == "fail" then print(c.name, c.status, c.text) end
end
```

```lua
-- /autorun.lua: log a warning when the chip is hot or the supply sags
sos.every(60, function()
  local s = sos.sensors()
  if (s.temp or 0) > 60 or (s.vsys or 5) < 4.2 then
    sos.log(string.format("health: %.1f C, %.2f V", s.temp or 0, s.vsys or 0), "warning")
  end
end)
```

A watchdog for the network, using the board health tables below and `sos.reboot`:

```lua
-- /autorun.lua: restart the board after an hour without Wi-Fi
local down_since
sos.every(60, function()
  if sos.wifi.status().up then
    down_since = nil
  elseif not down_since then
    down_since = sos.uptime()
  elseif sos.uptime() - down_since > 3600 then
    sos.reboot("no Wi-Fi for an hour")
  end
end)
```

## Hashes and base64

`sos.hash(alg, data [, raw])` returns the digest of the string `data`, and `sos.hmac(alg, key,
data [, raw])` its HMAC with `key` (up to 4 KB; an empty key is allowed, as HMAC defines it), as lower-case hex, or as raw bytes when `raw` is
true. `alg` is `"sha1"`, `"sha256"`, `"sha384"` or `"sha512"`. They run in wolfCrypt on core 0, the
code TLS uses: about 8 ms for each 4 KB of data (40 KB in 81 ms with SHA-256, 95 ms with
SHA-512, measured on the board), which a long hash spends without stopping anything else. Failures (not enough memory) return `nil` and a message.

`sos.base64.encode(s [, "url"])` and `sos.base64.decode(s [, "url"])` convert to and from base64:
the standard alphabet with `=` padding, or with `"url"` the URL-safe one (`-` and `_`) without
padding, as JWTs use it. Decoding skips spaces and line breaks and doesn't need the padding; for
anything that isn't base64 it returns `nil` and a message.

```lua
-- a webhook signed like GitHub's: X-Hub-Signature-256: sha256=<hex HMAC of the body>
local body = '{"text": "disk almost full"}'
local sig = "sha256=" .. sos.hmac("sha256", sos.read("/secret.txt"), body)
sos.http.post("https://hooks.example/sos", body,
              {content_type = "application/json", headers = {["X-Hub-Signature-256"] = sig}})

print(sos.base64.encode(sos.hash("sha256", "abc", true)))   -- ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=
```

## JSON

`sos.json.decode(s)` turns JSON text into Lua values: objects into tables with string keys,
arrays into tables indexed from 1, strings (escapes decoded, `\u` escapes as UTF-8), numbers
(integers when written without a fraction or exponent and within range, floats otherwise),
`true`, `false`, and `null` into `sos.json.null`, so that an array keeps its positions (`[1, null,
3]` has three entries). Text that isn't JSON returns `nil` and a message with the byte where it
went wrong (`"expected ',' or ']' at byte 9"`).

`sos.json.encode(v)` does the opposite. A table whose keys are exactly 1 to n is an array; any
other is an object, its keys (strings, or integers written as text) in sorted order, so the same
table always gives the same text, which matters when the text is signed (`sos.hmac`). An empty
table is `{}`; there's no way to write an empty array, and a table with holes (`{1, 2, nil, 4}`)
is an object (`{"1":1,"2":2,"4":4}`). A table with both the key `1` and the key `"1"` is an error,
since both would be the member `"1"`. Strings are written as their bytes: one that isn't UTF-8
gives text that isn't valid JSON (decoding doesn't check UTF-8 either). Floats are written as Lua prints them (14
significant digits). Functions, NaN, infinities, keys that are neither strings nor integers, and
tables nested more than 32 deep (which includes a table that contains itself) raise an error.
`nil` and `sos.json.null` encode as `null`. Decoding is limited to 32 levels too.

Decoded values live in the script's 128 KB, and Lua tables are larger than their text: about
180 bytes for a small object (`{"id": 1, "name": "item1", "ok": true}`), so a script can hold
the decoded form of roughly 20 KB of JSON. Speed, measured on the board: 150 such objects (5.5 KB)
encode in 26 ms and decode in 21 ms.

```lua
local r = sos.http.get("https://api.example/status")
if r and r.status == 200 then
  local s, err = sos.json.decode(r.body)
  if not s then print("bad JSON: " .. err) return end
  print(s.version, #s.items)
end

local body = sos.json.encode{text = "disk almost full", level = 2, tags = {"sos", "disk"}}
-- {"level":2,"tags":["sos","disk"],"text":"disk almost full"}
sos.http.post("https://hooks.example/sos", body, {content_type = "application/json"})
```

## The log

`sos.dmesg([cursor])` reads the system log kept in memory, the lines `dmesg` shows. It returns
three values:

- a list of entries, oldest first, each `{time=, level=, facility=, text=}`: seconds since boot
  (a float, millisecond steps), `"err"`, `"warning"`, `"notice"`, `"info"` or `"debug"`,
  `"kern"`, `"auth"`, `"daemon"`, `"user"` or `"none"`, and the message without the time and the
  level's tag;
- a cursor: pass it to the next call to get only the entries logged since;
- `lost`: `true` if entries after the cursor you passed are gone: dropped before this call, or
  the cursor is from before a reboot. Then the list starts at the oldest entry kept.

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
