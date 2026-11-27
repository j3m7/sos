# Build features: design note for turning subsystems on and off with CMake

Question (the user's, 2026-11-27): **can SOS be built with features switched on or off at
compile time** (the editor, Lua, SSH and so on), so that it can serve as a base for other
projects and stripped-down images can exist?

Short answer: **yes, and most of the work is in C, not CMake.** The sources are already split
one directory per subsystem, and the build already has three on/off switches
(`SOS_BURN_IMAGE`, `SOS_TEST_COMMANDS`, `SOS_FORCE_CONSOLE_ONLY`). What is in the way is that
three places know every subsystem by name: the command table in `shell/shell.c` (which also
holds several subsystems' commands), the boot sequence and main loop in `main.c`, and about a
dozen direct calls from one subsystem into another. Once those go through a small number of
seams, the CMake side is a list of options and a source list per feature.

Status (2026-11-27): design only. The code is changing quickly, so this note sets the rules and
the order of work; it does not ask for the refactor now. Where this note and the code differ in
detail, the code is the reference.

## 1. What exists today

### Build

`CMakeLists.txt` builds one executable, `sos`, from one list (`SOS_SOURCES`, about 90 files),
plus the third-party code compiled into the same target: wolfSSL and wolfSSH
(`cmake/wolfssl_pico.cmake`, `cmake/wolfssh_pico.cmake`), the copied SSH server
(`sos-ssh`), FatFs and the SD driver, and Lua. Third-party settings live in
`config/user_settings.h` (wolfSSL, wolfSSH) and `config/lwipopts.h`. Warnings are set per source
file with `set_source_files_properties()`. The existing switches add sources and a compile
definition, which is the pattern this note generalises.

### Who depends on whom

Includes between subsystem directories, `core` excluded as a target (everyone uses it):

| From | Includes from |
|---|---|
| `shell` | **every other subsystem**: chat, edit, files, lua, man, net, ssh, time, tls |
| `main.c` | chat, files, lua, net, shell, ssh, time, tls |
| `lua` | files, net (http, wifi), shell, time |
| `man` | edit (pager), files, net (http, for `man install`), shell |
| `ssh` | chat, files, shell |
| `edit` | files, shell |
| `tls` | files (`certstore.c`: approved roots on the card), shell, time |
| `time` | shell, tls (NTS) |
| `net` | shell, tls (HTTPS) |
| `chat`, `files` | shell |
| `core` | files and time (`logfile_board.c`), lua (`health_board.c`: core 1's stack peak; `health.c`: the Lua table) |

Everything depending on `shell` is fine: the shell is the base. The arrows that matter are the
ones **out of** `shell`, `main.c` and `core`, because they make the base depend on the features.

### Where the bytes are

From the map of the last dev build (2026-11-26, 1150 KB of flash, 148 KB static RAM). The
string literals of the whole image (about 96 KB) are merged into one section and not counted
per subsystem below, so every figure is a lower bound.

| Part | Flash | Static RAM | Notes |
|---|---|---|---|
| wolfSSL | 337 KB | 0.4 KB | largest: SP math (73 KB), Ed25519 (51 KB), ASN.1 (30 KB), TLS layer (`internal`, `tls`, `tls13`, `ssl`: 55 KB), ML-KEM (14 KB) |
| CYW43 driver and firmware | 235 KB | 2.6 KB | Wi-Fi chip; also drives the LED on the Pico 2 W |
| Lua (library + `src/lua`) | 103 KB | 8 KB | plus the script heap and core 1 |
| wolfSSH + `src/ssh` | 67 KB | 7 KB | plus about 23 KB of heap per session slot (3 slots) |
| lwIP + `src/net` | 51 KB | 75 KB | most of the RAM is lwIP's buffers |
| FatFs, SD driver, `src/files` | 48 KB | 13 KB | |
| `src/core` | 27 KB | 19 KB | |
| `src/shell` | 23 KB | 1 KB | |
| `src/time`, `src/tls` | 22 KB | 8 KB | |
| `src/edit`, `src/man`, `src/chat` | 15 KB | 7 KB | |

Flash is 4 MB, so size alone is not pressing on this board. The reasons that are: **RAM**
(no network frees about 75 KB static and 69 KB of SSH session heap), **attack surface** (an
image without the network stack, the SSH server and the TLS client cannot be reached over the
air), **boot and audit scope**, and **boards without the Wi-Fi chip**.

## 2. Decisions proposed

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
5. **The stored configuration does not change with features.** The record keeps every field
   (Wi-Fi, SSH keys, NTS server...) in every build; an absent feature ignores its fields and
   `config show` leaves them out. A board can move between a full and a stripped image without
   losing or reformatting its configuration, and there is one format to test, seal and
   provision. The cost is a few hundred bytes of an already-fixed 4 KB slot.
6. **The security core is not optional** (decided by the user, 2026-11-27): the root secret and key provider, sealed
   configuration, the console password, health gates, the watchdog, the fault record. These
   are what SOS is. A project that wants less security should fork; making it a switch invites
   shipping the wrong one. (`SOS_FORCE_CONSOLE_ONLY` and `SOS_BURN_IMAGE` stay as they are.)
7. **`main.c` stays explicit.** Boot order is security-relevant (`health_boot()` before any key,
   every key derived before `keyprov_lock()`), so it remains one readable sequence with
   `#if SOS_HAVE_...` around the optional steps, not a module registry that hides the order.
8. **Commands belong to their features.** Each feature exports a command table, as
   `files_commands[]` already does, and the shell's list of tables is the one place that says
   which exist (§4.1).

## 3. Features and their dependencies

| Option | What it contains | Needs |
|---|---|---|
| `NET` | Wi-Fi (`net/wifi*`), lwIP, CYW43, `net`, `wifi`, `ping`, SNTP | |
| `TLS` | `tls/`: TLS 1.3 client, built-in roots, `tls` command | NET |
| `HTTP` | `net/http*`, `http` command | TLS |
| `NTS` | `time/nts*`, `nts` command, `time server` | TLS |
| `SSH` | `ssh/`, wolfSSH, host key, `ssh` command, SSH keys in `setup` | NET |
| `CHAT` | `chat/`: `who`, `wall`, `chat` | |
| `LUA` | `lua/`, the Lua library, core 1 | |
| `PAGER` | `more`, `\| more` (`edit/pager.c`, `view.c`, `screen.c`) | |
| `EDITOR` | `edit` (`edit/editor.c`, `edit.c`) | PAGER (shares the screen code), FILES |
| `MAN` | `man/`, man pages on the card | PAGER, FILES |
| `FSCK` | `sd check` and its repair (`files/fsck*`, about 3,100 lines) | FILES |
| `FILES` | SD card, FatFs, file commands, `/var/log`, encrypted cards | later: see §4.6 |

Glue that follows its parents (decision 3): SFTP (SSH + FILES), approved roots on the card
(TLS + FILES), Lua `http` (LUA + HTTP), Lua `wifi` (LUA + NET), Lua file functions
(LUA + FILES), `man install` (MAN + HTTP), the Lua stack check in `health` (LUA).

Turning off NET turns off, by the rules above, TLS, HTTP, NTS and SSH: everything that faces
the air. That one switch is most of the size and RAM gain (about 450 KB of flash before
trimming wolfSSL, §5, and the 75 KB of lwIP buffers).

## 4. Shape of the code

### 4.1 Commands

Today `shell/shell.c` (2,500 lines) holds one `commands[]` table naming every command, and the
bodies of many that belong elsewhere: `cmd_wifi` (180 lines), `cmd_time`, `cmd_tls`, `cmd_nts`,
the Wi-Fi and SSH steps of `setup`, `run_ssh`. The change:

- Move each subsystem's command bodies and word lists into that subsystem
  (`net/wifi_cmd.c`, `time/time_cmd.c`, `tls/tls_cmd.c`, `ssh/ssh_cmd.c`...), each exporting a
  table: `extern const shell_cmd_t wifi_commands[];`. This is worth doing even without
  features: it shrinks `shell.c` and puts a command next to the code it drives.
- One file, `shell/commands.c`, lists the tables:

  ```c
  const shell_cmd_t *const command_tables[] = {
      core_commands, files_commands,
  #if SOS_HAVE_NET
      wifi_commands,
  #endif
  #if SOS_HAVE_SSH
      ssh_commands,
  #endif
      ...
      NULL};
  ```

  `help`, Tab completion and dispatch already walk `command_tables`, so they follow with no
  change. A linker-section registry (each table placing itself) was considered and rejected:
  it needs the linker scripts SOS already overrides, and the explicit list is easier to read.
- `setup` asks its Wi-Fi and SSH questions only when those features exist; the steps move to
  their subsystems as functions `setup` calls.

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

About eight such blocks. The LED blink uses the CYW43 chip's GPIO on the Pico 2 W; without NET
it goes through a small `board_led_set()` (a plain GPIO on boards that have one, nothing
otherwise).

### 4.3 Calls from one feature into another

For the few calls that cross from the base into a feature, the feature's header provides an
inline stand-in when the feature is off, so callers keep no `#if`:

```c
// chat/chat.h
#if SOS_HAVE_CHAT
void chat_deliver(shell_t *sh);
#else
static inline void chat_deliver(shell_t *sh) { (void)sh; }
#endif
```

The list is short and should stay so; each stand-in has an obvious "nothing" answer:

| Call | Stand-in when off |
|---|---|
| `wifi_up()` (main loop, `shell.c`, `lua_health.c`) | `false` |
| `chat_register()`, `chat_deliver()`, `chat_name()` | no-op, `""` |
| `shell_page()`, `shell_page_growing()` (pager) | print the text to the terminal, as without `\| more` |
| `sshd_poll()`, `sshd_running()` | no-op, `false` |
| `lua_host_poll()`, `lua_core1_stack_peak()` | no-op, 0 (the health check reports core 0 only) |
| `timesync_*` from `logfile_board.c` | stays: time is not optional |

Anything larger than a stand-in belongs inside the feature. Two specific inversions:

- **`config reset` calls `sdcard_owner_gone()` and `hostkey_owner_gone()` by name**
  (`shell.c`). It becomes a list of "owner gone" hooks that the features add themselves to at
  init, like `health_add()` does for checks.
- **`core/health.c` writes a Lua table** (`health_lua_table()`). It moves to
  `lua/lua_health.c`, which already exists, so `core` no longer includes from `lua`.

### 4.4 Lua

`lua_core1.c` registers every binding table in one function (`sos`, `sos.http`, `sos.wifi`,
`base64`, `json`). The `http` and `wifi` tables move to their own files (`lua_bind_http.c`...)
compiled only when both parents exist (decision 3), and the registration lists them with `#if`,
as §4.1 does for commands. The file functions (`sos.read`, `sos.write`, `sos.list`...) are
entries in `sos_lib` itself, so they move to a second array added to the `sos` table. A script
that calls a missing table gets Lua's usual "attempt to index a nil value"; `sos.features()`
(a table of the `SOS_HAVE_` values) lets a script check first.

### 4.5 CMake

```
cmake/features.cmake      options, dependency checks, SOS_HAVE_ definitions, summary
src/<subsystem>/sources.cmake   that subsystem's source list, per feature
```

`features.cmake` defines a helper so each rule is one line:

```cmake
sos_feature(NET    "Wi-Fi, lwIP, ping, SNTP")
sos_feature(TLS    "TLS 1.3 client" NEEDS NET)
sos_feature(SSH    "SSH server" NEEDS NET)
sos_feature(EDITOR "Full-screen editor" NEEDS PAGER FILES)
```

It defines `option(SOS_FEATURE_<X> ... ON)`, checks the `NEEDS` list (decision 2), adds
`SOS_HAVE_<X>=0|1` to the target, and prints one summary line at configure time
(`SOS features: NET TLS HTTP NTS SSH CHAT LUA PAGER EDITOR MAN FSCK`), like the existing
"the image is signed with..." line.

Subsystem lists are `include()`d, not `add_subdirectory()`'d: source file properties (the
per-file warnings) are scoped to the directory that sets them, and with
`cmake_minimum_required(3.13)` they can't be set across directories. Libraries follow their
features: wolfSSH, `sos-ssh` and SFTP with SSH; `pico_cyw43_arch_lwip_threadsafe_background` and
`pico_lwip_sntp` with NET (`pico_cyw43_arch_none` or nothing without it); Lua's sources and
`pico_multicore` with LUA.

One trap to keep: the Thumb-2 AES code is linked last on purpose (where it lands affects the
XIP cache and encrypted card speed by 16%, see `CMakeLists.txt`). Reordering sources around
features must keep it last, and `sd test` should be re-measured after the CMake change.

### 4.6 FILES, later

Making the SD card optional cascades furthest: `/var/log` (`logfile`), approved TLS roots
(`certstore`), man pages, the editor, Lua file functions, SFTP, `tee` and `>`. Since the card can
already be missing at run time, most of these cope with no card, but how well each one does
hasn't been checked for this note. It should come after the others have shown the pattern
works. Until then FILES is always on.

## 5. Trimming wolfSSL and wolfSSH

Leaving out SOS's TLS or SSH code drops `src/tls` or `src/ssh`, but most of the size is in the
libraries, and `--gc-sections` alone keeps whatever `config/user_settings.h` turns on. So
`user_settings.h` follows the features:

- Without TLS: `WOLFCRYPT_ONLY` (no TLS layer), and the TLS-only pieces off: P-384, RSA
  verification, SNI, ALPN, the keying exporter, AES-SIV for NTS.
- Without SSH: wolfSSH not compiled, `WOLFSSL_WOLFSSH` and the `WOLFSSH_*` settings off, ML-KEM
  off if TLS is off too.
- Always: what the core uses (AES-GCM and XTS, HMAC, SHA-2, the hash and curve the boot key
  check uses).

The `cmake/wolf*_pico.cmake` source lists drop the matching files. The savings here must be
measured, not predicted: some curve code is shared with the core's boot key check.

## 6. Presets

`CMakePresets.json` gets one preset per useful image, inheriting from today's:

| Preset | Off | For |
|---|---|---|
| `full` (default) | nothing | today's image |
| `offline` | NET (so TLS, HTTP, NTS, SSH) | console, card, Lua and the editor, nothing over the air |
| `headless` | LUA, EDITOR, MAN, CHAT | network appliance: SSH, SFTP, time, HTTPS |
| `minimal` | everything optional | the security core, the shell and the card |

## 7. Docs, man pages and help

- `help` and Tab follow the tables (§4.1) with no work.
- Man pages gain a `feature:` line naming the option a command needs. `tools/man-bundle.sh` keeps
  bundling every page (the bundle is the card's, not the image's), and `man <cmd>` for a
  command the image lacks says "not in this build" before the page.
- `docs/commands.md` keeps describing the full build and shows each command's feature.
- **The build's features are visible on the board.** `version` gets one line after the commit
  line, listing what is in and what is out, so a stripped image never looks like a full one
  with commands missing:

  ```
  SOS 4968639
  features: NET TLS HTTP NTS SSH CHAT PAGER FSCK (off: LUA EDITOR MAN)
  build: test commands
  ```

  The `build:` line names the existing switches that are on (`test commands`, `console-only
  forced`), and is left out when none is; the burn image keeps its own warning line. Lua scripts
  read the same with `sos.features()` (§4.4), and the `health` command's output starts with the
  features line too, since a health report is what gets pasted into an issue. One generated
  table (`SOS_FEATURE_LIST`, from `features.cmake`) feeds all three, so they can't disagree.
- `docs/building.md` gets a section on the options and presets.

## 8. Testing

- Host tests are already per module and mostly unaffected. Each stand-in header (§4.3) gets a
  compile test with its feature at 0.
- **Every preset must build and link.** With CI off, that's a `just features` recipe that
  configures and builds each preset into `build-<preset>/` and prints the size of each, plus
  each single feature turned off on its own (`-DSOS_FEATURE_X=OFF` with whatever it needs off
  too). The pre-commit hook does not run it (too slow); a PR that adds an `#if SOS_HAVE_` runs it
  before review.
- Board: a feature PR is tested on dev-1 with the `full` image (it must not change) and with
  the preset that removes what it touched.

## 9. Using SOS as a base

Once features exist, another project needs to add its own code without editing SOS's files.
The smallest version: `-DSOS_APP_DIR=<path>` includes `<path>/sos_app.cmake`, which adds sources
and sets `SOS_HAVE_APP=1`. The app provides `app_commands[]` (appended to `command_tables`),
`app_init()` (called after `keyprov_lock()`, so an app can't derive keys) and `app_poll()`
(called once per loop pass, under the same rule as every `*_poll()`: bounded work, no blocking).
That is enough for an app with its own commands and background work; Lua bindings and health
checks for apps can come later. This is the last phase: its interface should be fixed only
after the features have settled.

## 10. Phases

Each phase leaves the default image the same and is useful on its own.

1. **Commands move to their subsystems** (§4.1), no options yet. Pure refactor; `shell.c`
   shrinks by roughly half. The riskiest phase for conflicts with other work, so best done
   when shell work is quiet.
2. **Seams** (§4.3): the stand-in headers, the owner-gone hooks, the Lua health table moved.
   Still no options.
3. **`cmake/features.cmake` with NET, SSH, LUA** and the presets that use them, plus
   `just features`. The first options are the ones that pay most.
4. **The rest:** TLS, HTTP, NTS, CHAT, PAGER, EDITOR, MAN, FSCK.
5. **wolfSSL and wolfSSH trimming** (§5), measured per preset.
6. **FILES** (§4.6), if still wanted.
7. **The app hook** (§9).

## 11. Questions for the user

1. ~~Is the security core really fixed (decision 6)?~~ Yes (the user, 2026-11-27).
2. **Is a board without the Wi-Fi chip (plain Pico 2) a goal?** If so, `PICO_BOARD` and NET
   need to be tied together, and phase 3 includes the LED change. If not, NET off still works on
   the Pico 2 W, with the LED off.
3. **Timing:** phase 1 touches `shell.c`, which most open branches touch too. Start it now, or
   wait for the current shell and Lua work to land?
