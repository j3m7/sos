# dev-1: why it was secured this way, and what the run showed

dev-1 (serial `F94525FD61D9A1CC`, RP2350 A2) was secured on 2026-11-26 by
[RUNBOOK.md](RUNBOOK.md), with every step as written there. This page records why the run was
shaped as it was and what it showed. The general design is in
[`docs/security/design.md`](../../security/design.md), the decisions in
[`decisions.md`](../../security/decisions.md).

**Its OTP now:** page 4 spent (the rehearsal) and page 5 holding the root secret, both locked;
the two public dev keys' hashes in boot key slots 0 and 1, both valid; secure boot on; page 3
locked against the bootloader. Left open on purpose: debug, reboot into the bootloader on
command, key slots 2 and 3, and writes to pages 1 and 2 over USB.

## What the run was for

The aim, as stated on 2026-11-25: "what we are testing on dev-1 is that the mechanisms work. We
don't really care about protecting the data on the board." And: "We are not working with a
production board until dev-1 is tested and working."

So the run was the real procedure, in order, on real hardware, before a production board is
spent: the burn, the key provider reading OTP, the boot key slots, the boot ROM refusing other
firmware, and everyday development on a board that only takes signed images.

It does not make dev-1 secret-keeping:

- **Debug stays on,** so anyone with a probe reads the root secret out of the running chip.
- **The signing keys are public** (`keys/dev-INSECURE-*.pem`). Anyone can sign firmware that
  dev-1 runs, and that firmware can read the root secret.
- So what is sealed on dev-1 is protected from someone with only a USB cable and no key, and
  from nobody else.

dev-1 was the only board all sessions develop on, so a mistake at the last step would have left
the project without a board until another arrived. That is why everything that could be checked
on the open board was checked first (see the table below).

## What it got

The dev-1 column of the [decisions](../../security/decisions.md#summary): the root secret in
OTP, the two dev boot keys, and secure boot. Not debug off, not `DISABLE_WATCHDOG_SCRATCH`, not
the glitch detector bit, not the console-only marker (so it can still be provisioned from a
file), and no lock on the boot-flag and boot-key pages (decision 11).

Moving the root secret to OTP re-keyed the board: the sealed configuration no longer opened, the
SSH host key was replaced, the console password went with the configuration, and the encrypted
card was formatted again. dev-1's contents are disposable, so nothing was saved first.

## The rehearsal

Decision 6 first approved a rehearsal of the burn "in spare OTP pages". With dev-1 getting a
real root secret, the rehearsal and the real burn became the same run (agreed 2026-11-25):

1. The first burn on dev-1 was **stopped on purpose**, by a stop point in the burn command
   (`rootsecret burn rehearse`), after the data rows and their chaff were written and read back,
   before the check and the marker. The page was then locked like any other spent page. Not by pulling the cable: a power cut lands wherever it
   lands and tests a different state each time; the stop point gives the one state the skip
   logic is written for, and can be repeated. (The plan was a rehearsal option that the
   production burn image would be built without; as built, every burn image accepts
   `rehearse`.) A real power cut partway through a row is a different case and was not tested.
2. **Any ending but "rehearsal: stopped on purpose", with the page locked, stopped the run.** It
   was the first time `program()`, the boot ROM's write call and the read-back through the
   memory-mapped alias ran on a chip at all: a failed row would have meant one of them was
   wrong, and wrong for the real secret too.
3. Only then was the burn run again, without the stop point. It landed in page 5.
4. Reads of both pages from BOOTSEL were then refused.

**What the spent page holds:** 32 random bytes with their chaff and no marker. They were never a
root secret: nothing was derived from or sealed under them, and the second run took fresh bytes
from the TRNG. The plan before #87 expected the spent page to stay unlocked until the second
run, which is why the two runs were done in one sitting; as built, the rehearsal locks it
itself (runbook step 4: `OTP page 4: locked against the bootloader`). The plan also listed a
check that the second page's secret differs from the first page's contents; the runbook as run
has no such step.

This exercised the skip logic on the real page list, at the cost of one of dev-1's three list
pages. The alternative, a rehearsal build with its own list of spare pages, would have kept all
three but tested code the production board won't run.

## What ran on hardware for the first time

Everything here ran on 2026-11-26 and behaved as expected (results on
[#64](https://github.com/j3m7/sos/issues/64)), with one exception: an image signed with an
unknown key was not tried, only an unsigned one. The production board should try both.

| First run | Runbook step | Could it be checked before secure boot was on? |
|---|---|---|
| The burn command, its chaff, marker and locks; a stopped burn; the firmware skipping a spent page | 4, 5 | yes |
| The key provider reading the root secret from OTP and locking the page at the end of boot | 6 | yes |
| `picotool otp get` refused on the locked pages, from BOOTSEL | 6 | yes |
| `picotool otp load` of the boot key file; `picotool otp set` of a field | 7, 8 | yes |
| The slot byte order on a chip; `bootkey` saying `NOT YET` | 7 | yes |
| `bootkey` saying `MATCH`, for each of the two keys | 8, 9 | yes |
| `picotool load -f` of a signed image on a board with valid boot keys | 9 | yes |
| BOOTSEL by the button, then `picotool load --ser` by hand (the recovery path) | 2 | yes, on the open board |
| The boot ROM starting a signed image with secure boot on | 10 | **no** |
| The boot ROM refusing an unsigned image, and the board coming back to BOOTSEL so a good image can be loaded | 11 | **no, and it can't be:** an open board runs anything |
| `picotool load -f` and `ninja flash` on a secured board with the bootloader kept | after 10 | **no** |

The "no" rows were the accepted risk of doing this on the only board. What could be done about
them beforehand was everything above them: the key compared four ways, the signature verified
by `picotool`, and both keys shown to match.

**Not recorded:** the rehearsal's duration (`It took ... ms.`, runbook step 4), which #64 notes.
It is the time the chip needs per OTP row, and says how much margin the burn's watchdog feeding
has: record it on the next burn (#100).

**Not seen:** the `version` warning (`WARNING: the OTP rows say otherwise`) between an OTP write
and the next reset. The writes were made with `picotool` from BOOTSEL, after which the board
restarts, so there was no window to see it.

**Attack trials:** only the USB side was tried on dev-1 (reads of the locked pages refused, an
unsigned image refused). The rest of the production list is for the production board (#100).

## Page 3 lock

Decision 11 on dev-1: only page 3 and the root secret's list pages are locked against
bootloader writes. The list pages were locked by the burn, page 3 by runbook step 12 (lock
word 0x101010: bootloader read-only). Its test writes the value 0 to row 0x0ff, which changes no
bit even if the lock didn't take and is nowhere near the console-only marker's rows. The write
is refused (`permission failure`), while its `OLD_VALUE=0x000000` shows the bootloader can still
read page 3, as intended. No other value or row is used to test a lock.

## Living with it

- Every image flashed to dev-1 is signed with one of the two dev keys, for good. An unsigned
  image doesn't start; recovery is loading a signed one over USB (runbook step 2).
- Every session flashes dev-1 through the flash guard (`ninja flash` or `tools/flash.sh`, with
  `SOS_FLASH_SERIAL` set), whose key list (`~/.config/sos/flash-keys`) holds the two dev keys'
  hashes.
- If both dev keys were lost, dev-1 would keep its last firmware for ever. They are in the
  repository, which is their backup.
- Anyone with a cable can add a boot key to a blank slot and run their own firmware, or mark the
  dev keys invalid and end the board's life. Neither loses anything that matters on this board.
- The board registry records what was written to dev-1's OTP, with the date.
