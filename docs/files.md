# Files (SD card)

The Adalogger's microSD slot (SPI0: SCK GP18, MOSI GP19, MISO GP16, CS GP17, at 25 MHz) holds
the device's files. Cards over 2 GB (SDHC/SDXC) with exFAT or FAT32 work; the card is mounted
when first used. The file commands work in every shell, SSH included, and each shell has its own
working directory:

| Command | |
|---|---|
| `ls [path]`, `cd [dir]`, `pwd` | list, change and show the working directory |
| `cat <file>` | print a file |
| `cp <from> <to>`, `mv <from> <to>` | copy, move or rename (to a directory: keeps the name) |
| `rm <path>`, `mkdir <dir>` | delete a file or an empty directory, create a directory |
| `fetch [-f] [-k] <url> [<file>]` | download a file over HTTPS (or plain HTTP), with its SHA-256 ([TLS and HTTPS](tls.md#downloading-files)) |
| `echo <text>` | print the text; `> file` / `>> file` after any command (the shell's redirect) writes / appends its output to a file |
| `sha256sum <file>...`, `sha256sum -c <list>` | print files' SHA-256, or check files against a checksum list (also `sha1sum`, `sha512sum`; [below](#checksums)) |
| `gunzip [-k] [-f] <file>...`, `zcat <file>` | decompress `x.gz` to `x` (`x.tgz` to `x.tar`); print a gzip file decompressed ([below](#gzip-files)) |
| `gzip [-k] [-f] [-1..-9] <file>...` | compress `x` to `x.gz` ([below](#gzip-files)) |
| `tar tf <archive>`, `tar xf <archive> [-C <dir>] [-f]` | list or extract a tar archive, plain or gzipped ([below](#tar-archives)) |
| `tar cf <archive> [-C <dir>] [-f] <path>...`, `tar czf ...` | make a tar archive, plain or gzipped ([below](#making-an-archive)) |
| `sd` | card and volume information, whether it's encrypted, SPI clock |
| `sd format [exfat\|fat32] [plain]` | console only: erase the card and format it (one partition), encrypted unless `plain`; an encrypted format shows the card's key once, for offline recovery |
| `sd test [KB [keep]]`, `sd speed [MHz]` | console only: write/read benchmark (everything else waits meanwhile); SPI clock |
| `sd unmount` | console only: unmount the card, to remove it |
| `sd check` | console only, on an unmounted card: check its file system (exFAT or FAT32), reporting problems without writing |
| `sd check fix` | console only, on an unmounted card: check, show what it would repair, and after `yes` repair what loses no data (FAT32 for now) |

- **Long commands run in steps.** `ls`, `cat`, `cp`, `gunzip`, `zcat`, `gzip` and `tar` do a
  little work per pass of the main loop, so SSH sessions, the console and the watchdog keep
  going; Ctrl-C stops them (a stopped `cp` removes its partial copy). Commands typed meanwhile
  run afterwards.
- **A slow SSH client** slows `cat` down instead of losing output. Printing a file over SSH runs
  at about 130 KB/s (encryption-bound); copying on an unencrypted card at about 580 KB/s.
- **A file being written is synced once a second.** `tee`, `> file`, `cp` and SFTP uploads sync
  the file they write at most once a second while it grows, so `ls` shows its size and a reset
  loses at most the last second; until the first sync `ls` shows `being written` in place of the
  date.
- **Open files are protected:** a file that a running command has open can't be deleted or
  renamed, and the card can't be formatted or unmounted meanwhile.
- A file name with spaces is quoted or escaped, as in a Unix shell: `cat 'my file'`,
  `cp my\ file backup` (QUOTING in `man intro`). Tab doesn't offer such names yet. Files are
  dated with the system clock (UTC), which comes from the RTC or NTS.
- No card-detect switch is wired by default (solder jumper SJ1 connects it to GP15), so SOS
  doesn't notice a card swap: run `sd unmount` before taking the card out; the next file command
  initializes and mounts the new card.

## Checksums

`sha1sum`, `sha256sum` and `sha512sum` print a file's hash in GNU's format (`<hex>  <name>`),
and with `-c` check the files a published list names (full rules in `man sha256sum`; design:
[docs/design/archives.md](design/archives.md) §4):

```
sha256sum pkg-1.2.tar.gz
sha256sum -c /apps/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
```

- `-c` reads GNU lines (`<hex>  <name>`, `<hex> *<name>`, the `\` escape) and BSD lines
  (`SHA256 (<name>) = <hex>`, for the command's own algorithm). It prints `<name>: OK`, `FAILED`,
  `missing` or the error per file, then the counts (`2 OK, 1 FAILED`). The shell has no exit
  status, so that last line is the result.
- Names in a list are relative to the **list's** directory. Absolute names and names with `..`
  are refused (`unsafe path`): a list from a server can't make the board read outside its
  directory.
- A job like `cat`: one file at a time, 4 KB per step, Ctrl-C stops it. Hashing is wolfCrypt's,
  in software on core 0, except that `sha256sum` (and `fetch`'s SHA-256) uses the RP2350's
  SHA-256 block when no other shell is using it: the same result, faster.

## gzip files

`gunzip x.gz` decompresses to `x` and deletes `x.gz` (`-k` keeps it); `x.tgz` gives `x.tar`.
`zcat x.gz` prints it decompressed without changing it (`zcat old.log.gz | grep error`). Both
run as jobs (Ctrl-C stops them) and read what GNU gzip writes, including joined files
(`cat a.gz b.gz > c.gz`) and zero padding after the end.

- **Nothing half-written:** gunzip writes `x.part` and renames it to `x` only when the whole
  file has been read and its CRC-32 and length checked. A corrupt or cut file, a full card
  (`card full`) or Ctrl-C deletes the `.part` and keeps the input. An existing `x` is replaced
  only with `-f`.
- **Memory:** about 50 KB while it runs (`zlib` in `mem` is zlib's state and 32 KB window); it
  is refused when that would leave less than 64 KB free. One of gunzip, zcat and gzip runs at a
  time across all shells; another is refused with `busy: gunzip is running in another shell`.

`gzip x` compresses to `x.gz` and deletes `x` (`-k` keeps it); `-1` (fastest) to `-9`
(smallest), 6 by default. The output is standard gzip with no name and date 0 in its header
(like `gzip -n`), so `gunzip` on a computer reads it after an SFTP download.

- **Nothing half-written:** it writes `x.gz.part` and renames it to `x.gz` at the end; an error,
  a full card (`card full`) or Ctrl-C deletes the `.part` and keeps `x`. An existing `x.gz` is
  replaced only with `-f`. Names ending in `.gz` or `.tgz` and directories are refused.
- **Smaller memory, slightly bigger files:** a 4 KB window and zlib's memory level 6 take about
  54 KB (about 65 KB in all while it runs, with the same 64 KB kept free), where a computer's
  gzip settings would take 262 KB. Files come out larger than with `gzip` on a computer: about
  5 % for logs, up to 20 % for prose and source code
  ([measurements](design/archives.md#54-gzip)).

## tar archives

`tar tf pkg.tar.gz` lists an archive, `tar xf pkg.tar.gz -C /apps` extracts it (into the working
directory without `-C`; the directory must exist). `.tar`, `.tar.gz` and `.tgz` are read alike:
gzip is recognised by its first bytes and decompressed in the same pass. GNU tar, bsdtar and pax
archives work, long names included. The listing goes through pipes (`tar tf x.tgz | grep src/`).
Full rules in `man tar`; design: [docs/design/archives.md](design/archives.md) §6.

- **Names are checked first.** A leading `/` is removed; names with `..`, names FAT can't store,
  a name differing only in case from one already extracted, links, devices and FIFOs are skipped
  with the reason, and the rest is extracted. Protected paths need the console, as for `cp`.
- **Nothing half-written, nothing replaced silently:** each file is written to `<name>.part`
  and renamed at its end; Ctrl-C, a damaged archive or a full card stops with `stopped: <why>`,
  removing only the current `.part`. An existing file is skipped unless `-f` (which here means
  overwrite, not "archive file" as on Unix).
- **Dates are kept:** files and directories get the archive's dates (UTC, like every file
  date on the card). Modes and owners are ignored.
- **Memory:** about 21 KB, plus 40 KB for a gzipped archive; one of `tar`, `gunzip`, `zcat`,
  `gzip` and `fetch` runs at a time.

### Making an archive

`tar cf www.tar -C /apps www` makes an archive of `/apps/www` with names starting `www/`;
`tar czf` makes it gzipped. Paths are relative to `-C` (or the working directory), stored as
given without a leading `/`; directories are taken whole, up to 16 levels deep. The archive is
ustar with pax records for long names (mode 0644 for files, 0755 for directories, owner 0, the
card's UTC dates), so `tar xf` on a computer reads it after an SFTP download.

- **Skipped and said:** the archive itself, files being written (what `ls` shows as `being
  written`, or open for writing elsewhere, e.g. an SFTP upload or `tee`), unreadable files,
  paths that don't exist. A path with `..` is refused.
- **Nothing half-written:** it writes `<archive>.part` and renames it at the end; Ctrl-C, an
  error or a full card deletes the `.part`, and so does an archive with nothing in it. An
  existing archive is replaced only with `-f`.
- **Memory:** about 15 KB, plus 56 KB with `z`.

## Checking the card

FatFs keeps no journal, so a power cut during a write (and the log writer writes all the time)
can leave lost clusters, a file whose size doesn't match its clusters, or clusters two files
share. `sd check` looks for these, on the console, on an unmounted card:

```
sd unmount
sd check
```

It refuses while the card is mounted (`the card is mounted: run 'sd unmount' first`) or a file
is open. While it runs, nothing else reaches the card: file commands, SFTP and Lua fail as when
there's no card, an `http` request works only if the TLS roots were already loaded (they stay
in memory once read), the log writer keeps its lines in memory,
and the other `sd` commands say `the card is being checked (sd check)`. It prints one line per
problem and a summary, which also goes to the log (`sd: check: ...`). It reads only;
`sd check fix` (below) repairs. Ctrl-C stops it. When it ends the card is left as `sd unmount` leaves it: the
next file command mounts it again, and the log writer's once-a-minute retry doesn't. The report
can't be written to a file on the card: `sd check > file` opens the file first, which mounts the
card, so the check refuses and its "the card is mounted" line is what lands in the file.
`sd check | more` works, as the pager keeps its lines in memory.

The output looks like this (design: [docs/design/sd-check.md](design/sd-check.md)):

```
checking exFAT, 29.7 GB, 970752 clusters of 32 KB, encrypted (8 passes)
pass 1/8
/var/log/messages: 3 clusters for 70000 bytes, chain has 2
pass 2/8
...
lost: clusters 81920-81935 (512 KB)
pass 8/8
2 problems; 512 KB lost, 0 cross-linked
```

The lines, path first when there is one:

- **Entries:** a chain that loops, breaks or reaches a free or bad cluster; a size that needs
  more clusters than the chain has; names FatFs refuses or repeats; on FAT32 long-name and
  `.`/`..` entries, on exFAT entry sets (their checksum, name hash, counts, lengths) and
  NoFatChain files that run past the last cluster; directories that loop or nest too deep.
- **Clusters:** `lost: clusters <a>-<b> (<size>)`: in use (FAT32: in the FAT; exFAT: in the
  allocation bitmap) but reached by no file. `reached but free: ...` (exFAT): used by a file
  but free in the bitmap. `<path>: cross-linked with an earlier entry ...`: clusters another
  file uses too. On exFAT, where a contiguous file can share any part of another, a file is
  named when its last cluster is shared; otherwise one line says `<n> clusters used by more
  than one entry`.
- **The volume:** the backup boot sector or exFAT's backup boot region differing, exFAT's
  boot checksum, the FSInfo sector, the two FATs differing, `up-case table checksum ...`, and
  `the allocation bitmap's chain isn't contiguous ...` (FatFs won't mount such a card).

Lines starting `note:` are not problems: a chain longer than its file (wasted clusters), a
stale FSInfo free count, the volume flagged dirty or as having had a disk error (left by a PC
or camera; `volume flagged dirty` is added to the summary), FAT entry 0 not matching the media
byte, and an exFAT up-case table that maps characters unlike FatFs. A card that can't be checked
at all ends with `not checked: <why>`: no card, no FAT32 or exFAT volume, or a boot sector FatFs
would refuse.

A card has about a million clusters, too many to track at once beside Wi-Fi, TLS and SSH, so
the clusters are checked a window at a time, one pass over the directory tree per window: 16 KB
of memory gives 8 passes on a 32 GB card, and less memory more passes (it needs about 11 KB at
least).

## Repairing the card

`sd check fix` runs the same check, under the same rules (console only, unmounted card, nothing
else reaches the card until it ends), then shows what it would repair and asks for `yes`, as
`sd format` does. It repairs only what can't cost data a file still reaches (design:
[§10 of the design note](design/sd-check.md#10-repair)):

- **lost clusters** are freed;
- **chains longer than their file** are cut to the file's size, and the rest freed;
- **a second FAT** that differs from the first gets the first's sectors;
- **FSInfo's free count** is set to unknown, so the next mount counts free space again.

It never changes cross-linked files, broken or short chains, damaged entries or names, the boot
sector or the partition table: each needs a choice of whose data wins, which is for a PC and a
backup. With cross-links anywhere it cuts no chain (`trims skipped: cross-linked files, fix those
on a PC first`), and when part of the directory tree couldn't be read it frees and cuts nothing,
as what that part holds would look lost. An empty file that still has clusters is left alone.
On exFAT the allocation bitmap is what changes: lost clusters' bits are cleared, and clusters a
file uses but the bitmap calls free are marked in use; a chain that breaks on a free or bad FAT
entry means nothing is freed or cut, since the cluster it breaks on would look lost.

```
sd unmount
sd check fix
checking FAT32, 7.5 GB, 244096 clusters of 32 KB, encrypted (2 passes)
pass 1/2
note: /var/log/messages: 2 clusters for 40000 bytes, chain has 3
pass 2/2
lost: clusters 9000-9003 (128 KB)
1 problem; 128 KB lost, 0 cross-linked
to repair:
  free 4 lost clusters (128 KB)
  trim 1 chain longer than its file, freeing 1 cluster (32 KB)
  set FSInfo's free count to unknown (counted again at the next mount)
Cross-links, broken or short chains and damaged entries are only reported.
Ctrl-C stops the repair: wherever it stops, the card is no worse than before.
Repair the card? Type 'yes': yes
repairing (2 passes)
pass 1/2
pass 2/2
checking again (2 passes)
pass 1/2
pass 2/2
repaired: freed 5 clusters (160 KB), trimmed 1 chain, FSInfo's free count set to unknown; 4 sectors written
after repair: no problems
```

Anything but `yes` ends it with `not repaired`; a plan with nothing to write ends with
`nothing to repair`. Repair writes one sector at a time, in an order that keeps the card no
worse than before wherever it stops (Ctrl-C, a write error, a power cut): files read the same
bytes, and a check finds what it found before or less, plus at most a "FAT copies differ" note
and lost clusters at the end of a chain being cut, which `sd check fix` frees when run again. A
write error stops it at once: `stopped: write error at sector <n>; the card is no worse than
before (sd check shows what is left)`. A power cut can still leave a half-written sector, which
no order of writes prevents, so check a card again after one. The plan, what was written and
the final summary go to the log as `sd: check fix: ...`.

## Encrypted cards

`sd format` encrypts the card by default, so a card taken out of the device can't be read
without it. Everything written to the card from then on is encrypted, file names included;
FatFs, the file commands, SFTP and Lua work as before. Formatting doesn't erase the card's old
contents, though: files from earlier, unencrypted use stay readable on it until new data
overwrites them, so a card that held private files unencrypted is best replaced by a new one.
Cards formatted elsewhere, or with `sd format plain`, still work unencrypted; `sd` shows which
kind is in.

- **The key:** each card has a random data key, stored on the card encrypted with this board's
  card key. The card key is made from two things: the board's root secret, and the **owner
  secret**, 32 random bytes kept in the sealed configuration. So a card unlocks at boot with
  nobody logged in, and survives reflashing. A card made by another board is refused (`sd
  format` erases it for use here). The card holds two copies of its header; a damaged first
  copy is rewritten from the second.
- **Erasing the configuration locks the cards.** `config reset`, erasing the configuration
  over USB, and provisioning the board again all destroy the owner secret: the board that
  comes back is a new owner, and cards the old one encrypted can only be read with the key
  shown when each was formatted. This is deliberate: otherwise someone who has the board
  could erase its configuration, set it up as their own, and have it unlock the cards.
  `config reset` warns before it does this.
- **What it protects:** a lost or removed card. Not yet a stolen board: on a board without
  secure boot the root secret is in flash, which can be read out (issue #64).
- **Recovery:** an encrypted `sd format` prints the card's id and data key when it finishes, and
  only then: no command shows it later. Store it offline: it's the only way to read the card
  if the board is lost or its configuration erased. With it, a Linux PC reads the card
  (`tools/host/sdcrypt.c` decrypts an image of it with the key in a file, and `sdcrypt info`
  prints the `cryptsetup` line to open the card directly). A card whose key wasn't saved can't
  have it shown again; formatting it anew shows a new key (and erases the card).
- **Cards from older firmware** were encrypted under a device key kept in flash, which this
  firmware no longer uses. They are refused with a message: format them again, or read them
  on a PC with their saved key.
- **Speed:** the encryption is software AES (the RP2350 has no AES hardware), using wolfSSL's
  Thumb-2 assembly: about 1.1 MB/s write and 1.0 MB/s read on the card, against 2.0 / 1.7 MB/s
  unencrypted. Copying on the card slows down by about as much; SFTP is limited by SSH's own
  encryption and barely changes.
- A PC sees an encrypted card as unformatted. Don't let it format the card.

## File transfer (SFTP)

The SSH server also serves SFTP on the card, with the same login:

```sh
sftp <user>@<device-ip>            # or scp -s, FileZilla, WinSCP ...
sftp> put photo.jpg
sftp> get notes.txt
```

- Upload, download (resumable: `reput`, `reget`), `ls`, `mkdir`, `rmdir`, `rm`, `rename`. The root
  is the card's root. Changing attributes (`chmod`, times) isn't supported: FAT has no Unix
  permissions.
- About 250–300 KB/s up and 170–200 KB/s down (2 MB files, over Wi-Fi that loses a few percent of
  packets). It depends on the network: lost packets and round trips are what it waits for (see
  `config/lwipopts.h` and the SSH window in `config/user_settings.h`). Each request is handled in
  one pass of the main loop: a 32 KB read reply holds the console and other sessions for up to
  ~0.1 s.
- An upload is synced to the card about once a second while it runs, so `ls` shows how far it
  has got and a reset loses at most the last second of it (see above).
- Each session uses up to ~100 KB of heap while transferring. A transfer's open files count as in
  use, like a running `cp` (see above); a client that disconnects mid-transfer releases them.
- Every change to the card, from SFTP or the file commands, passes one check,
  `files_write_allowed()` in `src/files/files.c`. Everything is allowed except changing `/certs`, which
  only the console may do (see [Trusted root certificates](tls.md#trusted-root-certificates)).
