# Securing dev-1: runbook

Do the steps in order. Each gives a command, what you should see, and when to stop. Why each
step exists is in [`docs/security/design.md`](../../security/design.md) and [RECORD.md](RECORD.md).

- Board: dev-1, serial `F94525FD61D9A1CC`. **No other board connected.**
- **PERMANENT** steps write one-time-programmable (OTP) memory and can't be undone.
- **If what you see differs from "You should see", stop.** Don't retry and don't go on.
- Run from the repository root, on current main, with `picotool` 2.3.1.
- **Run one `picotool` or `flash.sh` command at a time.** Each one restarts the board. Wait for
  its console device (`/dev/serial/by-id/usb-Raspberry_Pi_Pico_F94525FD61D9A1CC-if00`) to come
  back before the next, or the next fails with `ERROR: ... rebooting`. That error is not a
  failed write: run the read-back again on its own.
- **A refused `picotool` command leaves the board in its bootloader** (no console; USB shows
  "RP2350 Boot"). Steps 6 and 12 expect refusals. Bring it back with
  `picotool reboot --ser $SOS_FLASH_SERIAL`.
- dev-1 went through this runbook on 2026-11-26, with every step as written here.

## Before you start

- [ ] `~/.config/sos/flash-keys` lists `e2f7cff0...0d51` and `b97c0fd7...c910`.
- [ ] Nobody else is using the board, and its USB cable and power won't be touched for an hour.
- [ ] You can reach the BOOTSEL button.

Open the console in a terminal of its own, and keep it open:

```sh
picocom -b 115200 /dev/serial/by-id/usb-Raspberry_Pi_Pico_F94525FD61D9A1CC-if00
```

It drops when the board restarts: quit (Ctrl-A Ctrl-X) and start it again once the board is
back.

Run the commands below in one terminal that stays open, and set this there, once. (In Claude
Code, each `!` command runs in a new shell, so an `export` there doesn't carry over.)

```sh
export SOS_FLASH_SERIAL=F94525FD61D9A1CC
```

From step 5 on, the board's saved configuration, SSH host key and SD card encryption no longer
open: they belonged to the old root secret. The board has no network until you set it up again
in step 13.

## 1. Save the OTP and build the images

```sh
picotool otp dump -f --ser $SOS_FLASH_SERIAL > dev-1-otp-before.txt
cmake --preset debug -DSOS_SIGN_KEY=$PWD/keys/dev-INSECURE-1.pem && cmake --build build
cmake --preset debug -B build-burn -DSOS_BURN_IMAGE=ON -DSOS_SIGN_KEY=$PWD/keys/dev-INSECURE-1.pem && cmake --build build-burn
cmake --preset debug -B build-key2 -DSOS_SIGN_KEY=$PWD/keys/dev-INSECURE-2.pem && cmake --build build-key2
cmake --preset debug -B build-unsigned -DSOS_SIGN_KEY=none && cmake --build build-unsigned
for b in build build-burn build-key2 build-unsigned; do tools/flash.sh -n $b/sos.bin; done
```

| Directory | What it is |
|---|---|
| `build` | The everyday firmware, signed with the first key. The two keys are the public dev keys in [`keys/`](../../../keys/README.md) |
| `build-burn` | The burn image: the everyday firmware plus `rootsecret burn`. Used only in steps 3 to 5 |
| `build-key2` | The everyday firmware signed with the second key, for step 9 |
| `build-unsigned` | Not signed, for step 11 |

You should see: the dump is a few hundred lines (keep it). The four checks print, in order,
`e2f7...0d51`, `e2f7...0d51`, `b97c...c910`, and
`flash guard: REFUSED. build-unsigned/sos.bin is not signed.`

## 2. Walk the recovery path

Unplug the board, hold BOOTSEL, plug it in, let go.

```sh
picotool info --ser $SOS_FLASH_SERIAL
picotool load -v -x build/sos.bin -t bin -o 0x10000000 --ser $SOS_FLASH_SERIAL
```

You should see: `info` shows `name: SOS`; `load` ends with `OK`, and the console comes back.
This is how you get the board back if a later flash leaves it in the bootloader.

## 3. Flash the burn image

```sh
tools/flash.sh build-burn/sos.bin
```

On the console: `version`, then `rootsecret burn plan`. Then hold BOOTSEL and run
`rootsecret burn plan` again.

You should see: `THIS IS THE BURN IMAGE`; `It would use OTP page 4 (rows 0x100 to 0x13f).`;
`BOOTSEL button: not held.`, and with the button held, `BOOTSEL button: held`.

## 4. Rehearsal: spend page 4 on purpose (PERMANENT)

On the console: `rootsecret burn rehearse`. At the serial prompt, type `F94525FD61D9A1CC`, then
`REHEARSE`. Hold BOOTSEL until it says you can let go.

You should see:

```
rehearsal: stopped on purpose before the check and the marker; that page is now spent.
OTP page 4: locked against the bootloader
It took ... ms.
```

Write down the time. **Stop if** you see `STOP HERE`, or the console goes quiet, or the board
restarts before these lines appear. In each case page 4 may be spent in a way that looks like a
good rehearsal, so don't run step 5.

## 5. Write the root secret (PERMANENT)

On the console: `rootsecret burn`. Type the serial, then `BURN`. Hold BOOTSEL until told to let
go.

You should see:

```
the root secret is written, verified and its page locked.
OTP page 5: locked against the bootloader
```

## 6. Everyday firmware back; check the secret

```sh
tools/flash.sh build/sos.bin
```

On the console: `rootsecret`. Then read each page on its own (`picotool` stops at the first
refused row, so one command can't test both):

```sh
picotool otp get -r -n -f --ser $SOS_FLASH_SERIAL 0x140
picotool otp get -r -n --ser $SOS_FLASH_SERIAL 0x100
picotool reboot --ser $SOS_FLASH_SERIAL
```

The second read has no `-f`: the first refusal left the board in its bootloader.

You should see: `root secret: present, in OTP page 5`. Both reads end
`ERROR: The RP2350 device returned an error: permission failure`. The board then restarts into
SOS. **Stop if** either read shows a value: that page isn't locked.

## 7. Write the boot key hashes (PERMANENT)

```sh
picotool otp load docs/otp/dev-1/boot-keys.json -f --ser $SOS_FLASH_SERIAL
```

On the console: `bootkey`.

You should see: slot 0 `e2f7cff033600f5ab00e33b6f891a0ff6e635ad0f2088613ca8b71b5f56d0d51` and
slot 1 `b97c0fd7efb68937e733c4916b44ce4f940141c4e6ecc90994c941f396d7c910`, both
`not marked valid`, and a last line starting `NOT YET`. Compare all 64 digits of each slot.

## 8. Mark both keys valid (PERMANENT)

```sh
picotool otp set BOOT_FLAGS1.KEY_VALID 3 -f --ser $SOS_FLASH_SERIAL
picotool otp get -c 3 -n -f --ser $SOS_FLASH_SERIAL BOOT_FLAGS1
```

You should see: `OLD_VALUE=0x000000`, then `KEY_VALID ... = 3` and `KEY_INVALID ... = 0`.
On the console, `bootkey` ends with `MATCH: slot 0`.

## 9. Check the second key

```sh
tools/flash.sh build-key2/sos.bin      # console: bootkey -> MATCH: slot 1
tools/flash.sh build/sos.bin           # console: bootkey -> MATCH: slot 0
```

**Stop if** either is not `MATCH`. After step 10, a key that doesn't match never boots.

## 10. Secure boot on (PERMANENT, no way back)

```sh
picotool info -a -f --ser $SOS_FLASH_SERIAL
picotool otp set CRIT1.SECURE_BOOT_ENABLE 1 -f --ser $SOS_FLASH_SERIAL
picotool otp get -c 8 -n -f --ser $SOS_FLASH_SERIAL CRIT1
```

On the console: `version`.

You should see: `info -a` shows `signature: verified`. Then `OLD_VALUE=0x000000`, then
`SECURE_BOOT_ENABLE ... = 1` with every other field 0. `version` shows `secure boot: on`.

If the console doesn't come back, do step 2's recovery with `build/sos.bin`. If that doesn't
run either, stop.

## 11. Check it refuses an unsigned image

```sh
SOS_FLASH_UNGUARDED=1 tools/flash.sh build-unsigned/sos.bin
picotool load -v -x build/sos.bin -t bin -o 0x10000000 --ser $SOS_FLASH_SERIAL
```

You should see: after the first command the console does **not** come back. After the second,
`OK` and the console is back. If `picotool` can't find the board, use BOOTSEL as in step 2.

## 12. Lock page 3 against the bootloader (PERMANENT)

```sh
picotool otp set -r PAGE3_LOCK1 0x101010 -f --ser $SOS_FLASH_SERIAL
picotool otp get -r -n -f --ser $SOS_FLASH_SERIAL PAGE3_LOCK1
picotool otp set -r 0x0ff 0 -f --ser $SOS_FLASH_SERIAL
picotool reboot --ser $SOS_FLASH_SERIAL
```

You should see: `OLD_VALUE=0x000000`, then `VALUE 0x101010`, then the second `otp set` shows
`OLD_VALUE=0x000000` (the bootloader may still read page 3) and ends
`ERROR: ... permission failure`. `picotool reboot` brings the board back from its bootloader.

The `otp set` of row 0x0ff writes the value 0, so it changes nothing even if the lock didn't
take. **Use exactly that command: no other row, no other value.**

## 13. Save the OTP and set the board up

```sh
picotool otp dump -f --ser $SOS_FLASH_SERIAL > dev-1-otp-after.txt
```

On the console: `setup` (console password, Wi-Fi, your SSH key), then `sd format` and `yes`.
**Write down the card key it shows.** Then `reboot`.

You should see: the console asks for the new password; Wi-Fi connects; the log line
`sshd: listening ... host key ...` shows a new fingerprint; `sd` shows the card mounted.

## Afterwards

- Board registry: pages 4 (spent) and 5 (root secret) locked, boot keys in slots 0 and 1, both
  valid, secure boot on, page 3 locked. Today's date.
- Issue #64: the rehearsal time from step 4, and anything that didn't match this page.
- SSH clients will warn that the host key changed; forget the old one.
- dev-1 now runs only images signed with one of the two dev keys. `ninja flash` (the guard)
  refuses anything else.

Left open on dev-1 on purpose: debug, reboot into the bootloader on command, key slots 2 and 3,
and writes to pages 1 and 2 over USB. dev-1 shows the mechanism works. It doesn't keep secrets
from someone who has it in hand.
