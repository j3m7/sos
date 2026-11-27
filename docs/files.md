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
| `echo <text>` | print the text; `> file` / `>> file` after any command (the shell's redirect) writes / appends its output to a file |
| `sd` | card and volume information, whether it's encrypted, SPI clock |
| `sd format [exfat\|fat32] [plain]` | console only: erase the card and format it (one partition), encrypted unless `plain`; an encrypted format shows the card's key once, for offline recovery |
| `sd test [KB [keep]]`, `sd speed [MHz]` | console only: write/read benchmark (everything else waits meanwhile); SPI clock |
| `sd unmount` | console only: unmount the card, to remove it |
| `sd check` | console only, on an unmounted card: check its file system, reporting problems without writing (FAT32 only for now) |

- **Long commands run in steps.** `ls`, `cat` and `cp` do a little work per pass of the main
  loop, so SSH sessions, the console and the watchdog keep going; Ctrl-C stops them (a stopped
  `cp` removes its partial copy). Commands typed meanwhile run afterwards.
- **A slow SSH client** slows `cat` down instead of losing output. Printing a file over SSH runs
  at about 130 KB/s (encryption-bound); copying on an unencrypted card at about 580 KB/s.
- **Open files are protected:** a file that a running command has open can't be deleted or
  renamed, and the card can't be formatted or unmounted meanwhile.
- A file name with spaces is quoted or escaped, as in a Unix shell: `cat 'my file'`,
  `cp my\ file backup` (QUOTING in `man intro`). Tab doesn't offer such names yet. Files are
  dated with the system clock (UTC), which comes from the RTC or NTS.
- No card-detect switch is wired by default (solder jumper SJ1 connects it to GP15), so SOS
  doesn't notice a card swap: run `sd unmount` before taking the card out; the next file command
  initializes and mounts the new card.

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
there's no card, an `http` request has no TLS roots, the log writer keeps its lines in memory,
and the other `sd` commands say `the card is being checked (sd check)`. It prints one line per
problem and a summary, which also goes to the log (`sd: check: ...`). It reads only; repair is
a later command. Ctrl-C stops it. When it ends the card is left as `sd unmount` leaves it: the
next file command mounts it again, and the log writer's once-a-minute retry doesn't.

The output looks like this (design: [docs/design/sd-check.md](design/sd-check.md)):

```
checking FAT32, 29.7 GB, 970752 clusters of 32 KB, encrypted (8 passes)
pass 1/8
/var/log/messages: 3 clusters for 70000 bytes, chain has 2
pass 2/8
...
pass 8/8
1 problem; 0 bytes lost, 0 cross-linked
```

Lines starting `note:` (a stale FSInfo free count, a chain longer than its file, the volume's
dirty flag) are notes, not problems.

A card has about a million clusters, too many to track at once beside Wi-Fi, TLS and SSH, so
the clusters are checked a window at a time, one pass over the directory tree per window: 16 KB
of memory gives 8 passes on a 32 GB card, and less memory more passes (it needs about 10 KB at
least). Only FAT32 is checked so far: an exFAT card (what `sd format` makes by default) ends with
`not checked: exFAT is not checked yet`.

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
- Each session uses up to ~100 KB of heap while transferring. A transfer's open files count as in
  use, like a running `cp` (see above); a client that disconnects mid-transfer releases them.
- Every change to the card, from SFTP or the file commands, passes one check,
  `files_write_allowed()` in `src/files/files.c`. Everything is allowed except changing `/certs`, which
  only the console may do (see [Trusted root certificates](tls.md#trusted-root-certificates)).
