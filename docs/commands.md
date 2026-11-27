# Shell commands

Every command the SOS shell accepts. The same shell runs on the serial console (USB or UART) and
in each SSH session. `help` lists the commands that the current channel may use.

**Console only** means the command is refused over SSH. Anything that changes the configuration
or other protected state needs physical access to the console. A command marked "console only to
change" can still *show* its value from SSH. The README explains each subsystem in more detail;
this page is the reference.

## Line editing

- **Enter** runs the line. **Backspace** deletes. Arrow keys are ignored; there's no history.
  (For files, there's a full-screen [editor](#editor).)
- **Ctrl-C** discards the line, cancels a question (`setup`, `wifi set`, …), or stops a running
  job (`ls`, `cat`, `cp`, a Lua script).
- **Ctrl-D** on an empty line logs out, like `logout`.
- Commands typed while a job runs are kept, and run after it finishes.

## System

| Command | Where | What it does |
|---|---|---|
| `help` (or `?`) | any | List the commands this channel may use |
| `uptime` | any | Time since boot, with `(last reset: watchdog)` if the watchdog caused the last reset, `(last reset: glitch detector)` if the glitch detector did, or a `last reset:` line if a hard fault or panic did |
| `version` | any | The commit the firmware was built from (12 hex digits, `-dirty` if the build had uncommitted changes, `unknown` if not built from a git checkout). It names the SOS commit only, not libraries a build takes from outside the tree through `local.cmake`. The commit is also shown above the first prompt on the console and after each SSH login. Two more lines follow: `board: serial …`, the board's serial number (the one it reports over USB and to `picotool`), and `chip: RP2350 A4 (revision 3, ROM 4)`, the chip revision named from the chip's revision field and the boot ROM version (`unknown revision` if the pair isn't one this firmware knows). Then what the chip's one-time-programmable memory says about how the board is secured: `secure boot: off` or `on`, and `debug port: enabled`, `Secure debug disabled` or `disabled`. These are the flags the chip latched at reset; a further line appears only if the OTP rows behind them can't be read or say something else. Then `root secret: in OTP`, `in flash (open board)` or `none`. And `sealed-only: no` or `yes`: whether the board refuses unsealed configuration and host key records in flash (a marker of its own in OTP, not secure boot; `yes` also when the marker can't be read or is damaged, and the line then says so) |
| `mem` | any | Heap in use, peak and total, and core 0's peak stack use. After a Lua script has run, also how much of core 1's 32 KB stack the last script used |
| `net` | any | Wi-Fi link state and signal strength (RSSI), IP address, gateway, hostname, and ARP packet counts with the keepalive state |
| `ssh` | any | Host key fingerprint; sessions in use and connections refused because all were busy; connections refused from a penalized address or dropped before login; logins; and each session (user and address, state, shell or SFTP, how long), yours marked `*` |
| `dmesg` | any | The system log kept in memory |
| `log` | any | Show the log levels: `console` (printed on the serial console) and `history` (kept for `dmesg`) |
| `log console\|history <level>` | console only | Set a log level until reboot: `err`, `warning`, `notice`, `info` or `debug` |
| `logout` (or `exit`, Ctrl-D) | any | End this session. On the console: lock it, so it asks for the console password again (not possible while no password is set). The console also locks itself when the USB terminal disconnects and after 10 minutes with nothing typed; a running command is ended |
| `reboot` | any | Restart the device |
| `crash fault\|stack\|panic\|hang` | console only | Testing only: stop the firmware with a hard fault, a stack overflow, a panic or a hang, to check that the next boot reports it |

## Messages

Messages between the people logged in: the console and each SSH shell (not SFTP). Each person is
named after the comment of the trusted key they logged in with (`key <n>` if it has none, see
`key list`), the console is `console`, and a second login with the same key gets its slot
added (`alice@laptop/2`). Messages are kept only in memory (the last 16), never in the log or on
the card. Times are UTC, or time since boot (`+1h02m`) until the clock is set.

| Command | Where | What it does |
|---|---|---|
| `who` | any | One line per shell: slot (0 is the console), name, where it's connected from, `chat` if it's in the chat, and how long it's been logged in. Yours is marked `*` |
| `wall <message>` | any | Show a message in every other shell at once, as `[wall 14:02 alice@laptop] message`. Says how many shells it was sent to |
| `chat` | any | Join the chat room: shows the last few chat lines and who is there, then each line you type at the `chat> ` prompt goes to everyone else in the room, as `[14:03] alice@laptop: hi`. Joining and leaving are announced. `/who` lists the room; `/quit`, Ctrl-C or Ctrl-D on an empty line leaves; start a line with `//` to send one beginning with `/` |

A message arrives between your keystrokes: the line you're typing is cleared, the message
printed, and the prompt and your typing redrawn. During a running command it appears on its
own line. A client that stops reading (a suspended `ssh`) doesn't hold anyone up: its messages
wait, and if more than 16 arrive meanwhile it's told how many it missed. (A sender whose
message would push out one that a reader hasn't had yet waits up to 2 seconds for it to catch
up, so a pasted burst isn't lost; Ctrl-C drops the message.)

## Configuration

Changes are saved in flash immediately and survive reflashing (see
[Configuration](../README.md#configuration)).

| Command | Where | What it does |
|---|---|---|
| `config` | any | Show the configuration: hostname, whether a console password is set, Wi-Fi, number of trusted keys, NTS server, periodic sync and RTC calibration |
| `glitch` | any | The glitch detector: its setting, whether it is armed now, how many times it has reset the board since power-on, and for the last such reset which detectors fired and how long the board had been up |
| `glitch watch <0-3>` | console only | Run the glitch detector's circuits at that sensitivity without arming them, now and at every boot. A trigger is logged and shown by `glitch` ("armed, it would have reset the board") and nothing is reset. A detector that has triggered may stay triggered until the next reset, so each of the four is reported once per boot |
| `glitch on <0-3>` | console only | Arm the RP2350's glitch detector by software, now and at every boot, at that sensitivity (0 lowest, 3 highest). The detector resets the chip when its clock or core supply is disturbed. **`on 3` is the default:** a board is armed at the highest sensitivity unless set otherwise. **Not protection:** it is armed a second or two into boot and software can disarm it; its purpose is to see whether the detector resets a healthy board before it is enabled for good in OTP. If it resets the board three times in a row within three minutes of starting, the next boot leaves it unarmed and says so. A detector reset loses the fault record |
| `glitch off` | console only | Disarm the detector, now and at boot: the way out if it resets a healthy board |
| `glitch default` | console only | Go back to the default: armed at sensitivity 3 |
| `glitch trig` | console only | Make one detector fire, to check that a detector reset is seen and reported. Resets the board if the detector is armed |
| `bootkey` | any | Would this firmware still start with secure boot on? It finds the public key the running firmware is signed with, hashes it as the boot ROM does, and compares that with the four boot key slots in the chip's one-time-programmable memory. It prints the firmware's key hash (or `not signed`), each slot that isn't blank with its mark (`valid`, `not marked valid`, `marked invalid`), whether secure boot is on, and a verdict: `MATCH` (a valid slot holds the hash of this firmware's key), `NOT YET` (a slot holds it but isn't marked valid), `NO MATCH`, or `NO VERDICT` when the slots can't be read or the firmware's key can't be determined. `MATCH` compares keys; it doesn't check the signature over the firmware, which `picotool info -a` does. It only reads. It exists for the procedure that secures a board: turning secure boot on with the wrong key hash in OTP leaves a board that runs nothing |
| `rootsecret` | any | The state of the root secret, from which the keys that protect stored data are derived, and where it is: in OTP (with whether its page is locked for this boot), in flash on an open board (where it isn't protected from someone who has the board), none (and why: a sealed-only board with none in OTP never uses its flash for one), or present but unusable (damaged, or from newer firmware; kept as it is). It's stored twice; if one copy is damaged the other is used and this says so. The saved configuration and the SSH host key are sealed under keys derived from it |
| `rootsecret forget` | console only | Clear a root secret record that can't be used, after you type `yes`: its whole flash sector is erased and the next boot makes a new one. Refused while the root secret is usable, and always on a sealed-only board. A configuration and host key sealed under the old one can't be read again |
| `config reset` | console only | Erase the saved configuration, after you type `yes`. **This also destroys the owner secret:** the board's SSH host key becomes unreadable and a new one is made at the next setup (SSH clients will warn that the host key changed), and SD cards this board encrypted can then only be read on a PC with the key shown when each was formatted, and setting the board up again does not bring them back; the command says so before asking. A mounted encrypted card is unmounted. The device is then as new: no Wi-Fi network, no trusted keys (so no SSH), hostname `sos`. Set it up again with `setup`, or provision it from the development machine (`ninja provision`) |
| `passwd` | console only | Set the console password, or change it after typing the current one. 8 to 64 printable ASCII characters, asked twice, not echoed. The console asks for it before taking any command; after a wrong answer the next one waits 1 s, 2 s, 4 s ... up to 30 s. If it's forgotten, the only way back in is to erase the configuration over USB, which locks the board's encrypted SD cards |
| `setup` | console only | Guided setup: the console password first if none is set, then: Wi-Fi network and password, and a public key to trust. Enter keeps the value in brackets, Ctrl-C cancels without changing anything |
| `hostname [<name>]` | console only to change | Show or set the hostname. A new name takes effect at the next Wi-Fi connect (`wifi connect`) |
| `key` / `key list` | any | List the trusted SSH public keys, numbered, with their fingerprints |
| `key add [<line>]` | console only | Trust a public key: one `authorized_keys` line (ssh-ed25519 or ecdsa-sha2-nistp256; up to 8 keys). Without a line it asks for one |
| `key del <n>` | console only | Stop trusting key *n* (as numbered by `key list`) |

## Wi-Fi

| Command | Where | What it does |
|---|---|---|
| `wifi` | any | The saved networks, numbered in the order they are preferred, each with whether a password is set and the connected one marked; connection state, power saving and country |
| `wifi set` | console only | Change the first network: asks for the SSID and the password (not echoed), saves them and reconnects |
| `wifi add` | console only | Save another network (up to 5 in all), after the others: asks for the SSID and the password. The board joins the first saved network in reach when it connects; while connected it doesn't notice a preferred network coming into reach until the link drops or `wifi connect`. Reconnects only if not connected |
| `wifi remove <n>` | console only | Forget network *n* (as numbered by `wifi`). Reconnects if it was the network in use |
| `wifi prefer <n>` | console only | Move network *n* to the front. Takes effect at the next connect |
| `wifi connect` | console only | Reconnect now, trying the saved networks from the first again |
| `wifi country <XX>\|worldwide` | console only | The country whose radio rules to follow, as two capital letters (e.g. `US`), or the chip's worldwide set (the default). Saved; takes effect at the next boot |
| `wifi power` | any | Show whether Wi-Fi power saving is on |
| `wifi power on\|off` | console only | Turn power saving on or off (saved; default off) |
| `wifi stats` | any | For chasing packet loss: signal, noise floor, channel, access point and transmit rate; packets sent, received and dropped per protocol since boot; TCP segments resent; and how full lwIP's pools got |
| `wifi stats chip` | any | The same, plus the Wi-Fi chip's own counters, raw (index: value). Their layout isn't published for this firmware; compare two readings. `src/shell/shell.c` lists the positions worked out so far |
| `wifi iovar <name> [<value>]` | console only | Debugging: read or set a 32-bit variable in the Wi-Fi chip (e.g. `2g_rate`, `ampdu_tx`, `qtxpower`). Not saved. An unknown name reads back as a meaningless value, not an error |
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
| `time cal [<ppm>\|off]` | console only to change | Show, or set, the RTC calibration. Give how fast the RTC runs in ppm (negative: slow), from -277 to 273; the RTC is slowed or sped up by the nearest step of 4.34 ppm. The value replaces the previous one, so add a newly measured error to the rate `time` shows. See [Timekeeping](../README.md#timekeeping) for how to measure |
| `time server [<name>\|default]` | console only to change | Show, or set, the NTS server (host name or IPv4 address). `default` goes back to the build-time server. Used from the next sync |
| `nts` | any | NTS state: the last key exchange (cookies held, NTP server) and the last query |
| `nts ke` | any | Run a key exchange with the build-time default server, not the one set with `time server` (development; doesn't set the clock) |
| `nts ke <host>` | console only | Run a key exchange with another server (development) |
| `nts query` | any | One authenticated NTS time query; `nts` then shows the offset. Doesn't set the clock |
| `tls roots` | any | The built-in root certificates, and the state of `/certs/roots.pem` |
| `tls roots approve` | console only | List each certificate in `/certs/roots.pem`, test-load it, and after `yes` trust exactly that file |
| `tls roots forget` | console only | Stop trusting `/certs/roots.pem`; TLS uses the built-in roots only |

## HTTPS

For testing the HTTPS client (`src/net/README.md`), from any shell: SSH users are the device's
trusted owners, and their Lua scripts can make the same requests through `sos.http`. The server's
certificate must chain to a built-in root or one in the approved `/certs/roots.pem`. Redirects
aren't followed.

| Command | Where | What it does |
|---|---|---|
| `http get <url>` | any | Print the status line and the response body (job: Ctrl-C stops), then the size and timings. Control characters in the body, headers and reason phrase show as `.`, including the C1 range (raw or UTF-8-encoded), and so do bidi controls (such as U+202E), zero-width and other invisible characters, tag characters and the variation selectors supplement; other UTF-8 text shows as itself |
| `http head <url>` | any | Print the status line and the response headers |
| `http post <url> <text>` | any | Send the text as `text/plain; charset=utf-8`, then print the response as `get` does |

URLs must start with `https://`, and the path must already be percent-encoded (no spaces).

Two HTTPS requests (`http` or `sos.http`, from any shells) can run at once; a third is refused
with "too many HTTPS requests at once" until one finishes. One more TLS connection is always
kept free for time sync (NTS). A request is also refused when the heap is too low for a TLS
handshake (about 64 KB free).

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
| `edit <file>` | any | Full-screen text editor; creates the file on the first save if it doesn't exist. See [Editor](#editor) |
| `sd` / `sd info` | any | Card and volume information, the SPI clock, and whether the card is encrypted (with its card id) |
| `sd speed` | any | Show the SPI clock, set and actual |
| `sd speed <MHz>` | console only | Set the SPI clock (1 to 75 MHz) and reinitialize the card. Not saved |
| `sd unmount` | console only | Unmount the card before removing it. Refused while a file is open |
| `sd test [<KB> [keep]]` | console only | Write and read benchmark: 4 to 65536 KB, default 256. Everything else waits until it finishes. `keep` leaves `/sos-test.txt` on the card |
| `sd format [exfat\|fat32] [plain]` | console only | Erase the whole card (all partitions) and format it with one partition, exFAT by default, after you type `yes`. The card is **encrypted** (XTS-AES-128, with a new key that this device keeps) unless `plain` is given; a PC can't read an encrypted card. When an encrypted format finishes, it shows the card's id and data key, **once**: store the key offline, since it's what a PC needs to read the card if the device is lost (`tools/host/sdcrypt.c`, or cryptsetup). It can't be shown again later. Old files on the card aren't erased: what was written unencrypted before stays readable until overwritten. SSH sessions pause until it finishes |

## Editor

`edit <file>` opens a file of up to 128 KB in a full-screen editor, on the console or over SSH.
While memory is short (SSH sessions and Lua share it) the limit is lower, and `edit` says how
much it can take; the text can't grow past that limit either.
It needs a VT100/xterm-compatible terminal and uses its alternate screen, so the shell's output
is back as it was afterwards. The screen size is asked from the terminal when the editor starts
and on Ctrl-L (so resize the window, then press Ctrl-L); without an answer it's 80×24. It uses
at most 400 columns and 200 rows of a bigger window.

| Key | What it does |
|---|---|
| typing, **Enter**, **Tab** | Insert (UTF-8 text is fine) |
| **Backspace** / **Delete**, **Ctrl-D** | Delete before / at the cursor |
| arrows, **Home**/**End** (**Ctrl-A**/**Ctrl-E**), **PgUp**/**PgDn** | Move |
| **Ctrl-S** (or **Ctrl-O**) | Save |
| **Ctrl-Q** (or **Ctrl-X**) | Quit; with unsaved changes, press it twice |
| **Ctrl-F** (or **Ctrl-W**) | Find: type the text, **Enter** finds the next match (empty: the last search again), **Ctrl-C** cancels |
| **Ctrl-L** | Redraw, and ask the terminal for its size again |

- **Saving** writes `<file>.~edit` first, then replaces the file with it, so an interrupted save
  never leaves half a file. If the power goes in the middle, `edit <file>` picks the text up from
  `<file>.~edit` and tells you. If two people edit the same file, the last save wins.
- **Files with CRLF line ends** keep them. Control characters, invalid UTF-8 and invisible or
  bidi characters show as `?` (they're kept as they are). Wide characters (CJK, most emoji) take
  two columns and accents written as combining marks none, as on the terminal; the cursor moves
  over a letter and its accents (up to two) as one. A further accent, or one with no letter before
  it, shows as `?`. A terminal built on an older Unicode than the editor's
  (16.0) can disagree about recent emoji. Long lines scroll sideways instead of wrapping.
- **Ctrl-C** doesn't stop the editor; Ctrl-Q does. If the SSH session drops, unsaved changes
  are lost.
- **Log messages** aren't printed on the console while the editor runs there. The status bar
  counts them, and they appear when you quit (and are in `dmesg`).
- `/certs` can be opened from SSH but only saved from the console.

## Lua

| Command | Where | What it does |
|---|---|---|
| `lua <file>` | any | Run a Lua script from the card on core 1 (job: Ctrl-C stops) |
| `lua -e <code>` | any | Run the given code |
| `lua` | any | Interactive prompt: each line runs as entered, in one Lua state; values of expressions are printed. Ctrl-C interrupts a line, Ctrl-D on an empty line or `exit` leaves |
| `lua -b <file>` | any | Run a script in the background: no shell, its output goes to the log (`dmesg`), and its end is logged |
| `lua ps` | any | The running script: what, where (a shell or the background), for how long |
| `lua stop` | any | Stop the running script, wherever it runs (the prompt is closed) |

One script runs at a time (foreground, prompt or background). `/autorun.lua`, if the card has
one, starts in the background a few seconds after boot, except after a crash or watchdog reset.
Use `lua ./ps` or `lua ./stop` to run files with those names. Scripts use
the `sos` library to reach the system, and `require` loads modules from the card (`/lib` and the
working directory); see [Lua scripts](../README.md#lua-scripts-prototype).
