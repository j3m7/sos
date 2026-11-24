# files: SD card and file commands

The SD card library ([no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico),
`lib/sdcard`) provides the SPI driver and FatFs (exFAT and FAT32, long file names).

## `sdcard.c`: the card

- **Hardware:** `sdcard.c` supplies the library's hardware configuration (`hw_config.h`):
  SPI0 (SCK GP18, MOSI GP19, MISO GP16, CS GP17) at 25 MHz.
- **Mounting:** `sdcard_mount()` mounts the card the first time something needs it: a file
  command, SFTP, or the TLS root certificates.
- **No card detect:** a swapped card isn't noticed, so run `sd unmount` before removing it.
- **`sd` command:** card information, a benchmark, and, on the console only, the SPI clock and
  formatting.
- **File dates:** the library's `get_fattime()` and `time()` read the RP2350's always-on timer,
  which `time/timesync.c` keeps equal to the system clock.

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
