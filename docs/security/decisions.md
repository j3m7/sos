# Securing a board: decisions

What was decided about securing SOS boards, when, why, and at what cost; what is deferred; and
what is still to be confirmed. How the mechanisms work is in [design.md](design.md).

The dev-1 column was confirmed on 2026-11-25 and is what dev-1 was given on 2026-11-26 (see
[its record](../otp/dev-1/RECORD.md)). The production column is what was recorded earlier. It
is **not final**, and is revisited before there is a production board: "We are not working with
a production board until dev-1 is tested and working" (2026-11-25).

## Summary

| # | Decision | dev-1 | Production board (to be revisited) |
|---|---|---|---|
| 1 | Debug port | Stays on | Off for good |
| 2 | USB bootloader | Kept | Kept for now |
| 3 | Reboot into bootloader on command | Kept | Removed (`DISABLE_WATCHDOG_SCRATCH`) |
| 4 | Signing keys | The two public dev keys in `keys/`; slots 2 and 3 left blank | Two keys made off the development machine; slots 2 and 3 marked invalid |
| 5 | Glitch detector | Armed by the firmware at boot (sensitivity 3); not set in OTP | Armed by the firmware the same way. **Deferred:** the OTP bit waits for the week-long test |
| 6 | Rehearsal burn | The first burn was the rehearsal, then the real one | Not applicable: dev-1 was the rehearsal |
| 7 | Wrong-password count across reboots | In the firmware (#82) | Same |
| 8 | Rollback protection | Not set | Not now |
| 9 | Settings from a file over USB (`ninja provision`) | Accepted | Refused: the board is set up on its console only |
| 10 | The board's SSH host key after its settings are wiped | A new one at the next setup (#88) | Same |
| 11 | OTP pages locked against the bootloader | Only the root secret's pages and SOS's flag page (page 3) | **Deferred** until dev-1 is done |
| 12 | Code that writes OTP | Only in a separate burn image (#87) | Same |

## The decisions

### 1. Debug port

With debug on, anyone with a probe reads the root secret out of the running chip. On dev-1 it
stays on: the board is for development, and what is tested is that the mechanisms work, not
that the board keeps a secret. On a production board it is turned off for good
(`CRIT1.DEBUG_DISABLE`), with no debug key.

### 2. USB bootloader

Kept on both, because more fixes are expected and they have to be flashable. The cost: the two
glitch attacks that go through the bootloader (E20, E21) stay open, and anyone with a cable can
erase or roll back the flash, though not read what is sealed. Disabling it later is one more
OTP write, after which the board can never be reflashed over USB. It would also remove the way
back from a forgotten console password: with the bootloader disabled, the configuration can't
be erased from outside, and the console is locked for good (SSH with a trusted key still
works).

### 3. Reboot into the bootloader on command

`ninja flash` and `tools/provision.sh` rely on it (`picotool load -f`), and on dev-1 the
sessions flash with nobody at the button, so dev-1 keeps it. On a production board
`BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` removes it, as the other workaround for E20; flashing
then needs someone to hold BOOTSEL while plugging the board in. Open questions 2 and 6 must be
closed first.

### 4. Signing keys

dev-1 uses two development keys that are published in the repository
(`keys/dev-INSECURE-1.pem` and `-2.pem`, #77), so they prove the mechanism and nothing more.
Being public is also their backup: a lost disk can't leave dev-1 unflashable. Its slots 2 and 3
stay blank, which leaves room to add a key and equally lets anyone with a cable add theirs.

A production board gets two keys made away from the development machine, never on a machine
where AI sessions run, each with an offline backup, and its unused slots marked invalid. How to
make them is in the [production runbook](production-runbook.md#make-the-signing-keys).

### 5. Glitch detector (OTP bit deferred)

**Decided:** the firmware arms the detector at every boot, at sensitivity 3, on every board
(#78). **Deferred:** arming it in OTP, and at which sensitivity. Once armed in OTP it can't be
turned off, and a detector too sensitive for the board would reset a healthy board for ever
(see [design.md](design.md#the-glitch-detector)).

**Evidence so far.** One soak on dev-1 (2026-11-25, #78): armed by software at sensitivity 3
for 3 hours, with 574 SFTP cycles of 1 MB written to and read back from the encrypted card. No
detector reset. What that does not show:

- One board (an A2), for 3 hours, not days.
- Powered over USB from a computer. Nothing about battery power or another supply.
- Indoors at room temperature, not measured. Nothing about heat or cold.
- Wi-Fi close to a strong access point (about -40 dBm). Nothing about transmitting at full power
  on a weak signal, which loads the supply more.
- Sensitivities 0 to 2 were not run; a clean run at 3 implies them only if the detectors behave
  monotonically.
- Software arming starts a second or two into boot, so the early boot was never covered. The
  OTP flag arms it from reset.
- The production board is a different board. Its own tolerance is unknown until it is armed on
  it, and by then the bit is burned.

**Gathered in passing (2026-11-25).** Rather than a multi-day soak now, SOS arms the detector
by default so that ordinary development use shows whether it resets a healthy board. A
detector reset nobody forced is recorded on #100 (`docs/development.md`, "Glitch detector
resets"). That is evidence, not a test: it says nothing about the conditions nobody happens to
run.

**The week-long test** comes later, with a more complete feature set, a spare board and battery
hardware. It records the chip's temperature and supply voltage throughout and with each
detector event (2026-11-25; the firmware is in place, #92), so a reset can be set against how
warm the chip was or a sagging battery. The chip's own temperature sensor and the board's
supply voltage (VSYS) are each read every 10 s, with whether USB power is present; readings are
logged with every detector trigger and kept across a detector reset; a line goes to the log at
boot, on each degree or 0.2 V of change, when USB power comes or goes, and at least hourly,
which a serial logger on the console records for the length of the test; `temp` and `power`
show the lowest and highest since boot. Limits when reading those numbers:

- The temperature is the chip's, not the room's, and moves with what the chip is doing.
- Uncalibrated, it is good for change over time on one board, not for an absolute figure or for
  comparing two boards.
- The supply reading is within a few percent, and one taken during a Wi-Fi transmission catches
  its dip (about 0.13 V on USB power on dev-1), so the lowest can be a brief one.

Its other conditions (load, power, how the temperature is varied, what counts as a pass) are
still to be defined.

**Proposed rule for the OTP bit** (not yet decided): one board, on one supply, justifies at
most "the highest setting that stayed clean, minus one". Arm it at that setting; if even the
lowest setting resets a healthy board, leave it off. Given how the OTP field can change, the
starting value matters more than "we can raise it later" suggests.

**What this decision needs:** after the week-long test, whether to arm the detector in OTP and
at which sensitivity. Until then production step 4.5 is skipped.

### 6. Rehearsal burn

The first time the burn code ran on a chip, it ran as a rehearsal that stopped on purpose
before the secret was complete, spending one of dev-1's three pages. Only because that ended as
expected was the real secret written, into the next page. First approved as a rehearsal "in
spare OTP pages"; on 2026-11-25 it was agreed that on dev-1 the rehearsal and the real burn are
the same run, on the real page list. The reasoning is in the
[dev-1 record](../otp/dev-1/RECORD.md#the-rehearsal). A production board has no rehearsal of
its own: dev-1 was it.

### 7. Wrong-password count across reboots

The delay after wrong console passwords survives a reboot (#82). While the bootloader is kept,
someone with a cable can still erase the count, so it slows down guessing only for someone who
can't or won't do that.

### 8. Rollback protection

Not set. With the bootloader kept, anyone can flash any older image signed with the same key,
including one with a flaw found since, or one from before the console-only marker (#84).
Preventing that costs OTP bits per version step (`BOOT_FLAGS0.ROLLBACK_REQUIRED`). Rolling back
the sealed configuration sectors is accepted too (#64).

### 9. Settings from a file over USB

A board normally accepts a settings file written to its flash over USB, which is how `ninja
provision` sets up dev-1. On a secured board that is a way in: anyone with a cable could load
settings that name their own SSH key. So a board can be switched, once and permanently, to
refuse such files and take its settings only from `setup` on its console: **console-only
setup** (#84; called "sealed-only" before #91). It is one OTP setting of its own, separate from
secure boot, which is why dev-1 can have secure boot without it. Decided 2026-11-25: the switch
is an OTP marker, not secure boot (why OTP: [design.md](design.md#knowing-a-board-is-secured-the-console-only-marker-84)).
dev-1 is not switched. A production board is.

### 10. The board's SSH host key after its settings are wiped

Erasing a board's configuration makes its SSH host key unreadable, and the next setup makes a
new one (#88). Otherwise someone who wiped a board and set it up as theirs would answer SSH as
the old board. The costs, accepted: the owner's own `config reset` changes the board's SSH
identity and clients warn; losing both configuration copies loses the SSH host key too. An
open board kept its existing host key when #88 landed (taken over once, same fingerprint); a
console-only board does not take over a key from before #88.

### 11. OTP pages locked against the bootloader (production deferred)

Found in review of #86. With the bootloader kept, `picotool otp set` and `otp load` still work
on a secured board for every page whose lock word allows the bootloader to write; secure boot
doesn't stop that, because the bootloader is the boot ROM's own code. Someone with a cable could
then, without opening the case:

- mark both boot keys invalid, after which the board never starts any firmware again;
- on a board with blank key slots, write a key of their own and mark it valid, after which the
  board runs their firmware, and their firmware reads the root secret;
- set further bits in `CRIT1` or the boot flags, or lock pages away from the firmware.

**dev-1:** only the root secret's pages (locked by the burn) and page 3 are locked; the
boot-flag and boot-key pages stay open for later OTP tests, knowingly. Anyone with a cable can
add a boot key to a blank slot and run their own firmware, or mark the dev keys invalid and end
the board's life. Neither loses anything that matters on this board.

**Production board: deferred** (2026-11-25), to be taken up when there is one. Locking pages 0
to 3, the remaining list pages and the lock pages 62 and 63 against the bootloader removes the
cable-only attack. It is not a full stop: on an A2 the bootloader's own lock check is what E21
defeats by glitching. What has to be weighed: page 1 holds `CRIT1` and the boot flags, and
three decisions count on a later `picotool` write to them:

- decision 2: disabling the USB bootloader later (`BOOT_FLAGS0.DISABLE_BOOTSEL_*`);
- decision 5: the glitch detector bit, after the week-long test (`CRIT1`);
- decision 8: rollback protection later (`BOOT_FLAGS0.ROLLBACK_REQUIRED`).

So either page 1 is locked only once those three are settled, and stays exposed to a cable
until then; or it is locked now and each of them later needs a signed image written for the
purpose, which writes the bit as Secure code (if the page's Secure lock is left open for that).
The same goes for page 2 and a later change of boot keys. Also: the lock check in the firmware
(#86) is no proof of history. Lock words can be written from the bootloader, so a board that
was ever out of its owner's hands unlocked can't be shown clean by reading them.

### 12. Code that writes OTP

The firmware a board runs every day has no code that can write OTP, so no bug in it can reach
OTP. The one command that writes the root secret is in a separate burn image, flashed for that
job and replaced by the everyday firmware afterwards (#87). The cost is one more build and two
more flashes, once per board. It does not protect against someone with a cable, who can write
OTP with `picotool` anyway; it is about the firmware's own mistakes. The alternative, one
firmware with the command always present, was considered and not chosen.

## Open questions

Nothing is burned on a production board while any of these is open. The numbers are the ones
used on #64.

- **1.** **The exact `picotool` commands and file contents for each production write.** dev-1's are
  in [`docs/otp/dev-1/`](../otp/dev-1/README.md) and all ran on 2026-11-26: `picotool otp load`
  accepted the boot key file, each `otp set` (KEY_VALID, SECURE_BOOT_ENABLE, the page 3 lock
  word 0x101010) wrote what its read-back expected, and the OTP dumps before and after differ
  in exactly those rows. A production board's are written when there is one: other keys, and
  the rows dev-1 leaves out. Also which rows and values step 4.8 locks, and whether Secure code
  should also be denied writes to pages 0 to 2 (decision 11).
- **2.** That `BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` leaves SOS's fault record and watchdog reset
  detection working. They use watchdog scratch registers 0 to 3 and the watchdog's reason
  register; the datasheet says the flag makes the boot ROM ignore the scratch registers at
  boot.
- **5.** Glitch detector: the week-long test's results, and that the detector's `LOCK` register stops
  software from disarming it.
- **6.** Whether anything in SOS relies on rebooting into BOOTSEL by command, beyond flashing.

**Closed:**

- **3.** How Secure code reads and soft-locks an OTP page, and that the bootloader lock (`LOCK_BL`) keeps
  `picotool` out on A2 other than through the E21 glitch: #86, and on dev-1 reads of rows
  0x100 and 0x140 from BOOTSEL were refused with `permission failure`. The E21 glitch was not
  attempted.
- **4.** Raw row writes from Secure code, for the chaff: host tested (#87), then ran on dev-1 on
  2026-11-26 in the rehearsal (page 4) and the burn (page 5).
- **7.** Which OTP pages the root secret uses: pages 4 to 6. Page 3 is reserved for SOS's own flags
  (the console-only marker has its first three rows).
- **8.** The SSH host key after a wipe: decision 10, built in #88.
