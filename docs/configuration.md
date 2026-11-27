# Console and configuration

## Console

The console runs over USB CDC (`/dev/ttyACM0` on Linux) and is mirrored on UART0 (GP0 TX / GP1 RX):

```sh
picocom -b 115200 /dev/ttyACM0
```

Up and Down recall the commands typed in this session, on the console and over SSH. The
history is kept in RAM only (about 1.3 KB per session), never holds answers such as passwords,
and is wiped when the console locks, logs out or logs in, and when an SSH session ends.

`help` lists the commands. [docs/commands.md](commands.md) is the full reference, and marks
which commands work only on the console. The greeting above the first prompt (and after each SSH
login) names the commit the firmware was built from, e.g. `SOS 9332d0364fb3`; `version` shows it
too, with `-dirty` if the build had uncommitted changes, followed by the board's serial number
and the RP2350 chip revision.

## Configuration

The Wi-Fi network, hostname and trusted public keys are set on the serial console
(USB or UART) and saved in flash. On a new board, connect to the console and run `setup`:

```
sos> setup
New console password (8 to 64 characters):         (not echoed; asked twice)
Wi-Fi network (SSID): my-network
Wi-Fi password (empty for an open network):        (not echoed)
Public key: ssh-ed25519 AAAAC3Nza... julio@laptop    (paste ~/.ssh/id_ed25519.pub)
```

| Command | |
|---|---|
| `config` | show the configuration |
| `wifi` / `wifi set` / `wifi connect` | show the networks / change the first network (password not echoed) / reconnect |
| `wifi add` / `wifi remove <n>` / `wifi prefer <n>` | save another network (up to 5) / forget one / move one to the front; the board joins the first saved network in reach |
| `wifi power on` / `wifi power off` | Wi-Fi power saving (default off; see below) |
| `wifi country US` / `wifi country worldwide` | radio rules to follow (two-letter country; default worldwide; after reboot) |
| `wifi stats` / `wifi stats chip` | signal, noise and packet counters, for chasing packet loss / plus the chip's raw counters |
| `ping <host> [<count>]` | is a host reachable? ICMP echo requests, one a second (default 4), with round-trip times and loss |
| `wifi keepalive on` / `wifi keepalive off` | address announcement (see below); off is for testing and lasts until reboot |
| `key list` / `key add [line]` / `key del <n>` | trusted keys (ssh-ed25519, ecdsa-sha2-nistp256; up to 8) |
| `hostname <name>` | hostname (applies at the next Wi-Fi connect) |
| `time auto <hours>` / `time auto off` | periodic NTS sync (default off; see [Timekeeping](time.md#timekeeping)) |
| `time cal <ppm>` / `time cal off` | RTC calibration: how fast the RTC runs, in ppm (default off; see [RTC calibration](time.md#rtc-calibration)) |
| `temp cal <degrees>` / `temp cal off` | chip temperature calibration: what a thermometer beside the board reads (default off; see [Chip temperature calibration](system.md#chip-temperature-calibration)) |
| `time server <name>` / `time server default` | NTS server: host name or IPv4 address / back to `SOS_NTS_SERVER` |
| `passwd` | set or change the console password |
| `logout` | on the console: lock it again |
| `config reset` | erase everything: no Wi-Fi network, no trusted keys, no console password, hostname back to `sos`; locks the board's encrypted SD cards |

### The console password

The serial console asks for a password before it takes any command, like a login. Until it's
typed the console shows nothing: no greeting, no log lines, no messages from other sessions.

- **Setting it:** a board with no console password opens straight into setting one (a new
  board: the whole of `setup`, which starts with it). `passwd` changes it, after asking for
  the current one. `logout` (or Ctrl-D) locks the console again. SSH sessions aren't
  affected: they log in with their keys.
- **Wrong passwords:** each check takes about 0.3 s by design, and after a wrong answer the
  next one waits 1 s, then 2 s, 4 s ... up to 30 s. The count of wrong answers is kept in
  flash and survives a reboot: it is written before each check starts and cleared by the right
  password, so unplugging the board between guesses, or in the middle of one, skips no wait.
  An answer interrupted with Ctrl-C counts as wrong. Someone with a USB cable can still erase
  the count along with the rest of the flash (the USB bootloader allows it), so what limits a
  determined guesser is the cost of each check: choose a password that isn't guessable in a
  few thousand tries. Wrong attempts are logged (`dmesg`).
- **It locks itself** when the USB terminal goes away (the cable is unplugged or the terminal
  program closes the port) and after 10 minutes with nothing typed
  (the CMake setting `SOS_CONSOLE_IDLE_LOCK_MIN`; 0 turns the idle lock off).
  - **Whatever the console was running is ended** by either lock: a copy (its partial file is
    removed), `sd test`, a Lua script started there, the chat, the editor. **Unsaved text in
    the editor is lost**, so save before leaving the console, and mind a loose cable.
  - **"Goes away" means the terminal program drops the DTR line** when it exits (a hang-up on
    close) or the cable is unplugged. A program that closes the port without hanging up leaves
    the console open until the idle lock. Check what yours does: log in, quit the program,
    start it again, and see whether the console asks for the password.
  - **A drop must last a second** before it locks: around a terminal closing and reopening the
    port the line can read "disconnected" for a moment, and a single such reading used to lock
    a login made just then (#153). Unplugging still locks within about a second. Each change
    is logged at `debug` level (`log history debug` keeps it), to see what a terminal does.
  - On the UART console there is no connection to watch, so only the idle lock applies.
  - Tools that open the port for each command (test scripts) are locked out at every close
    that lasts a second or more, once a password is set: use a board without a console
    password for those, or log in each time. A tool that reopens the port within a second
    stays logged in.
- **What is stored** is not the password but a verifier in the sealed configuration: PBKDF2
  of the password, then keyed with a key from the board's root secret. A copy of the flash
  isn't enough to try passwords against.
- **Forgotten:** there is no way round it. Erase the configuration over USB (or provision the
  board again): the board starts as new, and the SD cards it encrypted are locked, readable
  only with the key shown when each was formatted.
- **Log lines** that would have been printed while the console was locked wait in the log
  history and are printed after the login (`dmesg` in an SSH session shows them meanwhile).
- **What it protects:** the console of a board someone can plug a cable into. On a board
  without secure boot it's a speed bump, not a wall: the flash can be read and rewritten over
  USB (issue #64).

### How the configuration is applied and stored

- **Changes only on the console.** SSH sessions can view the configuration but not change it,
  so a remote session can't lock itself out. The console is the admin channel, behind its
  password.
- Changes apply immediately: a new key works for the next login, `wifi set` reconnects.
- Wi-Fi connects in the background, retries after failures (wrong password, network not
  found) and reconnects if the link drops; the console is usable throughout.
- On a network with several access points (a mesh), the device scans when connecting and
  ends up on the strongest one; the log names it and its signal.
- Wi-Fi power saving is off by default. With it on, the radio sleeps between beacons when
  idle: lower power, but the first packet after a quiet spell can take a little longer. It
  applies at once.
- While connected, SOS announces its address once a minute (a gratuitous ARP). Without it, a
  device that sends nothing stops receiving broadcasts (seen with the Pico 2 W within minutes,
  power saving on or off), so after the router forgets it (~20 minutes here) clients get
  "no route to host". `net` shows ARP packets received and sent; `wifi keepalive off` turns
  the announcement off until the next reboot, for testing.
- Storage: one record in two alternating flash sectors below the key sector, with a sequence
  number and CRC, so a power cut while saving keeps the previous version. It survives
  reflashing.
- It's stored sealed (AES-256-GCM under a key derived from the board's root secret): the flash
  contents don't show the Wi-Fi password, and a record someone else wrote doesn't load. On an
  open board (no secure boot) the root secret is itself in flash, so this protects nothing yet
  from someone who has the board; it's the format a secured board uses (issue #64). A record
  that isn't sealed (saved by older firmware, or written by provisioning) is loaded by an open
  board, stored sealed at once, and its unsealed copy erased.
  The record format only grows at the end: a record saved by older firmware still loads, with
  newer settings at their defaults. Firmware older than a saved record ignores it and starts
  empty; so does firmware from before sealing, for any sealed record.
- The firmware has no configuration of its own. With nothing saved (a new board, or after
  `config reset`) there is no network and no trusted key, so no SSH, until `setup` on the
  console or provisioning.

### Provisioning

For development and automated tests, a board's whole configuration can be written from this
machine instead of typed:

```sh
ninja -C build provision          # from provision.conf, or the file named by $SOS_PROVISION_FILE
tools/provision.sh other.conf     # the same, from another file; -n only builds and checks it
```

With more than one board on USB, name the board by its serial (`version` on its console, or
`picotool info`): `tools/provision.sh -s <serial>`, or `SOS_FLASH_SERIAL=<serial>`, the
variable `ninja flash` uses, which `ninja provision` follows too. It is required when the
machine has a flash key list (see [Flashing, and the flash guard](development.md#flashing-and-the-flash-guard));
without a list and without a serial, picotool writes to whichever board is attached.

`provision.conf` (git-ignored; see `provision.conf.example`) lists the console password, the Wi-Fi network, hostname,
trusted keys and the optional settings. The script builds the saved record on this
machine, with the same code the device uses, and loads it into the two configuration sectors
over USB with `picotool`, like flashing: the board reboots into BOOTSEL and back. It replaces
everything saved on the board, and with it the board's owner: it gets a new SSH host key
(clients will warn) and its encrypted cards are locked. The firmware stays as it is. The record it
writes is not sealed (this machine doesn't have the board's key): the board seals it at its
first boot and erases the unsealed copy. A `console_password` line sets the console password
(it travels in the image in the clear until that first boot); without one, the board asks for a
new password the first time its console is used. **Provisioning makes the board a new owner:** like
`config reset`, it locks the SD cards the board encrypted before (see
[Encrypted cards](files.md#encrypted-cards)); `provision.sh` says so when it finishes.

- `ninja flash` doesn't touch the configuration, so provisioning is needed once per board, and
  again after `config reset`, a full flash erase, or to put a board into a known state for a
  test.
- Provision from the source tree the board's firmware was built from: firmware older than the
  record's format ignores it and starts empty.
- Like the console, this needs physical access, which already allows reflashing. Nothing on
  the network can provision a board.
