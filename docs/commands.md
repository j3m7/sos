# Shell commands

Every command the SOS shell accepts. The same shell runs on the serial console (USB or UART) and
in each SSH session. `help` lists the commands that the current channel may use.

**Console only** means the command is refused over SSH. Anything that changes the configuration
or other protected state needs physical access to the console. A command marked "console only to
change" can still *show* its value from SSH. The README explains each subsystem in more detail;
this page is the reference.

## Line editing

- **Enter** runs the line. **Backspace** deletes. Arrow keys are ignored; there's no history.
- **Ctrl-C** discards the line, cancels a question (`setup`, `wifi set`, …), or stops a running
  job (`ls`, `cat`, `cp`, a Lua script).
- **Ctrl-D** on an empty line logs out, like `logout`.
- Commands typed while a job runs are kept, and run after it finishes.

## System

| Command | Where | What it does |
|---|---|---|
| `help` (or `?`) | any | List the commands this channel may use |
| `uptime` | any | Time since boot, with `(last reset: watchdog)` if the watchdog caused the last reset, or a `last reset:` line if a hard fault or panic did |
| `mem` | any | Heap in use, peak and total, and core 0's peak stack use. After a Lua script has run, also how much of core 1's 32 KB stack the last script used |
| `net` | any | Wi-Fi link state and signal strength (RSSI), IP address, gateway, hostname, and ARP packet counts with the keepalive state |
| `ssh` | any | Host key fingerprint; sessions in use and connections refused because all were busy; connections refused from a penalized address or dropped before login; logins; and each session (user and address, state, shell or SFTP, how long), yours marked `*` |
| `dmesg` | any | The system log kept in memory |
| `log` | any | Show the log levels: `console` (printed on the serial console) and `history` (kept for `dmesg`) |
| `log console\|history <level>` | console only | Set a log level until reboot: `err`, `warning`, `notice`, `info` or `debug` |
| `logout` (or `exit`, Ctrl-D) | any | End this session |
| `reboot` | any | Restart the device |
| `crash fault\|stack\|panic\|hang` | console only | Testing only: stop the firmware with a hard fault, a stack overflow, a panic or a hang, to check that the next boot reports it |

## Configuration

Changes are saved in flash immediately and survive reflashing (see
[Configuration](../README.md#configuration)).

| Command | Where | What it does |
|---|---|---|
| `config` | any | Show the configuration: hostname, SSH user, Wi-Fi, number of trusted keys, NTS server and periodic sync |
| `config reset` | console only | Erase the saved configuration, after you type `yes`. The device is then as new: no Wi-Fi network, no trusted keys (so no SSH), hostname `sos`, user `admin`. Set it up again with `setup`, or provision it from the development machine (`ninja provision`) |
| `setup` | console only | Guided setup: Wi-Fi network and password, SSH user, and a public key to trust. Enter keeps the value in brackets, Ctrl-C cancels without changing anything |
| `hostname [<name>]` | console only to change | Show or set the hostname. A new name takes effect at the next Wi-Fi connect (`wifi connect`) |
| `user [<name>]` | console only to change | Show or set the SSH user name |
| `key` / `key list` | any | List the trusted SSH public keys, numbered, with their fingerprints |
| `key add [<line>]` | console only | Trust a public key: one `authorized_keys` line (ssh-ed25519 or ecdsa-sha2-nistp256; up to 8 keys). Without a line it asks for one |
| `key del <n>` | console only | Stop trusting key *n* (as numbered by `key list`) |

## Wi-Fi

| Command | Where | What it does |
|---|---|---|
| `wifi` | any | Network name, whether a password is set, connection state, and power saving |
| `wifi set` | console only | Change the network: asks for the SSID and the password (not echoed), saves them and reconnects |
| `wifi connect` | console only | Reconnect now |
| `wifi power` | any | Show whether Wi-Fi power saving is on |
| `wifi power on\|off` | console only | Turn power saving on or off (saved; default off) |
| `wifi keepalive on\|off` | console only | Testing only: turn the address announcements off or back on. Not saved; it's on again after a reboot |

## Time and TLS

See [Timekeeping](../README.md#timekeeping) and
[Trusted root certificates](../README.md#trusted-root-certificates).

| Command | Where | What it does |
|---|---|---|
| `time` | any | The time (UTC) and where it came from, the RTC, the last sync and any failure, the NTS server, and the periodic sync |
| `time sync` | any | Sync the clock and the RTC from NTS now |
| `time sync sntp` | console only | One unauthenticated SNTP sync. `time` then shows the time as unauthenticated |
| `time auto [<hours>\|off]` | console only to change | Show, or set, the periodic NTS sync: every 1 to 720 hours after the last sync (or after boot, if the RTC had the time), or off (default) |
| `time server [<name>\|default]` | console only to change | Show, or set, the NTS server (host name or IPv4 address). `default` goes back to the build-time server. Used from the next sync |
| `nts` | any | NTS state: the last key exchange (cookies held, NTP server) and the last query |
| `nts ke` | any | Run a key exchange with the build-time default server, not the one set with `time server` (development; doesn't set the clock) |
| `nts ke <host>` | console only | Run a key exchange with another server (development) |
| `nts query` | any | One authenticated NTS time query; `nts` then shows the offset. Doesn't set the clock |
| `tls roots` | any | The built-in root certificates, and the state of `/certs/roots.pem` |
| `tls roots approve` | console only | List each certificate in `/certs/roots.pem`, test-load it, and after `yes` trust exactly that file |
| `tls roots forget` | console only | Stop trusting `/certs/roots.pem`; TLS uses the built-in roots only |

## HTTPS

For testing the HTTPS client (`src/net/README.md`). The server's certificate must chain to a
built-in root or one in the approved `/certs/roots.pem`. Redirects aren't followed.

| Command | Where | What it does |
|---|---|---|
| `http get <url>` | console only | Print the status line and the response body (job: Ctrl-C stops), then the size and timings. Control characters in the body show as `.` |
| `http head <url>` | console only | Print the status line and the response headers |
| `http post <url> <text>` | console only | Send the text as `text/plain; charset=utf-8`, then print the response as `get` does |

URLs must start with `https://`, and the path must already be percent-encoded (no spaces).

## Files and SD card

The file commands work in every shell; each shell has its own working directory. Paths are
relative to it unless they start with `/`. File names with spaces aren't supported yet. Only the
console can change anything under `/certs`. See [Files (SD card)](../README.md#files-sd-card).

| Command | Where | What it does |
|---|---|---|
| `ls [<path>]` | any | List a directory (job: Ctrl-C stops) |
| `cd [<dir>]` | any | Change the working directory (`/` without an argument) |
| `pwd` | any | Show the working directory |
| `cat <file>` | any | Print a file (job: Ctrl-C stops) |
| `cp <from> <to>` | any | Copy a file. Copying to a directory keeps the name (job: Ctrl-C stops and removes the partial copy) |
| `mv <from> <to>` | any | Move or rename. Moving to a directory keeps the name |
| `rm <path>` | any | Delete a file or an empty directory |
| `mkdir <dir>` | any | Create a directory |
| `echo <text> [> <file> \| >> <file>]` | any | Print the text, or write or append it as a line |
| `sd` / `sd info` | any | Card and volume information, and the SPI clock |
| `sd speed` | any | Show the SPI clock, set and actual |
| `sd speed <MHz>` | console only | Set the SPI clock (1 to 75 MHz) and reinitialize the card. Not saved |
| `sd unmount` | console only | Unmount the card before removing it. Refused while a file is open |
| `sd test [<KB> [keep]]` | console only | Write and read benchmark: 4 to 65536 KB, default 256. Everything else waits until it finishes. `keep` leaves `/sos-test.txt` on the card |
| `sd format [exfat\|fat32]` | console only | Erase the whole card (all partitions) and format it with one partition, exFAT by default, after you type `yes`. SSH sessions pause until it finishes |

## Lua

| Command | Where | What it does |
|---|---|---|
| `lua <file>` | any | Run a Lua script from the card on core 1 (job: Ctrl-C stops) |
| `lua -e <code>` | any | Run the given code |
| `lua` | any | Interactive prompt: each line runs as entered, in one Lua state; values of expressions are printed. Ctrl-C interrupts a line, Ctrl-D on an empty line or `exit` leaves |

One script runs at a time, and its output goes to the shell that started it. Scripts use
the `sos` library to reach the system, and `require` loads modules from the card (`/lib` and the
working directory); see [Lua scripts](../README.md#lua-scripts-prototype).
