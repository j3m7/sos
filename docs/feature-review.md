# Feature review: runbook

A walk through every feature SOS has on main, for a person to try by hand on a board. Each
step gives what to type and what you should see. Sections run from harmless to disruptive; the
last ones change or erase things and are marked.

- **This is a review, not a test gate.** If what you see differs from "You should see", write
  it in the [notes](#notes) at the end (section and step number, what you saw) and go on. Stop
  only where a step says **Stop**.
- Board: dev-1 (serial `F94525FD61D9A1CC`), on a main image at or after `6a99ef1` (#284), with an SD
  card in. Another board works too: change the serial below.
- What each feature does in detail is in its manual page (`man <command>` on the board, or
  [man/](../man/)) and the guides listed in the [README](../README.md#documentation). Where a
  step says "as `man x` says", that page is the reference.
- Expect about three hours for sections 1 to 18. Sections 19 and 20 are optional.

## Before you start

- [ ] Ask the Reviewer session for a dev-1 slot of about three hours, so it can fit the queue
  (other PRs' board runs, the #275 drift reading) around it, and check nothing else is using
  the board.
- [ ] The board runs current main: `version` on its console names a commit at or after
  `6a99ef1`. If not, flash main in your slot first (while a drift measurement runs on dev-1,
  only images with #284 may go on it).
- [ ] You know the console password, and your SSH key is one the board trusts.
- [ ] You have `sftp`, `ssh` and `picocom` on this machine, and a terminal at least 80x24.

Open the console in a terminal of its own:

```sh
picocom -b 115200 /dev/serial/by-id/usb-Raspberry_Pi_Pico_F94525FD61D9A1CC-if00
```

Open a second terminal for SSH, and set the board's address there once (`net` on the console
shows it):

```sh
export B=<board address>
```

Below, **[console]** means type it in the picocom terminal, **[ssh]** in an SSH session
(`ssh $B`), **[host]** in a shell on this machine. All test files go in `/review` on the card;
section 16 removes it.

## 1. Console login and lock

1. **[console]** Press Enter.
   You should see: a password question and nothing else: no greeting, no log lines.
2. Type a wrong password.
   You should see: a refusal after about 0.3 s. The next answer waits 1 s first.
3. Type the right password.
   You should see: the greeting naming the firmware commit (`SOS <commit>`), any log lines held
   while locked, then the `sos>` prompt.
4. Quit picocom (Ctrl-A Ctrl-X), start it again, press Enter.
   You should see: the password question again (closing the terminal locks the console).
   Log in.
5. `logout`
   You should see: the console locks. Log in again. (Ctrl-D on an empty line does the same.)
6. `dmesg | grep console`
   You should see: lines for the logins, the wrong password and the locks.

Not covered here: the 10-minute idle lock (leave the console untouched for 10 minutes if you
want to see it).

## 2. Typing a line

1. **[console]** Type `version`, Enter, then `uptime`, Enter. Press Up twice, then Down once.
   You should see: Up recalls `uptime`, then `version`; Down goes back to `uptime`.
2. Type `abc` and press Ctrl-C.
   You should see: the line is discarded and a fresh prompt.
3. Type `wi` and press Tab.
   You should see: `wifi ` completed.
4. Type `time s`, press Tab twice.
   You should see: a beep, then `sync` and `server` listed.
5. Type `man ` and press Tab twice (manual pages must be on the card; section 9 installs them).
   You should see: the page names.
6. `help`
   You should see: the commands this channel may use. Compare with `help` over SSH in section 5.

## 3. System information

All in **[console]**; each also works over SSH.

| # | Type | You should see |
|---|---|---|
| 1 | `version` | commit, board serial `F94525FD61D9A1CC`, chip revision A2, and how it is secured: secure boot, debug port, root secret, console-only setup (the key hash is `bootkey`'s, section 19) |
| 2 | `uptime` | time since boot and the last reset's cause |
| 3 | `uptime history` | the recent resets, each with its cause and how long the board had been up, and RTC events such as `rtc set (no cal, 12.5 pF)` |
| 4 | `temp` | chip temperature, lowest and highest since boot, then either `Calibrated ...` or `Uncalibrated:` |
| 5 | `power` | supply voltage, and USB power present |
| 6 | `mem` | heap and stack use, by feature; core 0 stack high-water mark |
| 7 | `net` | Wi-Fi link state, address, ARP counters |
| 8 | `health` | one line per check (resets, clocks, flash, trng, crypto, crypto-ssh, glitch, stack, heap, chip-temp, supply, rtc-drift ...), each PASS, WARN, FAIL or SKIP with a reason, and the boot checks' time |
| 9 | `health run heap` | that check run now, with its result |
| 10 | `health full` | every check run now; a healthy board shows no FAIL |

Write down any WARN or FAIL with its reason.

## 4. The system log

1. **[console]** `dmesg`
   You should see: the last 4 KB of log messages.
2. `dmesg -f auth`
   You should see: only logins and configuration changes.
3. `logger review step 4.3`, then `dmesg -f user`
   You should see: `logger (console): review step 4.3` (no level shown: dmesg tags only
   warnings and errors). Its level, `notice`, is in `/var/log/user.log` (next steps).
4. `log`
   You should see: the console, history and file levels, the log files on the card and the
   rotation schedule (every 7 days at 00:00 UTC unless changed).
5. `ls /var/log`
   You should see: `kern.log`, `auth.log`, `daemon.log`, `user.log` (and maybe `syslog`,
   `.1` files from an earlier rotation).
6. `tail` works only after a pipe, so: `cat /var/log/user.log | tail 3`
   You should see: your `logger` line among the last three, with a UTC timestamp.
7. **[ssh]** `rm /var/log/user.log`
   You should see: `rm: /var/log/user.log: not allowed to change this path` (only the console
   changes `/var/log`).

## 5. SSH

1. **[console]** `ssh`
   You should see: the host key fingerprint (`ssh-ed25519 SHA256:...`), the sessions (none yet)
   and counts of refused connections.
2. **[host]** `ssh-keygen -lf <(ssh-keyscan -t ed25519 $B 2>/dev/null)`
   You should see: the same fingerprint.
3. **[host]** `ssh $B`
   You should see: the greeting and a prompt, without a password.
4. **[ssh]** `ssh`, then `who`
   You should see: `ssh` lists your session as `<login>@<ip>:<port>`, marked `*`; `who` lists
   it, named after your key's comment, and `console`.
5. **[ssh]** `help`
   You should see: fewer commands than on the console (the console-only ones are missing).
6. **[ssh]** `passwd`, then `hostname newname`
   You should see: both refused as console only. `hostname` alone shows the name.
7. **[host]** With this session open, open three more in other terminals (`ssh $B` each).
   You should see: the first two log in (3 sessions in all); the third gets "Too many
   connections: all 3 SSH sessions are in use, try again later". Close the extra ones.
8. **[ssh]** Ctrl-D.
   You should see: the session ends.

## 6. Messages between sessions

Keep one SSH session open beside the console.

1. **[console]** `wall hello from the console`
   You should see: the message in the SSH session, and the line being typed there redrawn after it.
2. **[ssh]** `chat`, then **[console]** `chat`
   You should see: a `chat> ` prompt in each.
3. Type a line in each.
   You should see: it in the other one.
4. **[ssh]** `/who`
   You should see: both members.
5. **[ssh]** `/quit`; **[console]** Ctrl-C.
   You should see: both back at the shell prompt.

## 7. Wi-Fi

1. **[console]** `wifi`
   You should see: the saved networks (passwords not shown), the link state, power saving and
   country. (Signal is in `wifi stats` and `net`.)
2. `wifi stats`
   You should see: signal, noise and packet counters. `wifi stats chip` adds the raw counters.
3. `ping 1.1.1.1`
   You should see: four replies, one a second, with round-trip times and the loss.
4. `ping letsencrypt.org 2`
   You should see: the name resolved, two replies.
5. `wifi add`, then at the network name question press Tab twice, then Ctrl-C.
   You should see: the networks around, strongest first, with their signal; Ctrl-C cancels
   without saving anything.

## 8. Files

1. **[console]** `mkdir /review`, `cd /review`, `pwd`
   You should see: `/review`.
2. `echo one > a.txt`, `echo two >> a.txt`, `cat a.txt`
   You should see: `one` and `two`.
3. `cp a.txt b.txt`, `mv b.txt "my file.txt"`, `ls`
   You should see: `a.txt` and `my file.txt` with sizes and UTC dates.
4. `cat my\ file.txt` and `cat 'my file.txt'`
   You should see: the same two lines both times.
5. `cat my` and Tab.
   You should see: `cat my\ file.txt ` completed with the space escaped.
6. `rm "my file.txt"`, `ls`
   You should see: only `a.txt`.
7. `cd ..`, `rm /review`
   You should see: refused: the directory isn't empty.
8. `cd /review`, then `cp /var/log/daemon.log big.txt` and press Ctrl-C while it copies (if it
   finishes first, `rm big.txt` and skip this step).
   You should see: the copy stops and `big.txt` isn't left behind.

## 9. Pipes, pager and manual pages

1. **[console]** `man`
   You should see: the list of pages. If it says there are none, `man install` and check it
   downloads them (needs GitHub's root trusted; if it fails at the TLS handshake, note it and use
   `tools/man-install.sh $B` from **[host]**).
2. `man intro`
   You should see: the page in the pager. Space and b page, j and k scroll, g and G go to the
   ends.
3. Still in the pager: `/quoting`, Enter.
   You should see: every match on screen in reverse video (the match's line brought to the
   top, unless it is in the last screenful), and `found on line <n>` on the bottom row. Lower-case text ignores case, so
   it finds QUOTING.
4. Press `n` a few times.
   You should see: each press goes on past the previous match; after the last (the page has
   two) the bottom row says `search wrapped around: found on line <n>`.
5. `/Quoting`, Enter.
   You should see: `not found` on the bottom row: with a capital letter the text must match
   exactly, and the page has only QUOTING. q quits.
6. `dmesg | grep wifi`
   You should see: only lines holding `wifi`. Try `-i`, `-v`, `-n` too.
7. `help | wc`
   You should see: line, word and byte counts.
8. `dmesg | head 3`, then `dmesg | tail 3`
   You should see: the first three, then the last three lines.
9. `dmesg | grep sd | tee /review/sd.txt | wc`, then `cat /review/sd.txt | wc`
   You should see: the same counts twice.
10. `dmesg | more`
   You should see: the log in the pager, the first frame showing the key hint. q quits.
11. `ping 1.1.1.1 20 | more`
   You should see: lines added while you read; q stops the ping.
12. `more /var/log/daemon.log`
   You should see: the file a screen at a time, whatever its size.

## 10. Editor

1. **[console]** `edit /review/notes.txt`
   You should see: a full-screen editor, an empty file, a status bar.
2. Type a few lines, including a non-ASCII character (é) if your keyboard allows. Ctrl-F, find
   a word. Ctrl-S.
   You should see: the find moves to the word; the status bar says it saved.
3. Type more, then Ctrl-Q.
   You should see: a warning about unsaved changes; Ctrl-Q again quits. The shell's earlier
   output is back.
4. `cat /review/notes.txt`
   You should see: what you saved, not what you typed after.
5. **[ssh]** `wall during edit`, while **[console]** `edit /review/notes.txt` is open.
   You should see: nothing in the editor; the message appears when you quit.
6. **[ssh]** `edit /review/notes.txt` works the same over SSH; quit it.

## 11. File transfer (SFTP)

1. **[host]** Make a test file: `mkdir -p ~/sos-review && cd ~/sos-review && head -c 2000000 /dev/urandom > r.bin && sha256sum r.bin`
2. **[host]** `sftp $B`, then `cd /review`, `put r.bin`, `ls -l`, `get r.bin r2.bin`,
   `bye`
   You should see: upload and download complete (a couple of hundred KB/s each way).
3. **[host]** `sha256sum r2.bin`
   You should see: the same hash as step 1.
4. **[console]** `sha256sum /review/r.bin`
   You should see: the same hash again. (`sha256sum` uses the chip's SHA-256 hardware, #281,
   so this also checks that path. While something else holds it, such as a running `fetch`, it
   falls back to software: slower, same result. `sha1sum` and `sha512sum` stay in software.)
5. **[host]** `sftp $B`, then `put r.bin /certs/x.bin`, `bye`
   You should see: refused: only the console changes `/certs`.

## 12. Checksums, gzip and tar

1. **[host]** In `~/sos-review`: `sha256sum r.bin > SUMS && printf '%064d  missing.bin\n' 0 >> SUMS`,
   then `sftp $B` and `put SUMS /review/SUMS`.
2. **[console]** `sha256sum -c /review/SUMS`
   You should see: `r.bin: OK`, `missing.bin: missing`, then the counts.
3. `sha256sum -c --ignore-missing /review/SUMS`
   You should see: only `r.bin: OK` and the counts.
4. `sha1sum /review/a.txt`, `sha512sum /review/a.txt`
   You should see: GNU-format lines (`<hex>  <name>`); compare with the same commands on a
   copy on **[host]** if you like.
5. `cp /var/log/daemon.log /review/d.log`, `gzip /review/d.log`, `ls /review`
   You should see: `d.log.gz` in place of `d.log`, smaller.
6. `zcat /review/d.log.gz | head 3`
   You should see: the first three lines of the log.
7. `gunzip -k /review/d.log.gz`, `ls /review`
   You should see: `d.log` back, `d.log.gz` kept.
8. `gzip /review/d.log`
   You should see: refused because `d.log.gz` exists; `gzip -f` replaces it.
9. **[host]** In `~/sos-review`: `sftp $B`, `get /review/d.log.gz`, `bye`, then `gunzip -t d.log.gz`
   You should see: no error (the board's gzip is standard).
10. **[host]** Make two archives in `~/sos-review`: a normal one, with a symlink and two names
    that differ only in case, and one with a `..` name:
    ```sh
    mkdir -p pkg/docs && echo hi > pkg/README && echo x > pkg/readme && echo hello > pkg/docs/a.txt
    ln -sf README pkg/link
    tar -czf pkg.tgz pkg && tar -cf bad.tar -P ../sos-review/pkg/docs/a.txt
    ```
    then `sftp $B`, `put pkg.tgz /review/pkg.tgz`, `put bad.tar /review/bad.tar`, `bye`.
11. **[console]** `tar tf /review/pkg.tgz`
    You should see: one line per entry with its type (`d`, `-`, `l`), size, UTC date and name;
    the link reads `pkg/link -> README  (link skipped (FAT has no links))`. The gzip is
    recognised without `z`.
12. `tar tf /review/pkg.tgz | grep docs/`
    You should see: only the `pkg/docs/` and `pkg/docs/a.txt` lines.
13. `mkdir /review/out`, then `tar xvf /review/pkg.tgz -C /review/out`
    You should see: each name as it is extracted; skips as `tar: <name>: <why>`: the link, and
    one of `README`/`readme` as `same name as pkg/README on FAT` (or the other way round); then
    `<n> files, <n> directories, <n> skipped`.
14. `cat /review/out/pkg/docs/a.txt`
    You should see: `hello`.
15. The same `tar xvf` again.
    You should see: each file, `README` and `readme` both, skipped with `exists (-f replaces
    it)`. With `-f` added after the archive they are replaced, except the second of the case
    pair, which is again `same name as ...` (on this board `-f` means overwrite; the archive is always
    the word after the mode).
16. `tar xf /review/bad.tar -C /review/out`
    You should see: the entry skipped as `unsafe path`, then `0 files, 0 directories, 1 skipped`.
17. `tar czvf /review/back.tgz -C /review/out pkg`
    You should see: each name as it is added, one per line, stored as `pkg/...` (relative to
    `-C`; directories end in `/`), then `/review/back.tgz: <n> files, <n> directories, <bytes>
    bytes (0 skipped)`.
18. `tar tf /review/back.tgz`
    You should see: the same tree, `pkg/` first.
19. The same `tar czf` again.
    You should see: `tar: /review/back.tgz: already exists (-f replaces it)`; with `-f` it is
    replaced.
20. `cd /review`, then `tar cf bad2.tar ../review` and `tar cf none.tar nothing-here`
    You should see: `tar: ../review: unsafe path`, then `tar: nothing to archive; bad2.tar not
    made`; and `tar: nothing-here: no such file or directory`, then `tar: nothing to archive;
    none.tar not made`. Neither leaves a `.tar` or `.part` behind (`ls`). (A `..` or missing
    path beside good ones is only skipped: the archive is made from the rest.)
21. **[host]** In `~/sos-review`: `sftp $B`, `get /review/back.tgz`, `bye`, then
    `tar tzvf back.tgz`
    You should see: the tree read by your computer's tar: files `-rw-r--r--`, directories
    `drwxr-xr-x`, owner 0, the card's dates.

## 13. Time

1. **[console]** `time`
   You should see: the UTC time and where it came from (RTC, NTS or SNTP), the RTC reading and
   battery state, the calibration, the drift line, the last sync, the server and periodic
   settings, and a local time line if a zone is set. The RTC line ends with the crystal load
   (#284), and the drift line names the calibration and load, for example:
   ```
   rtc:    2026-11-28 15:55:18 UTC; crystal load 12.5 pF
   drift:  rtc measured at the next sync (no cal, 12.5 pF)
   ```
   or a measured rate (`rtc +103+-35 ppm in 8 h ...`) once two syncs have run.
2. `health run rtc-drift`
   You should see: the check's result. Right after a sync it is `SKIP  RTC set <n> min ago (no
   cal, 12.5 pF): drift at the next sync`; with a measurement it reads like `WARN  RTC
   +103+-35 ppm in 8 h, no cal: try time cal 103.1`. It only reads: it doesn't sync.
3. `time sync`: **on dev-1, ask the Reviewer first whether an RTC drift measurement is
   running (#275; one runs until about 06:30 UTC on 2026-11-29).** A manual sync starts the
   measurement over and loses the hours measured so far; skip this step while one runs.
   You should see: a sync over NTS within a few seconds; `time` then shows its correction and
   round trip.
4. `nts`
   You should see: cookies, the last key exchange and query.
5. `nts ke`, then `nts query`
   You should see: each step succeeds; neither sets the clock.
6. `time zone`
   You should see: the zone and its rule, or UTC, and the bundle's tzdata release.
7. `time zone list europe/ | more`
   You should see: the zones with their POSIX rules, paged. q quits.
8. `time zone madrid`
   You should see: refused, with `Europe/Madrid` offered.
9. `time zone Europe/Ma` and Tab twice.
   You should see: `Europe/Madrid`, `Europe/Malta`, `Europe/Mariehamn`.
10. If you change the zone here, note the old one and set it back after (`time zone <old>` or
   `time zone utc`).
11. `time auto`, `time server`, `time cal`
   You should see: each setting shown, unchanged (`time cal` is off on dev-1). Don't change
   it: the drift readings on #275 (the RTC runs about 100 ppm fast) are for you to decide a
   calibration from, and a change starts the measurement over.

## 14. TLS and HTTPS

1. **[console]** `tls roots`
   You should see: the two built-in roots (`SSL.com Root Certification Authority ECC`, `ISRG Root X1`) and the state of
   `/certs/roots.pem`.
2. `http head https://letsencrypt.org/`
   You should see: a 200 status and the headers.
3. `http get https://letsencrypt.org/ | head 5`
   You should see: the status line, then the first lines of the page.
4. `http get http://letsencrypt.org/`
   You should see: refused: `http` takes only https.
5. `fetch https://letsencrypt.org/ /review/le.html`
   You should see: a progress line, then `saved /review/le.html: ... bytes`, the rate, and its
   `sha256:`.
6. `fetch https://letsencrypt.org/ /review/le.html` again.
   You should see: refused because the file exists; `-f` replaces it.
7. `fetch http://letsencrypt.org/ /review/le2.html`
   You should see: `http: not encrypted; check its hash` and a redirect to https followed
   (`-> https://...`).
8. `fetch https://example.com/ /review/ex.html`
   You should see: fails at the handshake and names the issuer it couldn't verify (not one of
   the two roots), unless dev-1 has an approved `/certs/roots.pem` that covers it.

Not covered: approving a new `/certs/roots.pem` (`tls roots approve`). It changes the saved
configuration; follow [docs/tls.md](tls.md#trusted-root-certificates) if you want to review it.

## 15. Lua

1. **[console]** `lua -e 'print(1 + 1)'`
   You should see: `2`.
2. `lua`, then type `2^10`, `x = 5`, `x * 2`, `string.fo` and Tab, then `exit`.
   You should see: `1024.0`, `10`, Tab completing `string.format`, and the shell prompt.
3. Make a module and a script:
   ```
   mkdir /lib
   echo 'return { hello = function(w) return "hello, " .. w end }' > /lib/greet.lua
   echo 'print(require("greet").hello("pico"))' > /review/g.lua
   lua /review/g.lua
   ```
   You should see: `hello, pico`. (Skip `mkdir /lib` if it exists; remove `/lib/greet.lua` at
   the end if you made it.)
4. `lua -e 'print(sos.hash("sha256", "abc"))'`
   You should see: `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
5. `lua -e 'print(sos.json.encode({a=1, b={true, false}}))'`
   You should see: JSON for that table (key order may differ).
6. `lua -e 'local r = sos.http.get("https://letsencrypt.org/") print(r.status, #r.body)'`
   You should see: `200` and the body's size.
7. `lua -e 'local r = sos.http.get("https://letsencrypt.org/docs") print(r.status, r.redirects)'`
   You should see: `301` and `0`, tab-separated: without the `redirects` option a script gets
   the redirect itself.
8. `lua -e 'local r = sos.http.get("https://letsencrypt.org/docs", {redirects = 5}) print(r.status, r.redirects, r.url)'`
   You should see, tab-separated: `200`, the number of hops (1 or 2: that depends on how the
   site redirects that day, not on SOS) and `https://letsencrypt.org/docs/`, where they led.
9. `lua -e 'print(sos.http.get("https://github.com/"))'`
   You should see, tab-separated: `nil` and `TLS handshake: the server's certificate isn't
   signed by a root this firmware trusts (-188)`, whole, not cut short (#288). (The issuer's
   name is `fetch`'s addition, section 14, not `sos.http`'s.)
10. `lua -e 'print(sos.hashfile("/review/r.bin"))'`
   You should see: the same SHA-256 as section 11 gave for `r.bin`, after a few seconds
   (`sos.hashfile` always hashes in software, about 20 s for 10 MB; the SHA-256 hardware isn't
   given to Lua).
   `lua -e 'print(sos.hashfile("/review/r.bin", "sha512"))'` gives the file's SHA-512.
11. `lua -e 'print(sos.date("%Y-%m-%d %H:%M:%S"), sos.localdate("%H:%M %Z"))'`
   You should see: UTC now, and local time in the board's zone.
12. `echo 'sos.every(2, function() sos.log("tick") end)' > /review/tick.lua`, then
   `lua -b /review/tick.lua`, then `lua ps`.
   You should see: the script listed as running in the background.
13. Wait 5 s, then `dmesg -f user | tail 3`.
   You should see: `tick` lines tagged with the script's path.
14. **[ssh]** `lua stop`; then **[console]** `lua ps`.
   You should see: stopped from another shell; nothing running, and its end in the log.
15. `lua -e 'while true do end'`, then Ctrl-C.
   You should see: the script stops; the console stayed responsive meanwhile.
16. `ls /autorun.lua`
   You should see: whether dev-1 has an autorun script. Don't add one without telling the
   Reviewer: it runs at every boot.

## 16. SD card

1. **[console]** `sd` (`sd info` is the same)
   You should see: card size, file system, encrypted or not, SPI clock.
2. `sd speed`
   You should see: the SPI clock the card runs at. Don't change it.
3. `sd test 1024`
   You should see: write and read rates (about 1 MB/s on an encrypted card); everything else
   waits meanwhile. The test file is removed.
4. Clean up: `rm` removes one file or empty directory at a time, so empty `/review/out` from
   the inside out (`ls` each level; `pkg/docs/a.txt`, then `pkg/docs`, and so on), then `rm` each
   file in `/review`, then `rm /review`. Or, quicker, from **[host]**: `sftp $B` and `rm`/`rmdir`
   there.
5. `sd unmount`, then `sd check`
   You should see: one line per pass, any problems or `note:` lines, then a summary
   (`no problems`, or the counts). It only reads.
6. `ls /`
   You should see: the card mounts again and lists.

## 17. Setup on a configured board

`setup` is how a new board gets its console password, Wi-Fi network and SSH key. On a board
that already has them it asks only for the network and a key, and Enter keeps what's saved.
Finishing it still writes a new configuration record, even when nothing changed, so on dev-1
(whose configuration sectors the slot rules keep untouched) stop at step 5. The full first-time setup, from an empty configuration, is in
[section 20](#20-disruptive-steps-optional).

1. **[ssh]** `setup`
   You should see: refused as console only.
2. **[console]** `setup`
   You should see: `SOS setup. Press Enter to keep the value in brackets, Ctrl-C to cancel.`
   It doesn't ask for a console password (one is set), and asks
   `Wi-Fi network (SSID) [<saved network>]: `.
3. Press Tab twice.
   You should see: the networks around, strongest first, with their signal. Clear what Tab
   typed, if anything, so the line is empty.
4. Press Enter on the empty line.
   You should see: no password question (the saved network is kept), then `Trusted keys: <n>.`
   and a `Public key: ` question.
5. Press Ctrl-C.
   You should see: `setup cancelled; nothing changed`.
6. **Spare board only, not dev-1** (it writes the configuration sectors): `setup` again,
   Enter at the network, Enter at the key.
   You should see: `saved:` and the configuration, as `config` shows it. Wi-Fi doesn't
   reconnect (the network didn't change) and your SSH session stays up. Then
   `dmesg -f auth | tail 3` shows `config: setup completed`.

## 18. Provisioning from a file (build only)

The other way to set a board up is a provisioning file written on this machine and loaded over
USB ([docs/configuration.md](configuration.md#provisioning)). Loading one replaces the board's
whole configuration, so here you only build and check the image.

1. **[host]** In the repository, on current main: `cp provision.conf.example ~/sos-review/p.conf`,
   and fill in `wifi_ssid`, `hostname`, `ssh_key` (your public key) and a `console_password`.
   Add `tz = Europe/Madrid`.
2. **[host]** `tools/provision.sh -n ~/sos-review/p.conf`
   You should see: the record built and checked, and nothing written to a board. A
   5-character `console_password` is refused.
3. **[host]** `rm ~/sos-review/p.conf` (it holds a password in the clear).

## 19. Security state (read only)

Each command here only reads.

| # | Type | You should see |
|---|---|---|
| 1 | `config` | the saved configuration: hostname, networks (no passwords), the number of trusted keys, time and log settings (the glitch setting is `glitch`'s, row 5) |
| 2 | `key list` | the trusted SSH keys, numbered, with their comments |
| 3 | `rootsecret` | `in OTP`, with whether its page is locked for this boot |
| 4 | `bootkey` | the firmware's key hash, the boot key slots, secure boot on, and `MATCH` |
| 5 | `glitch` | armed at sensitivity 3, the number of detector resets since power-on, and the last one's details if any |

If `glitch` shows a detector reset you didn't cause, note it: issue #100 tracks them.

## 20. Disruptive steps (optional)

These restart the board, erase data or change its identity. Do only the ones you mean to, one
at a time, and tell the Reviewer before you start this section.

- **Restart and reset reporting** (safe on dev-1):
  1. `reboot`. You should see: the console drops; picocom back, log in; `uptime` shows a
     requested restart.
  2. `crash fault`. You should see: the board resets at once; after login, the boot log and
     `uptime` show `hard fault on core 0 at 0x...` with the addresses, and `health` shows the
     `resets` check as WARN.
  3. `crash hang`. You should see: nothing for about 8 s, then a watchdog reset; `uptime`
     shows `(last reset: watchdog timeout)`.
  4. Don't run three crashes in a row within three minutes of each other: that counts as a
     boot loop (and `/autorun.lua` is skipped after each).
- **Glitch detector:** `glitch trig` resets the board through the detector (it is armed).
  You should see: after login, `glitch` counts one more reset and names the detector. Note on
  issue #100 that this one was forced.
- **Repair the card:** `sd unmount`, `sd check fix`. You should see: the plan, then a `yes`
  question. Anything but `yes` ends with `not repaired`. Answer `yes` only on a card whose
  contents you can lose.
- **Format the card: Stop and ask first.** `sd format` erases everything on dev-1's card
  (log files, man pages, the time zone bundle) and shows a new recovery key once.
- **First-time setup: on a spare board, not dev-1.** It starts from an empty configuration,
  and getting there (`config reset`, provisioning, or erasing the configuration over USB) makes
  the board a new owner: a new SSH host key, the console password and Wi-Fi gone, and the
  cards it encrypted locked for good (readable only with the key shown when each was
  formatted). On dev-1 that means its card and its current setup; do it there only after
  agreeing a way back with the Reviewer. On a spare board flashed with main:
  1. **[console]** `config reset` (skip on a board that has never been set up).
     You should see: a warning that the encrypted cards will be locked, and a `yes` question.
  2. Press Enter on the console.
     You should see: no password question; the board goes straight into `setup`, starting with
     `This board has no console password yet.` and `New console password`.
  3. Type a 7-character password, then an 8-character one twice.
     You should see: the short one refused; the second accepted (not echoed).
  4. At the network question, Tab twice, pick your network, then its password.
  5. At `Public key:`, paste one line of your `~/.ssh/id_ed25519.pub`.
     You should see: `saved:` and the configuration. Shortly after, the log shows
     `wifi: connected, ip ...` and `sshd: listening on port 22 ... host key ssh-ed25519
     SHA256:...`.
  6. **[host]** `ssh <its address>`.
     You should see: if you logged in to this board before, your client warns that the host key
     changed (it is a new owner). Check the fingerprint against the log line, then
     `ssh-keygen -R <its address>` and log in.
  7. **[console]** `sd`
     You should see: a card it encrypted before is refused as locked; `sd format` makes it
     usable again (and erases it).
  8. `logout`, then press Enter.
     You should see: the console asks for the new password.
  9. To provision the same board from a file instead, make the file again as in section 18,
     then `tools/provision.sh -s <serial> ~/sos-review/p.conf`, and remove the file after. You should see: the board reboots into BOOTSEL
     and back, `provision.sh` warns the encrypted cards are locked, and after login `config`
     shows the file's settings, including the time zone.
- **Never** run `rootsecret forget` or any `picotool otp` command during this review.

## Coming soon (not on main yet)

Review these once they merge; their PRs describe what to try:

- Build feature options: leave features out of an image at build time (#250, design #279).

## When you finish

- [ ] `/review` removed (section 16), `~/sos-review` on this machine removed, the time zone back as it was, `/lib/greet.lua` removed
  if you made it.
- [ ] The board is on a main image and logged out (`logout` on the console).
- [ ] Tell the Reviewer the slot is free.

## Notes

| Section.step | What you saw | Issue? |
|---|---|---|
| | | |
