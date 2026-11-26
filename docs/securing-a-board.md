# Securing a board (draft procedure)

**Status: draft. The owner confirmed the decisions below on 2026-11-25. Nothing in it has been
run, and nothing in it may be run until the firmware it lists exists, the "to confirm" list is
closed, and the owner approves the procedure itself.** It covers steps 3 and 4 of
[issue #64](https://github.com/j3m7/sos/issues/64): the root secret in one-time-programmable
(OTP) memory, secure boot, and debug off. Every OTP write is permanent.

Sources: the RP2350 datasheet in `docs/rp2350/` (sections 10.5, 10.9, 13.4, 13.5, 13.8 and
appendix D), `picotool otp list` from picotool 2.3.1 for the field names, and Raspberry Pi's
post ["Security through transparency: RP2350 Hacking Challenge results are
in"](https://www.raspberrypi.com/news/security-through-transparency-rp2350-hacking-challenge-results-are-in/). Items I could not confirm
from those are marked *to confirm* and collected at the end.

## What a secured board gives, and what it doesn't

The production board will be an RP2350 **A2**, because A4 boards can't be bought at the moment.
That limits what this buys.

| Attacker | A secured A2 board |
|---|---|
| A PC and a USB cable | Stopped: only signed firmware runs, the flash holds sealed records, the root secret can't be read over USB |
| A debug probe (SWD) | Stopped: debug is disabled |
| A flash reader, or rewriting the flash chip | Reads only sealed records; can erase or roll back the configuration, which locks the cards (erase) or restores an older state (rollback, accepted in #64) |
| Glitching equipment (voltage or laser fault injection) | **Not stopped.** Four published attacks apply to A2: reading OTP through a disrupted supply (E16, no workaround), running unsigned code through the bootloader's reboot call (E20), reading locked OTP pages in BOOTSEL (E21), and swapping the flash contents during the signature check (E24, no workaround, also on A3) |
| Someone who can solder a second flash chip behind a switch | **Not stopped, on any chip revision.** SOS runs from external flash. After the bootrom has checked the signature, the flash the chip reads from can be switched to another one, with no glitching at all. The attacker's code then runs as Secure code and can read the root secret. Raspberry Pi's security white paper warns of this and says secure code should run from RAM; SOS is too large for that. Closing it is the "Later" section of #64 |
| A lab that can decapsulate the chip | Not stopped (OTP imaging); storing the secret with chaff makes it harder |

E20 and E21 have workarounds that cost features; they are decisions 2 and 3 below (the first deferred, the second taken). E16 and E24
have none on A2. So a secured A2 raises the bar from "anyone with a cable" to "someone with a
soldering iron and a second flash chip, or with fault-injection equipment", and no further.
Moving to an A4 board later removes the four glitch attacks. It does not remove the flash swap.

## Decisions (owner, 2026-11-25)

| # | Decision | Decided |
|---|---|---|
| 1 | **Debug** | **Off for good:** `CRIT1.DEBUG_DISABLE`. No debug key |
| 2 | **USB bootloader (BOOTSEL) on the secured board** | **Kept, for now.** More fixes and improvements are expected, and they have to be flashable. Consequences accepted: the two glitch attacks that go through BOOTSEL (E20, E21) stay open, and anyone with a cable can erase or roll back the flash, though not read what is sealed. Disabling it later remains possible: it is one more OTP write (`BOOT_FLAGS0.DISABLE_BOOTSEL_USB_PICOBOOT_IFC`, `..._USB_MSD_IFC`, `..._UART_BOOT`), after which the board can never be reflashed over USB |
| 3 | **Bootrom reboot types** | **`BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` is set** (the other E20 workaround). Cost accepted: the secured board no longer reboots into BOOTSEL on command, which `ninja flash` and `tools/provision.sh` (`picotool load -f`) rely on. Flashing it needs someone to hold the BOOTSEL button while plugging it in. SOS's own fault record uses other scratch registers and should be unaffected (*to confirm*) |
| 4 | **Signing keys** | **Two:** `BOOTKEY0` in use and `BOOTKEY1` as a spare, generated away from the development machine, each with an offline backup. Slots 2 and 3 are marked invalid. Instructions below |
| 5 | **Glitch detector** | **Not burned yet; open until a week-long test.** The owner's decision (2026-11-25): no multi-day soak for now. Every board running SOS arms the detector by software at boot, at the highest sensitivity (#78; `glitch off` turns it off), and whoever works with a board reports any detector reset. A week-long torture test follows once the feature set is more complete and there is a spare board and battery hardware; the OTP bit is decided after that. See "The glitch detector" below |
| 6 | **A rehearsal on the development board** | **Yes:** the root-secret burn is rehearsed on dev-1 in spare OTP pages before the production board, including a read attempt from BOOTSEL and one burn interrupted on purpose. This is the owner's word for those OTP writes on dev-1 and for nothing else in OTP |
| 7 | **Wrong-password count across reboots** | **Yes, add it.** Known limit: while the USB bootloader is kept (decision 2), someone with a cable can rewrite the flash and reset a counter stored there, so it slows down guessing at the console only for someone who can't or won't do that. It becomes a real limit if the bootloader is disabled later |
| 8 | **Firmware rollback** | **Not now (confirmed by the owner, 2026-11-25).** With the bootloader kept, anyone can flash any older image signed with the same key, including one with a flaw found since. Rollback versions in OTP (`BOOT_FLAGS0.ROLLBACK_REQUIRED`) would prevent it, at the cost of OTP bits per version step |

## The glitch detector

**What it is.** Four small circuits spread over the chip that notice when the processor's clock
or core voltage is disturbed enough for logic to misbehave, which is what a voltage or clock
glitch attack does on purpose. When one fires, the chip resets itself within one clock cycle,
instead of carrying on in a state the attacker chose. A flag (`CHIP_RESET.HAD_GLITCH_DETECT`)
then tells the firmware why it restarted, and `TRIG_STATUS` which detector fired. (Datasheet
section 10.9.)

**The two settings.**

- `CRIT1.GLITCH_DETECTOR_ENABLE` arms it from the first instant after reset. Once written to OTP
  it can't be turned off.
- `CRIT1.GLITCH_DETECTOR_SENS`, 0 to 3, sets how easily it fires. Each step makes the detectors
  react to a smaller disturbance (the delay they measure against goes from 75% to 120% of the
  clock period in steps of 15%). It is a two-bit OTP field, and OTP bits only go from 0 to
  1: from 0 any setting can still be reached, from 1 only 3, from 2 only 3, and nothing can be
  lowered. So "start low and raise it later" works from 0, but from 1 the only step left is
  straight to the highest.

**What it costs.** A detector that is too sensitive for the board resets a healthy board:
spurious restarts under load, when the Wi-Fi radio transmits, or on a marginal USB supply. A
reset from the detector also clears the watchdog scratch registers, where SOS keeps its record
of the last fault, so that record would be lost for such a reset. And it is permanent: a board
that turns out to reset itself can't have the detector switched off.

**What it buys on this board.** It makes voltage glitching harder. It does not make it
impossible, and it doesn't cover the other methods. From Raspberry Pi's write-up of the hacking
challenge results (January 2025):

- On voltage glitches, reporting Hextree's tests: "at the highest sensitivity setting, the
  glitch detectors can detect many voltage glitches; however, the rate of undetected glitches
  is still high enough to make attacks feasible with some effort."
- Raspberry Pi's own conclusion: "we've revised downward our estimate of the effectiveness of
  our glitch detection scheme".
- The same post describes attacks the detectors don't see at all: electromagnetic pulses from a
  small coil on top of the chip (the E21 attack) and a laser (E24) act on one spot and don't
  disturb the detectors; the E16 attack disturbs the OTP supply, not the core supply.

It does nothing against the flash swap or against reading the chip in a lab. So on an A2 the
detector removes none of the published attacks; it makes the voltage-glitch ones (E20) take
more attempts.

**Software can disarm it.** The detector block has a `DISARM` register. A secured board's
firmware should lock the detector's registers at boot (its `LOCK` register), so a bug can't be
used to switch the detector off (*to confirm* against the register description).

**It can be tried without burning anything.** The detector can also be armed by software, with
its `ARM` register, and its sensitivity set with the `SENSITIVITY` register. So a test build for
the development board can arm it at each sensitivity in turn and run the board's normal work
(Wi-Fi traffic, SSH, SFTP, the card, a Lua script) for hours, counting detector resets. That
tells us which settings this board design tolerates before any OTP bit is spent.

**What I'd do.** Run a soak test at all four sensitivities, for days, not hours (the owner has
put this off until there is a spare board: see below).
It is one board, at room temperature, on one power supply, so the most it justifies is "the
highest setting that stayed clean, minus one". Then burn "enabled" on the production board at
that setting; if even the lowest setting resets a healthy board, leave it off. Given how the
OTP field can change (above), the choice of starting value matters more than "we can raise it
later" suggests. The firmware should report a detector reset in the
log and in `uptime`, as it does for a watchdog reset.

**What has been run so far (2026-11-25, #78).** One soak on dev-1: armed by software at
sensitivity 3, the highest, for 3 hours, with 574 SFTP cycles of 1 MB written to and read back
from the encrypted card. No detector reset.

**What runs from now on (the owner's decision, 2026-11-25).** No multi-day soak yet. Instead
SOS arms the detector by software at every boot, at sensitivity 3, by default (#78), so that
ordinary development use shows whether it resets a healthy board. `glitch off` turns it off if
it becomes a problem. A detector reset nobody forced is recorded on #64 (`docs/development.md`,
"Glitch detector resets"). That is evidence gathered in passing, not a test: it says nothing for
the conditions nobody happens to run. The week-long torture test comes later, with a more
complete feature set, a spare board and battery hardware.

**What that soak does not show.** The OTP setting can't be undone, so its limits belong next to
the decision:

- One board (an A2), for 3 hours, not days.
- Powered over USB from a computer. Nothing about battery power or another supply.
- Indoors at room temperature, not measured. Nothing about heat or cold.
- Wi-Fi close to a strong access point (about -40 dBm). Nothing about transmitting at full power
  on a weak signal, which loads the supply more.
- Sensitivities 0 to 2 were not run; a clean run at 3 implies them only if the detectors behave
  monotonically.
- Software arming starts a second or two into boot, so the early boot was never covered. The
  OTP flag arms it from reset.
- The production board is a different board. Its own tolerance is unknown until step 4.5 is
  checked on it, and by then the bit is burned.

**The decision this needs:** after the week-long test, whether to enable the detector in OTP
and at which sensitivity. Until then step 4.5 is not done.

## Signing keys: how to make them

The boot key is a secp256k1 key pair. The private half signs firmware; a hash of the public
half goes into the board's OTP. Whoever holds a private key can make firmware a secured board
will run, and if both private keys are lost the board keeps its last firmware for ever.

Do this on a machine that is not the development machine, ideally one that is offline, and not
in a session with an AI assistant: the private keys should never be on a machine where agents
run.

1. Make the two keys, readable only by you:

   ```sh
   umask 077
   openssl ecparam -name secp256k1 -genkey -noout -out sos-boot0.pem    # the key in use
   openssl ecparam -name secp256k1 -genkey -noout -out sos-boot1.pem    # the spare
   ```

2. Export the public halves. These are not secret, and are what the procedure needs from you:

   ```sh
   openssl ec -in sos-boot0.pem -pubout -out sos-boot0.pub.pem
   openssl ec -in sos-boot1.pem -pubout -out sos-boot1.pub.pem
   ```

3. Compute each key's OTP hash: the SHA-256 of the public key's 64 raw bytes (X then Y).

   ```sh
   openssl ec -in sos-boot0.pem -pubout -outform DER | tail -c 64 | sha256sum
   openssl ec -in sos-boot1.pem -pubout -outform DER | tail -c 64 | sha256sum
   ```

   Write both hashes down. This is the first of the two independent computations the checklist
   asks for. The second is `picotool`'s: the `bootkey0` values it writes when asked to sign
   with that key (that file also carries the bit that enables secure boot, so it is read for
   the comparison and then deleted, never loaded). I checked with a throwaway key that the two
   methods give the same 32 bytes.

4. Back up each private key offline, on two separate media kept in different places (a USB
   stick and a printed copy, for example), and read each backup back once to see that it is
   complete. Keep the spare key's copies apart from the first key's.

5. Signing a build needs `sos-boot0.pem` on the machine that builds it, as `SOS_SIGN_KEY`
   (`docs/development.md`, "Signed builds"). Decide where that happens. The simplest safe
   arrangement: release builds for the secured board are built and signed on the machine that
   holds the key, and the development machine never has it. `sos-boot1.pem` stays offline
   until the first key is lost or has to be retired.

6. If a private key leaks: sign new firmware with the other key, and write
   `BOOT_FLAGS1.KEY_INVALID` for the leaked key's slot on the board. That is permanent, and
   leaves the board with one key.

## What has to be built first (reversible, ordinary pull requests)

None of this exists yet. Each item is firmware or tooling work with host tests and review, done
and merged before anything is burned.

1. **The key provider's OTP backend.** Reads the root secret from an OTP page as Secure code,
   and on `keyprov_lock()` advances that page's Secure lock in the `SW_LOCK` register so nothing
   can read it until the next reset. Today's backend reads a flash sector.
   - **Which backend a board uses depends on whether a valid root secret is in OTP, and on
     nothing else.** Not on secure boot: the board is set up in step 3.5, after the burn and
     before secure boot is on, and what is sealed then has to open after step 4.
   - **Finding the page.** The firmware has a short fixed list of pages (say three), tried in
     order. A page holds a complete secret only if a marker row, written last, is there and a
     check over the secret matches; the secret alone has no magic number. A page that is
     neither blank nor complete is spent and is skipped. A blank page ends the search: no
     secret yet.
2. **Writing the root secret, on the board.** A console command (working name `rootsecret
   burn`) that takes 32 bytes from the TRNG and writes them to the first blank page of the list.
   The secret never leaves the board and is never shown. In order:
   - the data rows, with error correction;
   - the chaff the datasheet recommends against imaging (section 13.8.2): for each data row
     *i*, read back its **raw** 24 bits, error-correction bits included, and write their
     bitwise complement as a **raw** row at 32 + *i*. Chaff rows are never read as
     error-corrected rows;
   - read everything back and compare;
   - the marker row and check, last;
   - the page's locks: bootloader inaccessible, Non-secure inaccessible, Secure read-only
     (which also stops a later bug from rewriting the page). On an A2 the bootloader lock is
     the one erratum E21 defeats by glitching; its workaround is disabling the USB bootloader, which decision 2 leaves for later.

   **It is not part of everyday firmware.** It is built only into a separate image for step 3
   (a CMake option, off by default), flashed for the burn and replaced afterwards, so boards in
   use carry no code that writes OTP. And it needs someone present: besides the console
   password and the board's serial typed in, a physical action while it runs (holding the
   BOOTSEL button, or a jumper on a pin).
3. **Knowing it is secured.** The firmware reads `CRIT1.SECURE_BOOT_ENABLE` at boot. When it
   is set the firmware refuses unsealed configuration and host-key records (`refuse_plain`,
   already implemented and off) and refuses `rootsecret forget`. `version` shows both facts:
   where the root secret is, and whether secure boot is on.
4. **Checking the boot key on the board:** the `bootkey` command (#79). It hashes the public
   key from the running image's own signature block, compares it with the boot key slots in
   OTP, and says `MATCH`, `NOT YET` (slot written, not marked valid), `NO MATCH` or
   `NO VERDICT`. It reads OTP only through the read aliases. Step 4.7 depends on it. Its
   `MATCH` path can't be run before a slot is written, so step 4.2 is its first real run; a
   mistake in it would show as `NO MATCH` with the board still open.
5. **A wrong-password count that survives reboots** (decision 7), kept in flash, so the delay
   after wrong console passwords doesn't start again at every boot. The count is stored
   *before* the password is checked and cleared after a right one: otherwise cutting the power
   after a wrong answer would give free guesses.
6. **The glitch detector by software** (decision 5): done in #78. A setting arms the detector
   at a chosen sensitivity (by default the highest) or only watches it, and detector resets
   are reported in the log, `uptime` and `glitch`. No OTP. Still to build: the week-long test
   itself (load, power and temperature conditions, and what counts as a pass).
7. **The signing key in the build** is already there (`SOS_SIGN_KEY`, #70). The build
   deliberately produces no OTP file.
8. **The OTP settings as files we write by hand,** one per stage below, in the form
   `picotool otp load` takes, checked into the repository without any key material and reviewed
   like code. Not the single file `picotool seal` can produce, which enables secure boot in the
   same load as the key hash.

Moving a board's root secret from flash to OTP changes every derived key. The board's sealed
configuration and host key become unreadable and its cards locked. So the production board is
set up fresh after step 3, never before.

## Before touching the production board

- [ ] The owner has approved this document. Decision 5 is settled only after the week-long
      test; until then step 4.5 is left out.
- [ ] All firmware in the previous section is merged, and the build that will be burned is
      tagged.
- [ ] The rehearsal (decision 6) has been done and written up, if chosen.
- [ ] The board's serial and chip revision are recorded from `version`, and added to the board
      registry as the production board.
- [ ] `picotool otp dump` of the blank board is saved, as the record of its state before.
- [ ] The signing keys exist, each has an offline backup that has been read back once, and
      the hash that will go into `BOOTKEY0` has been computed twice, independently, from the
      public key, and the two agree.
- [ ] The tagged firmware, signed with the `BOOTKEY0` key, **boots and works on this board
      while it is still open** (`picotool info -a` shows the signature verified; `version`,
      Wi-Fi and SSH work).
- [ ] Only this board is connected over USB. Every `picotool` command names its serial with
      `--ser`.
- [ ] The owner runs every OTP write themselves, or names each one for the session to run. The
      deny rule on `picotool otp set`, `load`, `permissions` and `white-label` stays in place
      otherwise.
- [ ] Stable power; nobody else is using the board.

## Step 3: the root secret into OTP

| # | Action | Check before going on | Can it be undone? |
|---|---|---|---|
| 3.1 | Flash the signed image that has the burn command (built from the same tag) | `version` shows the tag; no root secret in OTP; secure boot off | yes |
| 3.2 | Run the burn command on the console, with the physical action | It reports the page written, read back, marked and locked | **no** |
| 3.3 | Flash the everyday signed firmware (same tag, no burn command) and reboot | `rootsecret` reports the secret present in OTP. The log shows the key provider locked the page at the end of boot | yes |
| 3.4 | From BOOTSEL, try to read the page with `picotool otp get` | It is refused or returns nothing. `picotool otp dump` shows no secret in that page | |
| 3.5 | `setup` on the console (password, network, trusted key), then format the card | Configuration saves and loads across a reboot; the card mounts | yes |

If 3.2 fails partway (power lost, a read-back mismatch), that page is spent: it has no marker,
so the firmware skips it, and the command is run again for the next page of the list. With the
list used up the board can't be given a root secret by this firmware. This is part of what the
rehearsal tests.

At the end of step 3 the board still boots anything and still has debug: the secret can be read
by loading other firmware. It is protected only once step 4 is done. **Steps 3 and 4 are done in
one sitting, and the board is not left unattended between them.**

## Step 4: secure boot and debug off

The order is the datasheet's (section 10.5), with a check after each write. `SECURE_BOOT_ENABLE`
is last, because with it set and a wrong key the board can never run code again.

| # | Write | Check before going on | If it's wrong |
|---|---|---|---|
| 4.1 | The hash of the signing key into `BOOTKEY0_0` to `_15`; the spare key's hash into `BOOTKEY1_0` to `_15` | Read the rows back and compare with the two independently computed hashes | A wrong hash in a slot can be abandoned by marking that slot invalid in 4.3, as long as the other slot is right |
| 4.2 | `BOOT_FLAGS1.KEY_VALID` for slots 0 and 1 | Read back. The signed firmware still boots (secure boot is not on yet, so this proves nothing about the key by itself). **Run the on-board key check (`bootkey`, #79): it must say `MATCH` for the image that is on the board now.** `MATCH` compares the key's hash only, so also confirm the signature itself: `picotool info -a` on the image read back from the board must say `signature: verified`. `NOT YET`, `NO MATCH` and `NO VERDICT` all stop the procedure | A "no match" stops the procedure here, with the board still open |
| 4.3 | `BOOT_FLAGS1.KEY_INVALID` for slots 2 and 3 | Read back | |
| 4.4 | `BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` (decision 3). The USB bootloader is kept (decision 2), so none of the `DISABLE_BOOTSEL_*` flags | Read back. Reboot. `picotool load -f` no longer brings the board into BOOTSEL; holding the BOOTSEL button while plugging in does, and a signed image can still be loaded that way. SOS's fault record and watchdog reset detection still work | |
| 4.5 | `CRIT1.GLITCH_DETECTOR_ENABLE` and sensitivity, **only if decision 5 says so**, at the setting the week-long test supports | Read back. Reboot several times; run the board under normal load (Wi-Fi, SSH, card) with no detector resets | A detector that resets a healthy board can't be turned off again |
| 4.6 | `CRIT1.DEBUG_DISABLE` (decision 1) | A debug probe can no longer attach | |
| 4.7 | **`CRIT1.SECURE_BOOT_ENABLE`**, only after the on-board key check has said "match" again, for the image on the board at this moment | The board boots the signed firmware. `version` shows secure boot on. What was set up in 3.5 still loads | If the key in OTP isn't the key the image is signed with, the board is dead. This is why the key is compared off the board twice (4.1) and on the board twice (4.2 and here) |

After 4.7 the RISC-V cores are disabled for good as well; SOS doesn't use them.

## Afterwards: trying the attacks

The table at the top of the issue, run against the secured board, with results recorded on the
issue.

| Attempt | Expected |
|---|---|
| Attach a debug probe | Refused |
| Load an unsigned build over USB (if the bootloader was kept) | The bootrom won't run it |
| Load a build signed with another key | The bootrom won't run it |
| Load the signed build | Runs |
| `picotool otp get` and `otp dump` on the root-secret page | Refused or empty |
| `picotool save` of the whole flash | Succeeds (if the bootloader was kept), and shows only sealed records: no network name, no key comment, no host key |
| Write an unsealed configuration image (`ninja provision`) | The board ignores it and keeps its sealed configuration |
| Erase the configuration sectors over USB | The board starts as new, with an open console asking for a password, and the old card stays locked |
| Put back an older copy of the sealed configuration sectors | It loads: rollback is accepted in #64 |
| `picotool otp load` and `picotool otp set` of a harmless row | Record what the chip answers |
| Console without the password | Nothing but the prompt |

Not possible to try here: the glitch attacks, the flash swap and chip imaging.

## What can't be undone, in one place

- Every OTP bit written. In particular: the root-secret page and its locks, the boot key
  hashes, the disabled boot paths, the glitch detector, debug off, secure boot on.
- A lost signing key (both of them) means the board keeps its last firmware for ever. A leaked
  one means anyone can sign firmware for it; the remedy is to mark that slot invalid, which
  works only while the other key is safe.
- A forgotten console password on a secured board: erase the configuration over USB (if the
  bootloader was kept) and start again, losing the cards. With the bootloader disabled there
  is no way to erase it from outside, and the board is locked out of its own console for good
  (SSH with a trusted key still works). That is a cost to weigh if the bootloader is ever disabled (decision 2).

## To confirm before this is final

This list is a gate: the document is not final, and nothing is burned, while any item is open.

1. The exact `picotool` commands and file contents for each write. I have the field names from
   `picotool otp list`; I have not run any OTP write, and the selector and JSON syntax must be
   checked against a dry run (`picotool otp list` with the same selectors, and the rehearsal).
2. That `BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` leaves SOS's fault record and watchdog reset
   detection working. They use the watchdog scratch registers 0 to 3 and the watchdog's reason
   register; the datasheet says the flag makes the bootrom ignore the scratch registers at boot.
3. How Secure code reads and soft-locks an OTP page in the SDK version we use, and that the
   bootloader lock (`LOCK_BL`) keeps `picotool` out on A2 other than through the E21 glitch.
4. Writing raw rows from Secure code: the bootrom or SDK call for a raw (not error-corrected)
   row write and read, used for the chaff rows.
5. Glitch detector: the week-long test's results; and that the detector's `LOCK` register
   stops software from disarming it.
6. Whether anything in SOS relies on rebooting into BOOTSEL by command, beyond flashing.
7. Which free OTP page to use (pages 3 to 60 are free for user data), and that a failed write
   leaves the next page usable.
