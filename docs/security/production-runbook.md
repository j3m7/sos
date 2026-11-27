# Securing a production board: runbook (DRAFT, do not run)

> **This is a draft. Do not run any of it.** There is no production board yet. Before this page
> can be used:
>
> - every item [to confirm](decisions.md#to-confirm) is closed, including the exact commands
>   for each write (item 1): the steps below say what each write is, not yet how to type it;
> - decisions 1 and 2 (debug off or a debug key; keeping USB BOOTSEL), 5 (glitch detector in
>   OTP) and 11 (locking pages against the bootloader) are settled, and the production column
>   of the [decisions](decisions.md) is confirmed;
> - this page is rewritten in the form of the [dev-1 runbook](../otp/dev-1/RUNBOOK.md):
>   a command, what you should see, and when to stop, for every step;
> - you have reviewed and approved it.

Why each step exists is in [design.md](design.md) and [decisions.md](decisions.md). The step
numbers (3.1 to 4.8) are referred to from the source code and decisions.md; keep them.

- **PERMANENT** steps write one-time-programmable (OTP) memory and can't be undone.
- **If what you see differs from the check, stop.** Don't retry and don't go on.
- **You run every OTP write yourself,** or name each one for an AI session to run. The deny
  rule on `picotool otp set`, `load`, `permissions` and `white-label` stays in place otherwise.
- **Steps 3 and 4 are done in one sitting.** At the end of step 3 the board still boots
  anything and still has debug, so its new secret can be read by loading other firmware. Don't
  leave the board unattended between them.

## Make the signing keys

Do this once, before the board, on a machine that is not the development machine, ideally an
offline one, and not in a session with an AI assistant: the private keys must never be on a
machine where agents run.

1. Make the two keys, readable only by you:

   ```sh
   umask 077
   openssl ecparam -name secp256k1 -genkey -noout -out sos-boot0.pem    # the key in use
   openssl ecparam -name secp256k1 -genkey -noout -out sos-boot1.pem    # the spare
   ```

2. Export the public halves. These are not secret, and are what the rest of this page needs:

   ```sh
   openssl ec -in sos-boot0.pem -pubout -out sos-boot0.pub.pem
   openssl ec -in sos-boot1.pem -pubout -out sos-boot1.pub.pem
   ```

3. Compute each key's OTP hash, the SHA-256 of the public key's 64 raw bytes (X then Y):

   ```sh
   openssl ec -in sos-boot0.pem -pubout -outform DER | tail -c 64 | sha256sum
   openssl ec -in sos-boot1.pem -pubout -outform DER | tail -c 64 | sha256sum
   ```

   Write both hashes down. This is the first of two independent computations. The second is
   `picotool`'s: the `bootkey0` values it writes when asked to sign with that key. That file
   also carries the bit that enables secure boot, so read it for the comparison and then
   delete it. **Never load it.** Do both computations for both keys.

4. Back up each private key offline, on two separate media kept in different places (a USB
   stick and a printed copy, for example). Read each backup back once to see that it is
   complete. Keep the spare key's copies apart from the first key's.

5. Decide where builds are signed. Signing needs `sos-boot0.pem` on the building machine, as
   `SOS_SIGN_KEY` (`docs/development.md`, "Signed builds"). The simplest safe arrangement:
   release builds for the secured board are built and signed on the machine that holds the key,
   and the development machine never has it. `sos-boot1.pem` stays offline until the first key
   is lost or has to be retired.

If a private key ever leaks: sign new firmware with the other key, and write
`BOOT_FLAGS1.KEY_INVALID` for the leaked key's slot on the board. That is permanent, and leaves
the board with one key.

## Before you start

- [ ] The build that will be burned is a tag of main at or after #88, so it includes #79
      (`bootkey`), #84 (the console-only marker), #86 (the OTP key provider with its "OTP or not
      at all" rule), #87 (the burn image) and #88 (a new SSH host key after a wipe). Note the
      first 12 digits of `git rev-parse <tag>`: `version` shows those, not the tag's name.
- [ ] `picotool` 2.3.1.
- [ ] The board's serial and chip revision are recorded from `version`, and the board is added
      to the board registry as the production board.
- [ ] `picotool otp dump` of the blank board is saved, as the record of its state before.
- [ ] The signing keys exist, each has an offline backup that has been read back once, and the
      hash of each key has been computed twice, independently, and the two agree.
- [ ] The tagged firmware, signed with the `BOOTKEY0` key, **boots and works on this board while
      it is still open**: `picotool info -a` shows the signature verified; `version`, Wi-Fi and
      SSH work.
- [ ] The flash guard's key list (`~/.config/sos/flash-keys`, or `$SOS_FLASH_KEYS`) on the
      flashing machine has a `[<this board's serial>]` section listing only the production
      keys' hashes.
- [ ] Only this board is connected over USB. Every `picotool` command names its serial with
      `--ser`.
- [ ] Stable power; nobody else is using the board.

## Step 3: the root secret into OTP

| # | Do | Check before going on | Undo? |
|---|---|---|---|
| 3.1 | Flash the signed burn image, built from the same tag. On the console: `version`, then `rootsecret burn plan`, then `rootsecret burn plan` again with BOOTSEL held | `version` shows `SOS <the 12 digits>` with no `-dirty`, `THIS IS THE BURN IMAGE`, `root secret: in flash (open board)` and `secure boot: off`. The plan says `It would use OTP page 4 (rows 0x100 to 0x13f)`, and `BOOTSEL button: held` the second time | yes |
| 3.2 | **PERMANENT.** Run `rootsecret burn` on the console: type the serial, then `BURN`, and hold BOOTSEL until told to let go | `the root secret is written, verified and its page locked.` and `OTP page 4: locked against the bootloader`. Write down the time it prints (`It took ... ms.`) for #100 | **no** |
| 3.3 | Flash the everyday signed firmware (same tag, no burn command) | `rootsecret` shows `root secret: present, in OTP page 4` with no `(WARNING: ...)` after it, and `its OTP page is locked until the next reset`. The log has `rootsecret: OTP page 4 is locked until the next reset` | yes |
| 3.4 | From BOOTSEL, try to read the page with `picotool otp get`, then `picotool reboot --ser` | The read is refused (`permission failure`), which leaves the board in its bootloader; `picotool reboot` brings it back. `picotool otp dump` shows no secret in that page | |
| 3.5 | `setup` on the console (password, network, trusted key), then format the card. **Write down the card key it shows** | The configuration saves and loads across a reboot; the card mounts | yes |

**A burn that ends in an error is a full stop, not a cue to run it again.** The command says
`STOP HERE` and the image refuses further runs until the board restarts. Find the cause first:
a fault in how OTP is written would spend the second and third pages exactly as it spent the
first. The only error that isn't a stop is "the button wasn't held". If the very first write
fails and nothing landed (`Nothing landed in OTP: page N still reads blank and was left
unlocked.`), the page is left blank and is the one the next run uses; that is still a stop.
`LOCKING A PAGE FAILED: the bootloader can still write it` means a page is spent **and
unlocked**: stop, and don't leave the board.

**If the board restarts in the middle of a burn** (power lost, a reset), the run never reached
its locks. Bring the firmware back and run `rootsecret burn plan`, then stop and keep the board
attended until the cause is understood:

- `..., after locking 1 earlier, spent page`: the interrupted page is spent and still writable
  from the bootloader, which is the cable attack the burn's locks exist to stop.
- `there is no blank page to use (all spent, or one can't be read or is damaged)`: a restart
  while the marker was written can leave it damaged, and a damaged page stops the search, so the
  pages after it are never used. This firmware can't give the board a root secret.

If the board restarts during a burn while the BOOTSEL button is held, it comes up in the USB
bootloader. That is the button, not damage: `picotool reboot` brings the firmware back, and
`rootsecret burn plan` then says what state the pages are in.

## Step 4: secure boot and debug off

The order is the datasheet's (section 10.5). `SECURE_BOOT_ENABLE` is last, because with it set
and a wrong key the board can never run code again.

| # | Write | Check before going on | If it's wrong |
|---|---|---|---|
| 4.1 | **PERMANENT.** The signing key's hash into `BOOTKEY0_0` to `_15`; the spare key's into `BOOTKEY1_0` to `_15` | Read the rows back and compare with the two independently computed hashes. `bootkey` says `NOT YET` for slot 0, with the hash shown equal to the image's, and shows slot 1's hash equal to the spare key's | A wrong hash in a slot can be abandoned by marking that slot invalid in 4.3, as long as the other slot is right |
| 4.2 | **PERMANENT.** `BOOT_FLAGS1.KEY_VALID` for slots 0 and 1 | Read `BOOT_FLAGS1` back: `KEY_VALID` 3, `KEY_INVALID` 0. The signed firmware still boots. `bootkey` says `MATCH`, slot 0, for the image on the board. `picotool info -a` on the image read back from the board says `signature: verified`. `NOT YET`, `NO MATCH` and `NO VERDICT` all stop the procedure. (The spare key stays offline, so slot 1 is checked only by its hashes, in 4.1: a cost recorded under decision 4) | Stop here, with the board still open |
| 4.3 | **PERMANENT.** `BOOT_FLAGS1.KEY_INVALID` for slots 2 and 3 | Read `BOOT_FLAGS1` back: `KEY_VALID` 3, `KEY_INVALID` 12 | |
| 4.4 | **PERMANENT.** `BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` (decision 3). None of the `DISABLE_BOOTSEL_*` flags (decision 2) | Read back. Reboot. `picotool load -f` no longer brings the board into BOOTSEL; holding the BOOTSEL button while plugging in does, and a signed image can still be loaded that way. SOS's fault record and watchdog reset detection still work | From here every `picotool` command that needs the bootloader (the writes and read-backs below, the attack loads, getting back from a refused command) needs BOOTSEL entered by hand: unplug, hold the button, plug in |
| 4.5 | **PERMANENT. Only if decision 5 says so.** `CRIT1.GLITCH_DETECTOR_ENABLE` and sensitivity, at the setting the week-long test supports | Read back. Reboot several times; run the board under normal load (Wi-Fi, SSH, card) with no detector resets | A detector that resets a healthy board can't be turned off again |
| 4.6 | **PERMANENT.** `CRIT1.DEBUG_DISABLE` (decision 1) | Read back. After the board restarts: `version` shows `debug port: disabled` with no `WARNING: the OTP rows say otherwise`, and a debug probe can no longer attach | |
| 4.6a | **PERMANENT.** The console-only marker: the raw value 0x5ea1ed into rows 0x0c0, 0x0c1 and 0x0c2. **Only when the firmware on the board has the OTP key provider with the "OTP or not at all" rule (#86), and `rootsecret` shows `root secret: present, in OTP page 4` with no `(WARNING: ...)`:** a console-only board whose list pages aren't all locked has no root secret at all, so the board would lose its keys and the configuration from 3.5. And with older firmware a console-only board still takes its root secret from flash, which an attacker can write | Read the three rows back. Reboot: `version` says `console-only setup: yes (settings files written over USB are refused; ...`, not `, forced by this test build` and nothing about unreadable or damaged rows; the configuration from 3.5 still loads. (`rootsecret forget` is refused on any board whose secret is in OTP, so it proves nothing here.) **If it still says `no`, stop: don't go on to 4.7. Don't test it by provisioning this board:** that replaces the sealed configuration with a record the board refuses, and it comes up empty | From here the board can only be set up on its console. A partly written marker is still set |
| 4.6b | Walk the recovery path: unplug, hold BOOTSEL, plug in; `picotool info --ser` sees the board in BOOTSEL; `picotool load --ser` of the tagged everyday image (not the burn image), by hand and without `-f` | The board runs the image. After 4.7 this is the only way back from a bad flash | |
| 4.7 | **PERMANENT, no way back.** `CRIT1.SECURE_BOOT_ENABLE`, only after 4.6b, and after `bootkey` says `MATCH` and `picotool info -a --ser` says `signature: verified`, both for the image on the board at this moment | Read back. After the board restarts: the signed firmware boots, `version` shows `secure boot: on` with no warning, and what was set up in 3.5 still loads | The board is dead if the key in OTP isn't the key the image is signed with |

## Try the attacks

Run each against the secured board and record the results on #100.

| Attempt | Expected |
|---|---|
| Attach a debug probe | Refused |
| Load an unsigned build over USB. Through our own tools this needs `SOS_FLASH_UNGUARDED=1` for that one flash | The boot ROM won't run it |
| Load a build signed with another key (with `SOS_FLASH_UNGUARDED=1`, as above) | The boot ROM won't run it |
| Load the signed build | Runs |
| `picotool otp get` and `otp dump` on the root-secret page | Refused (`permission failure`); no secret in the dump |
| `picotool save` of the whole flash | Succeeds, and shows only sealed records: no network name, no key comment, no SSH host key |
| Erase the configuration sectors over USB | The board starts as new, with an open console asking for a password; the old card stays locked; a new SSH host key at the next setup, and clients warn |
| Put back an older copy of the sealed configuration sectors, saved with `picotool save` before an earlier change | It loads: rollback is accepted (decision 8) |
| `picotool otp set` of the value 0 to an unused row of an unlocked page | Record what the chip answers |
| Console without the password | Nothing but the prompt |
| **Last, because it empties the board:** write an unsealed configuration image (`ninja provision`) | The board refuses it and says so in its log. Its own configuration is gone all the same (provisioning overwrites both sectors): it comes up unconfigured, its cards stay locked, and the attacker's configuration is not loaded |

Not possible to try here: the glitch attacks, the flash swap and chip imaging.

Then save `picotool otp dump` again and compare it with the one from "Before you start": it
may differ only in the rows written above.

## Step 4.8: lock the OTP pages that matter against the bootloader

**Waits for decision 11.** Do it last, after 4.7 has been seen to work, the board has been
through "Try the attacks" above, and it has been set up again on its console (`setup`, then
format the card and write down the new card key), since the last attack empties it.

| # | Write | Check before going on | If it's wrong |
|---|---|---|---|
| 4.8 | **PERMANENT.** The lock words of the pages decision 11 names (proposed: pages 0 to 3, the root secret's list pages the burn didn't lock, and the lock pages 62 and 63, last, since the other pages' lock words are in them; to confirm against the datasheet, item 1): bootloader read-only | Read the lock words back. From BOOTSEL, a raw write of the value 0 to an unused row of each locked page is **refused** (a 0 changes no bit even if the lock didn't take; which rows is part of to-confirm item 1; no other value is ever used to test a lock). `version`, `bootkey` and `rootsecret` still read what they did | After this nothing in those pages can be changed over USB, by anyone: a wrong key hash or flag is permanent |

## What can't be undone

- Every OTP bit written. In particular: the root-secret page and its locks, the boot key hashes,
  the disabled boot paths, the glitch detector, debug off, secure boot on.
- Both signing keys lost: the board keeps its last firmware for ever. One leaked: anyone can sign
  firmware for it; the remedy is to mark that slot invalid, which works only while the other key
  is safe.
- A forgotten console password: erase the configuration over USB and start again, losing the
  cards. If the bootloader is ever disabled (decision 2), there is no way to erase it from
  outside, and the console is locked for good (SSH with a trusted key still works).
