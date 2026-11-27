# files: SD card and file commands

The SD card library ([no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico),
`lib/sdcard`) provides the SPI driver and FatFs (exFAT and FAT32, long file names). SOS builds
only the part of it the SPI slot needs: the top-level `CMakeLists.txt` lists the sources, leaving
out the SDIO driver and the library's crash handler, and `sd_stubs.c` has the two names the rest
still refers to.

## `sdcard.c`: the card

- **Hardware:** `sdcard.c` supplies the library's hardware configuration (`hw_config.h`):
  SPI0 (SCK GP18, MOSI GP19, MISO GP16, CS GP17) at 25 MHz.
- **Mounting:** `sdcard_mount()` mounts the card the first time something needs it: a file
  command, SFTP, or the TLS root certificates. Tab completion never mounts it
  (`sdcard_mounted()`): it lists directories only once the card is mounted.
- **No card detect:** a swapped card isn't noticed, so run `sd unmount` before removing it.
- **`sd` command:** card information, a benchmark, and, on the console only, the SPI clock,
  formatting (encrypted unless `plain`; an encrypted format shows the card's key once, at the
  end).
- **File dates:** the library's `get_fattime()` and `time()` read the RP2350's always-on timer,
  which `time/timesync.c` keeps equal to the system clock.

## `sdcrypt.c` and the encryption layer: encrypted cards (pure C, host tested)

```
FatFs ──► sdcard.c layer ──► driver          physical sector 0     header (plain)
          (sector n = card sector n + 8192;  physical sector 4096  header backup
           XTS-AES-128 on the way through)   physical 8192 ...     the encrypted volume
```

- **Format** (`files/sdcrypt.h` has the byte layout): each logical sector is encrypted with
  XTS-AES-128 under the card's data key, tweak = the logical sector number, little-endian
  (dm-crypt's `plain64`, so `cryptsetup --type plain --cipher aes-xts-plain64 --key-size 256
  --offset 8192` opens it). The header holds a random card id and the data key encrypted with
  AES-256-GCM under the board's card key; the GCM associated data covers the fields before the
  nonce.
- **The card key** (`cardkey.c`) is HMAC-SHA256 of the configuration's owner secret, keyed
  with a base key derived from the board's root secret (`KEYPROV_SD_WRAP_BASE`, derived at
  boot before the key provider locks and held in RAM). It's computed when a card is opened or
  formatted and wiped afterwards. No owner secret (nothing saved: a board never set up, or after `config
  reset`) means no card key: encrypted cards stay locked, and an encrypted `sd format` makes
  the owner secret and saves the configuration. A header with wrap id 1 (the device key of
  older firmware) is recognised and refused.
- **The layer** sits in `sdcard.c`'s existing block hooks (`card.read_blocks` etc.), so FatFs
  and everything above it don't change. Reads are decrypted in place; writes are encrypted into
  a 4 KB buffer a chunk at a time, since the caller's buffer must stay as it is. The sector
  count FatFs sees is 8192 smaller.
- **Memory:** the layer's state and its 4 KB write buffer (7.5 KB together) are allocated
  from the heap (`files` in `mem`) only while needed: for the moment it takes to look at a
  card's header, and from then on while an encrypted card is in use, until it is unmounted,
  taken out or reformatted. A board with a plain card or none doesn't hold it. If the memory
  isn't there the card can't be looked at: the mount fails saying so and the next mount tries
  again.
- **Detection** (`detect()`) runs after every card initialization (`init_layer` wraps the
  driver's `init`), so the layer always matches the card that's in, and again at mount if the
  kind was forgotten. The rules are in `sdcrypt_card_open()` (host tested): sector 0 a header
  that opens → encrypted; otherwise the backup at 4096 if it's a header, unless sector 0 is a
  boot sector or MBR (0x55AA: a plain card, or a PC formatted it). A backup that opens is
  copied back over sector 0 once the volume has mounted through it (not before: a card reused
  elsewhere can keep an old backup). A header the key doesn't open, or of an unknown version, makes the
  card *locked*: every read and write is refused (logged once per initialization, told to the
  shell at each try) and `sd format` is the way out. `sd unmount` forgets the key.
- **`sd format`** writes a new header (both copies) and turns the layer on before `f_mkfs`, so
  the volume is written encrypted. `sd format plain` also clears the backup header, which
  `f_mkfs` doesn't overwrite, so the card can't look encrypted again.
- **Speed:** AES is wolfSSL's Thumb-2 assembly (the inline version, `thumb2-aes-asm_c.c`,
  enabled for `aes.c` in `CMakeLists.txt`; issue #59): about 1.1 / 1.0 MB/s encrypted write and
  read. Its two 1 KB tables stay in flash, and the XIP cache holds them. wolfCrypt's C AES
  managed about 690 / 650 KB/s with its 8.4 KB of tables in SRAM, and a third of that with them
  in flash, where varied data kept missing the XIP cache.
- **Old data:** `f_mkfs` writes only the filesystem's structures, so formatting a used plain
  card encrypted leaves its old files readable in the clear until they're overwritten.
- **Host tool** `tools/host/sdcrypt.c` (OpenSSL's libcrypto) reads a card or image on a PC with
  the key shown when the card was formatted, kept in a file; `tests/test_sdcrypt.c` checks the device code against
  it and decrypts a test image with it.

## `files.c`: the file commands

`ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `echo`, in any shell (`> file` is the
shell's redirect, a quiet `tee`: `pipe_file_*()` here). Each shell has
its own working directory (`shell_t.cwd`).

- **Arguments:** the path commands take their words (`runv`, shell.h), each path one word, so a
  quoted or escaped name may hold spaces (`cat 'my file'`, `cp a\ b c`). `path_args()` checks
  the count (an empty word counts as missing) and prints the command's usage line;
  `resolve()` makes each word absolute against the working directory. `pwd` and `echo` take the
  joined text (`run`). `tests/test_files.c` runs them from typed lines over a card in memory.

- **Jobs:** `ls`, `cat` and `cp` run as shell jobs, a bounded amount of work per main-loop pass
  (see `shell/`). Ctrl-C stops them, and a stopped `cp` removes its partial copy.
- **Open files:** `files_hold()`/`files_release()` count the holders of open files: running
  jobs and SFTP transfers. While a file is held it can't be removed or renamed, and the card
  can't be formatted or unmounted.
- **`files_write_allowed(sh, path)`:** every change to the card passes this one check: the
  file commands, SFTP (`ssh/sftp_fs.c`) and Lua's `sos.write` etc. Everything is allowed except
  changes under `/certs` (TLS roots) and `/var/log` (the log files, #104), and to `/var`
  itself (renaming it would take `/var/log` along), which need the console
  (`sh->io->admin`). SFTP and Lua pass no shell (`NULL`), so they're refused there. The log
  writer itself writes through FatFs directly.

## `path.c`: paths (pure C, host tested)

- **`path_resolve()`** turns a path typed in a shell into an absolute one, with `.` and `..`
  resolved against the working directory.
- **`path_is_protected()`** decides whether a path is protected (`/certs` and what's in it,
  `/var/log` and what's in it, `/var` itself) the way FatFs would read it. `\` separates like `/`, names match case-insensitively (ASCII only, as FatFs folds
  them), and trailing spaces and dots are ignored. So `/CERTS.`, `\certs\x` and
  `/docs/../certs/x` are all protected.
- **Fails closed:** a path too long to check, or one with a drive prefix (`:`), counts as
  protected.

## `fsck.c`: checking the card (pure C, host tested)

The checker behind the coming `sd check` ([design](../../docs/design/sd-check.md)). Not in the
firmware yet: phase 3 adds the command, which runs it only while the card is unmounted.

- **Shape:** it reads sectors through a callback and hands back report lines, so it knows
  nothing of SPI, FatFs or the shell. All its memory is a buffer the caller gives it (about
  7 KB plus the cluster window), and `fsck_step()` does at most a given number of sector
  reads, so a check can run as a shell job a slice at a time.
- **What it checks, FAT32:** the volume is found and its boot sector judged as FatFs finds and
  mounts it; then the backup boot sector, FSInfo, the FAT copies, every chain (range, loops,
  end mark, size against chain), the directory tree (depth, loops, "." and "..", long-name
  sequences, names FatFs refuses, repeated names), and the clusters: cross-linked, lost, bad.
  `fsck.h` lists the rules the design note left open.
- **exFAT:** the same walk, with `fsck_exfat.c`'s pieces: the boot regions, every entry set,
  the up-case table, NoFatChain runs as well as FAT chains, and the clusters reached against
  the allocation bitmap (lost, reached but free). Names are hashed with the `upcase` function
  the caller passes (FatFs's `ff_wtoupper()` on the board). One entry set reader serves the
  whole walk: coming back from a subdirectory, a fresh one picks the parent up after the
  directory's set.
- **Windows:** a bit per cluster for a whole card doesn't fit beside everything else, so the
  clusters are accounted a window at a time, one pass over the tree per window. The report is
  the same whatever the window size; the tests check that.
- **Tests:** `tests/test_fsck.c` builds FatFs for the host over a RAM disk and makes its images
  with `f_mkfs` as `sd format` does (FAT32 and exFAT; MBR, no partition table, two FATs, 128 GB
  GPT images held sparse), then breaks one thing per case (exFAT's with
  `tests/fuzz/exfat_damage.h`) and expects the exact lines. `tests/fuzz/fuzz_fsck.c` checks
  mutated images of both kinds twice with different windows and requires the same answer.

## `fsck_exfat.c`: the card checker's exFAT parts (pure C, host tested)

The exFAT pieces of `sd check` ([design note](../../docs/design/sd-check.md) §4, §5, §6):
`fsck.c`'s walk calls them entry by entry and sector by sector.
No I/O, no malloc; each piece is a small state struct in the caller's memory, and problems come
back as bits with `*_text()` lines. `fsck_exfat.h` lists the rules decided here.

- **Boot region:** `fsck_exfat_vbr()` refuses what FatFs's `mount_volume()` refuses, plus what
  the checker can't read past; `fsck_exfat_boot_*()` take sectors 0-23 once each and check the
  boot checksum and the backup region (reported, not refused).
- **Entry sets:** `fsck_exfat_entry()` takes 32-byte entries and hands back finished sets
  (name, directory flag, first cluster, length, NoFatChain, cluster count) with their problems,
  checked as FatFs's `load_xdir()` reads them; the root's bitmap, up-case and label entries
  too.
- **Name hash:** computed with FatFs's `ff_wtoupper()`, passed in as a function, since FatFs
  finds names by that hash and never reads the volume's up-case table. The table is streamed
  (`fsck_exfat_upcase_*()`, no 128 KB copy): its checksum is checked, and mappings that differ
  from FatFs's are a note.
- **Cluster accounting:** `fsck_window_mark_run()` marks runs into the window and counts
  cross-links; `fsck_bitcmp_*()` give the lost and reached-but-free runs against the bitmap,
  a slice at a time, runs going on across slices and windows.
- **Tests:** `tests/test_fsck_exfat.c` tests the pieces on their own, on images FatFs makes and
  fills and on hand-made entries; `fuzz_fsck_exfat` fuzzes the parsers. The damage cases
  (`tests/fuzz/exfat_damage.h`) run through the whole checker in `tests/test_fsck.c`.
