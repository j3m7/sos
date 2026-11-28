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
- **`card_open()`:** the part of a mount before `f_mount`: `disk_initialize(0)` (which
  detects the card's kind on a real initialization), then the retries for a card locked for
  lack of a key or of memory and the detection of a forgotten kind, and the refusal of a
  locked or unsupported card. `mount()` and `sd check` both start with it; afterwards
  `disk_read(0, ...)` reads the volume, decrypted on an encrypted card.
- **`sd check`** ([design](../../docs/design/sd-check.md)): console only, refused while the
  card is mounted or files are held. It sets `checking` for the whole check: `sdcard_mount()`
  and `sdcard_mount_retry()` then return false without touching the card, and the other `sd`
  subcommands refuse. It unregisters FatFs's volume (a failed mount leaves it registered, and
  any `f_*` call would remount from it), opens the card with `card_open()`, allocates the
  checker's state and a 16 KB cluster window (8 KB, then 4 KB, if memory is short) from
  `HEAP_FILES`, and runs as a shell job of about 20 ms a step, reading through
  `disk_read(0)`. `sdcard_owner_gone()` (`config reset`) sets `check_stop`, tested before every
  step. Every end (finished, Ctrl-C, stopped, or refused once started) goes through
  `check_end()`: the memory wiped and freed, `crypt_off()`, `STA_NOINIT`, `checking` cleared,
  `unmounted_by_hand` set, and the last line printed and logged. The job's budget (sector
  reads per `fsck_step()`) doubles or halves to keep a step near 20 ms. Report lines lose
  their control characters on the way to the terminal (names come from the card).
- **`sd check fix`** (design §10.3): the same command and job around `fsck_repair.h`
  (`fsck_repair_state_size()` plus the same window). At `FSCK_PLAN` the step prints the check's
  summary and the plan (`fsck_plan.c`), then asks `yes` with `shell_ask()` and ends the job;
  `checking` stays set and the job's memory is kept (`fix_asking`). `on_fix_confirm()` ends it
  through `check_end()` on anything but `yes` (or after `config reset`), else calls
  `fsck_repair_confirm()` and runs the job again: the repair, then the final check. A question
  the console drops unanswered (it locks itself, which ends what runs on it) is ended by
  `check_orphaned()`, called from the `sd` commands, `sdcard_mount()` and the log writer's
  retry. Writes go one sector at a time through `disk_write(0)`, so through the layer and
  `sd fault`'s hook; the first that fails stops the repair. `check_end()` syncs the driver
  after any write. Its lines are logged as `sd: check fix: ...`.
- **`sd` command:** card information, a benchmark, and, on the console only, the SPI clock,
  formatting (encrypted unless `plain`; an encrypted format shows the card's key once, at the
  end), unmounting, checking and repairing.
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

## `sums.c`: `sha1sum`, `sha256sum`, `sha512sum`

[Design](../../docs/design/archives.md) §4. Its own command table (`sums_commands`, after
`files_commands` in `shell.c`); one `runv` handler for the three, picking the algorithm by
`argv[0]`. A shell job like `cat`: 4 KB read and hashed (wolfCrypt's `wc_Hash*`, on core 0) per
read, for `STEP_US` per pass, one file at a time, holding files (`files_hold()`). The job (two
`FIL`s, the 4 KB read buffer, the list's line buffer, the hash state: about 10 KB) comes from
`HEAP_FILES`. Each output line waits for `shell_write_space()` (up to 1 KB). `-c` reads the list
a block at a time into `sums_parse.c`'s line splitter (not `f_gets()`, whose UTF-8 build fills
fewer bytes than asked), resolves each name against the list's directory, and counts each
result. `tests/test_sums.c` runs it from typed lines over a card in memory, with the host's
wolfCrypt.

## `sums_parse.c`: checksum lists (pure C, host tested and fuzzed)

How a name is shown (GNU's `\` escape for `\n` and `\\`; other control characters as `?`), a
list split into lines of up to 4 KB (`line <n>: too long` beyond), each line parsed (GNU text and
binary, the escape form, BSD with the command's own tag, hex of either case and the right
length), and `sums_name_safe()`: no absolute name, no `..` component (split at `/` and `\` as
FatFs splits, and dots-and-spaces components FatFs may read as `..`), no `:` or control
character. `tests/test_sums_parse.c`; fuzz target `sums`.

## `gzip.c`, `gz.c`, `bigjob.c`: gunzip and zcat

[Design](../../docs/design/archives.md) §5. zlib is the submodule `lib/zlib`, built from the list
in `cmake/zlib.cmake` for both the firmware and the host tests (`Z_SOLO`, `ZLIB_CONST`, SYSTEM
includes, no SOS warnings).

- **`gz.c` (pure C, host tested and fuzzed):** a gzip file's bytes in, the data out, on
  `inflate` (windowBits 15 + 16). It reads the members one after the other, looks at the bytes
  between them itself (another member's `1f 8b`, zero padding, or garbage), and never makes more
  output per call than the caller's buffer holds, whatever the pieces the input comes in. zlib's
  memory (~40 KB: its state and 32 KB window) comes from `gz_zalloc()`, under `HEAP_ZLIB`.
  `tests/test_gz.c` decodes the files GNU gzip made in `tests/data/archives/`
  (`tools/make-archive-fixtures.sh`) in pieces of many sizes; `tests/fuzz/fuzz_gz.c` checks that
  any input decodes the same in one piece and in pieces.
- **`gzip.c`:** the commands, as shell jobs: each step reads 4 KB, inflates into 4 KB, or writes
  that out (gunzip: to `<name>.part`, synced once a second; zcat: as `shell_write_space()`
  allows), until `STEP_US`, at least one of them a step. The .part is renamed into place only
  after `gz_finish()`; an error, Ctrl-C or the session ending deletes it. Refused when the heap
  would keep less than 64 KB free. `tests/test_gzip.c` runs both over a card in memory.
- **`bigjob.c`** (shared with `fetch`): one archive job at a time across the shells.
  `bigjob_try_begin()` returns the running job's name when another holds the slot, and the
  caller prints `BIGJOB_BUSY_FMT` (`busy: <command> is running in another shell`).

## `tar.c`, `tar_parse.c`: tar tf and tar xf

[Design](../../docs/design/archives.md) §6.

- **`tar_parse.c` (pure C, host tested and fuzzed):** an archive's bytes in, in pieces of any
  size, its entries and their data out (`tar_feed()`: header, data pointing into the input, end,
  error). ustar (with its prefix), GNU (`L`/`K` long names, base-256 numbers), pax (`x`: path,
  size, linkpath; `g` skipped) and v7 headers; the checksum unsigned or signed; two zero blocks
  end it. No allocation: long names and pax records go into fixed buffers (`TAR_NAME_MAX`,
  `TAR_PAX_MAX`), and an entry whose name doesn't fit comes with `refused` set rather than a
  cut name. `tar_name_clean()` is §6.3's name rules as a pure function (leading `/`, `..`, `.`
  and empty components, `path_name_check()` on each component). `tests/test_tar_parse.c` reads
  the archives GNU tar, bsdtar and python's tarfile made in `tests/data/archives/tar/`
  (`tools/make-archive-fixtures.sh`, `tools/craft-tar.py`) whole and in pieces;
  `tests/fuzz/fuzz_tar.c` checks that any input reads the same in pieces and that every name
  the rules accept stays under the target.
- **`tar.c`:** the command, as a shell job: each step reads 4 KB, inflates into 4 KB when the
  archive starts with gzip's magic (`gz.c`), feeds the parser, or writes an entry's data to its
  `<name>.part`, until `STEP_US`. Each file is renamed into place by `files_install_part()`
  (`-f`: the old one moved aside first). The joined path is checked again with
  `path_resolve()`, every create goes through `files_write_allowed()`, and the archive itself
  is never a target. Case clashes: the paths this run created are kept as sorted 64-bit hashes;
  when a path exists, the name FatFs has stored for it (a stat per component) is looked up
  there. Dates via `f_utime()` (FatFs built with `FF_USE_CHMOD 1`: `config/fatfs/ffconf.h`).
  `tests/test_tar.c` runs it over a card in memory whose names compare ignoring case.

## `path.c`: paths (pure C, host tested)

- **`path_resolve()`** turns a path typed in a shell into an absolute one, with `.` and `..`
  resolved against the working directory.
- **`path_is_protected()`** decides whether a path is protected (`/certs` and what's in it,
  `/var/log` and what's in it, `/var` itself) the way FatFs would read it. `\` separates like `/`, names match case-insensitively (ASCII only, as FatFs folds
  them), and trailing spaces and dots are ignored. So `/CERTS.`, `\certs\x` and
  `/docs/../certs/x` are all protected.
- **Fails closed:** a path too long to check, or one with a drive prefix (`:`), counts as
  protected.
- **`path_name_check()`** checks one name that came from outside (the name a download's URL
  suggests, each component of a name in a tar archive) before anything is created under it: not empty,
  `.` or `..`, no `/`, `\`, control characters (C1 too) or `"*:<>?|`, not ending in a space
  or a dot (FatFs would drop them), valid UTF-8, at most 255 UTF-16 units.

## `bigjob.c`: one archive job at a time (pure C, host tested)

`fetch`, `gunzip`, `zcat`, `tar`, and later `gzip` ([archives.md](../../docs/design/archives.md)
§5.2), each hold tens of KB of heap while they run, so they take turns across all shells:
`bigjob_try_begin(name)` admits one (NULL) or names the one running, which the command reports
with `BIGJOB_BUSY_FMT` (`busy: <command> is running in another shell`); `bigjob_end()` when
the job ends, however it ends.

## `fsck.c`: checking the card (pure C, host tested)

The checker behind `sd check` ([design](../../docs/design/sd-check.md)), which runs it only
while the card is unmounted (see `sdcard.c` above).

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

## `fsck_repair.c`: repairing the card (pure C, host tested)

The repairs behind `sd check fix` ([design](../../docs/design/sd-check.md) §10), on top of
`fsck.c`. The command (`sdcard.c`) prints its plan and results with `fsck_plan.c`, below.

- **Shape:** a job of checker runs over the same memory (`fsck_repair_state_size()` plus the
  window): a check whose findings are the plan, a wait for the caller's yes
  (`fsck_repair_confirm()`), a repair run that writes at the checker's hook points, and a final
  check. `fsck_repair_step()` takes the same budget as `fsck_step()`, writes counted in it.
  The hooks reach `fsck.c` through a table, so a plain check links without this file.
- **What it repairs:** lost clusters are freed, and chains longer than their file are ended at
  the file's last cluster (their tail is then freed as lost). On FAT32, FSInfo's free count is
  set to unknown before anything else, and the second FAT gets every change after the first
  (and the first's bytes where it differs). On exFAT, freeing clears bits in the allocation
  bitmap, clusters reached but clear in it get their bit set, the end mark is 0xFFFFFFFF, and a
  NoFatChain file's clusters past its run are simply lost clusters. Nothing else is changed.
  Trims are skipped when anything is cross-linked; nothing is freed or trimmed when part of the
  tree couldn't be walked (on exFAT also when a chain breaks on a free or bad entry).
  `fsck_repair.h` lists the rules.
- **Writes:** one sector each, ordered so that stopping after any of them leaves every file
  reading the same bytes and the volume no worse; a failed read or write stops it at once. An
  exFAT bitmap sector's sets and clears go in one write.
- **Tests:** `tests/test_fsck_repair.c` repairs FatFs-made FAT32 and exFAT images damaged by
  `tests/fsck_damage.c`, one kind at a time and in seeded random mixes, and runs the fault
  harness (`tests/fsck_faults.c`) over them: a power cut after every write, every write failing,
  and reads failing. It checks the plan against the damage, what the volume and its files look
  like after every fault and after a full repair, and that a tiny window gives the same result.
  `tests/fuzz/fuzz_fsck.c` repairs mutated images on a copy-on-write disk when the selector's top
  bit is set.

## `fsck_plan.c`: the words of `sd check fix` (pure C, host tested)

What `sd check fix` prints and logs about a plan of `fsck_repair.h`: `fsck_plan_show()` the
lines before the question (what is repaired, what is left alone and why), `fsck_plan_brief()`
the plan on one log line, `fsck_done_line()` what a repair wrote. They follow
`fsck_plan_writes()`'s rules (frees and trims skipped when the tree wasn't all read, trims when
anything is cross-linked). `tests/test_fsck_plan.c` checks the lines.

## `sdfault.c`: injected write faults (test builds only, host tested)

`sd fault fail <N>` and `sd fault reboot <N>`, in images built with `-DSOS_TEST_COMMANDS=ON`
only, for testing repair on a board ([design §10.4](../../docs/design/sd-check.md#104-fault-injection),
usage in [development.md](../../docs/development.md#test-commands-sd-card-write-faults)).
`sdfault.c` counts writes and decides. `write_blocks_fed()` in `sdcard.c`, through which every
card write reaches the driver (FatFs's, the layer's chunks, the header writes), asks it once per
write. It fails the write, or closes the driver's open multi-block write and resets the board
(`fault_test_reset()` in `core/fault.c`, so the next boot says why). In other images, neither
the hook nor the file is built. `tests/test_sdfault.c` covers the counting.
