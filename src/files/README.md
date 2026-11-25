# files: SD card and file commands

The SD card library ([no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico),
`lib/sdcard`) provides the SPI driver and FatFs (exFAT and FAT32, long file names).

## `sdcard.c`: the card

- **Hardware:** `sdcard.c` supplies the library's hardware configuration (`hw_config.h`):
  SPI0 (SCK GP18, MOSI GP19, MISO GP16, CS GP17) at 25 MHz.
- **Mounting:** `sdcard_mount()` mounts the card the first time something needs it: a file
  command, SFTP, or the TLS root certificates.
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
  AES-256-GCM under the device key (`core/devkey.c`); the GCM associated data covers the fields
  before the nonce.
- **The layer** sits in `sdcard.c`'s existing block hooks (`card.read_blocks` etc.), so FatFs
  and everything above it don't change. Reads are decrypted in place; writes are encrypted into
  a 4 KB buffer a chunk at a time, since the caller's buffer must stay as it is. The sector
  count FatFs sees is 8192 smaller.
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

`ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `echo > / >>`, in any shell. Each shell has
its own working directory (`shell_t.cwd`).

- **Jobs:** `ls`, `cat` and `cp` run as shell jobs, a bounded amount of work per main-loop pass
  (see `shell/`). Ctrl-C stops them, and a stopped `cp` removes its partial copy.
- **Open files:** `files_hold()`/`files_release()` count the holders of open files: running
  jobs and SFTP transfers. While a file is held it can't be removed or renamed, and the card
  can't be formatted or unmounted.
- **`files_write_allowed(sh, path)`:** every change to the card passes this one check: the
  file commands, SFTP (`ssh/sftp_fs.c`) and Lua's `sos.write` etc. Everything is allowed except
  changes under `/certs`, which need the console (`sh->io->admin`). SFTP and Lua pass no shell
  (`NULL`), so they're refused there.

## `path.c`: paths (pure C, host tested)

- **`path_resolve()`** turns a path typed in a shell into an absolute one, with `.` and `..`
  resolved against the working directory.
- **`path_is_protected()`** decides whether a path is inside `/certs` the way FatFs would read
  it. `\` separates like `/`, names match case-insensitively (ASCII only, as FatFs folds
  them), and trailing spaces and dots are ignored. So `/CERTS.`, `\certs\x` and
  `/docs/../certs/x` are all protected.
- **Fails closed:** a path too long to check, or one with a drive prefix (`:`), counts as
  protected.
