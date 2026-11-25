# Securing a board (draft procedure)

**Status: draft, for the owner's decisions and for review. Nothing in it has been run, and
nothing in it may be run until the owner approves it.** It covers steps 3 and 4 of
[issue #64](https://github.com/j3m7/sos/issues/64): the root secret in one-time-programmable
(OTP) memory, secure boot, and debug off. Every OTP write is permanent.

Sources: the RP2350 datasheet in `docs/rp2350/` (sections 10.5, 13.4, 13.5, 13.8 and appendix
D), and `picotool otp list` from picotool 2.3.1 for the field names. Items I could not confirm
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
| A lab that can decapsulate the chip | Not stopped (OTP imaging); storing the secret with chaff makes it harder |

E20 and E21 have workarounds that cost features; they are decisions 2 and 3 below. E16 and E24
have none on A2. So a secured A2 raises the bar from "anyone with a cable" to "someone with
fault-injection equipment and skill", and no further. Moving to an A4 board later removes those
four.

## Decisions for the owner

These have to be settled before any firmware for this is written, because they change it.

| # | Decision | Options | What I'd pick, and why |
|---|---|---|---|
| 1 | **Debug** | (a) off for good: `CRIT1.DEBUG_DISABLE`. (b) a debug key kept offline, so a board can be debugged again | (a). A debug key is one more secret to keep, and development stays on the open board |
| 2 | **USB bootloader (BOOTSEL) on the secured board** | (a) keep it: signed firmware can be flashed over USB as now. (b) disable it: `BOOT_FLAGS0.DISABLE_BOOTSEL_USB_PICOBOOT_IFC`, `..._USB_MSD_IFC` and `..._UART_BOOT`, the datasheet's workaround for E20 and E21 | Yours to weigh. With (b) **the board can never be reflashed over USB again**: its firmware is frozen unless SOS first gets its own way to update itself, which doesn't exist. With (a) the two glitch attacks through BOOTSEL stay open, and anyone with a cable can still erase or roll back the flash (not read it) |
| 3 | **Bootrom reboot types** | (a) leave as is. (b) set `BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH`, the other E20 workaround | (b) if 2(a). Cost: the board no longer reboots into BOOTSEL on command (`picotool load -f`, `ninja flash`); someone has to hold the BOOTSEL button. SOS's own fault record uses other scratch registers and should be unaffected (*to confirm*) |
| 4 | **Signing keys** | How many boot keys, and where the private keys live | Two: `BOOTKEY0` for use and `BOOTKEY1` as a spare, both generated on a machine that is not this development machine, each backed up offline. Slots 2 and 3 marked invalid so nobody can add a key later |
| 5 | **Glitch detector** | `CRIT1.GLITCH_DETECTOR_ENABLE`, and a sensitivity 0 to 3 | Enable at the default sensitivity. Higher sensitivity can reset a healthy board on a noisy supply; that needs a soak test on the open board's settings first (*to confirm* how to test it without burning) |
| 6 | **A rehearsal on the development board** | (a) none: the first OTP write happens on the production board. (b) rehearse step 3 on dev-1 in a spare OTP page | (b). dev-1 has 58 free pages; using one is permanent but harmless, and it is the only way to test the code that writes and locks the root secret before it runs for real. Step 4 can't be rehearsed without securing dev-1 for good |
| 7 | **Wrong-password count across reboots** | Add it or not | Only worth adding with 2(b). While the USB bootloader can rewrite the flash, a counter stored there can be reset |

## What has to be built first (reversible, ordinary pull requests)

None of this exists yet. Each item is firmware or tooling work with host tests and review, done
and merged before anything is burned.

1. **The key provider's OTP backend.** Reads the root secret from an OTP page as Secure code,
   and on `keyprov_lock()` advances that page's Secure lock in the `SW_LOCK` register so nothing
   can read it until the next reset. Today's backend reads a flash sector.
2. **Writing the root secret, on the board.** A console command (working name `rootsecret
   burn`) that takes 32 bytes from the TRNG, writes them to the chosen page with error
   correction, writes the bitwise complement of each row 32 rows higher (the datasheet's chaff
   against imaging), reads everything back, and then programs the page's locks: bootloader
   inaccessible, Non-secure inaccessible, Secure read-only. The secret never leaves the board
   and is never shown. It asks for the console password and for the board's serial to be typed.
3. **Knowing it is secured.** The firmware reads `CRIT1.SECURE_BOOT_ENABLE` at boot. On a
   secured board it: uses the OTP backend only (the flash root-secret sector is ignored);
   refuses unsealed configuration and host-key records (`refuse_plain`, already implemented and
   off); refuses `rootsecret forget`; and shows the state in `version`.
4. **The signing key in the build** is already there (`SOS_SIGN_KEY`, #70). The build
   deliberately produces no OTP file.
5. **The OTP settings as files we write by hand,** one per stage below, in the form
   `picotool otp load` takes, checked into the repository without any key material and reviewed
   like code. Not the single file `picotool seal` can produce, which enables secure boot in the
   same load as the key hash.

Moving a board's root secret from flash to OTP changes every derived key. The board's sealed
configuration and host key become unreadable and its cards locked. So the production board is
set up fresh after step 3, never before.

## Before touching the production board

- [ ] The owner has approved this document, with the decisions above filled in.
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
| 3.1 | Flash the tagged, signed firmware | `version` shows the tag; the board is not secured | yes |
| 3.2 | Run the burn command on the console | It reports the page written, read back, and locked | **no** |
| 3.3 | Reboot | `rootsecret` reports the secret present in OTP. The log shows the key provider locked the page at the end of boot | |
| 3.4 | From BOOTSEL, try to read the page with `picotool otp get` | It is refused or returns nothing. `picotool otp dump` shows no secret in that page | |
| 3.5 | `setup` on the console (password, network, trusted key), then format the card | Configuration saves and loads across a reboot; the card mounts | yes |

If 3.2 fails partway (power lost, a read-back mismatch), that page is spent. The command must
detect a partly written page and refuse to use it, and the procedure continues with the next
free page. This is part of what the rehearsal tests.

At the end of step 3 the board still boots anything and still has debug: the secret can be read
by loading other firmware. It is protected only once step 4 is done.

## Step 4: secure boot and debug off

The order is the datasheet's (section 10.5), with a check after each write. `SECURE_BOOT_ENABLE`
is last, because with it set and a wrong key the board can never run code again.

| # | Write | Check before going on | If it's wrong |
|---|---|---|---|
| 4.1 | The hash of the signing key into `BOOTKEY0_0` to `_15`; the spare key's hash into `BOOTKEY1_0` to `_15` | Read the rows back and compare with the two independently computed hashes | A wrong hash in a slot can be abandoned by marking that slot invalid in 4.3, as long as the other slot is right |
| 4.2 | `BOOT_FLAGS1.KEY_VALID` for slots 0 and 1 | Read back. The signed firmware still boots (secure boot is not on yet, so this proves nothing about the key by itself) | |
| 4.3 | `BOOT_FLAGS1.KEY_INVALID` for slots 2 and 3 | Read back | |
| 4.4 | `BOOT_FLAGS0`: the boot paths chosen in decisions 2 and 3 | Read back. Reboot. If the USB bootloader is being kept, check it still works; if it is being disabled, **this is the last moment the board can be flashed over USB**: confirm the firmware on it is the one to keep | With the bootloader disabled there is no way back in |
| 4.5 | `CRIT1.GLITCH_DETECTOR_ENABLE` (and sensitivity) | Read back. Reboot several times; run the board under normal load (Wi-Fi, SSH, card) for the agreed soak time with no unexplained resets | A detector that resets a healthy board can't be turned off again |
| 4.6 | `CRIT1.DEBUG_DISABLE` (or the debug key, decision 1) | A debug probe can no longer attach | |
| 4.7 | **`CRIT1.SECURE_BOOT_ENABLE`** | The board boots the signed firmware. `version` shows it secured | If the key hash is wrong the board is dead. This is why 4.1's comparison is done twice |

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
| Console without the password | Nothing but the prompt |

Not possible to try here: the glitch attacks and chip imaging.

## What can't be undone, in one place

- Every OTP bit written. In particular: the root-secret page and its locks, the boot key
  hashes, the disabled boot paths, the glitch detector, debug off, secure boot on.
- A lost signing key (both of them) means the board keeps its last firmware for ever. A leaked
  one means anyone can sign firmware for it; the remedy is to mark that slot invalid, which
  works only while the other key is safe.
- A forgotten console password on a secured board: erase the configuration over USB (if the
  bootloader was kept) and start again, losing the cards. With the bootloader disabled there
  is no way to erase it from outside, and the board is locked out of its own console for good
  (SSH with a trusted key still works). That is another cost of decision 2(b).

## To confirm before this is final

1. The exact `picotool` commands and file contents for each write. I have the field names from
   `picotool otp list`; I have not run any OTP write, and the selector and JSON syntax must be
   checked against a dry run (`picotool otp list` with the same selectors, and the rehearsal).
2. That `BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` leaves SOS's fault record and watchdog reset
   detection working. They use the watchdog scratch registers 0 to 3 and the watchdog's reason
   register; the datasheet says the flag makes the bootrom ignore the scratch registers at boot.
3. How Secure code reads and soft-locks an OTP page in the SDK version we use, and that the
   bootloader lock (`LOCK_BL`) keeps `picotool` out on A2 other than through the E21 glitch.
4. That programming a page's chaff rows (row *i* and its complement in row 32 + *i*) is
   compatible with error-corrected rows, as the datasheet's section 13.8.2 says it is.
5. Glitch detector sensitivity: what the Pico 2 W's own supply tolerates.
6. Whether anything in SOS relies on rebooting into BOOTSEL by command, beyond flashing.
7. Which free OTP page to use (pages 3 to 60 are free for user data), and that a failed write
   leaves the next page usable.
