# Securing a board: design

How a secured SOS board protects its secrets, what each mechanism does, and what it leaves
open. This page describes the current firmware. Why each choice was made, and what is still
undecided, is in [decisions.md](decisions.md). The steps to run at a board are in the
runbooks (see [README.md](README.md)).

It covers steps 3 and 4 of [issue #64](https://github.com/j3m7/sos/issues/64): the root
secret in one-time-programmable (OTP) memory, secure boot, and (on a production board) debug
off.

Sources: the RP2350 datasheet in `docs/rp2350/` (sections 10.5, 10.9, 13.4, 13.5, 13.8 and
appendix D), `picotool otp list` from picotool 2.3.1 for the field names, and Raspberry Pi's
post ["Security through transparency: RP2350 Hacking Challenge results are
in"](https://www.raspberrypi.com/news/security-through-transparency-rp2350-hacking-challenge-results-are-in/).
Points not confirmed from those are listed under "To confirm" in
[decisions.md](decisions.md#to-confirm).

## What a secured board protects against

What securing a board buys depends on its chip revision. The production board's isn't settled:
an **A4** is preferred, but A4s are hard to get, so it may well be an **A2**, like dev-1. The
table assumes everything in the production column of the [decisions](decisions.md) is done,
and shows the A2 case where the revisions differ.

| Attacker | A secured board |
|---|---|
| A PC and a USB cable | Stopped: only signed firmware runs, the flash holds sealed records, the root secret can't be read over USB |
| A debug probe (SWD) | Stopped: debug is disabled |
| A flash reader, or rewriting the flash chip | Reads only sealed records; can erase or roll back the configuration, which locks the cards (erase) or restores an older state (rollback, accepted in #64) |
| Glitching equipment (voltage or laser fault injection) | **On an A2: not stopped.** Four published attacks apply to A2: reading OTP through a disrupted supply (E16, no workaround), running unsigned code through the bootloader's reboot call (E20), reading locked OTP pages in BOOTSEL (E21), and swapping the flash contents during the signature check (E24, no workaround, also on A3) |
| Someone who can solder a second flash chip behind a switch | **Not stopped, on any chip revision.** SOS runs from external flash. After the boot ROM has checked the signature, the flash the chip reads from can be switched to another one, with no glitching at all. The attacker's code then runs as Secure code and can read the root secret. Raspberry Pi's security white paper warns of this and says secure code should run from RAM; SOS is too large for that. Closing it is the "Later" section of #64. `health full`'s `image` check (the flash hashed again while the board runs) doesn't close it either: code from the other chip would be the code doing the check |
| A lab that can decapsulate the chip | Not stopped (OTP imaging); storing the secret with chaff makes it harder |

On an A2, E20 and E21 have workarounds that cost features (decisions 2 and 3), and E16 and E24
have none. So a secured A2 raises the bar from "anyone with a cable" to "someone with a
soldering iron and a second flash chip, or with fault-injection equipment", and no further. An
A4 removes the four glitch attacks. It does not remove the flash swap, so on either revision
the bar stops at the soldering iron.

A development board (dev-1) has the mechanisms but not the protection: debug stays on and its
signing keys are public. See [the dev-1 record](../otp/dev-1/RECORD.md).

## The root secret

Every key a board seals with (its configuration, its SSH host key, its SD card keys) is derived
from a 32-byte root secret. Until a root secret has been written to OTP it is kept in flash. On a
secured board (and on dev-1) it is in OTP, where it can't be read over USB once its page is locked.

**Moving the root secret from flash to OTP changes every derived key.** The board's sealed
configuration and SSH host key become unreadable and its encrypted cards locked. So a board is
set up fresh after its root secret is written, never before.

### Reading it: the key provider's OTP backend (#86)

The firmware reads the root secret from an OTP page as Secure code, and on `keyprov_lock()`
advances that page's lock in the `SW_LOCK` register, so nothing can read it until the next
reset. The page list is pages 4, 5 and 6; the format of a page is in `src/core/roototp.h` and is
what the burn command writes.

- **Which backend a board uses depends on whether a valid root secret is in OTP,** not on
  secure boot: a board is set up after the burn and before secure boot is on, and what is
  sealed then has to open afterwards.
- **On a console-only board the root secret comes from OTP or not at all.** The flash record
  is never read, never created and never a fallback there. Otherwise the console-only marker
  would be hollow: someone with a cable could write a root secret record of their own into
  flash, with a configuration sealed under it, and the board would load both as its own.
  Refusing unsealed records (#84) doesn't cover that; this rule does, with a host test. A
  console-only board with no usable secret in OTP has no keys and keeps its console locked.
- **Finding the page.** The firmware tries the pages of the list in order. A page holds a
  complete secret only if a marker row, written last, is there and a check over the secret
  matches; the secret alone has no magic number. A spent page (something written, marker row
  still blank) is skipped, and so is a blank page the firmware can't write, which the burn never
  makes: a lock alone can't block the list. A blank page ends the search: no secret yet. A
  damaged marker or an unreadable page also ends it, with no usable secret: the pages after it
  are never used.
- **Locks are checked at every boot.** If any list page up to and including the secret's page
  isn't locked against bootloader writes, an open board warns and a console-only board refuses
  the secret (see why under the burn, below).

### Writing it: the burn image (#87)

A console command, `rootsecret burn` (with `plan` and `rehearse`), takes 32 bytes from the
TRNG and writes them to the first blank page of the list. The secret never leaves the board and
is never shown. The order is the one in `src/core/rootburn.h`:

1. **It locks every list page before that one,** which are spent pages: bootloader read-only
   at least, and Secure read-only. A spent page holds 32 bytes and no marker. Left writable
   from the bootloader, someone with a cable could read those bytes, write the check rows and
   the marker, and so make it the board's root secret, one they know; on a console-only board
   they could then load a configuration sealed under it.
2. It writes the data rows, with error correction, reading each back.
3. It writes the chaff the datasheet recommends against imaging (section 13.8.2): for each data
   row *i*, it reads back the row's **raw** 24 bits, error-correction bits included, and writes
   their bitwise complement as a **raw** row at 32 + *i*. Chaff rows are never read as
   error-corrected rows. (`rehearse` stops here on purpose: the page is spent, and locked like
   any other spent page.)
4. It writes the check over the secret.
5. It reads the secret and the check back the careful way and verifies them.
6. It writes the marker. Only now does the page hold a root secret.
7. It locks the page: bootloader inaccessible, Non-secure inaccessible, Secure read-only (which
   also stops a later bug from rewriting the page). On an A2 the bootloader lock is the one
   erratum E21 defeats by glitching; its workaround is disabling the USB bootloader, which
   decision 2 leaves for later.

If anything fails after something was written, the page is spent: the run locks it and reports
where it stopped (unless the lock itself fails: `LOCKING A PAGE FAILED`). A restart in the
middle of a run never reaches the locks, so it leaves a spent page unlocked until the next run
locks it first; a restart during the marker write can leave the marker damaged, which ends the
search. A page that still reads blank is never locked: if the very first write fails and nothing
landed, the page is left blank and unlocked, and the next run uses it. A burn that ends in an
error other than "nobody held the button" prints `STOP HERE`, and the image refuses further runs
until the board restarts: a fault in how OTP is written would spend the next page exactly as it
spent this one.

**It is not part of everyday firmware** (decision 12). It is built only into a separate burn
image (`-DSOS_BURN_IMAGE=ON`, off by default), flashed for the burn and replaced afterwards, so
boards in use carry no code that writes OTP. It needs someone present: the console password,
the board's serial and the word `BURN` (or `REHEARSE`) typed in, and the BOOTSEL button held
while it runs. Every burn image accepts `rehearse`; there is no build without it.

## Knowing a board is secured: the console-only marker (#84)

A board refuses unsealed configuration and SSH host-key records (`refuse_plain`), and refuses
`rootsecret forget`, when its **console-only marker** is set. Secure boot alone does not do it,
so a development board with secure boot can still be provisioned from a file (decision 9).
`version` shows the three facts: where the root secret is, whether secure boot is on, and
whether the board is console-only.

The marker is three OTP rows that SOS defines: rows 0x0c0, 0x0c1 and 0x0c2 (the start of
page 3), each the raw 24-bit value 0x5ea1ed, read by a two-of-three vote through the read
aliases of #79. It fails closed: only blank rows mean "not console-only"; rows that can't be
read, or that are neither blank nor the marker, count as **set**. A marker written partly is
still set: the rows can be completed, never cleared. Firmware older than #84 ignores the
marker; that is the rollback case decision 8 leaves open.

Page 3 is reserved for SOS's own flags; the root secret's page list therefore starts at page 4.

Why OTP and not something else:

- *A build option* would make "console-only" a property of the image. Any image without it
  that the board's key has ever signed (an older one, a development one) would then accept an
  unsealed record again.
- *A field in the sealed configuration* disappears when someone erases the configuration over
  USB, which is exactly when it is needed.
- *An OTP row* can't be cleared by rewriting the flash, and is the same for every image.

## Secure boot and the boot keys

With `CRIT1.SECURE_BOOT_ENABLE` set, the boot ROM runs only images signed with a key whose hash
is in a boot key slot (`BOOTKEY0` to `BOOTKEY3`) marked valid in `BOOT_FLAGS1.KEY_VALID` and not
marked invalid in `BOOT_FLAGS1.KEY_INVALID`. The boot key is a secp256k1 key pair. The private
half signs firmware (`SOS_SIGN_KEY`, #70); the SHA-256 of the public key's 64 raw bytes (X then
Y) goes into the slot. Whoever holds a private key can make firmware a secured board will run,
and if every private key is lost the board keeps its last firmware for ever.

- **The boot key hashes are a file written by hand** (dev-1's is
  `docs/otp/dev-1/boot-keys.json`), in the form `picotool otp load` takes, checked into the
  repository with no key material and reviewed like code. Not the single file `picotool seal`
  can produce, which enables secure boot in the same load as the key hash. The build
  deliberately produces no OTP file. The other OTP writes are one field or one row each, made
  with `picotool otp set`.
- **`bootkey` checks the key on the board (#79).** It hashes the public key from the running
  image's own signature block, compares it with the boot key slots in OTP, and says `MATCH`,
  `NOT YET` (slot written, not marked valid), `NO MATCH` or `NO VERDICT`. It reads OTP only
  through the read aliases. The procedures run it before secure boot is turned on.
- **`version` warns between an OTP write and the next reset.** The chip acts on what it latched
  at reset; until then `version` shows the old state with `WARNING: the OTP rows say
  otherwise`.
- With secure boot on, the RISC-V cores are disabled for good as well; SOS doesn't use them.

### The flash guard (#80, #81)

`ninja flash` and `tools/flash.sh` refuse an image that isn't signed with an accepted key, so
that a checkout with a missing build setting can't leave a secured board in the bootloader by
accident. The accepted keys are a list of public key hashes kept per machine
(`$SOS_FLASH_KEYS`, or `sos/flash-keys` under `$XDG_CONFIG_HOME`, by default
`~/.config/sos/flash-keys`): a list, because a board may accept two keys; hashes, so the guard
works where no private key is present. The list can have a `[<serial>]` section per board.
With no list on the machine there is no guard. It guards
our own tools only: `picotool load` typed by hand goes around it. `SOS_FLASH_UNGUARDED=1` turns
it off for one flash.

## The glitch detector

**What it is.** Four small circuits spread over the chip that notice when the processor's clock
or core voltage is disturbed enough for logic to misbehave, which is what a voltage or clock
glitch attack does on purpose. When one fires, the chip resets itself within one clock cycle,
instead of carrying on in a state the attacker chose. A flag (`CHIP_RESET.HAD_GLITCH_DETECT`)
then tells the firmware why it restarted, and `TRIG_STATUS` which detector fired. (Datasheet
section 10.9.)

**Arming it in OTP.**

- `CRIT1.GLITCH_DETECTOR_ENABLE` arms it from the first instant after reset. Once written to OTP
  it can't be turned off.
- `CRIT1.GLITCH_DETECTOR_SENS`, 0 to 3, sets how easily it fires. Each step makes the detectors
  react to a smaller disturbance (the delay they measure against goes from 75% to 120% of the
  clock period in steps of 15%). It is a two-bit OTP field, and OTP bits only go from 0 to 1:
  from 0 any setting can still be reached, from 1 or 2 only 3, and nothing can be lowered. So
  "start low and raise it later" works from 0, but from 1 the only step left is straight to the
  highest.

**Arming it by software (#78).** The detector can also be armed with its `ARM` register, at a
sensitivity set in its `SENSITIVITY` register. SOS does this at every boot, at sensitivity 3 by
default (`glitch off` on the console turns it off), and reports detector resets in the log,
`uptime` and `glitch`, with the chip temperature and supply voltage at the time (#92).

The chip's own temperature sensor and the board's supply voltage (VSYS) are each read every 10
s, with whether USB power is present. The readings are logged with every detector trigger and
kept across a detector reset, and a line goes to the log at boot, on each degree or 0.2 V of
change, when USB power comes or goes, and at least hourly, so a serial logger on the console can
record them for a long test. `temp` and `power` show the lowest and highest since boot. Armed by
software it starts a second or two into boot, and software can turn it off again: it shows
whether the detector troubles a healthy board, but it is not the protection the OTP bit would
be.

**What it costs.** A detector too sensitive for the board resets a healthy board: spurious
restarts under load, when the Wi-Fi radio transmits, or on a marginal USB supply. A detector
reset also clears the watchdog scratch registers, where SOS keeps its record of the last fault,
so that record is lost for such a reset. Armed in OTP, it is permanent.

**What it buys.** It makes voltage glitching harder, not impossible, and it doesn't cover the
other methods. From Raspberry Pi's write-up of the hacking challenge results (January 2025):

- On voltage glitches, reporting Hextree's tests: "at the highest sensitivity setting, the
  glitch detectors can detect many voltage glitches; however, the rate of undetected glitches
  is still high enough to make attacks feasible with some effort."
- Raspberry Pi's own conclusion: "we've revised downward our estimate of the effectiveness of
  our glitch detection scheme".
- Attacks the detectors don't see at all: electromagnetic pulses from a small coil on top of
  the chip (E21) and a laser (E24) act on one spot and don't disturb the detectors; E16
  disturbs the OTP supply, not the core supply.

It does nothing against the flash swap or against reading the chip in a lab. So on an A2 the
detector removes none of the published attacks; it makes the voltage-glitch ones (E20) take
more attempts.

**Software can disarm it.** The detector block has a `DISARM` register, and the firmware
doesn't lock the detector's registers (its `LOCK` register) today, so a bug could be used to
switch it off. Locking them on a secured board is to-confirm item 5.

Whether and at which sensitivity to arm it in OTP is decision 5.

## Other protections in the firmware

- **Wrong-password count across reboots (#82).** The delay after wrong console passwords is
  kept in flash, so it doesn't start again at every boot. The count is stored *before* the
  password is checked and cleared after a right one: otherwise cutting the power after a wrong
  answer would give free guesses. While the USB bootloader is kept, someone with a cable can
  erase it.
- **A new SSH host key after a wipe (#88).** The SSH host key is sealed under a key made from
  the root secret and the owner secret in the configuration, the way an encrypted card's key
  is. Erasing or provisioning a board's configuration makes its host key unreadable,
  `config reset` also stops SSH at once, and the next setup makes a new one. Losing both
  configuration copies loses the SSH host key too. Firmware older than #88 makes a new SSH host
  key over the record #88 writes.
