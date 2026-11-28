# Build features: design note for turning subsystems on and off with CMake

Question (the user's, 2026-11-27): **can SOS be built with features switched on or off at
compile time** (the editor, Lua, SSH and so on), so that it can serve as a base for other
projects and stripped-down images can exist?

Short answer: **yes, and most of the work is in C, not CMake.** The sources are already split
one directory per subsystem, and the build already has three on/off switches
(`SOS_BURN_IMAGE`, `SOS_TEST_COMMANDS`, `SOS_FORCE_CONSOLE_ONLY`). What is in the way is that
four places know every subsystem by name: the command table in `shell/shell.c` (which also
holds several subsystems' commands), the health check table in `core/health_board.c` (whose
checks read several subsystems' state), the boot sequence and main loop in `main.c`, and a
couple of dozen direct calls from one subsystem into another. Once those go through a small
number of seams, the CMake side is a list of options and a source list per feature.

Status (2026-11-28): design, reviewed by Reviewer on #250. **Refreshed against main e3acb48**,
after the health checks (#233), time zones (#240), Lua features (#208), sd check (#213) and most
of the file tools (#265) landed: §1, §3, §4 and §10 rewritten, decision 10 and §4.5 added.
Work starts once the open work has landed (the user, 2026-11-27); §10 says what is still open
and when phase 1 can begin. Decided by the user: the security core stays fixed (decision 6),
and the plain Pico 2, without the Wi-Fi chip, is a target (decision 9, §4.8). Where this note
and the code differ in detail, the code is the reference.

## 1. What exists today

### Build

`CMakeLists.txt` builds one executable, `sos`, from one list (`SOS_SOURCES`, 122 files), plus
the third-party code compiled into the same target: wolfSSL and wolfSSH
(`cmake/wolfssl_pico.cmake`, `cmake/wolfssh_pico.cmake`), the copied SSH server (`sos-ssh`),
FatFs and the SD driver, Lua, and zlib (`cmake/zlib.cmake`, since #271). Third-party settings
live in `config/`: `user_settings.h` (wolfSSL, wolfSSH), `lwipopts.h`, `cyw43_sos.h` (the
Wi-Fi driver's hooks for the `wifi` health check, `CYW43_CONFIG_FILE`, #268), and
`fatfs/ffconf.h` (`FF_USE_CHMOD`, coming with #274). Warnings are set per source file with
`set_source_files_properties()`. The existing switches add sources and a compile definition,
which is the pattern this note generalises.

Since #250 also: every image is hashed (`pico_hash_binary`, unconditional since #262: the
`image` health check compares against it), and the end of flash holds six 4 KB sectors
(`core/flashlayout.h`, #255: keys, configuration A and B, root secret, wrong-password count,
reset and RTC history).

### Who depends on whom

Includes between subsystem directories on main e3acb48, `core` and `shell` left out as targets
(everyone uses them):

| From | Includes from |
|---|---|
| `shell` | **every other subsystem**: chat, edit, files, lua, man, net, ssh, time, tls |
| `main.c` | chat, files, lua, net, ssh, time, tls |
| `core` | **files, lua, net, ssh, time, tls**: almost all from the health checks (below) |
| `lua` | chat (`wall` from a script), files, net (http, ping, wifi), ssh and tls (`sos.health` and the `sos.ssh`/`sos.tls` tables), time |
| `man` | edit (pager), files, net (http, for `man install`) |
| `net` | files (`fetch.c` writes to the card), time, tls (HTTPS, and plain TCP through the TLS slots) |
| `time` | files and net (`tzzone.c`: the time zone database on the card, and downloading it), tls (NTS) |
| `tls` | files (`certstore.c`: approved roots on the card), time |
| `ssh` | chat, files |
| `edit` | files |
| `chat`, `files` | (shell only) |

`core`'s row is new since #250, and is the main change. Before, only `logfile_board.c` (files,
time) and `health_board.c` (Lua's stack peak) looked outward. Now the health checks do:

| Core file | Reaches into | For |
|---|---|---|
| `health.h`, `health.c` | lua (`lua_info.h`) | the `sos.health()` table |
| `health_board.c` | lua | core 1's stack peak (`stack` check) |
| `statecheck_board.c` | ssh, files, tls | `hostkey` check; `card` check (and its text names approved roots) |
| `countcheck_board.c` | net, files, lwIP | `wifi`, `sd-bus`, `net-pools` checks |
| `bgcheck_board.c` | lua, time | `core1` and `heap-sum` (the running Lua job), `rtc-drift` |
| `fullcheck.h`, `fullcheck_board.c` | files (`fsck.h`, in the header) | `sd-check` check |
| `logfile_board.c`, `history_board.c` | files, time | `/var/log`; the reset history's times |

Everything depending on `shell` is fine: the shell is the base. The arrows that matter are the
ones **out of** `shell`, `main.c` and `core`, because they make the base depend on the features.

### Where the bytes are

From the map of a debug build of main e3acb48 (2026-11-27, unsigned): **1,275 KB of flash**
(1,150 KB at #250), **114 KB of static RAM**. The string literals of the whole image (115 KB)
are merged into one section and not counted per part below, so every figure is a lower bound.

| Part | Flash | Static RAM | Notes |
|---|---|---|---|
| wolfSSL | 331 KB | 0.3 KB | SP math, Ed25519, ASN.1, the TLS layer (about 55 KB), ML-KEM |
| CYW43 driver and firmware | 235 KB | 2.6 KB | Wi-Fi chip; also drives the LED on the Pico 2 W |
| Lua (library 92 KB + `src/lua` 34 KB) | 125 KB | 2 KB | plus the script heap and core 1 |
| wolfSSH + `src/ssh` | 68 KB | 7 KB | plus about 23 KB of heap per session slot (3 slots) |
| lwIP + `src/net` | 57 KB | 56 KB | most of the RAM is lwIP's buffers |
| FatFs, SD driver | 37 KB | 0.7 KB | |
| `src/files` | 47 KB | 1 KB | `sd check` 24 KB, file tools (sums, gzip) 7 KB, the rest 16 KB |
| `src/core` | 52 KB | 24 KB | health checks 27 KB, the rest 25 KB |
| `src/shell` | 35 KB | 0.5 KB | |
| `src/time` | 23 KB | 4 KB | NTS 7 KB, time zones 6 KB |
| `src/edit`, `src/man`, `src/chat`, `src/tls` | 29 KB | 8 KB | |
| zlib | 11 KB | 0 | inflate; deflate comes with #273 |

Flash is 4 MB, so size alone is not pressing on this board. The reasons that are: **RAM** (no
network frees about 56 KB static and 69 KB of SSH session heap), **attack surface** (an image
without the network stack, the SSH server and the TLS client cannot be reached over the air),
**boot and audit scope**, and **boards without the Wi-Fi chip**.

## 2. Decisions

1. **One CMake option per feature, `SOS_FEATURE_<NAME>`, all ON by default.** The default
   build stays exactly today's image; nobody's build changes until they turn something off.
2. **A feature whose dependency is off is a configure error, not a silent drop.**
   `-DSOS_FEATURE_NET=OFF` with SSH still on stops with "SOS_FEATURE_SSH needs SOS_FEATURE_NET;
   turn it off too". An image never loses something its builder didn't ask to lose. Presets
   (§6) make the common combinations one word.
3. **Parts that only glue two features follow them automatically and have no option of their
   own.** Lua's `http` table exists when both LUA and HTTP do; SFTP when SSH and FILES do;
   `man install` when MAN and HTTP do.
4. **C code sees `SOS_HAVE_<NAME>` defined to 0 or 1, on every source of the target**, and tests
   it with `#if`, never `#ifdef`. Always defined, so `#if SOS_HAVE_SHH` (a typo) is caught by
   `-Wundef`, which the build already uses. Third-party sources compiled into the target see
   the same definitions, so `config/user_settings.h` can use them (§5).
5. **The stored formats do not change with features.** The configuration record keeps every
   field (Wi-Fi, SSH keys, NTS server...) in every build; an absent feature ignores its fields
   and `config show` leaves them out. Likewise the flash layout (`core/flashlayout.h`) keeps
   all six sectors, the SSH host key's included, in every build. A board can move between a
   full and a stripped image without losing or reformatting anything, and there is one format
   to test, seal and provision.
6. **The security core is not optional** (decided by the user, 2026-11-27): the root secret and
   key provider, sealed configuration, the console password, the health framework and its
   gates, the watchdog, the fault record and reset history. These are what SOS is. A project
   that wants less security should fork; making it a switch invites shipping the wrong one.
   (`SOS_FORCE_CONSOLE_ONLY` and `SOS_BURN_IMAGE` stay as they are.)
7. **`main.c` stays explicit.** Boot order is security-relevant (`health_boot()` before any key,
   every key derived before `keyprov_lock()`), so it remains one readable sequence with
   `#if SOS_HAVE_...` around the optional steps, not a module registry that hides the order.
8. **Commands belong to their features; their order belongs to the shell.** Each feature
   holds its commands' code and word lists. The main command table stays one ordered list,
   `commands[]` in `shell/shell.c`, with each feature's entries fenced by `#if`, because Tab offers
   commands in table order and that order is chosen, not accidental (§4.1). The tables that
   already exist (`files_commands[]`, `sums_commands[]`, `gzip_commands[]`) stay as they are.
9. **The plain Pico 2 is a target** (decided by the user, 2026-11-27), alongside the Pico 2 W.
   Same RP2350 chip, so the same security core (OTP, secure boot, glitch detector); the board
   just has no Wi-Fi chip, so its images are built with NET off. Board differences live in
   `*_board.c` files and a small `board.h`, not in `#if PICO_BOARD` scattered through the code
   (§4.8).
10. **Health checks belong to their features, the framework to the core.** The framework, the
    gates and the checks of the core itself (`trng`, `crypto`, `flash`, `config`,
    `rootsecret`, `otp`, `ram`, `image`...) are part of the security core and always built. A
    check that reads one feature's state (`hostkey`, `wifi`, `card`...) moves to that feature
    and is listed only when it is built (§4.3). A feature that is off has no check: `health`
    doesn't show it as passing or failing, and the features line (§7) says why.

## 3. Features and their dependencies

| Option | What it contains | Needs |
|---|---|---|
| `NET` | Wi-Fi (`net/wifi*`, `wifistat.c`), lwIP, CYW43, `net`, `wifi`, `ping`, SNTP; the `wifi` and `net-pools` checks | |
| `TLS` | `tls/`: TLS 1.3 client, built-in roots, `tls` command | NET |
| `HTTP` | `net/http*`, `http` and `fetch` (`net/fetch.c`) | TLS (plain HTTP also goes through the TLS slots) |
| `NTS` | `time/nts*`, `nts` command, `time server` | TLS |
| `SSH` | `ssh/`, wolfSSH, host key, `ssh` command, SSH keys in `setup`; the `hostkey` and `crypto-ssh` checks | NET |
| `CHAT` | `chat/`: `who`, `wall`, `chat` | |
| `LUA` | `lua/`, the Lua library, core 1; the `core1` check | |
| `PAGER` | `more`, `\| more` (`edit/pager.c`, `view.c`, `screen.c`) | |
| `EDITOR` | `edit` (`edit/editor.c`, `edit.c`) | PAGER (shares the screen code), FILES |
| `MAN` | `man/`, man pages on the card | PAGER, FILES |
| `FSCK` | `sd check` and its repair (`files/fsck*`, about 3,700 lines); the `sd-check` check | FILES |
| `ARCHIVE` | `gzip`, `gunzip`, `zcat` (`files/gz*.c`), `tar` (#274, #277), zlib | FILES |
| `FILES` | SD card, FatFs, file commands, `sha*sum`, `/var/log`, encrypted cards; the `card` and `sd-bus` checks | later: see §4.7 |

Not options: the time zones, the checksum commands (`files/sums*.c`, under 7 KB together with gzip's glue, and
the hashes are in the core anyway), and everything in decision 6. The time zones split in two: the zone's
rule is saved in the configuration (`tz_name` and its POSIX rule), and `time/tzrule.c`, which
turns it into local time, needs nothing from `files/`, so local time is shown with the card out:
that part is core. Only `time zone <name>` and `time zone list` read the database on the card
(`time/tzzone.c`): FILES glue.

Glue that follows its parents (decision 3): SFTP (SSH + FILES), approved roots on the card
(TLS + FILES), `fetch` to a file (HTTP + FILES), choosing and listing time zones from the
database on the card (FILES), downloading the database (`time zone install`, HTTP + FILES), `man install` (MAN + HTTP), and in Lua: `sos.http` (LUA + HTTP), `sos.wifi` and
`sos.net` (LUA + NET), `sos.ssh` (LUA + SSH), `sos.tls` (LUA + TLS), `sos.sd` and the file
functions (LUA + FILES), `wall` from a script (LUA + CHAT).

Turning off NET turns off, by the rules above, TLS, HTTP, NTS and SSH: everything that faces
the air. That one switch is most of the size and RAM gain (about 450 KB of flash before
trimming wolfSSL, §5, and lwIP's 56 KB of buffers).

## 4. Shape of the code

### 4.1 Commands

Today `shell/shell.c` (2,725 lines) holds the main `commands[]` table, and the bodies of many
commands that belong elsewhere: `cmd_wifi` (about 85 lines, more with its helpers),
`cmd_time` and its `auto`/`cal`/`server` parts, `cmd_tls`, `cmd_nts`, `cmd_net`, the Wi-Fi and
SSH steps of `setup`, `run_ssh`. Three subsystems already export their own tables:
`command_tables[]` is `{commands, files_commands, sums_commands, gzip_commands}`, and the tar
PRs may add more. `fetch` has its body in `net/fetch.c` but its entry in `commands[]`. The
change:

- Move each subsystem's command bodies and word lists into that subsystem
  (`net/wifi_cmd.c`, `time/time_cmd.c`, `tls/tls_cmd.c`, `ssh/ssh_cmd.c`...), exported through
  the subsystem's header (`cmd_wifi()`, `wifi_words[]`). Worth doing even without features: it
  shrinks `shell.c` and puts a command next to the code it drives.
- The `commands[]` table stays where it is, at the end of `shell/shell.c`: same entries, same
  order, now naming the subsystems' handlers. Tab completion (`complete.c`) offers commands in
  table order, so splitting it into one table per feature would reorder what Tab lists.
  Its home is `shell.c` for now. Moving it to a file of its own, `shell/commands.c`, was the
  first plan; it would have meant exporting about forty of the core's own handlers for nothing,
  so it moves only if a later phase has a reason to. Later phases fence entries in place:

  ```c
  static const shell_cmd_t commands[] = {
      {"help", run_help, NULL, NULL, 0},
      ...
  #if SOS_HAVE_NTS
      {"nts", cmd_nts, NULL, nts_words, 0},
  #endif
      {"mem", run_mem, NULL, NULL, 0},
  #if SOS_HAVE_NET
      {"net", run_net, NULL, NULL, 0},
      {"ping", ping_command, NULL, SHELL_VALUES_OF(ping_hosts), 0},
  #endif
      ...
  };
  const shell_cmd_t *const command_tables[] = {commands, files_commands, sums_commands,
  #if SOS_HAVE_ARCHIVE
      gzip_commands,
  #endif
      NULL};
  ```

  Dispatch, Tab completion and `shell_command_tables()` follow with no change. A
  linker-section registry (each table placing itself) was considered and rejected: it needs the
  linker scripts SOS already overrides, its order would be the link order, and the explicit
  list is easier to read.
- `help` is text written out in `cmd_help()`, not generated from the table, so it is fenced
  the same way: each feature's help lines go under its `#if`. (`files_help()` and
  `gzip_help()` already live with their subsystems.)
- **Phase 1 is a pure move** (§10): no behaviour change, the table's order the same, `help`'s
  text identical byte for byte, `docs/commands.md` regenerated identical, every command still
  dispatching, and Tab's candidates for an empty line (console and SSH, which differ by
  `SHELL_ADMIN`) identical before and after. That makes its review a mechanical check. (`shell.c`
  can't be built for the host, so Tab is compared on the board, §4.1.1.)

#### 4.1.1 Phase 1 as built

Moved, as exact line ranges of `shell.c` (a script cut them; nothing retyped):

| New home | What | Exported |
|---|---|---|
| `net/wifi_cmd.c` | `wifi` (with `wifi stats`, the SSID question and its Tab, `on_off` words), `net` | `wifi_command()`, `net_command()`, `wifi_words[]`, `wifi_show()` (for `config`), `wifi_ask_ssid()` (for `setup`) |
| `net/ping.c` | Tab's hosts for `ping` | `ping_hosts` |
| `time/time_cmd.c` | `time` (`auto`, `cal`, `server`, and `zone`'s values), `nts` | `time_command()`, `nts_command()`, `time_words[]`, `nts_words[]`, `time_nts_servers()` (for `ping`'s Tab) |
| `tls/tls_cmd.c` | `tls` | `tls_command()`, `tls_words[]` |
| `ssh/ssh_cmd.c` | `key`, `ssh` | `key_command()`, `ssh_command()`, `key_words[]`, `key_add_line()` (for `setup`) |

The only edits to moved lines: `require_admin(sh)` and `save(sh)` became `shell_require_admin()`
and `shell_save_settings()` (shell.h already exported them for this), `static` came off what is
exported, and exported functions got their subsystem's prefix. The two lines of comment that
introduced `nts` sat above `cmd_tls()`; they moved with `nts`. Still in `shell.c`, for phase 2:
`setup`'s Wi-Fi and key steps (they call `wifi_ask_ssid()` and `key_add_line()`), `config reset`'s
owner-gone calls, and `mem`'s lwIP lines.

Checks, before (main 0f2f664) and after:

- The images are the same size: text 1,338,504 bytes, bss 116,768, in both.
- Their `.rodata` strings are the same apart from the commit hash.
- Every line removed from `shell.c` is added in a new file, after undoing the edits above; the
  only other added lines are file headers, includes and the word-list macros.
- `cmd_help()` is byte for byte the same; the table's entries are in the same order, with the
  moved handlers' new names.
- Host tests 95/95, no warnings (firmware or tests), `just tidy` and `just lint` clean.
- On dev-1: the full image boots, and every command's Tab candidates for an empty line and for
  each command's words are the same as main's, on the console and over SSH (the PR shows the
  capture).
- `setup` asks its Wi-Fi and SSH questions only when those features exist; the steps move to
  their subsystems as functions `setup` calls.
- `mem` stays in the core but leaves out the lines of features that are off (lwIP's pools,
  core 1's stack).

### 4.2 Boot and main loop

`main.c` keeps its order, with the optional steps fenced:

```c
#if SOS_HAVE_SSH
    hostkey_init();
    hostkey_get(&hostkey, &hostkey_len);
#endif
    cardkey_init();
#if SOS_HAVE_NET
    if (cyw43_arch_init_with_country(wifi_country_code())) { ... }
    wifi_start();
#endif
    keyprov_lock();
```

About eight such blocks. The LED blink goes through `board_led_set()` (§4.8): the Wi-Fi
chip's GPIO on the Pico 2 W, GPIO 25 on the Pico 2.

One call is misplaced for this: `tls_init()` runs before `wolfCrypt_Init()` and only installs
wolfSSL's counting allocators (`wolfSSL_SetAllocators(count_malloc, count_free,
count_realloc)`). That hook counts every wolfCrypt allocation, not only TLS's: the core's own
users go through it too (sealing, the boot known-answer tests, `sos.hash` on core 0, making the
host key). A build without TLS still needs the hook and the counting functions, so they move
to the core with the heap tags. Their counts are tagged `HEAP_NET` today; in a build without
NET that name is wrong, so the tag is renamed (`HEAP_CRYPTO`, say) or split between crypto and
the network.

### 4.3 Health checks

`core/health_board.c` has one table, `checks[]`, of 26 checks in a fixed order with staggered
first runs. Under decision 10 it splits in the same way as the commands:

| Check | Belongs to | Its read step today |
|---|---|---|
| `hostkey` | SSH | `statecheck_board.c` |
| `crypto-ssh` | SSH or TLS (see below) | `health_board.c`, known answers in `selftest.c` |
| `wifi`, `net-pools` | NET | `countcheck_board.c` |
| `card`, `sd-bus` | FILES | `statecheck_board.c`, `countcheck_board.c` |
| `sd-check` | FSCK | `fullcheck_board.c` |
| `core1` | LUA | `bgcheck_board.c` |
| all others | core | unchanged |

- Each feature's read steps move to that feature (`ssh/hostkey_health.c`,
  `net/wifi_health.c`, `files/card_health.c`...). The judging halves (`statecheck.c`,
  `countcheck.c`, `bgcheck.c`: pure C, host tested) can stay where they are or move with them;
  they include nothing from the features.
- The table stays one table with `#if` around the feature checks, in `health_board.c`, as
  §4.1 does for commands: the order and the staggered first runs are tuned together and are
  easiest to read in one place. (`health_add()` exists and could let features register
  themselves, but the order would then follow the boot order by accident.)
- **The crypto known answers follow the algorithms.** `selftest.c` tests Ed25519 (public key,
  sign, verify) and X25519 in the `crypto-ssh` check. Ed25519 is used only by SSH (host key,
  logins); X25519 by SSH and by TLS's key exchange. So the Ed25519 tests are built with SSH,
  X25519 with SSH or TLS, and `crypto-ssh` (renamed `crypto-keyx` or similar) exists when
  either does. Without both, wolfSSL trimming (§5) can drop the code and its tests together.
- `heap-sum` names the running Lua job and `stack` reads core 1's stack peak: with LUA off they
  use the stand-ins in §4.4 and report core 0 alone.
- `statecheck_board.c`'s `card` text names approved roots (`certstore`): that part follows
  TLS + FILES.
- `fullcheck.h` includes `files/fsck.h` for a type; the type moves out of the header, so that
  `core`'s headers include nothing from features.

### 4.4 Calls from one feature into another

For the calls that cross from the base into a feature, the feature's header provides an inline
stand-in when the feature is off, so callers keep no `#if`:

```c
// chat/chat.h
#if SOS_HAVE_CHAT
void chat_deliver(shell_t *sh);
#else
static inline void chat_deliver(shell_t *sh) { (void)sh; }
#endif
```

The list should stay short; each stand-in has an obvious "nothing" answer:

| Call | Callers outside the feature | Stand-in when off |
|---|---|---|
| `wifi_up()` | main loop, `shell.c`, `lua_health.c` | `false` |
| `chat_register()`, `chat_deliver()`, `chat_name()`, `chat_peers()`, `chat_wall_from()` | main loop, `shell.c`, `ssh/`, Lua | no-op, `""`, 0 |
| `shell_page()`, `shell_page_growing()` (pager) | `shell_core.c` | print the text to the terminal, as without `\| more` |
| `sshd_poll()`, `sshd_running()`, `sshd_info()` | main loop, `lua_health.c` | no-op, `false` |
| `lua_host_poll()`, `lua_core1_stack_peak()`, the running job (`lua_job`) | main loop, `shell.c` (`mem`), `health_board.c`, `bgcheck_board.c` | no-op, 0, none |
| `timesync_*` | `logfile_board.c`, `history_board.c`, `bgcheck_board.c`, `tls/`, Lua | stays: time is not optional |

Anything larger than a stand-in belongs inside the feature. Two specific inversions:

- **`config reset` calls `sdcard_owner_gone()` and `hostkey_owner_gone()` by name**
  (`shell.c`). It becomes a list of "owner gone" hooks that the features add themselves to at
  init.
- **`core/health.c` writes a Lua table** (`health_lua_table()`), and `core/health.h` includes
  `lua/lua_info.h` for its writer type, so every file that includes `health.h` pulls in a Lua
  header. The function moves to `lua/lua_health.c`, which already exists, reading the results
  through a plain iterator in `health.h`; `lua_info_t` leaves `health.h` with it, and `core`
  no longer includes from `lua`.

### 4.5 Lua

`lua_core1.c` registers every binding table in one function: `sos`, and under it `sos.http`,
`sos.wifi`, `sos.sd`, `sos.ssh`, `sos.tls`, `sos.net`, plus `base64` and `json`.
`lua_health.c` builds `sos.health()` and the status tables, reading Wi-Fi, SSH, TLS roots, the
card and chat peers. The tables for features move to their own files (`lua_bind_http.c`...),
compiled only when both parents exist (decision 3), and the registration lists them with `#if`,
as §4.1 does for commands; `lua_health.c` leaves out the fields of features that are off. The
file functions (`sos.read`, `sos.write`, `sos.list`...) are entries in `sos_lib` itself, so
they move to a second array added to the `sos` table. A script that calls a missing table gets
Lua's usual "attempt to index a nil value"; `sos.features()` (a table of the `SOS_HAVE_` values)
lets a script check first.

### 4.6 CMake

```
cmake/features.cmake      options, dependency checks, SOS_HAVE_ definitions, summary
src/<subsystem>/sources.cmake   that subsystem's source list, per feature
```

`features.cmake` defines a helper so each rule is one line:

```cmake
sos_feature(NET     "Wi-Fi, lwIP, ping, SNTP")
sos_feature(TLS     "TLS 1.3 client" NEEDS NET)
sos_feature(SSH     "SSH server" NEEDS NET)
sos_feature(EDITOR  "Full-screen editor" NEEDS PAGER FILES)
sos_feature(ARCHIVE "gzip, gunzip, zcat, tar" NEEDS FILES)
```

It defines `option(SOS_FEATURE_<X> ... ON)`, checks the `NEEDS` list (decision 2), adds
`SOS_HAVE_<X>=0|1` to the target, and prints one summary line at configure time
(`SOS features: NET TLS HTTP NTS SSH CHAT LUA PAGER EDITOR MAN FSCK ARCHIVE`), like the
existing "the image is signed with..." line.

Subsystem lists are `include()`d, not `add_subdirectory()`'d: source file properties (the
per-file warnings) are scoped to the directory that sets them, and under the current
`cmake_minimum_required(3.13)` (also the Pico SDK's minimum) they can't be set across
directories. CMake 3.18 added `DIRECTORY` and `TARGET_DIRECTORY` to
`set_source_files_properties()`, so a later minimum would allow `add_subdirectory()`;
`include()` stays the simpler choice either way.

Libraries and settings follow their features:

| With | Goes |
|---|---|
| NET | `pico_cyw43_arch_lwip_threadsafe_background`, `pico_lwip_sntp`, `CYW43_CONFIG_FILE="cyw43_sos.h"` and its configure-time check (`pico_cyw43_arch_none` or nothing without NET) |
| SSH | wolfSSH, `sos-ssh`, SFTP (`sftp_wolf.c`) |
| LUA | Lua's sources, `pico_multicore` |
| ARCHIVE | zlib |
| FILES | FatFs and the SD driver, `USE_DBG_PRINTF` on `sd_card_spi.c` with its check of the counted function names (`sd-bus`), `config/fatfs/ffconf.h` |
| always | wolfCrypt, `pico_hash_binary`, the signing step, the flash layout |

Planned (#265 phase 8): `pico_sha256` for the RP2350's SHA-256 accelerator, with a wolfCrypt
fallback. It serves `sha256sum` and `fetch` and stays always-on with FILES.

One trap to keep: the Thumb-2 AES code is linked last on purpose (where it lands affects the
XIP cache and encrypted card speed by 16%, see `CMakeLists.txt`). Reordering sources around
features must keep it last, and `sd test` should be re-measured after the CMake change.

### 4.7 FILES, later

Making the SD card optional cascades furthest, and further than at #250: `/var/log`
(`logfile`), approved TLS roots (`certstore`), man pages, the editor, Lua file functions and
`sos.sd`, SFTP, `tee` and `>`, `fetch` to a file, choosing a time zone from the database (local time itself stays), the checksum and
archive commands, and the `card`, `sd-bus` and `sd-check` checks. Since the card can already be
missing at run time, most of these cope with no card, but how well each one does hasn't been
checked for this note. It should come after the others have shown the pattern works. Until
then FILES is always on.

### 4.8 Boards: Pico 2 W and Pico 2

`PICO_BOARD` (already a cache variable, `pico2_w` by default) chooses the board; the features
choose the software. The SDK's board files say whether the Wi-Fi chip is there
(`PICO_CYW43_SUPPORTED`, set by `pico2_w.h`), and `features.cmake` uses it:

- NET defaults to ON when the board has the chip and OFF when it hasn't. `PICO_CYW43_SUPPORTED`
  exists only after `pico_sdk_init()`, which reads the board header's `pico_board_cmake_set`
  lines (the SDK's `cmake/generic_board.cmake`), so `features.cmake` is included after that
  call, not next to the `PICO_BOARD` line at the top of `CMakeLists.txt`. This is the one
  exception to decision 1's "all ON": a default that can't build is no default.
- `-DSOS_FEATURE_NET=ON` on a board without the chip is a configure error (decision 2), naming
  the board.
- Everything that needs NET is then off by the usual rule: a Pico 2 image never has TLS, HTTP,
  NTS or SSH. `ninja -C build provision` and `setup` skip Wi-Fi, and the record keeps its
  Wi-Fi fields unused (decision 5).

The Wi-Fi chip is used outside `src/net` in these places on main e3acb48, and each needs a
board path:

| Where | What it does with the chip | On the Pico 2 |
|---|---|---|
| `main.c` | `cyw43_arch_init_with_country()`; the LED blink (`CYW43_WL_GPIO_LED_PIN`) | no init; LED on `PICO_DEFAULT_LED_PIN` (25) |
| `core/sensors_board.c` | USB power from the chip's GPIO (`CYW43_WL_GPIO_VBUS_PIN`); VSYS read only while holding the chip's lock, because VSYS shares its pin with the chip's SPI clock | USB power from `PICO_VBUS_PIN` (24); VSYS on `PICO_VSYS_PIN` (29) with no lock. The file already tests these macros with `#if defined(...)` (with an `#else` returning `SENSORS_NONE`), so this is filling in the other branch |
| `shell/shell.c` | `cyw43_arch_lwip_begin()` in `mem` (lwIP's pools), `net` (link state, RSSI, address), `wifi stats` (`wifi_stats()`), and Tab completion for `ping` (`tab_ping_hosts()`) | `net`, `wifi stats` and `ping` are NET commands and move with NET (§4.1); `mem` leaves out its lwIP lines |
| `core/countcheck_board.c`, `config/cyw43_sos.h` | `lwip_stats` and the driver's counters for the `wifi` and `net-pools` checks | NET checks: they move with NET (§4.3) |
| `lua/lua_health.c`, `lua/lua_net.c`, `time/timesync.c`, `time/nts.c`, `tls/tls.c`, `ssh/` | lwIP locking | NET-only code: not built |

Boards are not features: there is no `SOS_FEATURE_PICO2`. `board.h` declares the few things
that differ (`board_led_set()`, and the sensor reads already in `sensors_board.c`), with one
implementation per board family chosen by `PICO_BOARD` in CMake. Any other RP2350 board with
the same pins would work the same way; others are out of scope until someone has one.

The flash size detection in `CMakeLists.txt` already reads `${PICO_BOARD}.h`, so
`PICO_BOARD=pico2` needs no change there.

Testing needs a plain Pico 2 board, which the board registry doesn't have yet (§11).

## 5. Trimming wolfSSL and wolfSSH

Leaving out SOS's TLS or SSH code drops `src/tls` or `src/ssh`, but most of the size is in the
libraries, and `--gc-sections` alone keeps whatever `config/user_settings.h` turns on. So
`user_settings.h` follows the features:

- Without TLS: `WOLFCRYPT_ONLY` (no TLS layer), and the TLS-only pieces off: P-384, RSA
  verification, SNI, ALPN, the keying exporter, AES-SIV for NTS.
- Without SSH: wolfSSH not compiled, `WOLFSSL_WOLFSSH` and the `WOLFSSH_*` settings off,
  Ed25519 off (with its known answers, §4.3), ML-KEM off if TLS is off too.
- Without both: X25519 off, with its known answers.
- Always: what the core and the always-on commands use: AES-GCM and XTS, HMAC, SHA-1, SHA-2
  (`sha*sum`, `fetch`'s digests), and the hash and curve the boot key check uses.

The `cmake/wolf*_pico.cmake` source lists drop the matching files. The savings here must be
measured, not predicted: some curve code is shared with the core's boot key check.

## 6. Presets

`CMakePresets.json` gets one preset per useful image, inheriting from today's (`debug`,
`release` and `minsize` stay as build types):

| Preset | Off | For |
|---|---|---|
| `full` (default) | nothing | today's image |
| `offline` | NET (so TLS, HTTP, NTS, SSH) | console, card, Lua and the editor, nothing over the air |
| `headless` | LUA, EDITOR, MAN, CHAT, ARCHIVE | network appliance: SSH, SFTP, time, HTTPS, `fetch` |
| `minimal` | everything optional | the security core, the shell and the card |
| `pico2` | NET (by the board, §4.8) | `offline` built for the plain Pico 2: `PICO_BOARD=pico2` |
| `pico2-minimal` | everything optional | `minimal` built for the plain Pico 2 |

## 7. Docs, man pages and help

- `help` and Tab follow the tables (§4.1) with no work.
- Man pages gain a `feature:` line naming the option a command needs. `tools/man-bundle.sh`
  keeps bundling every page (the bundle is the card's, not the image's), and `man <cmd>` for a
  command the image lacks says "not in this build" before the page. `docs/commands.md` keeps
  describing the full build and shows each command's feature. The pre-commit hook's check that
  the bundle and `commands.md` are current is unaffected.
- **The build's features are visible on the board.** `version` gets one line after the commit
  line, listing what is in and what is out, so a stripped image never looks like a full one
  with commands missing:

  ```
  SOS 4968639
  features: NET TLS HTTP NTS SSH CHAT PAGER FSCK ARCHIVE (off: LUA EDITOR MAN)
  build: test commands
  ```

  The `build:` line names the existing switches that are on (`test commands`, `console-only
  forced`), and is left out when none is; the burn image keeps its own warning line. Lua scripts
  read the same with `sos.features()` (§4.5), and the `health` command's output starts with the
  features line too, since a health report is what gets pasted into an issue (and explains a
  check that isn't there, decision 10). One generated table (`SOS_FEATURE_LIST`, from
  `features.cmake`) feeds all three, so they can't disagree.
- `docs/building.md` gets a section on the options and presets; `docs/design/health.md` notes
  which checks belong to which feature.

## 8. Testing

- Host tests are already per module and mostly unaffected; the pure judging code of the health
  checks keeps its tests whichever directory it ends up in. Each stand-in header (§4.4) gets a
  compile test with its feature at 0.
- **Every preset must build and link.** With CI off, that's a `just features` recipe that
  configures and builds each preset into `build-<preset>/` and prints the size of each, plus
  each single feature turned off on its own (`-DSOS_FEATURE_X=OFF` with whatever it needs off
  too). The pre-commit hook does not run it (too slow); a PR that adds an `#if SOS_HAVE_` runs it
  before review. Warnings count: a stripped build must be as clean as `full`
  (`cmake/warnings.cmake`; unused functions and variables are what `#if` leaves behind).
- Board: a feature PR is tested on dev-1 with the `full` image (it must not change; `health`
  shows the same checks) and with the preset that removes what it touched (`health` shows
  none of that feature's checks, and no other check fails because of the absence). A PR that
  touches board code (§4.8) is also tested on a plain Pico 2 with the `pico2` preset: boot,
  console login, `version`, `uptime` (USB power and VSYS), `health`, the LED, the SD card.
- `just features` builds the `pico2` presets too, so a NET-only call left outside its `#if`
  fails to link there even when nobody has a Pico 2 at hand.

## 9. Using SOS as a base

Once features exist, another project needs to add its own code without editing SOS's files.
The smallest version: `-DSOS_APP_DIR=<path>` includes `<path>/sos_app.cmake`, which adds sources
and sets `SOS_HAVE_APP=1`. The app provides `app_commands[]` (appended to `command_tables`),
`app_init()` (called after `keyprov_lock()`, so an app can't derive keys) and `app_poll()`
(called once per loop pass, under the same rule as every `*_poll()`: bounded work, no blocking).
Health checks for an app go through `health_add()`, after the built-in table. That is enough for
an app with its own commands, background work and checks; Lua bindings for apps can come later.
This is the last phase: its interface should be fixed only after the features have settled.

## 10. Phases

Each phase leaves the default image the same and is useful on its own.

**Still open on main e3acb48** (asked of every session, 2026-11-28):

| Work | Touches | Effect on this plan |
|---|---|---|
| #273 gzip, #274 tar tf/xf, #277 tar cf/czf (open) | `files/`, zlib, `config/fatfs/ffconf.h`, command tables | they fill in ARCHIVE; may add tables to `command_tables` |
| #278 pager search (open) | `edit/pager.c`, `screen.h` | none: no new links |
| #265 phase 8 (planned) | `pico_sha256`; Lua `sos.hashfile`, redirects in `sos.http`, gunzip/untar from Lua | more LUA + FILES and LUA + ARCHIVE glue (§3) |
| #275 RTC CAP_SEL (planned, waits for the user) | `time/rtc.c`, `timesync.c` | none: time is not optional |

**Phase 1 can start once #273, #274, #277 and #278 have merged** (the user, 2026-11-28: go
ahead then; every PR reviewed by Reviewer and board-tested, **none merged until the user has
looked at the stack**, this note included). #274 and #277 touch `shell.c`'s table and help;
#278 doesn't, but the user asked for all four first. The two planned items don't
block it: their code lands in its new place if they come later.

Before phase 1, and again before each later phase, bring this note up to date against main:
§1's dependency tables and sizes regenerated, §4.3's checks, §4.4's call list and §4.8's table
re-checked, file names and line counts corrected. The commands:

```sh
# includes between subsystem directories (run in src/)
for f in $(find . -name '*.[ch]' | grep -v pico-sshd); do
  from=$(echo "$f" | cut -d/ -f2)
  grep -oE '#include "(core|shell|net|ssh|files|edit|time|tls|lua|chat|man)/[^"]+"' "$f" |
    sed -E 's/#include "([^/]+)\/(.*)"/\1 \2/' |
    while read -r to h; do [ "$to" != "$from" ] && echo "$from -> $to/$h  $f"; done
done | sort -u
# the Wi-Fi chip outside src/net
grep -rn 'cyw43\|CYW43\|lwip_stats' src --include=*.c | grep -v '^src/net/'
# the health checks and their read steps
sed -n '/static const health_check_t checks\[\]/,/^};/p' src/core/health_board.c
```

Sizes come from `build/sos.elf.map` of a debug build: sum each input file's allocated sections
at flash (0x10000000-0x103fffff) and SRAM (0x20000000-0x2008ffff) addresses, grouped by
`src/<subsystem>` or library. The merged string section (`.rodata...str1`, over 100 KB) is
listed under whichever object comes first; leave it out. Debug sections have address 0 and
other ranges; leave those out too.

1. **Commands move to their subsystems** (§4.1), no options yet. A pure move: same table
   order, same `help` text, same `docs/commands.md`; `shell.c` shrinks by roughly half. The riskiest phase for conflicts with other work, so it goes
   first, while shell work is quiet.
2. **Seams** (§4.3, §4.4): the health checks' read steps moved to their features, the
   stand-in headers, the owner-gone hooks, the Lua health table moved, `tls_init()` moved to
   the core, `fullcheck.h` freed of `fsck.h`. Still no options.
3. **`cmake/features.cmake` with NET, SSH, LUA** and the presets that use them, plus
   `just features`. The first options are the ones that pay most. Includes the board split
   (§4.8) and the `pico2` presets, since NET off is what a Pico 2 needs.
4. **The rest:** TLS, HTTP, NTS, CHAT, PAGER, EDITOR, MAN, FSCK, ARCHIVE.
5. **wolfSSL and wolfSSH trimming** (§5), with the known answers that follow the algorithms,
   measured per preset.
6. **FILES** (§4.7), if still wanted.
7. **The app hook** (§9).

## 11. Decisions and open items

Decided by the user, 2026-11-27:

1. The security core stays fixed (decision 6); the health framework is part of it
   (decision 10).
2. The plain Pico 2 is a target (decision 9, §4.8).
3. Work starts once the open work has landed; the note is updated first (§10).

Open:

1. **Starting phase 1**: approved by the user, 2026-11-28, once #273, #274, #277 and #278
   have merged (§10). Nothing in the stack merges before the user has looked at it.
2. **A plain Pico 2 board** for testing §4.8. Until there is one, the `pico2` presets are
   build-checked only (§8).
3. **Merging this refresh:** reviewed; waits for the user, with the rest of the stack.
