# Securing dev-1: runbook

For the person at the board. You start one script and it leads you through. It runs every
command and checks every result itself; you do the few things only a person can do.
Why each step exists is in [`docs/securing-a-board.md`](../../securing-a-board.md); you don't
need it open.

- Board: dev-1, serial `F94525FD61D9A1CC`. **No other board connected.**
- Six steps are **PERMANENT**: they write one-time-programmable memory and can't be undone.
  The script names each one, shows the exact command and what it changes for good, and waits
  for you to type that step's word.
- **If the script stops, stop.** It says what it expected and what it saw. Don't work around
  it by hand. Find out why first; the script can then be started again and carries on from
  where the board is.
- Nothing here has been run end to end on a real board. The script's read-only mode has been
  run on dev-1; its permanent steps have only ever run against a pretend board in tests.

## To see the steps without touching anything

```sh
tools/secure-board.py plan dev-1
```

This prints every step and every command, in order. It is the list of steps; this page does
not repeat it.

## Before you start

- [ ] This runbook and the script are approved and on main. Your checkout is current main with
      nothing uncommitted (the script checks, and stops otherwise).
- [ ] A copy of both signing keys (`sos-dev-sign.pem`, `sos-dev-sign-2.pem`) exists somewhere
      other than this computer. **Without it, a lost disk means dev-1 can never be reflashed.**
      The script asks you to confirm this twice.
- [ ] `~/.config/sos/flash-keys` lists both key hashes (the script checks).
- [ ] `~/.config/sos/sign-keys` has one line for the second key, so the script can build the
      image signed with it: `b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910 <path to sos-dev-sign-2.pem>`
- [ ] The board has **no console password** (dev-1 has none now). The script never types one
      and stops if the console asks.
- [ ] No terminal program is open on the board's console. The script uses the console itself.
- [ ] Everyone else who uses the board has been told, and nobody is using it.
- [ ] The board is on a USB port and power that nobody will touch for an hour, and you can
      reach its BOOTSEL button.

What will change on the board: its saved configuration, its SSH host key and its SD card's
encryption all belong to the root secret it has now. Once the new root secret is written they
no longer open. From then until the end of the run the board has no settings and no network.
You set it up again as the last step.

## Run it

From the repository root, in a terminal, yourself:

```sh
tools/secure-board.py run dev-1
```

## What you do during the run

The script tells you when. Nothing else is typed by hand.

| When the script says | You |
|---|---|
| Type `BACKUP` | Type it only if the key copy really exists. Asked at the start and again before secure boot. |
| Unplug the board, hold BOOTSEL, plug it in, let go | Do that, then type `DONE`. |
| Hold the BOOTSEL button | Hold it until the script says to let go. Three times: a test, the rehearsal, the real burn. |
| `PERMANENT:` ... Type `REHEARSE` / `BURN` / `KEYS` / `VALID` / `SECUREBOOT` / `LOCK` | Read the command and the "what this changes for good" line. Type the word to do it. Anything else stops, with nothing written. |
| The board is in its bootloader ... Type `FLASH` | Only appears if you start the script with the board sitting in its bootloader (for example after an earlier stop). Typing it flashes the everyday image through the guard. It writes flash, not OTP. |
| A question ending `Type YES or NO` | Read the output shown above it and answer truthfully. There are four: the board answering from its bootloader, the OTP reads being refused, the signature being verified, the test write being refused. These are outputs nobody has seen yet, so the script can't judge them. `NO` stops the run. |

`SECUREBOOT` is the step with no way back: after it the board runs only firmware signed with
one of the two keys, for ever.

## If a burn goes wrong

Two files can appear next to the script's records
(`~/.local/state/sos/boards/F94525FD61D9A1CC/`). While either is there the script refuses to
run on this board at all:

- `BURN-FAILED`: the burn image said STOP HERE.
- `BURN-UNFINISHED`: a burn was started and the script never finished checking its result (it
  timed out, was closed, or the board reset).

In both cases a page may be spent and locked, and the board can't say whether it holds what
the next step assumes. Don't remove the file until the cause is understood. A burn that failed
part way leaves its page spent without a secret; the script says so when it finds one, and
going on from there is a decision, not a retry.

## Last: set the board up (you, on the console)

When the OTP steps are done the script lets go of the console and waits. Open your terminal
program on the board's console and:

1. `setup`: the console password, the Wi-Fi network, your SSH public key.
2. `sd format`, type `yes`. **Write down the card key it shows.**
3. `reboot`.

You should see after the reboot: the console asks for the password you just set; Wi-Fi
connects; the log line `sshd: listening ... host key ...` shows a fingerprint different from
the old one; `sd` shows the card mounted. Then type `DONE` in the script.

The password and the card key never pass through the script or its record.

## Afterwards

- The script says where its record is (`~/.local/state/sos/boards/F94525FD61D9A1CC/<date>/`):
  the OTP before and after, every command with its output and time.
- Note in the board registry what was written and when: pages 4 and 5 (root secret, page 4
  spent), boot keys in slots 0 and 1, both valid, secure boot on, page 3 locked.
- Record on issue #64: the burn time the script printed, and anything that made it stop.
- SSH clients will warn about a changed SSH host key; forget the old one.
- From now on dev-1 runs only images signed with one of the two dev keys. Flash it with
  `ninja flash`; the flash guard refuses an image that isn't.

To look at the board later without changing anything: `tools/secure-board.py check dev-1`.

## What is left open on dev-1, on purpose

Debug is still enabled, the board still reboots into the bootloader on command, key slots 2
and 3 are blank, and pages 1 and 2 can still be written over USB. The glitch detector is on,
turned on by the firmware at every boot as before; it is not enabled in OTP. dev-1 shows that the
mechanism works. It does not keep secrets from someone with the board in their hands.
