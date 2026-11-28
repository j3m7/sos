# Download, checksums, gzip and tar: design note

Decision (the user's, 2026-11-27): **SOS gets enough file tooling to download a file, verify it
and unpack it on the board, and to make archives:** `fetch`, `sha1sum`/`sha256sum`/`sha512sum`,
`gzip`/`gunzip`/`zcat`, and `tar`. The commands use the Unix names. They work on files on the
card, not on pipes. Compression comes from zlib, added as a submodule.

Further decisions (the user's, 2026-11-27):
- `fetch` supports both `http://` and `https://` (§3.3).
- Pipes stay text as they are. Carrying binary data through them would need a separate error
  stream, back-pressure from filters, and file-writing filters (§2). The tools read and write
  files.
- Compression (`gzip`, `tar c`) is planned too, after unpacking (§9).
- The SHA-256 accelerator is a follow-up, not part of the first checksum PR (§4.3).

This note sets the rules so that the phases (§9) can be built without deciding them again.
Revised after Reviewer's review of #266: §3.1, §5.2, §6, §6.3, §6.5, §7, §10.

Where this note and the code differ in detail, the code and its header comments will be the
reference.

The workflow it is for:

```
fetch https://example.org/pkg-1.2.tar.gz
fetch https://example.org/SHA256SUMS
sha256sum -c SHA256SUMS
tar xf pkg-1.2.tar.gz -C /apps
```

## 1. What exists today

- **HTTPS client** ([src/net/http.c](../../src/net/http.c), `http_msg.c`): one request per
  connection, non-blocking (`http_start()`/`http_poll()`/`http_read()`), pulls the body only as
  fast as the caller reads it. Responses are parsed incrementally (`Content-Length`, chunked, or to
  the close; a body without a length is trusted only with `close_notify`). Only `https://`
  (`http_msg.h`'s URL parser refuses anything else), and no redirects: the caller sees the `3xx`.
  Two requests at once, a third refused; refused below ~64 KB of free heap.
- **`http get`** is a test command. It filters control bytes and prints timings and the
  certificate after the body, so it can't save a file.
- **Hashes:** wolfCrypt has SHA-1, SHA-256, SHA-384, SHA-512 and SHA-3, all plain C (SHA-256's
  table in SRAM: `SOS_WOLFCRYPT_TABLES_IN_RAM`). Lua's `sos.hash` uses them on core 0
  ([src/lua/lua_core1.c](../../src/lua/lua_core1.c)). No hardware hashing is used. The RP2350
  has a SHA-256 accelerator, and the SDK a driver for it (`pico_sha256`), not linked.
- **No compression code** is linked.
- **File commands** ([src/files/files.c](../../src/files/files.c)) run as shell jobs
  (`shell_run_job`, `STEP_US` = 20 ms of work per pass), stop on Ctrl-C, hold open files
  (`files_hold()`/`files_release()`, so the card can't be unmounted or formatted under them),
  check every change with `files_write_allowed()`, and sync files being written once a second
  (`files_sync_if_due()`).
- **FatFs:** long names (`FF_MAX_LFN` 255), exFAT and FAT32, `FF_FS_LOCK` 16, and
  `FF_USE_CHMOD 0`: no `f_utime()`, so a file's date can't be set (§6.5).
- **The heap:** about 345 KB, shared (`heap_free()`). A TLS handshake needs ~64 KB free; an SSH
  session up to ~100 KB while transferring.
- **The shell has no exit status.** A command reports failure by what it prints.

## 2. Why files and not pipes

The shell's pipe passes `(buf, len)` unchanged; only the terminal turns `\n` into `\r\n`. What
keeps it text is the rest of its design ([shell-pipes.md](shell-pipes.md)):

1. **One stream.** A pipe carries everything a command prints, its errors and status included
   (`cat` prints `\ncat: <error>` into its output). There is no stderr, so a binary stream would
   have messages inside it.
2. **Only filters read a pipe.** They are synchronous functions inside the producer's write.
   Commands have no stdin.
3. **No back-pressure.** `pipe_write()` returns nothing; a filter must take every byte at once.
   One compressed block can expand to megabytes and many file writes, far past `STEP_US`.

Fixing those is a design of its own. The card has gigabytes free, so the tools take file names
and keep the intermediate file. The one exception is `zcat`, whose output is text for the
terminal or a pipe (`zcat log.gz | grep x`), and which is a job with output limited by
`shell_write_space()`, like `cat`.

## 3. `fetch`

```
fetch [-f] [-k] <url> [<file>]
```

- Saves the body exactly as received. `<file>` defaults to the last segment of the URL's path,
  percent-decoded and checked like any name (§6.3). No segment, or a name refused: an error that
  asks for `<file>`. A directory as `<file>`: the default name inside it.
- **`.part`, then rename.** The body goes to `<file>.part`; only a complete, checked body is
  renamed to `<file>`. An existing `<file>` is not replaced without `-f`. A failure or Ctrl-C
  deletes the `.part`. So a cut download never looks finished.
- **Complete means:** the length in `Content-Length` arrived, or the last chunk, or, with neither,
  `close_notify` on https. On http with neither, the end can't be told from a cut (§3.3): the
  file is kept, and the summary says `size not known: check its hash`.
- **Status:** only `2xx` is saved. Anything else prints the status line and up to 256 bytes of
  the body, filtered as `http get` does, and saves nothing.
- **Progress:** one line, redrawn at most twice a second: bytes, percent of `Content-Length` when
  known, rate. Over SSH and piped it prints only the summary.
- **Summary:** size, time, rate, and **the SHA-256 of what was written**, hashed as it is written
  (`sha256: <hex>`). Comparing it with a published hash needs no second pass over the file.
- Runs as a job; writes through `files_write_allowed()`, holds the file, syncs it once a second.
- Heap: the request (~6 KB, plus TLS on https) and a 4 KB write buffer.

### 3.1 Redirects

Followed in `http.c`, so `sos.http` can have them too (opt-in there, §8):
- `301`, `302`, `303`, `307`, `308` with a `Location`. At most 5. A relative `Location` is
  resolved against the request URL.
- **No downgrade:** https to http is refused unless `-k` was given. http to https is allowed.
- Each hop is a new connection with a fresh certificate check. Each hop's URL is printed
  (`-> <url>`), so where the file came from is on screen.
- `fetch` sends only `GET`; `303` changes nothing for it.
- A `Location` longer than the URL parser's limit, or holding `user:password@`, is refused.

### 3.2 TLS roots

There are 2 built-in roots. Many download hosts chain to others (GitHub's release assets do), and
the request fails at the handshake. The failure message names the issuer it couldn't check and
points at `tls roots approve`; the man page says so.

### 3.3 Plain http

- `http_msg.c`'s URL parser learns `http://` (default port 80). `http.c` gets a transport below it,
  TLS or plain TCP (lwIP raw TCP, the same callbacks `tls/` uses underneath). Everything above
  (framing, limits, timeouts, pull-not-push) is shared.
- Without TLS, nothing protects the bytes or the end of the body. `fetch` prints
  `http: not encrypted; check its hash` before the progress line. `-k` is needed only for an
  https-to-http redirect; plain http needs nothing extra.
- A body without a length can only end at the TCP close (§3), which an attacker or a dropped link
  can also produce.
- `http get|head|post` and `sos.http` stay https-only: plain http is for `fetch`, where the hash
  is the check.
- The connection limit (two at once) counts plain connections too.

## 4. Checksums

```
sha256sum <file>...          (and sha1sum, sha512sum)
sha256sum -c <list>
```

### 4.1 Printing

- GNU format, one line per file: `<hex>  <name>`, the name as given. `sha256sum f > f.sha256`
  makes a list `-c` reads.
- A name with a newline or `\` is printed GNU's way: a leading `\` and the name escaped (`\n`,
  `\\`). Control characters other than newline are refused for printing (`<name>: name has
  control characters`), since the line goes to a terminal.
- An unreadable file: `sha256sum: <name>: <error>`, and the rest continue.
- `sha1sum` exists for old published lists. Its man page says SHA-1 is broken for collisions and
  not to rely on it for files from others.

### 4.2 Checking (`-c`)

- Reads the list a line at a time (lines up to 4 KB; longer: `line <n>: too long`). Accepts:
  - GNU: `<hex>  <name>` (text mode) and `<hex> *<name>` (binary mode; the same on SOS), and the
    leading `\` escape form;
  - BSD: `SHA256 (<name>) = <hex>`. The algorithm in the line must match the command.
  Hex is case-insensitive and must be the algorithm's length. Blank lines and lines starting with
  `#` are skipped. Other lines: counted as `improperly formatted`, as GNU does.
- Names are relative to the **list's directory**, not the working directory, so
  `sha256sum -c /apps/SHA256SUMS` works from anywhere. A name that is absolute or holds `..` is
  refused (`<name>: unsafe path`): a list from a server can't make the board read outside its
  directory.
- One line per file: `<name>: OK`, `<name>: FAILED`, `<name>: missing`, `<name>: <error>`.
- Last line: `<n> OK` and, when any, `, <n> FAILED, <n> missing, <n> unreadable, <n> unsafe,
  <n> improperly formatted` (`unreadable`: a file there but not read, `<name>: <error>`;
  `unsafe`: names refused). Since the shell has no exit status, the result is that line. A list
  with no usable line says `no checksums found`; one whose files were all skipped by
  `--ignore-missing` says `no file was verified`. Ctrl-C prints `stopped` instead of the counts.
- `--ignore-missing` (GNU's name): files not on the card are skipped and not counted. Useful for
  a `SHA256SUMS` listing every platform's file.

### 4.3 Speed and the accelerator

- Reads 4 KB per step, hashes it, yields after `STEP_US`. One file at a time.
- The first checksum PR measures it against the card's read rate (`sd test`) on dev-1, for each
  algorithm, and records the numbers here.
- **Follow-up (the user's call):** the RP2350's SHA-256 block through the SDK's `pico_sha256`,
  with a lock: `sha256sum` and `fetch` take the block when it's free and use wolfCrypt when it
  isn't (TLS and SSH keep wolfCrypt). wolfSSL's Thumb-2 SHA-256/512 assembly is the alternative
  that also speeds up SHA-512; the measurement decides which, or neither.

## 5. gzip, gunzip, zcat

```
gunzip [-k] [-f] <file>...
zcat <file>
gzip [-k] [-f] [-1..-9] <file>...
```

### 5.1 zlib

- Submodule `lib/zlib`, at the current release tag. License: zlib. Recorded in THIRD_PARTY.md.
- Built like the other libraries: `SYSTEM` includes, no SOS warnings (`cmake/warnings.cmake`
  applies to SOS's code only). Only the files needed: inflate, deflate, crc32, adler32, trees,
  zutil (no gz* file I/O: SOS does its own through FatFs). `Z_SOLO` and our own `zalloc`/`zfree`
  on a new heap tag, `HEAP_ZLIB`, so `mem` shows it.
- Built for the host tests too, so the code under test is the code on the board.

### 5.2 gunzip

- `<name>.gz` and `<name>.tgz` give `<name>` and `<name>.tar`. Other names: refused (`unknown
  suffix`) as GNU does.
- `inflateInit2(windowBits 15 + 16)`: gzip only, the full 32 KB window (a file compressed with a
  smaller window is read the same way).
- Concatenated members (`cat a.gz b.gz > c.gz`) are decompressed one after the other. Zero bytes
  after the last member are ignored with one line, `ignoring trailing zeros`, as GNU does:
  block-padded files (written through `dd` or tape-style tools) are real, and deleting the output
  for padding would look like a broken download. Any other bytes after the last member that
  aren't another member are an error.
- Each member's CRC-32 and size (mod 2^32) are checked by zlib. A mismatch deletes the output.
- Output to `<name>.part`, renamed when the input ended cleanly. An existing `<name>` is not
  replaced without `-f`. The input is deleted after success unless `-k` (Unix behaviour).
- The gzip header's file name and date are ignored: the output name comes from the input name.
- Per step: inflate until 4 KB of output or the input buffer (4 KB) is used, write, repeat until
  `STEP_US`. A small input can produce huge output (a "zip bomb"); a step's bounded output and
  the card's free space are the only limits. A full card stops with `card full` and deletes the
  `.part`.
- Heap: zlib's inflate state (~7 KB) + the 32 KB window + 2 × 4 KB buffers ≈ 47 KB. Refused when
  `heap_free()` minus that is below 64 KB, so time sync and an `http` request still fit.
- **One at a time:** `gunzip`, `zcat`, `gzip`, `tar` and `fetch` count each other across all
  shells. Two at once would need ~110 KB, so while one runs another is refused (`busy: <command>
  is running in another shell`), as `http.c` refuses a third request.

### 5.3 zcat

- The same decoder, output through `shell_write()` like `cat`, limited by `shell_write_space()`.
- The file isn't changed. No `.part`, no `-k`.

### 5.4 gzip

- `<name>` gives `<name>.gz`. Refused: a name already ending in `.gz` or `.tgz`, a directory.
- **Memory decides the settings.** zlib's deflate needs `(1 << (windowBits + 2)) +
  (1 << (memLevel + 9))` plus ~6 KB. The defaults (15, 8) need ~262 KB, most of the heap. SOS uses
  `windowBits 12` (4 KB window) and `memLevel 6`: ~54 KB, plus 2 × 4 KB buffers. The output is
  standard gzip any gunzip reads. Files come out a few percent larger than with the defaults;
  phase 6 measures how much on logs and on a source tree, and records it here.
- `-1` to `-9`: zlib's levels, default 6. The level changes speed, not memory.
- Output to `<name>.gz.part`, renamed at the end; the input deleted unless `-k`. The gzip header
  holds no name and date 0 (`gzip -n`), since FatFs dates are local and coarse.
- Heap check as `gunzip`, with ~62 KB.
- Speed: measured in phase 6 per level. Expected well under 1 MB/s; each step yields after
  `STEP_US`, so the shell stays responsive whatever the speed.

## 6. tar

```
tar tf <archive>
tar xf <archive> [-C <dir>] [-f]
tar cf <archive> [-C <dir>] <path>...
tar czf <archive> [-C <dir>] <path>...
```

- Options are one word as Unix tar writes them (`tf`, `xf`, `xzf`, `cf`, `czf`; also with a
  leading `-`, and `v` accepted). `-f` after the archive means overwrite (§6.4); on Unix it names
  the archive, but here the archive is always the word after the mode, so there is no clash. The
  man page says in one sentence that this `-f` differs from Unix tar's.
- Reading recognises gzip by its magic bytes (`1f 8b`), so `xf` and `tf` take `.tar`, `.tar.gz`
  and `.tgz` alike, in one pass, with no temporary `.tar`. `z` is accepted and not needed.
- `c` writes plain tar; `cz` writes gzip (§5.4 settings).

### 6.1 The format read

- **ustar** (POSIX 1988) and **GNU** (`ustar  \0`) headers; the old v7 format (no magic) too, by
  its checksum.
- The header checksum is checked (unsigned sum; signed accepted as GNU does). A mismatch stops:
  `bad header at <offset>`.
- Sizes: octal, and GNU base-256 for large sizes. Over 4 GB minus 1 on FAT32: refused
  (`<name>: too large for FAT32`); exFAT takes it.
- **Long names:** GNU `L` (and `K`, link names, read to skip the link) and pax `x` headers:
  `path`, `size`, `linkpath` used, other keys ignored. pax `g` (global) headers: `path` and `size`
  ignored there as GNU does, others ignored. The ustar `prefix` field is joined to the name.
- Two zero blocks end the archive. End of file without them: `archive ends early` (what was
  extracted stays, §6.4). Data after them is ignored.
- The parser is pure C over 512-byte blocks fed in any split (`tar_parse.c`), host-tested and
  fuzzed. It allocates nothing; long names and pax records go into a fixed buffer of
  `FF_MAX_LFN` × 3 + 1 bytes (UTF-8): longer is refused, not truncated. A pax record set larger
  than 4 KB is refused.

### 6.2 Entry types

| Type | Read | Created |
|---|---|---|
| `0`, `\0`, `7` (file) | extracted | yes |
| `5` (directory) | created | yes |
| `1` (hard link), `2` (symlink) | skipped: `<name>: link skipped (FAT has no links)` | no |
| `3`, `4`, `6` (devices, FIFO) | skipped: `<name>: <type> skipped` | no |
| `L`, `K`, `x`, `g` | used as §6.1 | `x` for long names |
| anything else | skipped with its type letter | no |

A link is never written as a file holding the target's name, and never followed.

### 6.3 Names and paths (safety)

The archive comes from elsewhere; these rules hold for `x` and are checked before anything is
created. A refused entry is skipped (its data read past), printed, and counted; extraction
continues.

- **Leading `/`** is removed, as GNU does, and said once (`removing leading /`). The name is then
  relative to the target directory (`-C`, or the working directory).
- **`-C <dir>` must already exist**; it isn't created (`<dir>: no such directory`).
- **Every name source follows these rules:** a pax `path` and a GNU `L` name are checked exactly
  as a header name is. They are the longest names, and the likeliest place for a trick.
- **`..` anywhere** in the name: refused (`<name>: unsafe path`). Also `.` components are
  dropped and empty components (`a//b`) joined.
- **Never outside the target:** after joining, the path must stay under the target directory.
  With no links on FAT and `..` refused, that holds by construction; the check is still made
  (`path_resolve()` and a prefix compare).
- **FAT names:** components with `"*:<>?\|`, control characters, or ending in a space or `.` are
  refused (`<name>: not a valid FAT name`). Too long (over 255 UTF-16 units per component, or
  over the path limit): refused.
- **Case:** FAT is case-insensitive. A second entry differing only in case from one written in
  this run: refused (`<name>: same name as <other> on FAT`), not written over it. Entries named
  the same exactly (a tar can hold a file twice): the later one wins, as Unix tar.
- **Protected paths:** every create goes through `files_write_allowed()`, so `/certs` needs the
  console as for every file command, and `tar` can't do what `cp` couldn't.
- **The archive itself** is never written over (same path as an entry: refused).

### 6.4 Writing files

- Each file is written to `<name>.part` and renamed at its end, so an interrupted extraction
  leaves no half file under a real name.
- **No overwrite by default** (differs from Unix tar, which overwrites): an existing file is
  skipped (`<name>: exists`) unless `-f`. An existing directory is fine (merged). A file where a
  directory is needed, or the other way round: refused for that entry.
- Directories are created as needed, also those implied by a file's path.
- Ctrl-C, a read error or a bad header stops; the current `.part` is deleted, files already
  extracted stay. The summary counts them: `<n> files, <n> directories, <n> skipped`, then
  `stopped: <why>` when it stopped.
- Holds files and syncs the open one once a second, like `cp`.
- Heap: 512-byte block buffer, 4 KB write buffer, the name buffer; plus §5.2's ~40 KB when
  gzipped. Same heap rule.

### 6.5 Dates and modes

- `FF_USE_CHMOD` is 0, so there is no `f_utime()`: extracted files get the time of extraction.
  The design wants archive dates. `ff.h` includes `"ffconf.h"` with quotes, and the library's
  copy is in `src/include`, not beside `ff.c`, so the include order decides which one is found.
  Phase 5 tries first: a SOS directory holding its own `ffconf.h` (a copy with `FF_USE_CHMOD 1`),
  placed before `${SDCARD_ROOT}/src/include`, which overrides it without patching the submodule.
  The check is that FatFs's own objects see the same header: one definition, or `FIL`'s layout
  would differ between them and SOS. If that can't be made to hold, dates stay the extraction
  time and this note says so.
- Modes, owners and groups are ignored. The FAT read-only attribute isn't set from the mode.

### 6.6 Listing (`tf`)

- One line per entry: type letter (`-`, `d`, `l`, `h`, other), size, date (from the header, UTC,
  shown in the configured zone like `ls`), name. Names with control characters are shown with
  them as `?`. Long names and pax names shown in full. Refused entries are listed with the reason
  `x` would give, so `tf` shows what `xf` would do.
- Through `shell_write()`, so `tar tf x.tgz | grep foo` and `| more` work.

### 6.7 Creating (`c`)

- `<path>...` relative to `-C <dir>` (default the working directory), stored as given, without
  a leading `/`. Directories are walked recursively, in directory order.
- The walk keeps its stack on the heap (one `DIR` and the path, per level; at most 16 levels,
  deeper: refused for that directory), not the C stack (`-Wstack-usage=1536`).
- Headers: ustar; a name over 100 bytes that `prefix` can't hold gets a pax `x` record (`path`).
  Size, mtime from the FAT date (local time converted to UTC with the configured zone), mode
  `0644` files and `0755` directories, uid/gid 0, no user names.
- Skipped with a note: the archive itself (and its `.part`), files being written by someone else
  (open for writing: what `ls` shows as `being written`), anything not readable.
- Output to `<archive>.part`, renamed at the end. Not replaced without `-f`.
- Host tests: archives made here are listed and extracted by GNU tar and bsdtar, and theirs by
  ours (§7).

## 7. Tests

| What | Where |
|---|---|
| checksum list parser (GNU, BSD, escapes, bad lines, unsafe names) | host test + fuzz target |
| URL parser with http, `Location` resolution, redirect rules | host test (`test_http_msg.c`) + existing fuzz |
| gunzip wrapper: multi-member, trailing bytes, bad CRC/size, truncation; output split at every step size | host test against GNU gzip output + fuzz (input bytes) |
| gzip round trip: ours → GNU gunzip, ours → ours, each level | host test |
| tar parser: ustar, GNU, v7, base-256, `L`/`K`, pax `x`/`g`, prefix, bad checksum, end without zero blocks, every split | host test + fuzz target |
| tar name rules (§6.3): `..`, leading `/`, FAT characters, case clash, too long | host test (pure function) |
| tar create: GNU tar and bsdtar list and extract it, names over 100 bytes | host test (skips when the tools are missing, as other host tests do) |
| board | per phase (§9): a real release tarball and its `SHA256SUMS` fetched, checked, extracted; Ctrl-C at each stage; full card; heap low |

Fixtures are small archives committed under `tests/data/archives/`, made by a script
(`tools/make-archive-fixtures.sh`, checked by shellcheck through `just lint` like the other
scripts) so they can be remade. Fuzz corpora are replayed in ctest like the others.

## 8. Lua (later)

Not in the first phases. Candidates, each a small PR after the commands: `sos.hash` over a file
(`sos.hashfile(path, "sha256")`), redirects in `sos.http` (opt-in option), and file-to-file
`sos.gunzip`/`sos.untar`. They run on core 0 as `sos.hash` does.

## 9. Phases

Each phase is one PR with its man pages, `docs/commands.md` (regenerated) and docs, reviewed by
Reviewer and tested on dev-1 in its slot.

| Phase | PR | Needs |
|---|---|---|
| 1 | Checksums: `sha1sum`, `sha256sum`, `sha512sum`, `-c`, `--ignore-missing`; speed measured (§4.3) | — |
| 2 | `fetch` over https: `.part`, redirects in `http.c`, progress, SHA-256 in the summary | — |
| 3 | Plain http: transport under `http.c`, URL parser, `fetch` warnings | 2 |
| 4 | zlib submodule; `gunzip`, `zcat` | — |
| 5 | `tar tf`, `tar xf` (plain and gzip), `-C`, `-f`; dates if `FF_USE_CHMOD` can be set (§6.5) | 4 |
| 6 | `gzip`, sizes and speeds measured (§5.4) | 4 |
| 7 | `tar cf`, `tar czf` | 5, 6 |
| 8 | Follow-ups: SHA-256 accelerator (§4.3), Lua (§8) | 1 |

Phases 1 and 2 give download and verify; 4 and 5 add unpacking. 1, 2 and 4 don't depend on each
other.

## 10. Settled in review

Reviewer's review of #266 (2026-11-27):
- `tar xf` doesn't overwrite by default (§6.4), unlike Unix tar. Kept: with no exit status and
  `.part`-then-rename everywhere, "never silently replace" is the rule `fetch`, `gunzip` and
  `cp` follow too; `-f` gives Unix behaviour.
- `gunzip` ignores trailing zeros with a note, as GNU does (§5.2), rather than refusing them.
- Plain http only in `fetch` (§3.3): the hash is the check there and nowhere else.
- Added: the `Location` limits (§3.1), the one-at-a-time rule (§5.2), `-C` must exist and pax
  names follow the same rules (§6.3), how to try `FF_USE_CHMOD` (§6.5).
