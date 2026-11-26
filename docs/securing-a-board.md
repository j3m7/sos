# Securing a board (draft procedure)

**Status: not yet approved by the owner, and nothing in it has been run. No OTP write has been
made on any board.** The owner confirmed the decisions below on 2026-11-25. The firmware it
lists is merged and the part for the development board ("Variant for the development board",
with its files in `docs/otp/dev-1/`) has been reviewed; that part waits for the owner's approval
and is what is run first. The production board's part is not final: it waits for dev-1 to be
tested and working, by the owner's word, and has questions deferred until then. **Nothing in
this document may be run until the owner approves it and names each write.** It covers steps 3
and 4 of
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
| 6 | **A rehearsal on the development board** | **Yes:** the root-secret burn is rehearsed on dev-1 in spare OTP pages before the production board, including a read attempt from BOOTSEL and one burn interrupted on purpose. This is the owner's word for those OTP writes on dev-1 and for nothing else in OTP. Since then the owner has chosen to run steps 3 and 4 on dev-1 itself: see "Variant for the development board (dev-1)" |
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

1. **The key provider's OTP backend** (#86, in review). Reads the root secret from an OTP page
   as Secure code, and on `keyprov_lock()` advances that page's lock in the `SW_LOCK` register
   so nothing can read it until the next reset. The page list is pages 4, 5 and 6; the format
   of a page is in `src/core/roototp.h` and is what the burn command must write.
   - **Which backend a board uses depends on whether a valid root secret is in OTP,** not on
     secure boot: the board is set up in step 3.5, after the burn and before secure boot is
     on, and what is sealed then has to open after step 4.
   - **On a sealed-only board the root secret comes from OTP or not at all.** The flash record
     is never read, never created and never a fallback there. Otherwise the marker would be
     hollow: someone with a cable could write a root secret record of their own into flash,
     with a configuration sealed under it, and the board would load both as its own.
     Refusing unsealed records (#84) doesn't cover that; this rule does, with a host test. A
     sealed-only board with no usable secret in OTP has no keys and keeps its console locked.
   - **Finding the page.** The firmware has a short fixed list of pages (say three), tried in
     order. A page holds a complete secret only if a marker row, written last, is there and a
     check over the secret matches; the secret alone has no magic number. A page that is
     neither blank nor complete is spent and is skipped. A blank page ends the search: no
     secret yet.
2. **Writing the root secret, on the board** (#87, in review; never yet run on a chip). A
   console command (`rootsecret burn`, with `plan` and `rehearse`) that takes 32 bytes from the TRNG and writes them to the first blank page of the list.
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
     the one erratum E21 defeats by glitching; its workaround is disabling the USB bootloader, which decision 2 leaves for later;
   - **the locks of every list page before it,** which are spent pages: at least "bootloader
     read-only", and Secure read-only. A spent page holds 32 bytes and no marker. Left
     writable from the bootloader, someone with a cable could read those bytes, write the
     check rows and the marker, and so make it the board's root secret, one they know; on a
     sealed-only board they could then load a configuration sealed under it. The firmware
     checks these locks at every boot (#86): an open board warns, a sealed-only board refuses
     the secret.

   **It is not part of everyday firmware.** It is built only into a separate image for step 3
   (a CMake option, off by default), flashed for the burn and replaced afterwards, so boards in
   use carry no code that writes OTP. And it needs someone present: besides the console
   password and the board's serial typed in, a physical action while it runs (holding the
   BOOTSEL button, or a jumper on a pin).
3. **Knowing it is secured: a marker of its own, not secure boot** (the owner's decision,
   2026-11-25). A board refuses unsealed configuration and host-key records (`refuse_plain`,
   already implemented and off) and refuses `rootsecret forget` when its *sealed-only marker*
   is set. `SECURE_BOOT_ENABLE` alone does not do it, so that a development board with secure
   boot can still be provisioned from a file. `version` shows the three facts: where the root
   secret is, whether secure boot is on (#79), and whether the board is sealed-only.

   **The marker is one OTP row that SOS defines** (agreed by the owner, 2026-11-25), in a free
   page of its own, written on a production board as step 4.6a and never on a development
   board. Why OTP and not the alternatives:
   - *A build option* would make "sealed-only" a property of the image. Any image without it
     that the board's key has ever signed (an older one, a development one) would then accept
     an unsealed record again.
   - *A field in the sealed configuration* disappears when someone erases the configuration
     over USB, which is exactly when it is needed.
   - *An OTP row* can't be cleared by rewriting the flash, and is the same for every image.
   Built in #84: rows 0x0c0, 0x0c1 and 0x0c2 (the start of page 3), each the raw 24-bit value
   0x5ea1ed, read by a two-of-three vote through the read aliases of #79. It fails closed:
   only blank rows mean "not sealed-only"; rows that can't be read, or that are neither blank
   nor the marker, count as **set**. Firmware older than #84 ignores the marker; that is the
   rollback case decision 8 leaves open. Page 3 is therefore taken: the root secret's page
   list starts elsewhere.
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
so the firmware skips it. With the list used up the board can't be given a root secret by this
firmware.

**A burn that ends in an error is a full stop, not a cue to run it again.** The command says
"STOP HERE" and the image refuses further runs until the board is restarted. The cause is
found first: a fault in how OTP is written (the boot ROM call, the read-back) would spend the
second and third page exactly as it spent the first. The one error that isn't a stop is
"the button wasn't held". If the very first write fails and nothing landed, the page is left
blank and unlocked, and is the page the next run uses; that is still a stop.

If the board restarts during a burn while the BOOTSEL button is held, it comes up in the USB
bootloader. That is the button, not damage: `picotool reboot` brings the firmware back, and
`rootsecret burn plan` then says what state the pages are in.

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
| 4.6 | `CRIT1.DEBUG_DISABLE` (decision 1) | Between the write and the next reset `version` still shows the old state, with `WARNING: the OTP rows say otherwise`: the chip acts on what it latched at reset. That warning is expected here and only here. After a reset: `version` shows `debug port: disabled` with no warning, and a debug probe can no longer attach | |
| 4.6a | The sealed-only marker (firmware item 3): the raw value 0x5ea1ed into rows 0x0c0, 0x0c1 and 0x0c2. Production boards only. **Never before the firmware on the board has the OTP key provider with the "OTP or not at all" rule (item 1), and `rootsecret` reports the secret in OTP:** with older firmware a sealed-only board still takes its root secret from flash, which an attacker can write | Read the three rows back. **If `version` still says `sealed-only: no` after the write and a reboot, stop: do not go on to 4.7.** Reboot: `version` says `sealed-only: yes` with nothing after it about unreadable or damaged rows; the configuration from 3.5 still loads; `rootsecret forget` is refused. **Do not test it by provisioning this board:** that replaces the sealed configuration with a record the board refuses, and it comes up empty (shown on dev-1 with a test build, #84) | From here the board can only be set up on its console. A marker written partly is still "set": the rows can be completed, never cleared |
| 4.7 | **`CRIT1.SECURE_BOOT_ENABLE`**, only after the on-board key check has said "match" again, for the image on the board at this moment | Before the reset `version` still says `secure boot: off` with `WARNING: the OTP rows say otherwise`, as in 4.6. After it: the board boots the signed firmware. `version` shows `secure boot: on` with no warning. What was set up in 3.5 still loads | If the key in OTP isn't the key the image is signed with, the board is dead. This is why the key is compared off the board twice (4.1) and on the board twice (4.2 and here) |

After 4.7 the RISC-V cores are disabled for good as well; SOS doesn't use them.

### Step 4.8: lock the OTP pages that matter against the bootloader

Found in review of #86. With the USB bootloader kept (decision 2), `picotool otp set` and
`otp load` still work on a secured board for every page whose lock word allows the bootloader
to write. Secure boot doesn't stop that: the bootloader is the boot ROM's own code. Someone
with a cable could then, without opening the case:

- mark both boot keys invalid (`BOOT_FLAGS1.KEY_INVALID`), after which the board never starts
  any firmware again;
- on a board with blank key slots, write a key of their own into one and mark it valid, after
  which the board runs their firmware, and their firmware reads the root secret;
- set further bits in `CRIT1` or the boot flags (disable the bootloader, change the boot
  path), or lock pages away from the firmware.

| # | Write | Check before going on | If it's wrong |
|---|---|---|---|
| 4.8 | The lock words of pages 0, 1 and 2 (chip configuration, boot flags, boot keys), page 3 (SOS's flags), the root secret's list pages not locked by the burn, and the two lock pages 62 and 63: bootloader read-only | Read the lock words back. From BOOTSEL, `picotool otp set` of a harmless row in one of those pages is **refused**. `version`, `bootkey` and `rootsecret` still read what they did | **After this, nothing in those pages can be changed over USB, by anyone.** A wrong key hash or flag is then permanent. So it comes last, after 4.7 has been seen to work and the board has been through "Afterwards: trying the attacks" |

It is not a full stop: on an A2 the bootloader's own lock check is the one erratum E21
defeats by glitching, and the remedy for that is disabling the bootloader. What it removes is
the attack that needs only a cable and `picotool`.

Exactly which rows, the values, and whether the firmware (Secure code) should also be denied
writes to pages 0 to 2 are for the reviewed OTP files ("to confirm" item 1).

**It closes doors the earlier decisions left open, and that is for the owner to weigh.**
Page 1 holds `CRIT1` and the boot flags. Once it is locked against the bootloader, no later
`picotool` write reaches them, and three decisions count on such a write:

- decision 2: disabling the USB bootloader later (`BOOT_FLAGS0.DISABLE_BOOTSEL_*`);
- decision 5: the glitch detector bit, after the week-long test (`CRIT1`);
- decision 8: rollback protection later (`BOOT_FLAGS0.ROLLBACK_REQUIRED`).

So either page 1 is locked only once those three are settled, and stays exposed to a cable
until then; or it is locked now and each of them later needs a signed image written for the
purpose, which writes the bit as Secure code (if the page's Secure lock is left open for
that). The same goes for page 2 and a later change of boot keys. The lock check in the
firmware (#86) is also no proof of history: lock words can be written from the bootloader, so
a board that was ever out of its owner's hands unlocked can't be shown clean by reading them.

**Deferred, by the owner's word (2026-11-25): "We are not working with a production board
until dev-1 is tested and working."** This is a question for the production board, to be
taken up then. It blocks nothing now. On dev-1, 4.8 covers page 3 and the root secret's list
pages only (see the dev variant).

## Afterwards: trying the attacks

The table at the top of the issue, run against the secured board, with results recorded on the
issue.

| Attempt | Expected |
|---|---|
| Attach a debug probe | Refused |
| Load an unsigned build over USB (if the bootloader was kept). Through our own tools this needs `SOS_FLASH_UNGUARDED=1` for that one flash: the flash guard refuses an unsigned image before it reaches the board | The bootrom won't run it |
| Load a build signed with another key | The bootrom won't run it |
| Load the signed build | Runs |
| `picotool otp get` and `otp dump` on the root-secret page | Refused or empty |
| `picotool save` of the whole flash | Succeeds (if the bootloader was kept), and shows only sealed records: no network name, no key comment, no host key |
| Write an unsealed configuration image (`ninja provision`) | The board refuses it and says so in its log. Its own sealed configuration is gone all the same, because provisioning overwrites both configuration sectors: the board comes up with no configuration, to be set up on its console, and its cards stay locked. The attacker's configuration is not loaded. (Run this last: it empties the board) |
| Erase the configuration sectors over USB | The board starts as new, with an open console asking for a password, and the old card stays locked. **Its SSH host key is still there** (it is in another sector, sealed under the same root secret), so the board set up again by whoever is at the console presents the old host key, until the change in "to confirm" item 8 is built: after it, the wipe makes the old host key unreadable and setup makes a new one |
| Put back an older copy of the sealed configuration sectors | It loads: rollback is accepted in #64 |
| `picotool otp load` and `picotool otp set` of a harmless row | Record what the chip answers |
| Console without the password | Nothing but the prompt |

Not possible to try here: the glitch attacks, the flash swap and chip imaging.

## Variant for the development board (dev-1)

**What dev-1 is for, in the owner's words:** "what we are testing on dev-1 is that the
mechanisms work. We don't really care about protecting the data on the board." And: "We are
not working with a production board until dev-1 is tested and working."

**Status: reviewed; waiting for the owner's approval. No OTP write on dev-1 is approved by
this section, and none has been made.** The owner chose on 2026-11-25 to run steps 3 and 4 on
dev-1 with the development signing key ("lets test the whole thing"), and then: "Lets wait for
the firmware to be ready and then enable all of the features in the intended order." The
firmware is merged (#79, #80, #81, #82, #84, #86, #87, #88, #89). What remains is the owner's
approval of this document, the gates below, and the owner naming and running each write.

dev-1 is the only board all sessions develop on (serial F94525FD61D9A1CC, RP2350 A2). A mistake
at the last step leaves the project without a board until another arrives.

### The whole run on one page: what the owner does, in order

Everything below is explained further down and in [`docs/otp/dev-1/`](otp/dev-1/README.md),
which has every command in full. **Bold** steps write OTP and can't be undone. A session
builds and flashes firmware and reads the console; the owner types the confirmations, holds
the button and runs every `picotool otp` command.

*Before the sitting, the owner confirms:*

1. This document is approved.
2. A backup of both dev keys exists off the development machine.
3. Only dev-1 is connected, on a power supply that won't be disturbed, and every session has
   been told and is on current main.

*Preparation, nothing permanent:*

4. `picotool otp dump` of the board is saved.
5. The recovery path is walked: unplug, hold BOOTSEL, plug in, `picotool info` sees the board,
   `picotool load` of the dev-signed image by hand, and it runs.
6. The burn image is flashed. On the console: `rootsecret burn plan` says it would use OTP
   page 4, and that the button reads "not held".

*Step 3, the root secret (one sitting, with what follows):*

7. **`rootsecret burn rehearse`**: type the serial, type `REHEARSE`, hold BOOTSEL. It must end
   "rehearsal: stopped on purpose", page 4 locked. Write down the time it took. **Any other
   ending: stop here.**
8. **`rootsecret burn`**: type the serial, type `BURN`, hold BOOTSEL. It must end "written,
   verified and its page locked", page 5. **Any other ending: stop here.**
9. The everyday firmware is flashed. `rootsecret` says the secret is in OTP page 5, locked
   until the next reset. **From here until step 11 the board has no configuration:** what it
   had was sealed under the old root secret and no longer opens. No network, no SSH, an open
   console. That is expected.
10. From BOOTSEL, `picotool otp get` of a row in page 5 (the secret) **and** of a row in
    page 4 (the spent page) is refused: both were locked by the burn.
11. `setup` on the console (password, network, trusted key), and the card is formatted. After
    a reboot the configuration loads and the card mounts. The board has a new SSH host key.

*Step 4, the boot keys and secure boot:*

12. **Load `1-boot-keys.json`.** Read the rows back. `bootkey` on the board says `NOT YET`.
13. Trial load, then **load `2-key-valid.json`.** Read back. `bootkey` says `MATCH`, slot 0.
    An image signed with the second key is flashed: `MATCH`, slot 1. The first key's image is
    flashed back. (The second-key image is the same source built in a build directory of its
    own, `cmake --preset debug -B build-key2 -DSOS_SIGN_KEY=<the second key>`, so that the
    everyday build and its setting in `local.cmake` are not touched.)
14. The owner confirms the key backup once more. `bootkey` says `MATCH` and
    `picotool info -a` of the image read back from the board says `signature: verified`.
15. Trial load, then **load `3-secure-boot.json`: the last bit.** Read back. `version` still
    says "off", with a warning, until the reset; after it the board boots and `version` says
    `secure boot: on`.
16. First a plain `ninja flash` of the signed image: flashing a secured board through the
    bootloader must still work. Then an unsigned image is flashed, **with
    `SOS_FLASH_UNGUARDED=1` for that one flash** (our own flash guard would otherwise refuse
    it, which is its job): the board must not run it, and must come back with a signed one
    loaded from BOOTSEL.
17. Trial load, then **load `4-page3-lock.json`.** Read back. The zero write to row 0x0ff is
    refused; a read of page 1 is answered.

*Afterwards:* the board registry records each write with the date, and the results go on
issue #64.

### What it is for, and what it does not prove

It runs the real procedure, in order, on real hardware before a production board is spent: the
burn, the key provider reading OTP, the boot key slots, the boot ROM refusing other firmware,
and everyday development on a board that only takes signed images.

It does not make dev-1 secret-keeping, and nobody should treat it as such:

- **Debug stays on,** so anyone with a probe reads the root secret out of the running chip.
- **The signing key is a file on the development machine that every agent session can read.**
  Anyone who can read it can sign firmware that dev-1 runs, and that firmware can read the
  root secret.
- So what is sealed on dev-1 is protected from someone with only a USB cable and no key, and
  from nobody else. dev-1 tests the mechanism.

### What differs from the production procedure

| | Production board | dev-1 | Why |
|---|---|---|---|
| Signing key | Made away from the development machine | `~/slop/sos-dev-sign.pem`, on the development machine | A development key, never for a production board or a release |
| `BOOTKEY0` / `BOOTKEY1` | In use / spare | The dev key / a second dev key (not made yet) | The owner's answer 5 |
| Slots 2 and 3 (step 4.3) | Marked invalid | **Left blank, not marked invalid** | Room to add a key later; the cost is that someone else could too |
| `DISABLE_WATCHDOG_SCRATCH` (4.4) | Set | **Not set** | "We keep the ability to reboot in the bootloader": agents flash with `picotool load -f`, with nobody at the button |
| Glitch detector in OTP (4.5) | Decision 5, after the week-long test | **Not set** | Decision 5 unchanged |
| `DEBUG_DISABLE` (4.6) | Set | **Not set** | "Do not turn debug off on dev-1" |
| `SECURE_BOOT_ENABLE` (4.7) | Set, last | Set, last | |
| Sealed-only marker (4.6a) | Set | **Not set** | The owner's decision: dev-1 stays provisionable with `ninja provision` |
| Root secret in OTP (step 3) | Yes | Yes | |
| Set up fresh after step 3 | Yes | Yes: dev-1 is re-keyed, see below | |

### Moving the root secret re-keys dev-1

The root secret in OTP is a new one, so every key derived from it changes. On dev-1 that means:
the sealed configuration no longer opens (set up again with `setup` or `ninja provision`), the
SSH host key is replaced (clients will see a changed host key and must forget the old one), the
console password is gone with the configuration, and the encrypted card no longer mounts and is
formatted again. dev-1's contents are disposable, so nothing is saved first. Whoever does it
says so to the other sessions before and after.

### How the rehearsal (decision 6) fits

Decision 6 approved a rehearsal of the burn on dev-1 "in spare OTP pages", with a read attempt
from BOOTSEL and one burn interrupted on purpose, as preparation for a production board. With
dev-1 now getting a real root secret, the rehearsal and the real thing are the same run.
**Agreed by the owner on 2026-11-25** (it goes beyond the words of decision 6, which spoke of
spare pages). It is a decision about how the run goes, not the go-ahead for it: the gates
below still apply and the owner names each write.

1. The first run of the burn command on dev-1 is **interrupted on purpose**, by a stop point
   in the burn command, not by pulling the cable: the command is given an explicit rehearsal
   argument and stops after the data rows and their chaff are written and read back, before
   the marker and the locks. A power cut lands wherever it lands and would test a different
   state each time; the stop point gives the one state the skip logic is written for, and it
   can be repeated. The stop point exists only in a burn image built with a rehearsal option;
   the image used on a production board is built without it. (A real power cut partway
   through a row is a different case and is not covered by this run: said here so that nobody
   takes it as tested.)
2. **The rehearsal must end with "rehearsal: stopped on purpose", and the page locked. If it
   ends any other way, the real burn is not run.** The rehearsal is the first time
   `program()`, the boot ROM's write call and the read-back through the memory-mapped alias
   run on a chip at all; a result of "a write failed or didn't read back" on some row means
   one of them is wrong, and it would be wrong for the real secret too. The image enforces
   this for the rest of that boot; the procedure enforces it after a restart.
   **Write down how long the rehearsal took** (the command prints it): the time the chip
   needs per OTP row is the one number nobody has yet, and it says how much margin the
   watchdog feeding really has.
3. Only then: the burn is run again, without the rehearsal argument. It lands in the second
   page. That is dev-1's root secret.
4. The read attempt from BOOTSEL is step 3.4, on that page and on the spent one: the burn
   locked both, and both must refuse.

**What the spent page then holds:** 32 random bytes with their chaff, complete, with no marker.
They were never a root secret: nothing was ever derived from or sealed under them, and the
second run takes fresh bytes from the TRNG. **Between the two runs the page has no locks, and
it must not stay that way:** the second run, which writes the real secret, also locks the
spent page against writes from the bootloader (see the burn's steps above). Until it has, a
cable-holder could complete the spent page into a root secret of their choosing, so the two
runs are done in one sitting. Afterwards the bytes can still be read with `picotool otp get`
if the lock is "read-only"; that is harmless and gets one line in the board registry, so that
nobody later takes a readable 32-byte block in dev-1's OTP for a leak. One more check: the
second page's secret differs from the first page's contents.

This exercises the skip logic on the real page list instead of a parallel one, and leaves dev-1
one page of the list unused. The alternative is a rehearsal build with its own list of spare
pages, which keeps all three real pages but tests code the production board won't run.

### Gates

**Before any OTP write on dev-1:**

- [ ] Merged and reviewed: the key provider's OTP backend and the burn image (firmware items 1
      and 2), and the sealed-only marker (item 3), so that secure boot on dev-1 changes nothing
      about how it is provisioned.
- [ ] "To confirm" items 1 and 7 are closed (the exact commands and files, in
      `docs/otp/dev-1/`; which pages). Items 3 and 4 (reading and soft-locking a page from
      Secure code; raw row writes for the chaff) **are settled on paper and in host tests, and
      can't be closed any further before the run: this run is where they first happen on a
      chip.** The soft lock and the guarded read were tried on dev-1 on a blank page (#86);
      raw and error-corrected row writes first run in the rehearsal (step 7 of the one-page
      list), and the bootloader's lock keeping `picotool` out is first seen at step 10. That
      is why the rehearsal comes first and why any ending but the expected one stops the run.
      Items 2 and 6 concern `DISABLE_WATCHDOG_SCRATCH` and item 5 the glitch detector: none is
      written on dev-1, so they don't gate this run.
- [ ] The second dev key exists. The hash of each key has been computed twice, independently
      (`bootkey` on a board flashed with an image signed by that key is one of the two).
- [ ] The OTP files for dev-1 are written by hand, one per write, reviewed, and contain
      nothing but public hashes and flag values. Every command names the board with `--ser`.
- [ ] `picotool otp dump` of dev-1 as it is now is saved.
- [ ] The flash guard (#80) is merged, and has been seen to refuse an unsigned image and one
      signed with another key.
- [ ] **Every session flashes dev-1 through the guard** (`ninja flash` or `tools/flash.sh`, with
      `SOS_FLASH_SERIAL` set), and no longer with `picotool load` directly: the guard only
      helps where it is used. The shared notes on the board say so, and each session has
      confirmed it. Scripts that call `picotool load` themselves are changed or retired first.
- [ ] Every session that uses dev-1 has been told, has the signing key in its build settings,
      and has confirmed its builds come out signed.

**Before `SECURE_BOOT_ENABLE`, in addition:**

- [ ] **The owner has been asked, and has confirmed, that a backup of both dev keys exists off
      the development machine** (the owner's answer 4: "get back to me and confirm the backup
      exists when we are about to turn all of this on").
- [ ] The checks marked "before the last bit" in the list below have passed.
- [ ] **The recovery path has been walked on the board while it is still open:** unplug, hold
      the BOOTSEL button, plug in, and `picotool info --ser` sees the board in BOOTSEL; then
      `picotool load --ser` of the dev-signed image, by hand and without `-f`, and the board
      runs it. After D4.7 this is the only way back from a bad flash, and it uses the button
      and a cable that the everyday `picotool load -f` path never exercises.
- [ ] `bootkey` says `MATCH` for the image on the board at that moment, and `picotool info -a`
      of the image read back from the board says `signature: verified`.

### The run

Steps 3.1 to 3.5 as written above, with the interrupted burn first.
Then:

| # | Write (the owner runs it) | Check before going on |
|---|---|---|
| D4.1 | The dev key's hash into `BOOTKEY0`, the second dev key's into `BOOTKEY1` | Rows read back equal the two computed hashes. **`bootkey` says `NOT YET` for slot 0** with the hash shown equal to the image's: the first run of that path, and a check of the slot byte order while a wrong slot can still be abandoned |
| D4.2 | `BOOT_FLAGS1.KEY_VALID` for slots 0 and 1 | `bootkey` says `MATCH`, slot 0. Flash an image signed with the second key: `bootkey` says `MATCH`, slot 1. Flash the first key's image back |
| | *4.3 to 4.6a are not done on dev-1* | |
| D4.7 | `CRIT1.SECURE_BOOT_ENABLE`, after the gate above | Before the reset, `version` says `secure boot: off` with the "OTP rows say otherwise" warning. After it: the signed firmware boots, `version` says `secure boot: on` with no warning, and what was set up in 3.5 still loads |

### What runs on hardware for the first time

None of these has run on a real chip. dev-1 is where each runs first.

| First run | When | Before the last bit? |
|---|---|---|
| The burn command, its chaff, marker and locks; an interrupted burn; the firmware skipping a spent page | 3.2 | yes |
| The key provider reading the root secret from OTP and locking the page at the end of boot | 3.3 | yes |
| `picotool otp get` refused on the locked page, from BOOTSEL | 3.4 | yes |
| `picotool otp load` of our hand-written files | D4.1 | yes |
| The slot byte order on a chip; `bootkey` saying `NOT YET` | D4.1 | yes |
| `bootkey` saying `MATCH`, for each of the two keys | D4.2 | yes |
| `picotool load -f` of a signed image on a board with valid boot keys | D4.2 | yes |
| BOOTSEL by the button, then `picotool load --ser` by hand (the recovery path) | gate before D4.7 | yes, on the open board; on the secured board only after D4.7 |
| `version` warning between an OTP write and the reset | D4.7 | no: it is the last bit |
| The boot ROM starting a signed image with secure boot on | D4.7 | **no** |
| The boot ROM refusing an unsigned image and one signed with an unknown key, and the board coming back to BOOTSEL afterwards so that a good image can be loaded | after D4.7 | **no, and it can't be:** an open board runs anything |
| `picotool load -f` and `ninja flash` on a secured board with the bootloader kept | after D4.7 | **no** |

The three "no" rows are the risk the owner accepts by doing this on the only board. What can be
done about them beforehand is everything above them: the key compared four ways, the
signature verified by `picotool`, and both keys shown to match.

After D4.7 the checks of "Afterwards: trying the attacks" are run on dev-1 except the debug
probe (which is expected to attach) and the reboot-into-BOOTSEL row (which is expected to work).

### dev-1 and step 4.8 (decided by the owner, 2026-11-25)

"For dev-1 leave them open for later OTP tests, what we are testing on dev-1 is that the
mechanisms work. We don't really care about protecting the data on the board."

So on dev-1 **only page 3 and the root secret's list pages are locked against writes from the
bootloader.** Pages 1 and 2 (boot flags, boot keys) and the lock pages stay open, for later
OTP tests. The list pages are locked by the burn itself; page 3 by the owner with `picotool`.

| # | Write (the owner runs it) | Check |
|---|---|---|
| D4.8 | Page 3's lock word: bootloader read-only | Read it back. From BOOTSEL, a raw write of the value 0 to row 0x0ff (the exact command is in `docs/otp/dev-1/README.md`: it changes no bit even if the lock didn't take, and is nowhere near the sealed-only marker's rows) is **refused**; `picotool otp get` of a row in page 1 or 2 is **answered**, showing those pages were left open as intended. No other value or row is used to test a lock |

What stays possible on dev-1 as a result, knowingly: anyone with a cable can add a boot key to
a blank slot and run their own firmware, or mark the dev keys invalid and end the board's
life. Neither loses anything the owner cares about on this board.

### Living with it afterwards

- Every image flashed to dev-1 is signed with one of the two dev keys, for good. An unsigned
  image doesn't start; the expected recovery is loading a signed one over USB.
- If both dev keys are lost, dev-1 keeps its last firmware for ever.
- The board registry records what was written to dev-1's OTP, with the date.

### Decided by the owner for this variant (2026-11-25: "Yes to both, flash guard and separate marker")

- **A guard on flashing.** `ninja flash` (and `tools/provision.sh`, where it flashes) refuses
  an image that isn't signed with the expected key, so a session with a missing build setting
  can't leave dev-1 in BOOTSEL by accident. Built and merged before D4.7. The expected keys
  are a **list** of public key hashes in the build settings, not the private key file: a list,
  because dev-1 accepts two keys and step D4.2 flashes an image signed with the second; hashes,
  so that the guard also works where no private key is present. It guards our own tools only:
  `picotool load` typed by hand goes around it. Two things it doesn't do yet, to add before a
  second board is in use: refuse when more than one board is attached and none is named, and
  keep the accepted keys per board serial instead of per machine (a production board will
  accept other keys than dev-1).
- **A separate marker for "sealed-only".** Secure boot alone does not make a board refuse
  unsealed records or `rootsecret forget`. Firmware item 3 above has the proposal for the
  marker; dev-1 never gets it.

- **The sealed-only marker is an OTP row,** and **the interrupted burn is the first run on
  the real page list** (both agreed 2026-11-25; see firmware item 3 and "How the rehearsal
  fits").

Nothing in this variant is left open for the owner. What remains is the firmware, the "to
confirm" items named in the gates, and the owner's word for each write.

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

1. The exact `picotool` commands and file contents for each write. **For dev-1 they are in
   [`docs/otp/dev-1/`](otp/dev-1/README.md), for review:** one file per write, the read-back
   command after each, and what the blank board answers to each read today. The read commands
   have been run on dev-1; no file has been loaded, and that page lists what can only be
   confirmed by loading (that `picotool` accepts each file as written, the lock word's form in
   particular). Files for a production board are written when there is one: other keys, and
   the rows dev-1 leaves out.
2. That `BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` leaves SOS's fault record and watchdog reset
   detection working. They use the watchdog scratch registers 0 to 3 and the watchdog's reason
   register; the datasheet says the flag makes the bootrom ignore the scratch registers at boot.
3. How Secure code reads and soft-locks an OTP page in the SDK version we use, and that the
   bootloader lock (`LOCK_BL`) keeps `picotool` out on A2 other than through the E21 glitch.
   *The first half is done: #86, with the soft lock and the guarded read tried on dev-1 on a
   blank page. The second half is first seen on a chip in the dev-1 run, when a read of the
   locked pages from BOOTSEL is refused.*
4. Writing raw rows from Secure code: the bootrom or SDK call for a raw (not error-corrected)
   row write and read, used for the chaff rows. *Written and host tested (#87) against the
   SDK's description of the call; first run on a chip in the dev-1 rehearsal.*
5. Glitch detector: the week-long test's results; and that the detector's `LOCK` register
   stops software from disarming it.
6. Whether anything in SOS relies on rebooting into BOOTSEL by command, beyond flashing.
7. Which free OTP pages the root secret uses (pages 3 to 60 are free for user data), and that
   a failed write leaves the next page usable. **Page 3 is reserved for SOS's own flags:** the
   sealed-only marker has its first three rows (#84) and the rest is kept for later ones, so
   the root secret's list starts at page 4 or later.
8. **The host key after a wipe: decided by the owner and built (#88, merged).** The SSH host
   key is sealed under a key made from the root secret and the owner secret, the way an
   encrypted card's key is. Erasing or provisioning a board's configuration makes its host
   key unreadable, `config reset` also stops SSH at once, and the next setup makes a new
   one. Costs the owner accepted: the owner's own `config reset` changes the board's SSH
   identity (clients warn), and losing both configuration copies loses the host key too.
   An open board kept its existing host key when #88 landed (it is taken over once, same
   fingerprint); a sealed-only board does not take over a key from before #88. On dev-1 the
   host key changes during this run, at step 3.5, because the root secret moves to OTP.
   Firmware older than #88 makes a new host key over the record #88 writes.
